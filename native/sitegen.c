#include "sitegen.h"
#include "unicode.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define LIT(s) ((Text){s, sizeof(s) - 1})
#define EMPTY LIT("")
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Slabs grow geometrically without moving strings already referenced by the build. */
struct ArenaBlock {
    ArenaBlock *previous;
    size_t used, capacity;
    _Alignas(max_align_t) unsigned char bytes[];
};

__attribute__((noinline))
void site_fail(Arena *arena, const char *error) {
    arena->error = error;
    __builtin_longjmp(arena->recovery, 1);
}

void *site_alloc(Arena *arena, size_t size) {
    const size_t alignment = _Alignof(max_align_t);
    ArenaBlock *block = arena->block;
    if (size > SIZE_MAX - alignment - sizeof(ArenaBlock))
        site_fail(arena, "Native site generator allocation is too large");
    size = (size + alignment - 1) & ~(alignment - 1);
    if (!block || size > block->capacity - block->used) {
        size_t capacity = arena->next_capacity ? arena->next_capacity : 65536;
        if (capacity < size) capacity = size;
        block = malloc(sizeof(*block) + capacity);
        if (!block) site_fail(arena, "Native site generator ran out of memory");
        block->previous = arena->block;
        block->capacity = capacity;
        block->used = 0;
        arena->block = block;
        if (arena->next_capacity < 1024 * 1024)
            arena->next_capacity = capacity < 512 * 1024 ? capacity * 2 : 1024 * 1024;
    }
    void *p = block->bytes + block->used;
    block->used += size;
    return p;
}

void site_free(Arena *arena) {
    ArenaBlock *block = arena->block;
    while (block) {
        ArenaBlock *previous = block->previous;
        free(block);
        block = previous;
    }
}

static Text slice(Text s, size_t start, size_t end) {
    return (Text){s.data ? s.data + start : "", end - start};
}

static bool equal(Text a, Text b) {
    return a.len == b.len && (a.len == 0 || !memcmp(a.data, b.data, a.len));
}

static bool starts(Text s, Text prefix) {
    return s.len >= prefix.len && equal(slice(s, 0, prefix.len), prefix);
}

static bool ends(Text s, Text suffix) {
    return s.len >= suffix.len && equal(slice(s, s.len - suffix.len, s.len), suffix);
}

static Text copy_text(Arena *arena, Text s) {
    if (!s.len) return EMPTY;
    char *p = site_alloc(arena, s.len);
    memcpy(p, s.data, s.len);
    return (Text){p, s.len};
}

static unsigned at(Text s, size_t i) {
    return i < s.len ? (unsigned char)s.data[i] : 256;
}

static size_t find_byte(Text s, unsigned char byte, size_t start) {
    if (start >= s.len) return s.len;
    const char *match = memchr(s.data + start, byte, s.len - start);
    return match ? (size_t)(match - s.data) : s.len;
}

static size_t find_text(Text s, Text needle, size_t start) {
    if (needle.len > s.len) return s.len;
    if (!needle.len) return start < s.len ? start : s.len;
    Text candidates = slice(s, 0, s.len - needle.len + 1);
    for (size_t i = find_byte(candidates, needle.data[0], start);
         i < candidates.len; i = find_byte(candidates, needle.data[0], i + 1))
        if (!memcmp(s.data + i, needle.data, needle.len)) return i;
    return s.len;
}

static bool alpha(unsigned c) { return (c | 32u) - 'a' < 26; }
static bool digit(unsigned c) { return c - '0' < 10; }
static bool alnum(unsigned c) { return alpha(c) || digit(c); }
static bool horizontal(unsigned c) { return c == ' ' || c == '\t'; }

static uint32_t decode_utf8(Text s, size_t *i) {
    uint32_t c = (unsigned char)s.data[(*i)++];
    if (c < 128) return c;
    uint32_t n = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    c &= 0x7f >> (n + 1);
    while (n--) c = (c << 6) | ((unsigned char)s.data[(*i)++] & 63);
    return c;
}

static uint8_t combining_class(uint32_t cp) {
    if (cp < 0x300) return 0;
    size_t lo = 0, hi = COUNT(unicode_classes);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (unicode_classes[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return lo < COUNT(unicode_classes) && unicode_classes[lo].cp == cp
        ? unicode_classes[lo].ccc : 0;
}

static bool is_mark(uint32_t cp) {
    if (cp < 0x300) return false;
    size_t lo = 0, hi = COUNT(unicode_marks);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (unicode_marks[mid].last < cp) lo = mid + 1;
        else hi = mid;
    }
    return lo < COUNT(unicode_marks) && unicode_marks[lo].first <= cp;
}

static const uint32_t *decompose(uint32_t cp, uint32_t local[3], size_t *len) {
    if (cp >= 0xac00 && cp <= 0xd7a3) {
        uint32_t n = cp - 0xac00;
        local[0] = 0x1100 + n / 588;
        local[1] = 0x1161 + (n % 588) / 28;
        local[2] = 0x11a7 + n % 28;
        *len = local[2] == 0x11a7 ? 2 : 3;
        return local;
    }
    if (cp >= 0xa0) {
        size_t lo = 0, hi = COUNT(unicode_decompositions);
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (unicode_decompositions[mid].cp < cp) lo = mid + 1;
            else hi = mid;
        }
        if (lo < COUNT(unicode_decompositions) && unicode_decompositions[lo].cp == cp) {
            *len = unicode_decompositions[lo].length;
            return unicode_pool + unicode_decompositions[lo].offset;
        }
    }
    local[0] = cp;
    *len = 1;
    return local;
}

static size_t nfkd(Text s, uint32_t *out) {
    size_t len = 0;
    for (size_t i = 0; i < s.len;) {
        uint32_t local[3];
        size_t count;
        const uint32_t *part = decompose(decode_utf8(s, &i), local, &count);
        for (size_t k = 0; k < count; k++) {
            const uint32_t cp = part[k];
            const uint8_t ccc = combining_class(cp);
            size_t j = len++;
            if (ccc) {
                while (j && combining_class(out[j - 1]) > ccc) {
                    out[j] = out[j - 1];
                    j--;
                }
            }
            out[j] = cp;
        }
    }
    return len;
}

static Text slug_text(Arena *arena, Text text) {
    char out[64];
    size_t len = 0;
    bool gap = false;
    uint32_t local[192], *normalized = NULL;
    size_t n = text.len;
    for (size_t i = 0; i < text.len; i++) {
        if ((unsigned char)text.data[i] >= 128) {
            /* The longest mapping expands to 18 code points from a 3-byte character. */
            if (text.len > SIZE_MAX / (6 * sizeof(uint32_t)))
                site_fail(arena, "Native site generator slug is too large");
            size_t capacity = text.len * 6;
            normalized = capacity <= COUNT(local) ? local : site_alloc(arena, capacity * sizeof(*normalized));
            n = nfkd(text, normalized);
            break;
        }
    }
    for (size_t i = 0; i < n && len < sizeof(out); i++) {
        uint32_t c = normalized ? normalized[i] : (unsigned char)text.data[i];
        if (is_mark(c)) continue;
        if (alnum(c)) {
            if (gap) out[len++] = '-';
            if (len < sizeof(out)) out[len++] = alpha(c) ? (char)(c | 32u) : (char)c;
            gap = false;
        } else {
            gap = len != 0;
        }
    }
    if (len && out[len - 1] == '-') len--;
    return len ? copy_text(arena, (Text){out, len}) : LIT("untitled");
}

typedef struct {
    Arena *arena;
    char *data;
    size_t len, capacity;
} Buffer;

static void reserve(Buffer *b, size_t extra) {
    if (extra <= b->capacity - b->len) return;
    if (extra > SIZE_MAX - b->len)
        site_fail(b->arena, "Native site generator string is too large");
    size_t minimum = b->len + extra;
    size_t capacity = b->capacity > SIZE_MAX / 2 ? minimum : b->capacity * 2;
    if (capacity < 128) capacity = 128;
    if (capacity < minimum) capacity = minimum;
    char *p = site_alloc(b->arena, capacity);
    if (b->len) memcpy(p, b->data, b->len);
    b->data = p;
    b->capacity = capacity;
}

static void append(Buffer *b, Text s) {
    if (!s.len) return;
    reserve(b, s.len);
    memcpy(b->data + b->len, s.data, s.len);
    b->len += s.len;
}

