#include "board_lua.h"
#include "board_events.h"

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lualib.h"
#include "workbench.h"

enum {
    MAX_VM_BYTES = 64 * 1024,
    MAX_RESPONSE_BYTES = 24 * 1024,
    MAX_LOG_BYTES = 1024,
    MAX_FORM_BYTES = 1280,
    MAX_JSON_DEPTH = 16,
    VM_TASK_STACK_BYTES = 14 * 1024,
    INSTRUCTION_INTERVAL = 1000,
    MAX_HOOK_CALLS = 4000,
};

_Static_assert(sizeof(lua_Integer) == 4 && sizeof(lua_Number) == 4, "Use the Espressif Lua component ABI");

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} json_buffer_t;

typedef struct {
    const char *source;
    size_t source_bytes;
    uint32_t timeout_ms;
    SemaphoreHandle_t done;
    char *response;
    size_t vm_bytes;
    unsigned hook_calls;
    int64_t started;
    int64_t deadline;
    int64_t yielded;
    const char *kind;
    char *pending_reply;
    json_buffer_t output;
    char log[MAX_LOG_BYTES];
    size_t log_bytes;
} script_run_t;

static const char json_null, json_array_metatable;

static script_run_t *script_run(lua_State *lua)
{
    return *(script_run_t **)lua_getextraspace(lua);
}

static void *allocate_lua(void *context, void *pointer, size_t old_size, size_t size)
{
    script_run_t *run = context;
    if (!pointer) old_size = 0;
    if (!size) {
        free(pointer);
        run->vm_bytes -= old_size;
        return NULL;
    }
    if (size > MAX_VM_BYTES - (run->vm_bytes - old_size)) return NULL;
    void *result = realloc(pointer, size);
    if (result) run->vm_bytes = run->vm_bytes - old_size + size;
    return result;
}

static uint32_t remaining_ms(lua_State *lua)
{
    script_run_t *run = script_run(lua);
    const int64_t remaining = run->deadline - esp_timer_get_time();
    if (remaining < 1000) {
        run->kind = "timeout";
        luaL_error(lua, "script deadline exceeded");
        return 0;
    }
    return (uint32_t)(remaining / 1000);
}

static void count_instructions(lua_State *lua, lua_Debug *event)
{
    (void)event;
    script_run_t *run = script_run(lua);
    remaining_ms(lua);
    if (++run->hook_calls >= MAX_HOOK_CALLS) {
        run->kind = "limit";
        luaL_error(lua, "script instruction limit exceeded");
    }
    const int64_t now = esp_timer_get_time();
    if (now - run->yielded >= 20000) {
        vTaskDelay(1);
        run->yielded = esp_timer_get_time();
    }
}

static void append_json(json_buffer_t *json, const char *data, size_t length)
{
    if (json->failed) return;
    if (length > MAX_RESPONSE_BYTES - json->length) {
        json->failed = true;
        return;
    }
    const size_t needed = json->length + length + 1;
    if (needed > json->capacity) {
        size_t capacity = json->capacity ? json->capacity * 2 : 256;
        if (capacity < needed) capacity = needed;
        if (capacity > MAX_RESPONSE_BYTES + 1) capacity = MAX_RESPONSE_BYTES + 1;
        char *buffer = realloc(json->data, capacity);
        if (!buffer) {
            json->failed = true;
            return;
        }
        json->data = buffer;
        json->capacity = capacity;
    }
    memcpy(json->data + json->length, data, length);
    json->length += length;
    json->data[json->length] = '\0';
}

static void append_literal(json_buffer_t *json, const char *literal)
{
    append_json(json, literal, strlen(literal));
}

static size_t utf8_codepoint_length(const unsigned char *data, size_t length)
{
    const unsigned first = data[0];
    if (first < 0x80) return 1;
    if (first < 0xc2 || first > 0xf4) return 0;
    const size_t width = first <= 0xdf ? 2 : first <= 0xef ? 3 : 4;
    if (length < width) return 0;
    if ((first == 0xe0 && data[1] < 0xa0) || (first == 0xed && data[1] >= 0xa0) ||
        (first == 0xf0 && data[1] < 0x90) || (first == 0xf4 && data[1] >= 0x90)) return 0;
    for (size_t i = 1; i < width; ++i) {
        if ((data[i] & 0xc0) != 0x80) return 0;
    }
    return width;
}

