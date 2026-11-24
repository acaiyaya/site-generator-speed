#include <node_api.h>
#include <stdlib.h>
#include <string.h>

#include "sitegen.h"

static void check(Arena *arena, napi_status status) {
    if (status != napi_ok) site_fail(arena, "Native site generator could not read or write a JavaScript value");
}

static napi_value property(napi_env env, Arena *arena, napi_value object, const char *key) {
    napi_value value;
    check(arena, napi_get_named_property(env, object, key, &value));
    return value;
}

static napi_value element(napi_env env, Arena *arena, napi_value array, uint32_t index) {
    napi_value value;
    check(arena, napi_get_element(env, array, index, &value));
    return value;
}

static void check_surrogates(napi_env env, Arena *arena, napi_value value) {
    size_t len;
    check(arena, napi_get_value_string_utf16(env, value, NULL, 0, &len));
    char16_t *units = site_alloc(arena, (len + 1) * sizeof(*units));
    check(arena, napi_get_value_string_utf16(env, value, units, len + 1, &len));
    for (size_t i = 0; i < len; i++) {
        if (units[i] >= 0xd800 && units[i] <= 0xdbff &&
            i + 1 < len && units[i + 1] >= 0xdc00 && units[i + 1] <= 0xdfff) {
            i++;
        } else if (units[i] >= 0xd800 && units[i] <= 0xdfff) {
            site_fail(arena, "SITEGE_FALLBACK: input contains an unpaired UTF-16 surrogate");
        }
    }
}

static Text read_string(napi_env env, Arena *arena, napi_value value) {
    size_t len;
    check(arena, napi_get_value_string_utf8(env, value, NULL, 0, &len));
    if (!len) return (Text){"", 0};
    char *data = site_alloc(arena, len + 1);
    check(arena, napi_get_value_string_utf8(env, value, data, len + 1, &len));
    /* UTF-8 conversion maps each unpaired surrogate to U+FFFD. */
    for (size_t i = 0; i + 2 < len;) {
        const unsigned char *p = memchr(data + i, 0xef, len - i - 2);
        if (!p) break;
        if (p[1] == 0xbf && p[2] == 0xbd) {
            check_surrogates(env, arena, value);
            break;
        }
        i = (const char *)p - data + 1;
    }
    return (Text){data, len};
}

static double read_number(napi_env env, Arena *arena, napi_value value) {
    double number;
    check(arena, napi_get_value_double(env, value, &number));
    return number;
}

static napi_value keys(napi_env env, Arena *arena, napi_value object, uint32_t *len) {
    napi_value names;
    check(arena, napi_get_all_property_names(env, object, napi_key_own_only,
          napi_key_enumerable | napi_key_skip_symbols, napi_key_numbers_to_strings, &names));
    check(arena, napi_get_array_length(env, names, len));
    return names;
}

static Value read_value(napi_env env, Arena *arena, napi_value value, unsigned depth) {
    napi_valuetype type;
    check(arena, napi_typeof(env, value, &type));
    if (type == napi_string)
        return (Value){V_STRING, {.text = read_string(env, arena, value)}};
    if (type == napi_number)
        return (Value){V_NUMBER, {.number = read_number(env, arena, value)}};
    if (type == napi_boolean) {
        bool boolean;
        check(arena, napi_get_value_bool(env, value, &boolean));
        return (Value){V_BOOL, {.number = boolean}};
    }
    if (type == napi_object) {
        if (depth == 64) site_fail(arena, "SITEGE_FALLBACK: site title is nested too deeply");
        bool array;
        check(arena, napi_is_array(env, value, &array));
        uint32_t len;
        if (array) {
            check(arena, napi_get_array_length(env, value, &len));
            Value *items = site_alloc(arena, len * sizeof(*items));
            for (uint32_t i = 0; i < len; i++)
                items[i] = read_value(env, arena, element(env, arena, value, i), depth + 1);
            return (Value){V_ARRAY, {.array = {items, len}}};
        }
        napi_value names = keys(env, arena, value, &len);
        Field *fields = site_alloc(arena, len * sizeof(*fields));
        for (uint32_t i = 0; i < len; i++) {
            napi_value key = element(env, arena, names, i), item;
            check(arena, napi_get_property(env, value, key, &item));
            fields[i] = (Field){read_string(env, arena, key), read_value(env, arena, item, depth + 1)};
        }
        return (Value){V_OBJECT, {.object = {fields, len}}};
    }
    if (type != napi_null && type != napi_undefined)
        site_fail(arena, "SITEGE_FALLBACK: site title contains an unsupported value");
    return (Value){0};
}

