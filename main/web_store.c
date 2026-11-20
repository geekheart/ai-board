#include "web_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "psa/crypto.h"

#define WEB_PATH "/workspace/web"
#define BUNDLE_SUFFIX ".bundle"
#define UPLOAD_PATH WEB_PATH "/upload.tmp"
#define STATE_PATH WEB_PATH "/state.bin"
#define STATE_TEMP WEB_PATH "/state.tmp"

static const char bundle_magic[] = "AIWEB001";
static const char state_magic[] = "AIWSTATE";

enum {
    BUNDLE_ASSETS = 3,
    BUNDLE_VERSION_OFFSET = sizeof(bundle_magic) - 1,
    BUNDLE_LENGTHS_OFFSET = BUNDLE_VERSION_OFFSET + WEB_STORE_VERSION_LEN,
    BUNDLE_HASH_OFFSET = BUNDLE_LENGTHS_OFFSET + BUNDLE_ASSETS * 4,
    BUNDLE_HASH_SIZE = 32,
    BUNDLE_HEADER_SIZE = BUNDLE_HASH_OFFSET + BUNDLE_HASH_SIZE,
    BUNDLE_PATH_SIZE = sizeof(WEB_PATH "/") + WEB_STORE_VERSION_LEN + sizeof(BUNDLE_SUFFIX) - 1,
    MIN_GZIP_SIZE = 20,
    UPLOAD_CHUNK_SIZE = 2048,
};

typedef struct {
    char version[WEB_STORE_VERSION_LEN + 1];
    size_t lengths[BUNDLE_ASSETS];
} bundle_info_t;

typedef struct {
    char magic[sizeof(state_magic) - 1];
    web_store_state_t selection;
    // Keep the original on-disk layout readable by earlier firmware.
    char legacy_previous[WEB_STORE_VERSION_LEN + 1];
} disk_state_t;

static bool store_ready;
static web_store_state_t selection = { .active = WEB_STORE_EMBEDDED };

static esp_err_t file_error(int error)
{
    switch (error) {
    case ENOENT: return ESP_ERR_NOT_FOUND;
    case ENOSPC: return ESP_ERR_NOT_FINISHED;
    case ENOMEM: return ESP_ERR_NO_MEM;
    default: return ESP_FAIL;
    }
}

static bool valid_version(const char *version)
{
    if (strnlen(version, WEB_STORE_VERSION_LEN + 1) != WEB_STORE_VERSION_LEN) {
        return false;
    }
    for (size_t i = 0; i < WEB_STORE_VERSION_LEN; ++i) {
        const char c = version[i];
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) {
            return false;
        }
    }
    return true;
}

static void bundle_path(char path[BUNDLE_PATH_SIZE], const char *version)
{
    snprintf(path, BUNDLE_PATH_SIZE, WEB_PATH "/%.*s" BUNDLE_SUFFIX, WEB_STORE_VERSION_LEN, version);
}

static bool active_version(const char *version)
{
    return strcmp(version, selection.active) == 0;
}

static void collect_orphans(void)
{
    unlink(UPLOAD_PATH);
    unlink(STATE_TEMP);
    DIR *dir = opendir(WEB_PATH);
    if (!dir) {
        return;
    }
    for (struct dirent *entry; (entry = readdir(dir)); ) {
        const char *name = entry->d_name;
        if (strlen(name) != WEB_STORE_VERSION_LEN + sizeof(BUNDLE_SUFFIX) - 1 ||
            strcmp(name + WEB_STORE_VERSION_LEN, BUNDLE_SUFFIX) != 0) {
            continue;
        }
        char version[WEB_STORE_VERSION_LEN + 1] = {0};
        memcpy(version, name, WEB_STORE_VERSION_LEN);
        if (valid_version(version) && !active_version(version)) {
            char path[BUNDLE_PATH_SIZE];
            bundle_path(path, version);
            unlink(path);
        }
    }
    closedir(dir);
}