static void append_string(json_buffer_t *json, const char *data, size_t length)
{
    append_literal(json, "\"");
    for (size_t i = 0; i < length && !json->failed;) {
        const unsigned byte = (unsigned char)data[i];
        if (byte == '"' || byte == '\\') {
            append_literal(json, "\\");
            append_json(json, data + i++, 1);
        } else {
            const size_t width = utf8_codepoint_length((const unsigned char *)data + i, length - i);
            if (byte < 32 || !width) {
                char escaped[7];
                snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
                append_json(json, escaped, 6);
                ++i;
            } else {
                append_json(json, data + i, width);
                i += width;
            }
        }
    }
    append_literal(json, "\"");
}

static bool encode_value(lua_State *lua, int index, unsigned depth, json_buffer_t *json);

static bool encode_table(lua_State *lua, int index, unsigned depth, json_buffer_t *json)
{
    size_t count = 0, last = 0;
    int key_type = LUA_TNONE;
    lua_pushnil(lua);
    while (lua_next(lua, index)) {
        const int type = lua_type(lua, -2);
        const bool mixed = count && key_type != type;
        if (mixed || (type != LUA_TSTRING && (type != LUA_TNUMBER || !lua_isinteger(lua, -2)))) {
            lua_pop(lua, 2);
            return false;
        }
        key_type = type;
        if (type == LUA_TNUMBER) {
            const lua_Integer key = lua_tointeger(lua, -2);
            if (key < 1) {
                lua_pop(lua, 2);
                return false;
            }
            if ((size_t)key > last) last = key;
        }
        ++count;
        lua_pop(lua, 1);
    }
    bool array = key_type == LUA_TNUMBER;
    if (!count && lua_getmetatable(lua, index)) {
        lua_rawgetp(lua, LUA_REGISTRYINDEX, &json_array_metatable);
        array = lua_rawequal(lua, -1, -2);
        lua_pop(lua, 2);
    }
    if (array && last != count) return false;
    append_literal(json, array ? "[" : "{");
    if (array) {
        for (size_t i = 1; i <= count; ++i) {
            if (i > 1) append_literal(json, ",");
            lua_rawgeti(lua, index, (lua_Integer)i);
            const bool ok = encode_value(lua, -1, depth + 1, json);
            lua_pop(lua, 1);
            if (!ok) return false;
        }
    } else {
        lua_pushnil(lua);
        for (size_t i = 0; lua_next(lua, index); ++i) {
            if (i) append_literal(json, ",");
            size_t length;
            const char *key = lua_tolstring(lua, -2, &length);
            append_string(json, key, length);
            append_literal(json, ":");
            if (!encode_value(lua, -1, depth + 1, json)) {
                lua_pop(lua, 2);
                return false;
            }
            lua_pop(lua, 1);
        }
    }
    append_literal(json, array ? "]" : "}");
    return !json->failed;
}

static bool encode_value(lua_State *lua, int index, unsigned depth, json_buffer_t *json)
{
    if (depth >= MAX_JSON_DEPTH || json->failed) return false;
    char number[40];
    size_t length;
    const char *string;
    switch (lua_type(lua, index)) {
        case LUA_TNIL:
            append_literal(json, "null");
            break;
        case LUA_TLIGHTUSERDATA:
            if (lua_touserdata(lua, index) != &json_null) return false;
            append_literal(json, "null");
            break;
        case LUA_TBOOLEAN:
            append_literal(json, lua_toboolean(lua, index) ? "true" : "false");
            break;
        case LUA_TNUMBER:
            if (lua_isinteger(lua, index)) {
                snprintf(number, sizeof(number), "%lld", (long long)lua_tointeger(lua, index));
            } else {
                const lua_Number value = lua_tonumber(lua, index);
                if (!isfinite(value)) return false;
                snprintf(number, sizeof(number), "%.*g", FLT_DECIMAL_DIG, (double)value);
            }
            append_literal(json, number);
            break;
        case LUA_TSTRING:
            string = lua_tolstring(lua, index, &length);
            append_string(json, string, length);
            break;
        case LUA_TTABLE:
            return lua_checkstack(lua, 4) && encode_table(lua, lua_absindex(lua, index), depth, json);
        default:
            return false;
    }
    return !json->failed;
}