static SiteInput read_input(napi_env env, Arena *arena, napi_value value) {
    napi_value site = property(env, arena, value, "site");
    SiteInput input = {
        .title = read_value(env, arena, property(env, arena, site, "title"), 0),
        .base = read_string(env, arena, property(env, arena, site, "base_url")),
        .per_page = read_number(env, arena, property(env, arena, site, "per_page")),
        .recent = read_number(env, arena, property(env, arena, site, "recent")),
    };

    napi_value templates = property(env, arena, value, "templates");
    uint32_t count;
    napi_value names = keys(env, arena, templates, &count);
    input.template_count = count;
    input.templates = site_alloc(arena, count * sizeof(*input.templates));
    for (uint32_t i = 0; i < count; i++) {
        napi_value key = element(env, arena, names, i), source;
        check(arena, napi_get_property(env, templates, key, &source));
        input.templates[i] = (TemplateInput){read_string(env, arena, key), read_string(env, arena, source)};
    }

    napi_value files = property(env, arena, value, "files");
    check(arena, napi_get_array_length(env, files, &count));
    input.file_count = count;
    input.files = site_alloc(arena, count * sizeof(*input.files));
    for (uint32_t i = 0; i < count; i++) {
        napi_value file = element(env, arena, files, i);
        input.files[i] = (File){
            read_string(env, arena, property(env, arena, file, "path")),
            read_string(env, arena, property(env, arena, file, "content")),
        };
    }
    return input;
}

static napi_value write_output(napi_env env, Arena *arena, SiteOutput output) {
    napi_value object;
    check(arena, napi_create_object(env, &object));
    for (size_t i = 0; i < output.count; i++) {
        const File *file = &output.files[i];
        napi_value key, content;
        check(arena, napi_create_string_utf8(env, file->path.data, file->path.len, &key));
        check(arena, napi_create_string_utf8(env, file->content.data, file->content.len, &content));
        check(arena, napi_set_property(env, object, key, content));
    }
    return object;
}

static napi_value run(napi_env env, napi_callback_info info) {
    Arena *arena = calloc(1, sizeof(*arena));
    if (!arena) {
        napi_throw_error(env, NULL, "Native site generator ran out of memory");
        return NULL;
    }
    napi_value result = NULL;
    if (__builtin_setjmp(arena->recovery)) {
        bool pending = false;
        napi_is_exception_pending(env, &pending);
        if (!pending) napi_throw_error(env, NULL, arena->error);
    } else {
        size_t count = 1;
        napi_value arg;
        check(arena, napi_get_cb_info(env, info, &count, &arg, NULL, NULL));
        if (!count) site_fail(arena, "Native site generator requires an input");
        SiteInput input = read_input(env, arena, arg);
        result = write_output(env, arena, site_build(arena, &input));
    }
    site_free(arena);
    free(arena);
    return result;
}

NAPI_MODULE_INIT() {
    napi_value function;
    if (napi_create_function(env, "run", NAPI_AUTO_LENGTH, run, NULL, &function) != napi_ok ||
        napi_set_named_property(env, exports, "run", function) != napi_ok) {
        napi_throw_error(env, NULL, "Native site generator could not export run");
        return NULL;
    }
    return exports;
}