static void append_byte(Buffer *b, char byte) {
    reserve(b, 1);
    b->data[b->len++] = byte;
}

static Text written(const Buffer *b) {
    return b->len ? (Text){b->data, b->len} : EMPTY;
}

static Text join(Arena *arena, const Text *parts, size_t count) {
    size_t len = 0;
    for (size_t i = 0; i < count; i++) len += parts[i].len;
    if (!len) return EMPTY;
    char *p = site_alloc(arena, len);
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        if (parts[i].len) memcpy(p + pos, parts[i].data, parts[i].len);
        pos += parts[i].len;
    }
    return (Text){p, len};
}

#define JOIN(arena, ...) join(arena, (Text[]){__VA_ARGS__}, COUNT(((Text[]){__VA_ARGS__})))

static Text digits(char local[24], long long n) {
    size_t pos = 24;
    unsigned long long magnitude = n < 0 ? -(unsigned long long)n : (unsigned long long)n;
    do {
        local[--pos] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    if (n < 0) local[--pos] = '-';
    return (Text){local + pos, 24 - pos};
}

static bool js_space(uint32_t cp) {
    return cp == 0x20 || (cp >= 9 && cp <= 13) || cp == 0xa0 || cp == 0x1680 ||
        (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202f || cp == 0x205f || cp == 0x3000 || cp == 0xfeff;
}

static bool has_space(Text s) {
    for (size_t i = 0; i < s.len;) {
        if (js_space(decode_utf8(s, &i))) return true;
    }
    return false;
}

static Text trim_js(Text s) {
    size_t start = 0, end = s.len;
    while (start < end) {
        size_t i = start;
        if (!js_space(decode_utf8(s, &i))) break;
        start = i;
    }
    while (end > start) {
        size_t i = end - 1;
        while (((unsigned char)s.data[i] & 0xc0) == 0x80) i--;
        size_t tail = i;
        if (!js_space(decode_utf8(s, &tail))) break;
        end = i;
    }
    return slice(s, start, end);
}

static size_t skip_horizontal(Text s, size_t start) {
    while (horizontal(at(s, start))) start++;
    return start;
}

static Text trim_horizontal(Text s) {
    size_t start = skip_horizontal(s, 0), end = s.len;
    while (end > start && horizontal((unsigned char)s.data[end - 1])) end--;
    return slice(s, start, end);
}

static bool line_terminator(Text s) {
    for (size_t i = 0; i < s.len; i++) {
        unsigned c = (unsigned char)s.data[i];
        if (c == '\r' || c == '\n' || (c == 0xe2 && at(s, i + 1) == 0x80 &&
            (at(s, i + 2) == 0xa8 || at(s, i + 2) == 0xa9))) return true;
    }
    return false;
}

static int compare_text(Text a, Text b) {
    size_t n = a.len < b.len ? a.len : b.len;
    int order = n ? memcmp(a.data, b.data, n) : 0;
    if (!order) return (a.len > b.len) - (a.len < b.len);
    /* Astral characters sort before BMP U+E000..U+FFFF in UTF-16. */
    if (order < 0 && (unsigned char)b.data[0] < 0x80 && n == 1) return order;
    for (size_t i = 0; i < n; i++) {
        unsigned x = (unsigned char)a.data[i], y = (unsigned char)b.data[i];
        if (x == y) continue;
        if ((x >= 0xf0 && y >= 0xee && y < 0xf0) ||
            (y >= 0xf0 && x >= 0xee && x < 0xf0)) return -order;
        return order;
    }
    return order;
}

#define DEFINE_SORT(name, Type, cmp) \
    static void name(Arena *arena, Type *items, size_t n) { \
        if (n < 2) return; \
        Type *tmp = site_alloc(arena, n * sizeof(*tmp)); \
        Type *src = items; Type *dst = tmp; \
        for (size_t width = 1; width < n; width *= 2) { \
            for (size_t lo = 0; lo < n; lo += 2 * width) { \
                size_t mid = lo + width < n ? lo + width : n; \
                size_t hi = mid + width < n ? mid + width : n; \
                size_t i = lo, j = mid, k = lo; \
                while (i < mid && j < hi) { \
                    if (cmp(src[i], src[j]) <= 0) dst[k++] = src[i++]; \
                    else dst[k++] = src[j++]; \
                } \
                while (i < mid) dst[k++] = src[i++]; \
                while (j < hi) dst[k++] = src[j++]; \
            } \
            Type *next = src; src = dst; dst = next; \
        } \
        if (src != items) memcpy(items, src, n * sizeof(*items)); \
    }

DEFINE_SORT(sort_text, Text, compare_text)

typedef struct {
    Text key;
    void *value;
    uint64_t hash;
} Entry;

typedef struct {
    Entry *slots;
    size_t capacity, len;
} Map;

static uint64_t hash_text(Text s) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < s.len; i++)
        hash = (hash ^ (unsigned char)s.data[i]) * UINT64_C(1099511628211);
    return hash ? hash : 1;
}

static Entry *find_slot(const Map *map, Text key, uint64_t hash) {
    const size_t mask = map->capacity - 1;
    size_t i = hash & mask;
    while (map->slots[i].hash && (map->slots[i].hash != hash || !equal(map->slots[i].key, key)))
        i = (i + 1) & mask;
    return &map->slots[i];
}

static void *map_get(const Map *map, Text key) {
    if (!map->capacity) return NULL;
    Entry *slot = find_slot(map, key, hash_text(key));
    return slot->hash ? slot->value : NULL;
}

static void map_grow(Arena *arena, Map *map) {
    Map old = *map;
    map->capacity = old.capacity ? old.capacity * 2 : 16;
    map->slots = site_alloc(arena, map->capacity * sizeof(*map->slots));
    for (size_t i = 0; i < map->capacity; i++) map->slots[i].hash = 0;
    for (size_t i = 0; i < old.capacity; i++) {
        Entry *e = &old.slots[i];
        if (e->hash) *find_slot(map, e->key, e->hash) = *e;
    }
}

static void map_put(Arena *arena, Map *map, Text key, void *value) {
    if (map->len * 2 >= map->capacity) map_grow(arena, map);
    uint64_t hash = hash_text(key);
    Entry *slot = find_slot(map, key, hash);
    if (!slot->hash) {
        *slot = (Entry){key, value, hash};
        map->len++;
    } else {
        slot->value = value;
    }
}

#define VECTOR(Type) struct { Type *items; size_t len, capacity; }

static void *grow_vector(Arena *arena, const void *items, size_t len, size_t capacity, size_t width) {
    void *p = site_alloc(arena, capacity * width);
    if (len) memcpy(p, items, len * width);
    return p;
}

#define PUSH(arena, vec, item) do { \
    if ((vec).len == (vec).capacity) { \
        (vec).capacity = (vec).capacity ? (vec).capacity * 2 : 8; \
        (vec).items = grow_vector(arena, (vec).items, (vec).len, (vec).capacity, sizeof(*(vec).items)); \
    } \
    (vec).items[(vec).len++] = (item); \
} while (0)

static Text slug_of(Arena *arena, Map *cache, Text title) {
    Text *cached = map_get(cache, title);
    if (cached) return *cached;
    Text *slug = site_alloc(arena, sizeof(*slug));
    *slug = slug_text(arena, title);
    map_put(arena, cache, title, slug);
    return *slug;
}

static Text claim_slug(Arena *arena, Map *used, Text slug) {
    uintptr_t next = (uintptr_t)map_get(used, slug);
    if (!next) {
        map_put(arena, used, slug, (void *)(uintptr_t)2);
        return slug;
    }
    Text candidate;
    do {
        char local[24];
        candidate = JOIN(arena, slug, LIT("-"), digits(local, (long long)next++));
    } while (map_get(used, candidate));
    map_put(arena, used, slug, (void *)next);
    map_put(arena, used, candidate, (void *)(uintptr_t)2);
    return candidate;
}

/* Front matter */

typedef enum { TITLE, DATE, TAGS, DRAFT, LAYOUT, SLUG, SUMMARY, FIELD_COUNT } MatterKey;
static const Text matter_keys[] = {LIT("title"), LIT("date"), LIT("tags"), LIT("draft"), LIT("layout"), LIT("slug"), LIT("summary")};
static const Text month_names[] = {
    LIT("January"), LIT("February"), LIT("March"), LIT("April"), LIT("May"), LIT("June"),
    LIT("July"), LIT("August"), LIT("September"), LIT("October"), LIT("November"), LIT("December")
};
static const unsigned month_lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