static esp_err_t write_all(int fd, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = write(fd, bytes + offset, length - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return n < 0 ? file_error(errno) : ESP_FAIL;
        }
        offset += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t finish_file(int fd, esp_err_t result, const char *temporary, const char *destination)
{
    if (result == ESP_OK && fsync(fd) != 0) {
        result = file_error(errno);
    }
    if (close(fd) != 0 && result == ESP_OK) {
        result = file_error(errno);
    }
    if (result == ESP_OK && rename(temporary, destination) != 0) {
        result = file_error(errno);
    }
    if (result != ESP_OK) {
        unlink(temporary);
    }
    return result;
}

static esp_err_t load_selection(bool *has_legacy_previous)
{
    FILE *file = fopen(STATE_PATH, "rb");
    if (!file) {
        return errno == ENOENT ? ESP_OK : file_error(errno);
    }

    disk_state_t state;
    bool valid = fread(&state, 1, sizeof(state), file) == sizeof(state) &&
                 fgetc(file) == EOF && !ferror(file);
    fclose(file);
    const web_store_state_t *saved = &state.selection;
    if (!valid || memcmp(state.magic, state_magic, sizeof(state.magic)) != 0 ||
        !memchr(saved->active, 0, sizeof(saved->active)) ||
        (!valid_version(saved->active) && strcmp(saved->active, WEB_STORE_EMBEDDED) != 0)) {
        return ESP_ERR_INVALID_STATE;
    }

    selection = *saved;
    *has_legacy_previous = state.legacy_previous[0] != '\0';
    return ESP_OK;
}

static esp_err_t persist_selection(const web_store_state_t *next)
{
    disk_state_t state = { .selection = *next };
    memcpy(state.magic, state_magic, sizeof(state.magic));
    int fd = open(STATE_TEMP, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return file_error(errno);
    }
    esp_err_t result = finish_file(fd, write_all(fd, &state, sizeof(state)), STATE_TEMP, STATE_PATH);
    if (result == ESP_OK) {
        selection = *next;
    }
    return result;
}

esp_err_t web_store_init(void)
{
    if (store_ready) {
        return ESP_OK;
    }
    if (!esp_littlefs_mounted("workspace")) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (mkdir(WEB_PATH, 0700) != 0 && errno != EEXIST) {
        return file_error(errno);
    }
    struct stat info;
    if (stat(WEB_PATH, &info) != 0) {
        return file_error(errno);
    }
    if (!S_ISDIR(info.st_mode)) {
        return ESP_ERR_INVALID_STATE;
    }
    bool has_legacy_previous = false;
    esp_err_t result = load_selection(&has_legacy_previous);
    if (result != ESP_OK) {
        return result;
    }
    collect_orphans();
    if (has_legacy_previous) {
        // The active bundle stays available even when the metadata rewrite
        // needs to retry during the next boot or successful upload.
        (void)persist_selection(&selection);
    }
    store_ready = true;
    return ESP_OK;
}

esp_err_t web_store_status(web_store_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_ready) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *state = selection;
    return ESP_OK;
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t parse_header(const uint8_t header[BUNDLE_HEADER_SIZE], size_t length, bundle_info_t *info)
{
    if (length < BUNDLE_HEADER_SIZE || length > WEB_STORE_MAX_BUNDLE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(header, bundle_magic, BUNDLE_VERSION_OFFSET) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(info->version, header + BUNDLE_VERSION_OFFSET, WEB_STORE_VERSION_LEN);
    info->version[WEB_STORE_VERSION_LEN] = '\0';
    if (!valid_version(info->version)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t remaining = length - BUNDLE_HEADER_SIZE;
    for (size_t i = 0; i < BUNDLE_ASSETS; ++i) {
        size_t asset_length = read_u32(header + BUNDLE_LENGTHS_OFFSET + i * 4);
        if (asset_length < MIN_GZIP_SIZE || asset_length > remaining) {
            return ESP_ERR_INVALID_SIZE;
        }
        info->lengths[i] = asset_length;
        remaining -= asset_length;
    }
    return remaining == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t receive_all(web_store_receive_t receive, void *context, void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    size_t offset = 0;
    while (offset < length) {
        int n = receive(context, bytes + offset, length - offset);
        if (n <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        if ((size_t)n > length - offset) {
            return ESP_ERR_INVALID_SIZE;
        }
        offset += (size_t)n;
    }
    return ESP_OK;
}

static bool gzip_header_valid(const uint8_t *bytes)
{
    return bytes[0] == 0x1f && bytes[1] == 0x8b && bytes[2] == 8 && (bytes[3] & 0xe0) == 0;
}

static esp_err_t receive_payload(int fd, const bundle_info_t *info, const uint8_t *expected_hash,
                                 web_store_receive_t receive, void *context)
{
    uint8_t *buffer = malloc(UPLOAD_CHUNK_SIZE);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    esp_err_t result = psa_hash_setup(&hash, PSA_ALG_SHA_256) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
    for (size_t i = 0; i < BUNDLE_ASSETS && result == ESP_OK; ++i) {
        size_t remaining = info->lengths[i];
        while (remaining > 0 && result == ESP_OK) {
            size_t chunk = remaining < UPLOAD_CHUNK_SIZE ? remaining : UPLOAD_CHUNK_SIZE;
            result = receive_all(receive, context, buffer, chunk);
            if (result != ESP_OK) {
                break;
            }
            if (remaining == info->lengths[i] && !gzip_header_valid(buffer)) {
                result = ESP_ERR_INVALID_ARG;
            } else if (psa_hash_update(&hash, buffer, chunk) != PSA_SUCCESS) {
                result = ESP_FAIL;
            } else {
                result = write_all(fd, buffer, chunk);
            }
            remaining -= chunk;
        }
    }
    if (result == ESP_OK) {
        uint8_t digest[BUNDLE_HASH_SIZE];
        size_t digest_length = 0;
        if (psa_hash_finish(&hash, digest, sizeof(digest), &digest_length) != PSA_SUCCESS ||
            digest_length != sizeof(digest)) {
            result = ESP_FAIL;
        } else if (memcmp(digest, expected_hash, sizeof(digest)) != 0) {
            result = ESP_ERR_INVALID_CRC;
        }
    }
    psa_hash_abort(&hash);
    free(buffer);
    return result;
}

esp_err_t web_store_install(size_t length, web_store_receive_t receive, void *context, web_store_state_t *state)
{
    if (!receive || !state) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_ready) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (length < BUNDLE_HEADER_SIZE || length > WEB_STORE_MAX_BUNDLE) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t header[BUNDLE_HEADER_SIZE];
    bundle_info_t info;
    esp_err_t result = receive_all(receive, context, header, sizeof(header));
    if (result != ESP_OK) {
        return result;
    }
    result = parse_header(header, length, &info);
    if (result != ESP_OK) {
        return result;
    }
    if (active_version(info.version)) {
        return ESP_ERR_INVALID_STATE;
    }
    collect_orphans();

    int fd = open(UPLOAD_PATH, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return file_error(errno);
    }
    result = write_all(fd, header, sizeof(header));
    if (result == ESP_OK) {
        result = receive_payload(fd, &info, header + BUNDLE_HASH_OFFSET, receive, context);
    }
    char path[BUNDLE_PATH_SIZE];
    bundle_path(path, info.version);
    result = finish_file(fd, result, UPLOAD_PATH, path);
    if (result != ESP_OK) {
        return result;
    }

    web_store_state_t next = {0};
    memcpy(next.active, info.version, sizeof(next.active));
    result = persist_selection(&next);
    collect_orphans();
    if (result == ESP_OK) {
        *state = selection;
    }
    return result;
}

esp_err_t web_store_open(const char *version, web_store_asset_t asset, FILE **file, size_t *length)
{
    if (!version || (unsigned)asset >= BUNDLE_ASSETS || !file || !length) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_ready) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!valid_version(version) || !active_version(version)) {
        return ESP_ERR_NOT_FOUND;
    }

    char path[BUNDLE_PATH_SIZE];
    bundle_path(path, version);
    FILE *opened = fopen(path, "rb");
    if (!opened) {
        return file_error(errno);
    }

    struct stat stats;
    uint8_t header[BUNDLE_HEADER_SIZE];
    bundle_info_t info;
    esp_err_t result = ESP_OK;
    if (fstat(fileno(opened), &stats) != 0) {
        result = file_error(errno);
    } else if (!S_ISREG(stats.st_mode) || fread(header, 1, sizeof(header), opened) != sizeof(header)) {
        result = ESP_FAIL;
    } else {
        result = parse_header(header, (size_t)stats.st_size, &info);
        if (result == ESP_OK && strcmp(info.version, version) != 0) {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    if (result != ESP_OK) {
        fclose(opened);
        return result;
    }

    size_t offset = BUNDLE_HEADER_SIZE;
    for (size_t i = 0; i < (unsigned)asset; ++i) {
        offset += info.lengths[i];
    }
    if (fseek(opened, (long)offset, SEEK_SET) != 0) {
        result = file_error(errno);
        fclose(opened);
        return result;
    }
    *file = opened;
    *length = info.lengths[asset];
    return ESP_OK;
}
