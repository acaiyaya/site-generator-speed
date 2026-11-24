#ifndef SITEGEN_H
#define SITEGEN_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *data;
    size_t len;
} Text;

typedef struct {
    Text path, content;
} File;

typedef struct {
    Text name, source;
} TemplateInput;

typedef enum { V_NULL, V_BOOL, V_NUMBER, V_STRING, V_ARRAY, V_OBJECT } ValueType;
typedef struct Value Value;
typedef struct Field Field;

struct Value {
    ValueType type;
    union {
        double number;
        Text text;
        struct { Value *items; size_t len; } array;
        struct { Field *fields; size_t len; } object;
    } as;
};

struct Field {
    Text name;
    Value value;
};

typedef struct {
    Value title;
    Text base;
    double per_page, recent;
    TemplateInput *templates;
    size_t template_count;
    File *files;
    size_t file_count;
} SiteInput;

typedef struct {
    File *files;
    size_t count;
} SiteOutput;

typedef struct ArenaBlock ArenaBlock;
typedef struct {
    ArenaBlock *block;
    size_t next_capacity;
    intptr_t recovery[5];
    const char *error;
} Arena;

void *site_alloc(Arena *arena, size_t size);
void site_free(Arena *arena);
void site_fail(Arena *arena, const char *error) __attribute__((noreturn));
SiteOutput site_build(Arena *arena, SiteInput *input);

#endif