typedef struct {
    Text values[FIELD_COUNT];
    VECTOR(Text) tags;
    Text body;
    unsigned present;
    bool draft;
} Matter;

static unsigned decimal(Text s) {
    unsigned result = 0;
    for (size_t i = 0; i < s.len; i++) result = result * 10 + (unsigned)(s.data[i] - '0');
    return result;
}

static bool valid_date(Text s) {
    if (s.len != 10 || s.data[4] != '-' || s.data[7] != '-') return false;
    for (size_t i = 0; i < s.len; i++)
        if (i != 4 && i != 7 && !digit((unsigned char)s.data[i])) return false;
    unsigned year = decimal(slice(s, 0, 4));
    unsigned month = decimal(slice(s, 5, 7));
    unsigned day = decimal(slice(s, 8, 10));
    bool leap = month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return year >= 1970 && year <= 2099 && month >= 1 && month <= 12 &&
        day >= 1 && day <= month_lengths[month - 1] + leap;
}

static bool layout_name(Text s) {
    if (!s.len) return false;
    for (size_t i = 0; i < s.len; i++) {
        unsigned c = (unsigned char)s.data[i];
        if (!digit(c) && c - 'a' >= 26 && c != '-') return false;
    }
    return true;
}

static bool explicit_slug(Text s) {
    if (!s.len || s.len > 64) return false;
    bool separator = true;
    for (size_t i = 0; i < s.len; i++) {
        unsigned c = (unsigned char)s.data[i];
        if (digit(c) || c - 'a' < 26) separator = false;
        else if (c == '-' && !separator) separator = true;
        else return false;
    }
    return !separator;
}