static void skip_space(const char **cursor)
{
    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' || **cursor == '\n') ++*cursor;
}

static int digit_value(char input)
{
    if (input >= '0' && input <= '9') return input - '0';
    if (input >= 'a' && input <= 'f') return input - 'a' + 10;
    if (input >= 'A' && input <= 'F') return input - 'A' + 10;
    return -1;
}

static bool read_u16(const char **cursor, unsigned *value)
{
    *value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const int digit = digit_value(**cursor);
        if (digit < 0) return false;
        *value = *value * 16 + digit;
        ++*cursor;
    }
    return true;
}

static bool read_unicode(const char **cursor, luaL_Buffer *buffer)
{
    unsigned value;
    if (!read_u16(cursor, &value)) return false;
    if (value >= 0xd800 && value <= 0xdbff) {
        unsigned low;
        if (**cursor != '\\' || (*cursor)[1] != 'u') return false;
        *cursor += 2;
        if (!read_u16(cursor, &low) || low < 0xdc00 || low > 0xdfff) return false;
        value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
    } else if (value >= 0xdc00 && value <= 0xdfff) {
        return false;
    }

    if (value >= 0x10000) {
        luaL_addchar(buffer, 0xf0 | (value >> 18));
        luaL_addchar(buffer, 0x80 | ((value >> 12) & 0x3f));
    } else if (value >= 0x800) {
        luaL_addchar(buffer, 0xe0 | (value >> 12));
    } else if (value >= 0x80) {
        luaL_addchar(buffer, 0xc0 | (value >> 6));
    }
    if (value >= 0x800) luaL_addchar(buffer, 0x80 | ((value >> 6) & 0x3f));
    luaL_addchar(buffer, value < 0x80 ? value : 0x80 | (value & 0x3f));
    return true;
}

static bool decode_string(lua_State *lua, const char **cursor)
{
    if (**cursor != '"') return false;
    const char *start = ++*cursor;
    const char *end = start;
    while (*end && *end != '"' && *end != '\\') ++end;
    if (*end == '"') {
        lua_pushlstring(lua, start, end - start);
        *cursor = end + 1;
        return true;
    }
    if (!*end) return false;

    luaL_Buffer buffer;
    luaL_buffinit(lua, &buffer);
    luaL_addlstring(&buffer, start, end - start);
    const int buffer_index = lua_gettop(lua);
    *cursor = end;
    bool valid = true;
    while (**cursor && **cursor != '"') {
        char value = *(*cursor)++;
        if (value == '\\') {
            value = **cursor;
            if (!value) break;
            ++*cursor;
            if (value == 'u') {
                valid = read_unicode(cursor, &buffer);
                if (!valid) break;
                continue;
            }
            const char *codes = "\"\\/bfnrt";
            const char *code = strchr(codes, value);
            if (!code) {
                valid = false;
                break;
            }
            value = "\"\\/\b\f\n\r\t"[code - codes];
        }
        luaL_addchar(&buffer, value);
    }
    if (!valid || **cursor != '"') {
        lua_settop(lua, buffer_index - 1);
        return false;
    }
    ++*cursor;
    luaL_pushresult(&buffer);
    return true;
}

static bool decode_value(lua_State *lua, const char **cursor, unsigned depth);

static bool decode_table(lua_State *lua, const char **cursor, unsigned depth, bool object)
{
    const int initial_top = lua_gettop(lua);
    const char close = object ? '}' : ']';
    ++*cursor;
    lua_newtable(lua);
    if (!object) {
        lua_rawgetp(lua, LUA_REGISTRYINDEX, &json_array_metatable);
        lua_setmetatable(lua, -2);
    }
    skip_space(cursor);
    if (**cursor != close) {
        for (lua_Integer i = 1;; ++i) {
            if (object) {
                if (!decode_string(lua, cursor)) break;
                skip_space(cursor);
                if (**cursor != ':') break;
                ++*cursor;
            }
            if (!decode_value(lua, cursor, depth + 1)) break;
            if (object) lua_rawset(lua, -3);
            else lua_rawseti(lua, -2, i);
            skip_space(cursor);
            if (**cursor == close) {
                ++*cursor;
                return true;
            }
            if (**cursor != ',') break;
            ++*cursor;
            skip_space(cursor);
        }
        lua_settop(lua, initial_top);
        return false;
    }
    ++*cursor;
    return true;
}

