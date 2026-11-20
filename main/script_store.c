#include "script_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board_lua.h"
#include "esp_littlefs.h"
#include "esp_partition.h"

#define WORKSPACE_LABEL "workspace"
#define WORKSPACE_PATH "/workspace"
#define SCRIPTS_PATH WORKSPACE_PATH "/scripts"
#define UPLOAD_PATH SCRIPTS_PATH "/.upload.tmp"

enum {
    WORKSPACE_SIZE = 384 * 1024,
    SCRIPT_COUNT_MAX = 64,
    SCRIPT_PATH_SIZE = sizeof(SCRIPTS_PATH) + 1 + SCRIPT_STORE_NAME_MAX,
};

typedef struct {
    char name[SCRIPT_STORE_NAME_MAX + 1];
    size_t size;
} script_info_t;

static bool store_ready;

static esp_err_t file_error(int error)
{
    switch (error) {
    case ENOENT: return ESP_ERR_NOT_FOUND;
    case ENOSPC: return ESP_ERR_NOT_FINISHED;
    case ENOMEM: return ESP_ERR_NO_MEM;
    default: return ESP_FAIL;
    }
}

static esp_err_t require_erased(const esp_partition_t *partition)
{
    uint8_t block[512];
    for (size_t offset = 0; offset < partition->size; offset += sizeof(block)) {
        esp_err_t err = esp_partition_read(partition, offset, block, sizeof(block));
        if (err != ESP_OK) {
            return err;
        }
        for (size_t i = 0; i < sizeof(block); ++i) {
            if (block[i] != UINT8_MAX) {
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t prepare_script_directory(void)
{
    struct stat info;
    if (stat(SCRIPTS_PATH, &info) == 0) {
        if (!S_ISDIR(info.st_mode)) {
            return ESP_ERR_INVALID_STATE;
        }
    } else if (errno != ENOENT || mkdir(SCRIPTS_PATH, 0700) != 0) {
        return file_error(errno);
    }

    if (stat(UPLOAD_PATH, &info) != 0) {
        return errno == ENOENT ? ESP_OK : file_error(errno);
    }
    if (!S_ISREG(info.st_mode)) {
        return ESP_ERR_INVALID_STATE;
    }
    return unlink(UPLOAD_PATH) == 0 ? ESP_OK : file_error(errno);
}

esp_err_t script_store_init(void)
{
    if (store_ready) {
        return ESP_OK;
    }

    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, WORKSPACE_LABEL);
    if (!partition) {
        return ESP_ERR_NOT_FOUND;
    }
    if (partition->size != WORKSPACE_SIZE || partition->encrypted) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_vfs_littlefs_conf_t config = {
        .base_path = WORKSPACE_PATH,
        .partition_label = WORKSPACE_LABEL,
    };
    esp_err_t err = esp_vfs_littlefs_register(&config);
    if (err == ESP_FAIL) {
        // A mount failure alone cannot distinguish blank flash from damaged data.
        err = require_erased(partition);
        if (err != ESP_OK) {
            return err;
        }
        config.format_if_mount_failed = true;
        err = esp_vfs_littlefs_register(&config);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = prepare_script_directory();
    if (err != ESP_OK) {
        esp_vfs_littlefs_unregister(WORKSPACE_LABEL);
        return err;
    }
    store_ready = true;
    return ESP_OK;
}

static bool ascii_alnum(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool script_store_validate_name(const char *name)
{
    if (!name || !ascii_alnum(name[0])) {
        return false;
    }
    size_t length = strnlen(name, SCRIPT_STORE_NAME_MAX + 1);
    if (length < 5 || length > SCRIPT_STORE_NAME_MAX || strcmp(name + length - 4, ".lua") != 0) {
        return false;
    }
    for (size_t i = 1; i < length - 4; ++i) {
        char c = name[i];
        if (!ascii_alnum(c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

static void script_path(char path[SCRIPT_PATH_SIZE], const char *name)
{
    snprintf(path, SCRIPT_PATH_SIZE, SCRIPTS_PATH "/%.*s", SCRIPT_STORE_NAME_MAX, name);
}

static esp_err_t scan_scripts(script_info_t *entries, size_t *count)
{
    DIR *dir = opendir(SCRIPTS_PATH);
    if (!dir) {
        return file_error(errno);
    }

    esp_err_t err = ESP_OK;
    *count = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) {
            err = errno == 0 ? ESP_OK : file_error(errno);
            break;
        }
        if (entry->d_type != DT_REG || !script_store_validate_name(entry->d_name)) {
            continue;
        }
        if (*count == SCRIPT_COUNT_MAX) {
            err = ESP_ERR_INVALID_STATE;
            break;
        }
        if (entries) {
            char path[SCRIPT_PATH_SIZE];
            script_path(path, entry->d_name);
            struct stat info;
            if (stat(path, &info) != 0) {
                err = file_error(errno);
                break;
            }
            script_info_t *script = &entries[*count];
            strcpy(script->name, entry->d_name);
            script->size = (size_t)info.st_size;
        }
        ++*count;
    }
    if (closedir(dir) != 0 && err == ESP_OK) {
        err = file_error(errno);
    }
    return err;
}

esp_err_t script_store_write(const char *name, const char *bytes, size_t size)
{
    if (!script_store_validate_name(name) || !bytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size == 0 || size > BOARD_LUA_MAX_SOURCE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memchr(bytes, '\0', size)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[SCRIPT_PATH_SIZE];
    script_path(path, name);
    struct stat info;
    if (stat(path, &info) == 0) {
        if (!S_ISREG(info.st_mode)) {
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        if (errno != ENOENT) {
            return file_error(errno);
        }
        size_t count;
        esp_err_t err = scan_scripts(NULL, &count);
        if (err != ESP_OK) {
            return err;
        }
        if (count == SCRIPT_COUNT_MAX) {
            return ESP_ERR_NOT_FINISHED;
        }
    }

    int fd = open(UPLOAD_PATH, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return file_error(errno);
    }

    esp_err_t err = ESP_OK;
    size_t written = 0;
    while (written < size) {
        ssize_t n = write(fd, bytes + written, size - written);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            err = n == 0 ? ESP_FAIL : file_error(errno);
            break;
        }
        written += (size_t)n;
    }
    if (err == ESP_OK && fsync(fd) != 0) {
        err = file_error(errno);
    }
    if (close(fd) != 0 && err == ESP_OK) {
        err = file_error(errno);
    }
    if (err == ESP_OK && rename(UPLOAD_PATH, path) != 0) {
        err = file_error(errno);
    }
    if (err != ESP_OK) {
        unlink(UPLOAD_PATH);
    }
    return err;
}

esp_err_t script_store_read(const char *name, char **buffer, size_t *size)
{
    if (!buffer || !size) {
        return ESP_ERR_INVALID_ARG;
    }
    *buffer = NULL;
    *size = 0;
    if (!script_store_validate_name(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!store_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[SCRIPT_PATH_SIZE];
    script_path(path, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return file_error(errno);
    }

    struct stat info;
    esp_err_t err = ESP_OK;
    char *data = NULL;
    if (fstat(fd, &info) != 0) {
        err = file_error(errno);
    } else if (!S_ISREG(info.st_mode)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (info.st_size <= 0 || info.st_size > BOARD_LUA_MAX_SOURCE_BYTES) {
        err = ESP_ERR_INVALID_SIZE;
    } else {
        data = malloc((size_t)info.st_size + 1);
        if (!data) {
            err = ESP_ERR_NO_MEM;
        }
    }

    size_t received = 0;
    while (err == ESP_OK && received < (size_t)info.st_size) {
        ssize_t n = read(fd, data + received, (size_t)info.st_size - received);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            err = n == 0 ? ESP_FAIL : file_error(errno);
            break;
        }
        received += (size_t)n;
    }
    if (close(fd) != 0 && err == ESP_OK) {
        err = file_error(errno);
    }
    if (err != ESP_OK) {
        free(data);
        return err;
    }
    data[received] = '\0';
    *buffer = data;
    *size = received;
    return ESP_OK;
}

static int compare_script_names(const void *left, const void *right)
{
    const script_info_t *a = left;
    const script_info_t *b = right;
    return strcmp(a->name, b->name);
}

esp_err_t script_store_list(char **json)
{
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }
    *json = NULL;
    if (!store_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    script_info_t *entries = malloc(SCRIPT_COUNT_MAX * sizeof(*entries));
    if (!entries) {
        return ESP_ERR_NO_MEM;
    }
    size_t count;
    esp_err_t err = scan_scripts(entries, &count);
    if (err != ESP_OK) {
        free(entries);
        return err;
    }
    qsort(entries, count, sizeof(*entries), compare_script_names);

    const char prefix[] = "{\"scripts\":[";
    const char suffix[] = "]}";
    const char format[] = "%s{\"name\":\"%s\",\"bytes\":%zu}";
    // Reserve space for punctuation, the validated ASCII name, and decimal size.
    size_t capacity = sizeof(prefix) + sizeof(suffix) +
                      count * (sizeof(format) + SCRIPT_STORE_NAME_MAX + 3 * sizeof(size_t));
    char *output = malloc(capacity);
    if (!output) {
        free(entries);
        return ESP_ERR_NO_MEM;
    }
    memcpy(output, prefix, sizeof(prefix) - 1);
    size_t length = sizeof(prefix) - 1;
    for (size_t i = 0; i < count; ++i) {
        length += snprintf(output + length, capacity - length, format,
                           i == 0 ? "" : ",", entries[i].name, entries[i].size);
    }
    memcpy(output + length, suffix, sizeof(suffix));
    free(entries);
    *json = output;
    return ESP_OK;
}