static Text read_field(Arena *arena, Matter *data, MatterKey key, Text raw) {
    switch (key) {
    case TITLE:
    case SUMMARY:
        if (at(raw, 0) == '[') return LIT("a bare string opens on [");
        if (at(raw, 0) == '"') {
            size_t end = raw.len - 1;
            if (!end || at(raw, end) != '"') return LIT("a quoted string is not closed");
            Buffer out = {arena};
            size_t start = 1;
            for (size_t i = 1; i < end; i++) {
                if (raw.data[i] == '"') return LIT("a quoted string holds a stray quote");
                if (raw.data[i] == '\\') {
                    unsigned next = at(raw, i + 1);
                    if (i + 1 == end || (next != '"' && next != '\\'))
                        return LIT("a quoted string holds an escape other than \\\" or \\\\");
                    append(&out, slice(raw, start, i));
                    append_byte(&out, (char)next);
                    start = ++i + 1;
                }
            }
            if (start == 1) raw = slice(raw, 1, end);
            else {
                append(&out, slice(raw, start, end));
                raw = written(&out);
            }
        }
        break;
    case DATE:
        if (!valid_date(raw)) return LIT("not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
        break;
    case TAGS: {
        if (at(raw, 0) != '[' || !ends(raw, LIT("]"))) return LIT("not a list [a, b]");
        Text inner = slice(raw, 1, raw.len - 1);
        if (!trim_js(inner).len) break;
        for (size_t start = 0;;) {
            size_t end = find_byte(inner, ',', start);
            Text name = trim_js(slice(inner, start, end));
            if (!name.len || find_byte(name, '"', 0) != name.len ||
                find_byte(name, '[', 0) != name.len || find_byte(name, ']', 0) != name.len)
                return LIT("an empty tag, or one holding \" [ or ]");
            PUSH(arena, data->tags, name);
            if (end == inner.len) break;
            start = end + 1;
        }
        break;
    }
    case DRAFT:
        if (!equal(raw, LIT("true")) && !equal(raw, LIT("false"))) return LIT("true or false");
        data->draft = equal(raw, LIT("true"));
        break;
    case LAYOUT:
        if (!layout_name(raw)) return LIT("not a template name");
        break;
    case SLUG:
        if (!explicit_slug(raw)) return LIT("not a slug");
        break;
    default:
        break;
    }
    data->values[key] = raw;
    data->present |= 1u << key;
    return EMPTY;
}

static Text normalize_lines(Arena *arena, Text text) {
    size_t first = find_byte(text, '\r', 0);
    if (first == text.len) return text;
    char *p = site_alloc(arena, text.len);
    if (first) memcpy(p, text.data, first);
    size_t len = first;
    for (size_t i = first; i < text.len; i++) {
        char c = text.data[i];
        if (c == '\r') {
            c = '\n';
            if (at(text, i + 1) == '\n') i++;
        }
        p[len++] = c;
    }
    return (Text){p, len};
}

static Text read_front_matter(Arena *arena, Text text, Matter *data) {
    if (!starts(text, LIT("---\n")))
        return equal(text, LIT("---")) ? LIT("the front matter is not closed") : LIT("no front matter");
    size_t close = find_text(text, LIT("\n---\n"), 3);
    if (close == text.len) {
        if (!ends(text, LIT("\n---"))) return LIT("the front matter is not closed");
        close = text.len - 4;
    }
    size_t body = close + 5 < text.len ? close + 5 : text.len;
    data->body = slice(text, body, text.len);
    for (size_t start = 4, number = 2; start < close; number++) {
        size_t end = find_byte(text, '\n', start);
        Text line = slice(text, start, end);
        start = end + 1;
        if (at(line, 0) == '#' || skip_horizontal(line, 0) == line.len) continue;
        size_t colon = 0;
        while (at(line, colon) - 'a' < 26 || at(line, colon) == '_') colon++;
        if (!colon || at(line, colon) != ':' ||
            (colon + 1 < line.len && at(line, colon + 1) != ' ') || line_terminator(line)) {
            char local[24];
            return JOIN(arena, LIT("line "), digits(local, (long long)number), LIT(" is not \"key: value\""));
        }
        Text name = slice(line, 0, colon);
        MatterKey key = 0;
        while (key < FIELD_COUNT && !equal(name, matter_keys[key])) key++;
        if (key == FIELD_COUNT) return JOIN(arena, LIT("unknown key \""), name, LIT("\""));
        if (data->present & (1u << key)) return JOIN(arena, LIT("key \""), name, LIT("\" is given twice"));
        size_t value = colon + 2, limit = line.len;
        while (limit > value && horizontal(at(line, limit - 1))) limit--;
        if (value >= limit) return JOIN(arena, LIT("key \""), name, LIT("\" has no value"));
        Text error = read_field(arena, data, key, slice(line, value, limit));
        if (error.len) return JOIN(arena, LIT("key \""), name, LIT("\": "), error);
    }
    return data->present & (1u << TITLE) ? EMPTY : LIT("no title");
}

/* Markdown */

static Value string_value(Text s) { return (Value){V_STRING, {.text = s}}; }
static Value number_value(double n) { return (Value){V_NUMBER, {.number = n}}; }
static Value bool_value(bool b) { return (Value){V_BOOL, {.number = b}}; }
static Value array_value(Value *items, size_t len) {
    return (Value){V_ARRAY, {.array = {items, len}}};
}
static Value object_value(Field *fields, size_t len) {
    return (Value){V_OBJECT, {.object = {fields, len}}};
}

static Text html_escape(unsigned c) {
    switch (c) {
    case '&': return LIT("&amp;");
    case '<': return LIT("&lt;");
    case '>': return LIT("&gt;");
    case '"': return LIT("&quot;");
    case '\'': return LIT("&#39;");
    default: return EMPTY;
    }
}

enum { INLINE_MARKER = 1, HTML_SPECIAL };
static const unsigned char inline_special[256] = {
    ['&'] = HTML_SPECIAL, ['<'] = HTML_SPECIAL, ['>'] = HTML_SPECIAL,
    ['"'] = HTML_SPECIAL, ['\''] = HTML_SPECIAL,
    ['\\'] = INLINE_MARKER, ['\x60'] = INLINE_MARKER,
    ['*'] = INLINE_MARKER, ['['] = INLINE_MARKER,
};

static void append_html(Buffer *out, Text text) {
    size_t start = 0;
    for (size_t i = 0; i < text.len; i++) {
        unsigned c = (unsigned char)text.data[i];
        if (inline_special[c] == HTML_SPECIAL) {
            append(out, slice(text, start, i));
            append(out, html_escape(c));
            start = i + 1;
        }
    }
    append(out, slice(text, start, text.len));
}

typedef VECTOR(Text) TextList;

typedef struct {
    Text path, dir, base;
    const Map *source_urls, *statics;
    Map *broken;
    TextList *report;
} SourceLink;

static bool absolute_link(Text target) {
    unsigned c = at(target, 0);
    if (c == '#' || c == '/') return true;
    if (!alpha(c)) return false;
    size_t i = 1;
    while (alnum(c = at(target, i)) || c == '+' || c == '.' || c == '-') i++;
    return c == ':';
}

static Text resolve_link(Arena *arena, const SourceLink *link, Text target) {
    if (absolute_link(target)) return target;
    size_t hash = find_byte(target, '#', 0);
    Text rel = slice(target, 0, hash), fragment = slice(target, hash, target.len);
    Text local[16];
    TextList parts = {local, 0, COUNT(local)};
    if (link->dir.len) {
        size_t start = 0;
        for (;;) {
            size_t end = find_byte(link->dir, '/', start);
            PUSH(arena, parts, slice(link->dir, start, end));
            if (end == link->dir.len) break;
            start = end + 1;
        }
    }
    size_t start = 0;
    for (;;) {
        size_t end = find_byte(rel, '/', start);
        Text part = slice(rel, start, end);
        if (equal(part, LIT(".."))) {
            if (!parts.len) goto broken;
            parts.len--;
        } else if (part.len && !equal(part, LIT("."))) {
            PUSH(arena, parts, part);
        }
        if (end == rel.len) break;
        start = end + 1;
    }
    Text resolved = parts.len ? parts.items[0] : EMPTY;
    if (parts.len > 1) {
        Buffer path = {.arena = arena};
        append(&path, resolved);
        for (size_t i = 1; i < parts.len; i++) {
            append_byte(&path, '/');
            append(&path, parts.items[i]);
        }
        resolved = written(&path);
    }
    Text *url = map_get(link->source_urls, resolved);
    if (url) return fragment.len ? JOIN(arena, *url, fragment) : *url;
    if (map_get(link->statics, resolved)) return JOIN(arena, link->base, LIT("/"), resolved, fragment);

broken:;
    Text line = JOIN(arena, LIT("broken-link "), link->path, LIT(" -> "), target);
    if (!map_get(link->broken, line)) {
        map_put(arena, link->broken, line, link->broken);
        PUSH(arena, *link->report, line);
    }
    return target;
}

static bool escapable(unsigned c) {
    switch (c) {
    case '\\': case '\x60': case '*': case '_': case '[': case ']':
    case '(': case ')': case '#': case '+': case '-': case '.':
    case '!': case '>': return true;
    default: return false;
    }
}

static size_t delimiter(Text text, Text marker, size_t from, bool single_star) {
    size_t search = from;
    for (;;) {
        size_t pos = marker.len == 1 ? find_byte(text, marker.data[0], search)
                                     : find_text(text, marker, search);
        if (pos == text.len) return pos;
        size_t before = pos;
        while (before > from && at(text, before - 1) == '\\') before--;
        if ((pos - before) & 1) search = pos + 1;
        else if (single_star && at(text, pos + 1) == '*') search = pos + 2;
        else return pos;
    }
}

static void write_inline(Buffer *out, Text text, const SourceLink *link) {
    size_t start = 0;
    for (size_t i = 0; i < text.len;) {
        unsigned c = (unsigned char)text.data[i];
        unsigned special = inline_special[c];
        if (!special) {
            i++;
            continue;
        }
        append(out, slice(text, start, i++));
        if (special == HTML_SPECIAL) {
            append(out, html_escape(c));
        } else if (c == '\\' && escapable(at(text, i))) {
            unsigned next = at(text, i++);
            if (next == '>') append(out, LIT("&gt;"));
            else append_byte(out, (char)next);
        } else if (c == '\x60') {
            size_t close = find_byte(text, '\x60', i);
            if (close > i && close < text.len) {
                append(out, LIT("<code>"));
                append_html(out, slice(text, i, close));
                append(out, LIT("</code>"));
                i = close + 1;
            } else append_byte(out, (char)c);
        } else if (c == '*') {
            bool strong = at(text, i) == '*';
            if (strong) i++;
            Text marker = strong ? LIT("**") : LIT("*");
            size_t close = delimiter(text, marker, i, !strong);
            if (close > i && close < text.len) {
                append(out, strong ? LIT("<strong>") : LIT("<em>"));
                write_inline(out, slice(text, i, close), link);
                append(out, strong ? LIT("</strong>") : LIT("</em>"));
                i = close + marker.len;
            } else append(out, marker);
        } else if (c == '[') {
            size_t label = delimiter(text, LIT("]"), i, false);
            size_t close = at(text, label + 1) == '(' ? find_byte(text, ')', label + 2) : text.len;
            if (close < text.len && close > label + 2) {
                Text target = slice(text, label + 2, close);
                if (!has_space(target)) {
                    append(out, LIT("<a href=\""));
                    append_html(out, resolve_link(out->arena, link, target));
                    append(out, LIT("\">"));
                    write_inline(out, slice(text, i, label), link);
                    append(out, LIT("</a>"));
                    i = close + 1;
                    start = i;
                    continue;
                }
            }
            append_byte(out, (char)c);
        } else {
            append_byte(out, (char)c);
        }
        start = i;
    }
    append(out, slice(text, start, text.len));
}

typedef struct {
    Text body, line, detail;
    size_t start, end, level;
} MarkdownLine;

typedef enum { PARAGRAPH, BLANK, FENCE, HEADING, RULE } BlockKind;

static void markdown_advance(MarkdownLine *cursor) {
    cursor->start = cursor->end + 1;
    cursor->end = find_byte(cursor->body, '\n', cursor->start);
    cursor->line = cursor->start <= cursor->end
        ? slice(cursor->body, cursor->start, cursor->end) : EMPTY;
}

static bool fence_end(Text line) {
    return starts(line, LIT("\x60\x60\x60")) && skip_horizontal(line, 3) == line.len;
}

static BlockKind markdown_boundary(MarkdownLine *cursor) {
    Text line = cursor->line;
    if (skip_horizontal(line, 0) == line.len) return BLANK;
    if (starts(line, LIT("\x60\x60\x60"))) {
        size_t i = 3;
        unsigned c;
        while (alnum(c = at(line, i)) || c == '_' || c == '+' || c == '-') i++;
        if (skip_horizontal(line, i) == line.len) {
            cursor->detail = slice(line, 3, i);
            return FENCE;
        }
    } else if (at(line, 0) == '#') {
        size_t level = 1;
        while (at(line, level) == '#') level++;
        if (level <= 6 && horizontal(at(line, level))) {
            Text title = trim_horizontal(slice(line, level, line.len));
            if (title.len && !line_terminator(title)) {
                cursor->detail = title;
                cursor->level = level;
                return HEADING;
            }
        }
    } else if ((starts(line, LIT("---")) || starts(line, LIT("***"))) &&
               skip_horizontal(line, 3) == line.len) {
        return RULE;
    }
    return PARAGRAPH;
}

static Text take_line(MarkdownLine *cursor, size_t offset) {
    Text line = trim_horizontal(slice(cursor->line, offset, cursor->line.len));
    markdown_advance(cursor);
    return line;
}

typedef struct {
    Buffer joined;
    Text first;
    size_t count;
} MarkdownLines;

static void collect_line(MarkdownLines *lines, Text line) {
    if (!lines->count) lines->first = line;
    else {
        if (lines->count == 1) append(&lines->joined, lines->first);
        append_byte(&lines->joined, '\n');
        append(&lines->joined, line);
    }
    lines->count++;
}

static Text collected_lines(MarkdownLines *lines) {
    Text text = lines->count == 1 ? lines->first : written(&lines->joined);
    lines->count = 0;
    lines->joined.len = 0;
    return text;
}

static size_t list_marker(Text line, bool bullet) {
    if (bullet) {
        if ((at(line, 0) == '-' || at(line, 0) == '*') && at(line, 1) == ' ' &&
            !line_terminator(slice(line, 2, line.len))) return 2;
    } else {
        size_t i = 0;
        while (i < 9 && digit(at(line, i))) i++;
        if (i && at(line, i) == '.' && at(line, i + 1) == ' ' &&
            !line_terminator(slice(line, i + 2, line.len))) return i + 2;
    }
    return 0;
}

static bool list_continuation(Text line) {
    if (at(line, 0) != ' ' || at(line, 1) != ' ') return false;
    size_t i = 2;
    while (at(line, i) == ' ') i++;
    return i < line.len && !js_space(decode_utf8(line, &i));
}

typedef struct {
    Text content, excerpt;
    Value toc;
} Markdown;

static Markdown markdown(Arena *arena, Text body, Map *slugs, const SourceLink *link) {
    Buffer out = {.arena = arena};
    MarkdownLines lines = {.joined = {.arena = arena}};
    MarkdownLine cursor = {.body = body, .end = SIZE_MAX};
    VECTOR(Value) toc = {0};
    Map ids = {0};
    Text excerpt = {0};

    markdown_advance(&cursor);
    while (cursor.start <= body.len) {
        BlockKind kind = markdown_boundary(&cursor);
        if (kind == BLANK) {
            markdown_advance(&cursor);
        } else if (kind == FENCE) {
            Text language = cursor.detail;
            if (language.len) {
                append(&out, LIT("<pre><code class=\"language-"));
                append(&out, language);
                append(&out, LIT("\">"));
            } else append(&out, LIT("<pre><code>"));
            markdown_advance(&cursor);
            size_t start = cursor.start;
            while (cursor.start <= body.len && !fence_end(cursor.line)) markdown_advance(&cursor);
            if (cursor.start <= body.len) {
                append_html(&out, slice(body, start, cursor.start));
                markdown_advance(&cursor);
            } else if (start <= body.len) {
                append_html(&out, slice(body, start, body.len));
                append_byte(&out, '\n');
            }
            append(&out, LIT("</code></pre>\n"));
        } else if (kind == HEADING) {
            Text id = claim_slug(arena, &ids, slug_of(arena, slugs, cursor.detail));
            char level = (char)('0' + cursor.level);
            append(&out, LIT("<h"));
            append_byte(&out, level);
            append(&out, LIT(" id=\""));
            append(&out, id);
            append(&out, LIT("\">"));
            size_t start = out.len;
            write_inline(&out, cursor.detail, link);
            if (cursor.level == 2 || cursor.level == 3) {
                Field *fields = site_alloc(arena, 3 * sizeof(*fields));
                fields[0] = (Field){LIT("level"), number_value(cursor.level)};
                fields[1] = (Field){LIT("id"), string_value(id)};
                fields[2] = (Field){LIT("html"), string_value(slice(written(&out), start, out.len))};
                PUSH(arena, toc, object_value(fields, 3));
            }
            append(&out, LIT("</h"));
            append_byte(&out, level);
            append(&out, LIT(">\n"));
            markdown_advance(&cursor);
        } else if (kind == RULE) {
            append(&out, LIT("<hr>\n"));
            markdown_advance(&cursor);
        } else if (at(cursor.line, 0) == '>') {
            do {
                Text line = take_line(&cursor, 1);
                if (line.len) collect_line(&lines, line);
            } while (at(cursor.line, 0) == '>');
            append(&out, LIT("<blockquote>\n"));
            if (lines.count) {
                append(&out, LIT("<p>"));
                write_inline(&out, collected_lines(&lines), link);
                append(&out, LIT("</p>\n"));
            }
            append(&out, LIT("</blockquote>\n"));
        } else {
            bool bullet = at(cursor.line, 0) == '-' || at(cursor.line, 0) == '*';
            size_t marker = list_marker(cursor.line, bullet);
            if (marker) {
                unsigned first = bullet ? 1 : decimal(slice(cursor.line, 0, marker - 2));
                if (bullet) append(&out, LIT("<ul>\n"));
                else if (first == 1) append(&out, LIT("<ol>\n"));
                else {
                    char local[24];
                    append(&out, LIT("<ol start=\""));
                    append(&out, digits(local, first));
                    append(&out, LIT("\">\n"));
                }
                do {
                    collect_line(&lines, take_line(&cursor, marker));
                    while (list_continuation(cursor.line)) collect_line(&lines, take_line(&cursor, 0));
                    append(&out, LIT("<li>"));
                    write_inline(&out, collected_lines(&lines), link);
                    append(&out, LIT("</li>\n"));
                    marker = list_marker(cursor.line, bullet);
                } while (marker);
                append(&out, bullet ? LIT("</ul>\n") : LIT("</ol>\n"));
            } else {
                do {
                    collect_line(&lines, take_line(&cursor, 0));
                } while (cursor.start <= body.len && markdown_boundary(&cursor) == PARAGRAPH);
                append(&out, LIT("<p>"));
                size_t start = out.len;
                write_inline(&out, collected_lines(&lines), link);
                if (!excerpt.data) excerpt = slice(written(&out), start, out.len);
                append(&out, LIT("</p>\n"));
            }
        }
    }
    return (Markdown){written(&out), excerpt.data ? excerpt : EMPTY, array_value(toc.items, toc.len)};
}

/* Templates */

typedef enum { TEXT, ESCAPED, RAW, TAG, LOOP, IF, UNLESS, JUMP, INCLUDE } Opcode;
typedef struct {
    Text *parts;
    size_t len;
} Path;

typedef struct Program Program;
typedef struct {
    Opcode op;
    Text text, binding;
    Path path;
    size_t end;
    const Program *include;
} Instruction;
struct Program {
    Instruction *items;
    size_t len;
};
typedef VECTOR(Instruction) Instructions;

static bool identifier_start(unsigned c) { return alpha(c) || c == '_'; }
static bool identifier_part(unsigned c) { return alnum(c) || c == '_'; }

static bool valid_identifier(Text s) {
    if (!identifier_start(at(s, 0))) return false;
    size_t i = 1;
    while (identifier_part(at(s, i))) i++;
    return i == s.len;
}

static Path read_path(Arena *arena, Text s) {
    size_t count = 0;
    for (size_t start = 0;;) {
        size_t end = find_byte(s, '.', start);
        if (!valid_identifier(slice(s, start, end))) return (Path){0};
        count++;
        if (end == s.len) break;
        start = end + 1;
    }
    Text *parts = site_alloc(arena, count * sizeof(*parts));
    size_t start = 0;
    for (size_t i = 0; i < count; i++) {
        size_t end = find_byte(s, '.', start);
        parts[i] = slice(s, start, end);
        start = end + 1;
    }
    return (Path){parts, count};
}

static Text path_error(Arena *arena, Text token) {
    static const char hex[] = "0123456789abcdef";
    Buffer out = {.arena = arena};
    append_byte(&out, '"');
    for (size_t i = 0; i < token.len; i++) {
        unsigned c = at(token, i);
        switch (c) {
        case '"': append(&out, LIT("\\\"")); break;
        case '\\': append(&out, LIT("\\\\")); break;
        case '\b': append(&out, LIT("\\b")); break;
        case '\f': append(&out, LIT("\\f")); break;
        case '\n': append(&out, LIT("\\n")); break;
        case '\r': append(&out, LIT("\\r")); break;
        case '\t': append(&out, LIT("\\t")); break;
        default:
            if (c < 32) {
                append(&out, LIT("\\u00"));
                append_byte(&out, hex[c >> 4]);
                append_byte(&out, hex[c & 15]);
            } else append_byte(&out, (char)c);
        }
    }
    append(&out, LIT("\" is not a path"));
    return written(&out);
}

static Text tokenize(Arena *arena, Text source, Instructions *tokens) {
    size_t literal = 0;
    size_t search = 0;
    for (;;) {
        size_t open = find_byte(source, '{', search);
        if (open == source.len) break;
        unsigned next = at(source, open + 1);
        if (next != '{' && next != '%') {
            search = open + 1;
            continue;
        }
        bool tag = next == '%';
        bool raw = !tag && at(source, open + 2) == '{';
        size_t width = raw ? 3 : 2;
        Text closer = tag ? LIT("%}") : raw ? LIT("}}}") : LIT("}}");
        size_t close = find_text(source, closer, open + width);
        if (close == source.len)
            return JOIN(arena, LIT("a "), slice(source, open, open + width), LIT(" is not closed"));
        size_t literal_end = open;
        search = close + width;
        if (tag) {
            size_t line_start = open;
            while (line_start && horizontal(at(source, line_start - 1))) line_start--;
            if (!line_start || at(source, line_start - 1) == '\n') {
                size_t line_end = skip_horizontal(source, search);
                if (line_end == source.len || at(source, line_end) == '\n') {
                    literal_end = line_start;
                    search = line_end < source.len ? line_end + 1 : line_end;
                }
            }
        }
        if (literal_end > literal) {
            Instruction text = {.op = TEXT, .text = slice(source, literal, literal_end)};
            PUSH(arena, *tokens, text);
        }
        Text token = trim_js(slice(source, open + width, close));
        Instruction instruction = {.op = TAG, .text = token};
        if (!tag) {
            instruction.op = raw ? RAW : ESCAPED;
            instruction.path = read_path(arena, token);
            if (!instruction.path.len) return path_error(arena, token);
        }
        PUSH(arena, *tokens, instruction);
        literal = search;
    }
    if (literal < source.len) {
        Instruction text = {.op = TEXT, .text = slice(source, literal, source.len)};
        PUSH(arena, *tokens, text);
    }
    return EMPTY;
}

static Text control_path(Text token, Instruction *block) {
    Text path = EMPTY;
    if (starts(token, LIT("for "))) {
        size_t end = find_byte(token, ' ', 4);
        Text binding = slice(token, 4, end);
        if (valid_identifier(binding) && starts(slice(token, end, token.len), LIT(" in "))) {
            block->op = LOOP;
            block->binding = binding;
            path = slice(token, end + 4, token.len);
        }
    } else if (starts(token, LIT("if "))) {
        path = slice(token, 3, token.len);
        block->op = IF;
        if (starts(path, LIT("not "))) {
            path = slice(path, 4, path.len);
            block->op = UNLESS;
        }
    }
    return has_space(path) ? EMPTY : path;
}

typedef struct { size_t block, skip; } OpenBlock;

static Text compile(Arena *arena, Text source, Program *program) {
    Instructions tokens = {0};
    Text error = tokenize(arena, source, &tokens);
    if (error.len) return error;

    /* Compact the tokens in place now that all interpolation paths have been validated. */
    Instruction *code = tokens.items;
    VECTOR(OpenBlock) blocks = {0};
    size_t len = 0;
    for (size_t i = 0; i < tokens.len; i++) {
        Instruction instruction = tokens.items[i];
        if (instruction.op != TAG) {
            code[len++] = instruction;
            continue;
        }
        Text token = instruction.text;
        OpenBlock *top = blocks.len ? &blocks.items[blocks.len - 1] : NULL;
        Text path = control_path(token, &instruction);
        if (path.len) {
            instruction.path = read_path(arena, path);
            if (!instruction.path.len) return path_error(arena, path);
            OpenBlock block = {len, SIZE_MAX};
            PUSH(arena, blocks, block);
            code[len++] = instruction;
        } else if (equal(token, LIT("else"))) {
            if (!top || code[top->block].op == LOOP || top->skip != SIZE_MAX)
                return LIT("an else outside an if");
            top->skip = len;
            code[len++] = (Instruction){.op = JUMP};
            code[top->block].end = len;
        } else if (equal(token, LIT("endfor")) || equal(token, LIT("endif"))) {
            bool loop = equal(token, LIT("endfor"));
            if (!top || (code[top->block].op == LOOP) != loop)
                return loop ? LIT("an endfor with no open for") : LIT("an endif with no open if");
            code[top->skip == SIZE_MAX ? top->block : top->skip].end = len;
            blocks.len--;
        } else {
            Text name = EMPTY;
            if (starts(token, LIT("include \"")) && ends(token, LIT("\"")) && token.len >= 10)
                name = slice(token, 9, token.len - 1);
            if (!layout_name(name)) return JOIN(arena, LIT("{% "), token, LIT(" %} is not a tag"));
            code[len++] = (Instruction){.op = INCLUDE, .text = name};
        }
    }
    if (blocks.len)
        return code[blocks.items[blocks.len - 1].block].op == LOOP
            ? LIT("a for is not closed") : LIT("a if is not closed");
    *program = (Program){code, len};
    return EMPTY;
}

typedef struct Scope Scope;
struct Scope {
    Text name;
    Value value, loop;
    const Scope *parent;
};

static Value own(Value value, Text name) {
    if (value.type == V_OBJECT) {
        for (size_t i = 0; i < value.as.object.len; i++) {
            const Field *field = &value.as.object.fields[i];
            if (equal(field->name, name)) return field->value;
        }
    } else if (value.type == V_ARRAY && equal(name, LIT("length"))) {
        return number_value(value.as.array.len);
    }
    return (Value){0};
}

static Value lookup(Value root, const Scope *scope, Path path) {
    Text name = path.parts[0];
    Value value;
    if (scope && equal(name, LIT("loop"))) {
        value = scope->loop;
    } else {
        while (scope && !equal(scope->name, name)) scope = scope->parent;
        value = scope ? scope->value : own(root, name);
    }
    for (size_t i = 1; i < path.len && value.type != V_NULL; i++)
        value = own(value, path.parts[i]);
    return value;
}

static long long integral_number(Arena *arena, double number) {
    /* Beyond the safe-integer range JS can choose a shorter decimal representation. */
    if (!(number > -0x1p53 && number < 0x1p53) || (double)(long long)number != number)
        site_fail(arena, "SITEGE_FALLBACK: number cannot be formatted as a native integer");
    return (long long)number;
}

static void format_value(Buffer *out, Value value, bool escaped) {
    switch (value.type) {
    case V_STRING:
        if (escaped) append_html(out, value.as.text);
        else append(out, value.as.text);
        break;
    case V_NUMBER: {
        char local[24];
        append(out, digits(local, integral_number(out->arena, value.as.number)));
        break;
    }
    case V_BOOL:
        append(out, value.as.number ? LIT("true") : LIT("false"));
        break;
    case V_ARRAY:
        for (size_t i = 0; i < value.as.array.len; i++) {
            if (i) append(out, LIT(", "));
            format_value(out, value.as.array.items[i], escaped);
        }
        break;
    default:
        break;
    }
}

static bool truthy(Value value) {
    switch (value.type) {
    case V_NULL: return false;
    case V_BOOL:
    case V_NUMBER: return value.as.number != 0;
    case V_STRING: return value.as.text.len != 0;
    case V_ARRAY: return value.as.array.len != 0;
    default: return true;
    }
}

#define FIELD(key, val) (Field){LIT(key), (val)}

static void emit(const Instruction *code, size_t start, size_t end, Value root,
                 const Scope *scope, unsigned depth, Buffer *out) {
    for (size_t i = start; i < end;) {
        const Instruction *instruction = &code[i++];
        Opcode op = instruction->op;
        switch (op) {
        case TEXT:
            append(out, instruction->text);
            break;
        case ESCAPED:
        case RAW:
            format_value(out, lookup(root, scope, instruction->path), op == ESCAPED);
            break;
        case IF:
        case UNLESS:
            if (truthy(lookup(root, scope, instruction->path)) == (op == UNLESS))
                i = instruction->end;
            break;
        case JUMP:
            i = instruction->end;
            break;
        case LOOP: {
            Value values = lookup(root, scope, instruction->path);
            if (values.type == V_ARRAY && values.as.array.len) {
                size_t len = values.as.array.len;
                Field fields[] = {
                    FIELD("index", number_value(0)), FIELD("first", bool_value(false)),
                    FIELD("last", bool_value(false)), FIELD("length", number_value(len)),
                };
                Scope child = {.name = instruction->binding, .loop = object_value(fields, COUNT(fields)),
                               .parent = scope};
                for (size_t k = 0; k < len; k++) {
                    child.value = values.as.array.items[k];
                    fields[0].value.as.number = k + 1;
                    fields[1].value.as.number = k == 0;
                    fields[2].value.as.number = k + 1 == len;
                    emit(code, i, instruction->end, root, &child, depth, out);
                }
            }
            i = instruction->end;
            break;
        }
        case INCLUDE:
            if (instruction->include && depth < 8)
                emit(instruction->include->items, 0, instruction->include->len, root, scope, depth + 1, out);
            break;
        case TAG:
            break;
        }
    }
}

/* Site build */

typedef struct Source Source;
struct Source {
    Matter matter;
    Text path, dir, slug, permalink, url;
    const Program *layout;
    bool is_post;
    Source *newer, *older;
    Value tags, link, card, page;
};
typedef VECTOR(Source *) Sources;

typedef struct {
    Text slug;
    Field info[3];
    Sources posts;
} TagGroup;
typedef VECTOR(TagGroup *) Groups;

static int compare_template(TemplateInput a, TemplateInput b) { return compare_text(a.name, b.name); }
static int compare_file(File a, File b) { return compare_text(a.path, b.path); }
static int compare_post(const Source *a, const Source *b) {
    int order = compare_text(b->matter.values[DATE], a->matter.values[DATE]);
    return order ? order : compare_text(a->slug, b->slug);
}
static int compare_group(const TagGroup *a, const TagGroup *b) {
    if (a->posts.len != b->posts.len) return a->posts.len > b->posts.len ? -1 : 1;
    return compare_text(a->slug, b->slug);
}

DEFINE_SORT(sort_templates, TemplateInput, compare_template)
DEFINE_SORT(sort_files, File, compare_file)
DEFINE_SORT(sort_posts, Source *, compare_post)
DEFINE_SORT(sort_groups, TagGroup *, compare_group)

static Map compile_templates(Arena *arena, SiteInput *input, TextList *report) {
    Map templates = {0};
    sort_templates(arena, input->templates, input->template_count);
    Program *programs = site_alloc(arena, input->template_count * sizeof(*programs));
    size_t count = 0;
    for (size_t i = 0; i < input->template_count; i++) {
        TemplateInput entry = input->templates[i];
        Text error = compile(arena, entry.source, &programs[count]);
        if (error.len) {
            Text line = JOIN(arena, LIT("template \""), entry.name, LIT("\": "), error);
            PUSH(arena, *report, line);
        } else {
            map_put(arena, &templates, entry.name, &programs[count++]);
        }
    }
    for (size_t i = 0; i < count; i++) {
        for (size_t k = 0; k < programs[i].len; k++) {
            Instruction *instruction = &programs[i].items[k];
            if (instruction->op == INCLUDE)
                instruction->include = map_get(&templates, instruction->text);
        }
    }
    return templates;
}

typedef VECTOR(File) Files;
typedef struct {
    Sources sources, posts;
    Files statics;
    Map source_urls, static_paths;
} Collection;

static Collection collect_files(Arena *arena, SiteInput *input, Text base,
                                const Map *templates, Map *slugs, TextList *report) {
    Collection collection = {0};
    Map directories = {0}, root = {0};
    map_put(arena, &directories, EMPTY, &root);
    claim_slug(arena, &root, LIT("posts"));
    claim_slug(arena, &root, LIT("tags"));
    claim_slug(arena, &root, LIT("page"));
    sort_files(arena, input->files, input->file_count);
    for (size_t i = 0; i < input->file_count; i++) {
        File file = input->files[i];
        if (!ends(file.path, LIT(".md"))) {
            PUSH(arena, collection.statics, file);
            map_put(arena, &collection.static_paths, file.path, &input->files[i]);
            continue;
        }
        Matter matter = {0};
        Text error = read_front_matter(arena, normalize_lines(arena, file.content), &matter);
        bool post = starts(file.path, LIT("posts/"));
        Text layout = matter.present & (1u << LAYOUT) ? matter.values[LAYOUT]
                                                     : post ? LIT("post") : LIT("page");
        const Program *program = map_get(templates, layout);
        if (!error.len) {
            if (starts(file.path, LIT("tags/"))) error = LIT("tags/ is the generator's own");
            else if (starts(file.path, LIT("page/"))) error = LIT("page/ is the generator's own");
            else if (post && !(matter.present & (1u << DATE))) error = LIT("a post needs a date");
            else if (!program) error = JOIN(arena, LIT("layout \""), layout, LIT("\" is no template that parses"));
        }
        if (error.len) {
            Text line = JOIN(arena, LIT("refused "), file.path, LIT(": "), error);
            PUSH(arena, *report, line);
            continue;
        }
        if (matter.draft) {
            Text line = JOIN(arena, LIT("draft "), file.path);
            PUSH(arena, *report, line);
            continue;
        }

        size_t slash = file.path.len;
        while (slash && at(file.path, slash - 1) != '/') slash--;
        Text dir = slash ? slice(file.path, 0, slash - 1) : EMPTY;
        Text output_dir = post ? LIT("posts") : dir;
        Map *used = map_get(&directories, output_dir);
        if (!used) {
            used = site_alloc(arena, sizeof(*used));
            *used = (Map){0};
            map_put(arena, &directories, output_dir, used);
        }
        Text slug = matter.present & (1u << SLUG) ? matter.values[SLUG]
                                                  : slug_of(arena, slugs, matter.values[TITLE]);
        slug = claim_slug(arena, used, slug);
        Text permalink = output_dir.len ? JOIN(arena, output_dir, LIT("/"), slug) : slug;
        Source *source = site_alloc(arena, sizeof(*source));
        *source = (Source){
            .matter = matter, .path = file.path, .dir = dir, .slug = slug,
            .permalink = permalink, .url = JOIN(arena, base, LIT("/"), permalink, LIT("/")),
            .layout = program, .is_post = post,
        };
        PUSH(arena, collection.sources, source);
        if (post) PUSH(arena, collection.posts, source);
        map_put(arena, &collection.source_urls, file.path, &source->url);
    }
    return collection;
}

static Value tag_references(Arena *arena, Source *source, Text base, Map *slugs,
                            Map *by_tag, Groups *groups) {
    size_t len = source->matter.tags.len;
    if (!len) return array_value(NULL, 0);
    Value *refs = site_alloc(arena, len * sizeof(*refs));
    Text *seen = site_alloc(arena, len * sizeof(*seen));
    size_t count = 0;
    for (size_t i = 0; i < len; i++) {
        Text name = source->matter.tags.items[i];
        Text slug = slug_of(arena, slugs, name);
        size_t j = 0;
        while (j < count && !equal(slug, seen[j])) j++;
        if (j < count) continue;
        TagGroup *group = map_get(by_tag, slug);
        if (!group && source->is_post) {
            group = site_alloc(arena, sizeof(*group));
            *group = (TagGroup){
                .slug = slug,
                .info = {FIELD("name", string_value(name)), FIELD("slug", string_value(slug)),
                         FIELD("url", string_value(JOIN(arena, base, LIT("/tags/"), slug, LIT("/"))))},
            };
            map_put(arena, by_tag, slug, group);
            PUSH(arena, *groups, group);
        }
        Field *fields;
        if (group) {
            fields = group->info;
            if (source->is_post) PUSH(arena, group->posts, source);
        } else {
            fields = site_alloc(arena, sizeof(group->info));
            fields[0] = FIELD("name", string_value(name));
            fields[1] = FIELD("slug", string_value(slug));
            fields[2] = FIELD("url", string_value(EMPTY));
        }
        seen[count] = slug;
        refs[count++] = object_value(fields, 3);
    }
    return array_value(refs, count);
}

static size_t reading_minutes(Text body) {
    size_t words = 0;
    bool in_word = false;
    for (size_t i = 0; i < body.len;) {
        unsigned c = (unsigned char)body.data[i];
        bool word;
        if (c < 128) {
            i++;
            word = c != ' ' && c - 9 > 4;
        } else {
            word = !js_space(decode_utf8(body, &i));
        }
        words += word && !in_word;
        in_word = word;
    }
    return words ? (words - 1) / 200 + 1 : 1;
}

static Text long_date(Arena *arena, Text date) {
    if (!date.len) return EMPTY;
    char local[24];
    Text day = digits(local, decimal(slice(date, 8, 10)));
    Text month = month_names[decimal(slice(date, 5, 7)) - 1];
    return JOIN(arena, day, LIT(" "), month, LIT(" "), slice(date, 0, 4));
}

enum { LINK_FIELDS = 4, CARD_FIELDS = 8, PAGE_NEWER = 11, PAGE_OLDER, PAGE_FIELDS = 14 };

static void make_page(Arena *arena, Source *source, Markdown rendered) {
    const Matter *matter = &source->matter;
    Text date = matter->values[DATE];
    Field fields[] = {
        FIELD("title", string_value(matter->values[TITLE])),
        FIELD("url", string_value(source->url)),
        FIELD("date", string_value(date)),
        FIELD("date_long", string_value(long_date(arena, date))),
        FIELD("summary", string_value(matter->values[SUMMARY])),
        FIELD("excerpt", string_value(rendered.excerpt)),
        FIELD("reading_minutes", number_value(reading_minutes(matter->body))),
        FIELD("tags", source->tags),
        FIELD("slug", string_value(source->slug)),
        FIELD("content", string_value(rendered.content)),
        FIELD("toc", rendered.toc),
        FIELD("newer", (Value){0}),
        FIELD("older", (Value){0}),
        FIELD("is_post", bool_value(source->is_post)),
    };
    Field *page = site_alloc(arena, sizeof(fields));
    memcpy(page, fields, sizeof(fields));
    source->link = object_value(page, LINK_FIELDS);
    source->card = object_value(page, CARD_FIELDS);
    source->page = object_value(page, PAGE_FIELDS);
}

static size_t slice_index(double index, size_t len) {
    if (index != index) return 0;
    if (index <= -(double)len) return 0;
    if (index >= (double)len) return len;
    return index <= -1 ? len - (size_t)-index : index < 0 ? 0 : (size_t)index;
}

static Value slice_array(Value array, double start, double end) {
    size_t len = array.as.array.len;
    size_t from = slice_index(start, len), to = slice_index(end, len);
    return to > from ? array_value(array.as.array.items + from, to - from) : array_value(NULL, 0);
}

typedef struct {
    Files files;
    Map positions;
    Buffer buffer;
} Output;

static void set_output(Output *out, Text path, Text content) {
    /* Assignment of a string to Object.prototype's __proto__ setter has no effect. */
    if (equal(path, LIT("__proto__"))) return;
    Arena *arena = out->buffer.arena;
    size_t position = (uintptr_t)map_get(&out->positions, path);
    if (position) {
        out->files.items[position - 1].content = content;
    } else {
        File file = {path, content};
        PUSH(arena, out->files, file);
        map_put(arena, &out->positions, path, (void *)(uintptr_t)out->files.len);
    }
}

static void render(Output *out, Text path, const Program *program, Value root) {
    out->buffer.len = 0;
    emit(program->items, 0, program->len, root, NULL, 0, &out->buffer);
    set_output(out, path, copy_text(out->buffer.arena, written(&out->buffer)));
}

static Text index_url(Arena *arena, Text base, long long number) {
    char local[24];
    return number == 1 ? JOIN(arena, base, LIT("/"))
                       : JOIN(arena, base, LIT("/page/"), digits(local, number), LIT("/"));
}

static void render_indexes(Output *out, const Program *index, Field *root, Text base,
                           Value cards, double per) {
    Arena *arena = out->buffer.arena;
    double pages = (double)cards.as.array.len / per;
    if (pages != pages) return;
    long long total = 1;
    if (pages > 1) {
        if (pages >= 0x1p53) site_fail(arena, "Native site generator pagination is too large");
        total = (long long)pages;
        if ((double)total < pages) total++;
    }
    for (long long number = 1; number <= total; number++) {
        Field pager[] = {
            FIELD("number", number_value(number)),
            FIELD("total", number_value(total)),
            FIELD("url", string_value(index_url(arena, base, number))),
            FIELD("newer_url", string_value(number == 1 ? EMPTY : index_url(arena, base, number - 1))),
            FIELD("older_url", string_value(number == total ? EMPTY : index_url(arena, base, number + 1))),
        };
        root[3] = FIELD("posts", slice_array(cards, (number - 1) * per, number * per));
        root[4] = FIELD("pager", object_value(pager, COUNT(pager)));
        char local[24];
        Text path = number == 1 ? LIT("index.html")
            : JOIN(arena, LIT("page/"), digits(local, number), LIT("/index.html"));
        render(out, path, index, object_value(root, 5));
    }
}

static Value post_cards(Arena *arena, Sources posts) {
    Value *cards = site_alloc(arena, posts.len * sizeof(*cards));
    for (size_t i = 0; i < posts.len; i++) cards[i] = posts.items[i]->card;
    return array_value(cards, posts.len);
}

SiteOutput site_build(Arena *arena, SiteInput *input) {
    TextList report = {0};
    Map templates = compile_templates(arena, input, &report);
    Text base = input->base;
    while (base.len && at(base, base.len - 1) == '/') base.len--;
    Map slugs = {0}, by_tag = {0}, broken = {0};
    Collection collection = collect_files(arena, input, base, &templates, &slugs, &report);
    Sources sources = collection.sources, posts = collection.posts;
    Groups groups = {0};

    sort_posts(arena, posts.items, posts.len);
    for (size_t i = 0; i < posts.len; i++) {
        Source *post = posts.items[i];
        post->newer = i ? posts.items[i - 1] : NULL;
        post->older = i + 1 < posts.len ? posts.items[i + 1] : NULL;
        post->tags = tag_references(arena, post, base, &slugs, &by_tag, &groups);
    }
    for (size_t i = 0; i < sources.len; i++) {
        Source *source = sources.items[i];
        SourceLink link = {source->path, source->dir, base, &collection.source_urls,
                           &collection.static_paths, &broken, &report};
        Markdown rendered = markdown(arena, source->matter.body, &slugs, &link);
        if (!source->is_post) source->tags = tag_references(arena, source, base, &slugs, &by_tag, &groups);
        make_page(arena, source, rendered);
    }

    sort_groups(arena, groups.items, groups.len);
    Value *tags = site_alloc(arena, groups.len * sizeof(*tags));
    for (size_t i = 0; i < groups.len; i++) {
        TagGroup *group = groups.items[i];
        Field *fields = site_alloc(arena, 4 * sizeof(*fields));
        memcpy(fields, group->info, sizeof(group->info));
        fields[3] = FIELD("count", number_value(group->posts.len));
        tags[i] = object_value(fields, 4);
    }
    Value cards = post_cards(arena, posts);
    Field site[] = {FIELD("title", input->title), FIELD("base_url", string_value(base))};
    Field root[5] = {
        FIELD("site", object_value(site, COUNT(site))),
        FIELD("tags", array_value(tags, groups.len)),
        FIELD("recent", slice_array(cards, 0, input->recent)),
    };
    Output out = {.buffer = {.arena = arena}};
    for (size_t i = 0; i < sources.len; i++) {
        Source *source = sources.items[i];
        Field *page = source->page.as.object.fields;
        if (source->newer) page[PAGE_NEWER].value = source->newer->link;
        if (source->older) page[PAGE_OLDER].value = source->older->link;
        root[3] = FIELD("page", source->page);
        render(&out, JOIN(arena, source->permalink, LIT("/index.html")), source->layout, object_value(root, 4));
    }

    const Program *index = map_get(&templates, LIT("index"));
    if (index) render_indexes(&out, index, root, base, cards, input->per_page);

    const Program *tag = map_get(&templates, LIT("tag"));
    if (tag) {
        for (size_t i = 0; i < groups.len; i++) {
            TagGroup *group = groups.items[i];
            Field fields[5];
            memcpy(fields, tags[i].as.object.fields, 4 * sizeof(*fields));
            fields[4] = FIELD("posts", post_cards(arena, group->posts));
            root[3] = FIELD("tag", object_value(fields, COUNT(fields)));
            render(&out, JOIN(arena, LIT("tags/"), group->slug, LIT("/index.html")), tag, object_value(root, 4));
        }
    }
    const Program *tags_layout = map_get(&templates, LIT("tags"));
    if (tags_layout) render(&out, LIT("tags/index.html"), tags_layout, object_value(root, 3));

    for (size_t i = 0; i < collection.statics.len; i++) {
        File file = collection.statics.items[i];
        if (map_get(&out.positions, file.path)) {
            Text line = JOIN(arena, LIT("shadowed "), file.path, LIT(": a built page has its path"));
            PUSH(arena, report, line);
        } else {
            set_output(&out, file.path, file.content);
        }
    }
    sort_text(arena, report.items, report.len);
    Buffer report_text = {.arena = arena};
    for (size_t i = 0; i < report.len; i++) {
        append(&report_text, report.items[i]);
        append_byte(&report_text, '\n');
    }
    char local[24];
    append(&report_text, LIT("built "));
    append(&report_text, digits(local, sources.len - posts.len));
    append(&report_text, LIT(" pages, "));
    append(&report_text, digits(local, posts.len));
    append(&report_text, LIT(" posts, "));
    append(&report_text, digits(local, groups.len));
    append(&report_text, LIT(" tags and "));
    append(&report_text, digits(local, collection.statics.len));
    append(&report_text, LIT(" static files\n"));
    set_output(&out, LIT("build-report.txt"), written(&report_text));
    return (SiteOutput){out.files.items, out.files.len};
}