static bool decode_value(lua_State *lua, const char **cursor, unsigned depth)
{
    if (depth >= MAX_JSON_DEPTH || !lua_checkstack(lua, 4)) return false;
    skip_space(cursor);
    switch (**cursor) {
        case '"': return decode_string(lua, cursor);
        case '{': return decode_table(lua, cursor, depth, true);
        case '[': return decode_table(lua, cursor, depth, false);
        case 'n':
            if (strncmp(*cursor, "null", 4) != 0) return false;
            *cursor += 4;
            lua_pushlightuserdata(lua, (void *)&json_null);
            return true;
        case 't':
            if (strncmp(*cursor, "true", 4) != 0) return false;
            *cursor += 4;
            lua_pushboolean(lua, true);
            return true;
        case 'f':
            if (strncmp(*cursor, "false", 5) != 0) return false;
            *cursor += 5;
            lua_pushboolean(lua, false);
            return true;
        default: {
            char *end;
            const double value = strtod(*cursor, &end);
            if (end == *cursor || !isfinite(value)) return false;
            char *integer_end;
            const int64_t integer = strtoll(*cursor, &integer_end, 10);
            if (integer_end == end && integer >= LUA_MININTEGER && integer <= LUA_MAXINTEGER) {
                lua_pushinteger(lua, (lua_Integer)integer);
            } else {
                lua_pushnumber(lua, (lua_Number)value);
            }
            *cursor = end;
            return true;
        }
    }
}

static bool identifier(const char *key, size_t length)
{
    if (!length || length >= 32) return false;
    for (size_t i = 0; i < length; ++i) {
        const char ch = key[i];
        if (!(ch >= 'a' && ch <= 'z') && ch != '_' && !(i && ch >= '0' && ch <= '9')) return false;
    }
    return true;
}

static bool form_value(const char *value, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        const unsigned ch = (unsigned char)value[i];
        if (ch < 32 || ch > 126 || ch == '&' || ch == '=' || ch == '%' || ch == '+') return false;
    }
    return true;
}

static int board_call(lua_State *lua)
{
    size_t op_bytes;
    const char *op = luaL_checklstring(lua, 1, &op_bytes);
    if (!identifier(op, op_bytes)) return luaL_argerror(lua, 1, "invalid operation name");
    const bool raw_json = !lua_isnoneornil(lua, 3);
    if (raw_json && strcmp(luaL_checkstring(lua, 3), "json") != 0) {
        return luaL_argerror(lua, 3, "expected json");
    }

    char form[MAX_FORM_BYTES];
    int used = snprintf(form, sizeof(form), "op=%s", op);
    if (!lua_isnoneornil(lua, 2)) {
        luaL_checktype(lua, 2, LUA_TTABLE);
        lua_pushnil(lua);
        while (lua_next(lua, 2)) {
            size_t key_bytes;
            const char *key = lua_type(lua, -2) == LUA_TSTRING ? lua_tolstring(lua, -2, &key_bytes) : NULL;
            if (!key || !identifier(key, key_bytes) || strcmp(key, "op") == 0) {
                return luaL_argerror(lua, 2, "invalid parameter name");
            }
            const char *value;
            size_t value_bytes;
            if (lua_isboolean(lua, -1)) {
                value = lua_toboolean(lua, -1) ? "yes" : "no";
                value_bytes = strlen(value);
            } else if (lua_type(lua, -1) == LUA_TSTRING || lua_isinteger(lua, -1)) {
                value = lua_tolstring(lua, -1, &value_bytes);
            } else {
                return luaL_argerror(lua, 2, "parameter values must be strings, integers, or booleans");
            }
            if (!form_value(value, value_bytes)) return luaL_argerror(lua, 2, "invalid parameter value");
            if (key_bytes + value_bytes + 2 >= sizeof(form) - (size_t)used) {
                return luaL_argerror(lua, 2, "workbench parameters are too long");
            }
            used += snprintf(form + used, sizeof(form) - used, "&%s=%s", key, value);
            lua_pop(lua, 1);
        }
    }

    script_run_t *run = script_run(lua);
    const char *diagnostic = NULL;
    const uint32_t available_ms = remaining_ms(lua);
    const esp_err_t result = workbench_lua_command(op, form, available_ms, &run->pending_reply, &diagnostic);
    remaining_ms(lua);
    if (result != ESP_OK) {
        free(run->pending_reply);
        run->pending_reply = NULL;
        lua_pushnil(lua);
        lua_pushstring(lua, diagnostic ? diagnostic : esp_err_to_name(result));
        return 2;
    }
    if (!run->pending_reply) return luaL_error(lua, "workbench returned no result");
    if (raw_json) {
        lua_pushstring(lua, run->pending_reply);
        free(run->pending_reply);
        run->pending_reply = NULL;
        return 1;
    }
    const char *cursor = run->pending_reply;
    const bool decoded = decode_value(lua, &cursor, 0);
    skip_space(&cursor);
    const bool complete = decoded && *cursor == '\0';
    free(run->pending_reply);
    run->pending_reply = NULL;
    if (!complete) return luaL_error(lua, "invalid workbench result");
    return 1;
}

static int board_sleep(lua_State *lua)
{
    const lua_Integer ms = luaL_checkinteger(lua, 1);
    luaL_argcheck(lua, ms >= 0 && ms <= BOARD_LUA_MAX_TIMEOUT_MS, 1, "invalid delay");
    if (ms >= (lua_Integer)remaining_ms(lua)) {
        script_run(lua)->kind = "timeout";
        return luaL_error(lua, "delay exceeds remaining script time");
    }
    if (ms > 0) {
        const TickType_t ticks = ((uint32_t)ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS;
        vTaskDelay(ticks);
    }
    remaining_ms(lua);
    return 0;
}

static void log_text(script_run_t *run, const char *text, size_t length)
{
    const size_t room = sizeof(run->log) - run->log_bytes;
    if (length > room) length = room;
    memcpy(run->log + run->log_bytes, text, length);
    run->log_bytes += length;
}

static int script_print(lua_State *lua)
{
    script_run_t *run = script_run(lua);
    const int count = lua_gettop(lua);
    for (int i = 1; i <= count && run->log_bytes < sizeof(run->log); ++i) {
        size_t length;
        const char *text = luaL_tolstring(lua, i, &length);
        if (i > 1) log_text(run, "\t", 1);
        log_text(run, text, length);
        lua_pop(lua, 1);
    }
    log_text(run, "\n", 1);
    return 0;
}

static void open_library(lua_State *lua, const char *name, lua_CFunction library)
{
    luaL_requiref(lua, name, library, 1);
    lua_pop(lua, 1);
}

static void configure_libraries(lua_State *lua)
{
    open_library(lua, "_G", luaopen_base);
    open_library(lua, LUA_TABLIBNAME, luaopen_table);
    open_library(lua, LUA_STRLIBNAME, luaopen_string);
    open_library(lua, LUA_MATHLIBNAME, luaopen_math);

    const char *forbidden[] = {
        "dofile", "loadfile", "load", "pcall", "xpcall", "warn", "getmetatable", "setmetatable",
    };
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        lua_pushnil(lua);
        lua_setglobal(lua, forbidden[i]);
    }
    lua_getglobal(lua, "string");
    // Pattern matching runs in native code. Keep native helpers with bounded
    // work, and leave repeated or pattern-driven logic to the script VM.
    const char *string_forbidden[] = {"dump", "find", "gmatch", "gsub", "match"};
    for (size_t i = 0; i < sizeof(string_forbidden) / sizeof(string_forbidden[0]); ++i) {
        lua_pushnil(lua);
        lua_setfield(lua, -2, string_forbidden[i]);
    }
    lua_pop(lua, 1);
    lua_pushcfunction(lua, script_print);
    lua_setglobal(lua, "print");

    const luaL_Reg functions[] = {{"call", board_call}, {"sleep", board_sleep}, {NULL, NULL}};
    luaL_newlib(lua, functions);
    lua_pushlightuserdata(lua, (void *)&json_null);
    lua_setfield(lua, -2, "null");
    lua_setglobal(lua, "board");

    lua_newtable(lua);
    lua_pushboolean(lua, false);
    lua_setfield(lua, -2, "__metatable");
    lua_rawsetp(lua, LUA_REGISTRYINDEX, &json_array_metatable);
}

static int run_script(lua_State *lua)
{
    script_run_t *run = script_run(lua);
    configure_libraries(lua);
    const int loaded = luaL_loadbufferx(lua, run->source, run->source_bytes, "ai-board", "t");
    if (loaded != LUA_OK) {
        run->kind = loaded == LUA_ERRMEM ? "memory" : "syntax";
        return lua_error(lua);
    }
    remaining_ms(lua);
    lua_sethook(lua, count_instructions, LUA_MASKCOUNT, INSTRUCTION_INTERVAL);
    lua_call(lua, 0, 1);
    remaining_ms(lua);

    append_literal(&run->output, "{\"ok\":true,\"result\":");
    if (!encode_value(lua, -1, 0, &run->output) ||
        run->output.length + 6 * run->log_bytes + 96 > MAX_RESPONSE_BYTES) {
        run->kind = "result";
        return luaL_error(lua, "return value must be a bounded JSON scalar, array, or object");
    }
    return 0;
}

static void run_task(void *context)
{
    script_run_t *run = context;
    run->started = esp_timer_get_time();
    run->deadline = run->started + (int64_t)run->timeout_ms * 1000;
    run->yielded = run->started;

    lua_State *lua = lua_newstate(allocate_lua, run, esp_random());
    int status = LUA_ERRMEM;
    if (lua) {
        *(script_run_t **)lua_getextraspace(lua) = run;
        lua_pushcfunction(lua, run_script);
        status = lua_pcall(lua, 0, 0, 0);
    }
    if (status != LUA_OK) {
        free(run->output.data);
        run->output = (json_buffer_t){0};
        const char *kind = run->kind ? run->kind : status == LUA_ERRMEM ? "memory" : "runtime";
        const char *message = status == LUA_ERRMEM ? "Lua memory limit exceeded" : "script failed";
        size_t length = strlen(message);
        if (lua && lua_type(lua, -1) == LUA_TSTRING) message = lua_tolstring(lua, -1, &length);
        if (length > 512) length = 512;
        append_literal(&run->output, "{\"ok\":false,\"error\":");
        append_string(&run->output, message, length);
        append_literal(&run->output, ",\"kind\":");
        append_string(&run->output, kind, strlen(kind));
    }
    if (lua) lua_close(lua);
    free(run->pending_reply);

    char elapsed[48];
    const int64_t elapsed_ms = (esp_timer_get_time() - run->started) / 1000;
    snprintf(elapsed, sizeof(elapsed), ",\"elapsed_ms\":%" PRId64 "}", elapsed_ms);
    append_literal(&run->output, ",\"log\":");
    append_string(&run->output, run->log, run->log_bytes);
    append_literal(&run->output, elapsed);
    if (run->output.failed) free(run->output.data);
    else run->response = run->output.data;
    board_event_record(BOARD_EVENT_LUA, run->output.failed ? LUA_ERRMEM : status, (uint32_t)elapsed_ms);
    xSemaphoreGive(run->done);
    vTaskDelete(NULL);
}

esp_err_t board_lua_run(const char *source, size_t length, uint32_t timeout_ms, char **response)
{
    if (!response) return ESP_ERR_INVALID_ARG;
    *response = NULL;
    if (!source || !length || length > BOARD_LUA_MAX_SOURCE_BYTES ||
        timeout_ms < BOARD_LUA_MIN_TIMEOUT_MS || timeout_ms > BOARD_LUA_MAX_TIMEOUT_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    script_run_t *run = calloc(1, sizeof(*run));
    if (!run) return ESP_ERR_NO_MEM;
    run->source = source;
    run->source_bytes = length;
    run->timeout_ms = timeout_ms;
    run->done = xSemaphoreCreateBinary();
    if (run->done && xTaskCreate(run_task, "board_lua", VM_TASK_STACK_BYTES, run, 3, NULL) == pdPASS) {
        xSemaphoreTake(run->done, portMAX_DELAY);
        *response = run->response;
    }
    if (run->done) vSemaphoreDelete(run->done);
    free(run);
    return *response ? ESP_OK : ESP_ERR_NO_MEM;
}
