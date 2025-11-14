// include/roole/common.h
// (Mostly unchanged, just ensure result_t is defined)

#ifndef ROOLE_COMMON_H
#define ROOLE_COMMON_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include "roole/logger/logger.h"

// Error codes (backward compatible)
typedef enum {
    RESULT_OK = 0,
    RESULT_ERR_NOMEM = -1,
    RESULT_ERR_INVALID = -2,
    RESULT_ERR_NOTFOUND = -3,
    RESULT_ERR_TIMEOUT = -4,
    RESULT_ERR_NETWORK = -5,
    RESULT_ERR_EXISTS = -6,
    RESULT_ERR_FULL = -7,
    RESULT_ERR_EMPTY = -8
} result_status_t;

// Enhanced result type
typedef struct result {
    int code;
    char message[256];
    const char *source_file;
    int source_line;
    const char *function;
} result_t;

// Result construction macros
#define RESULT_SUCCESS() ((result_t){ \
    .code = RESULT_OK, .message = {0}, \
    .source_file = NULL, .source_line = 0, .function = NULL \
})

#define RESULT_ERROR(error_code, fmt, ...) ({ \
    result_t __res = { \
        .code = (error_code), \
        .source_file = __FILE__, \
        .source_line = __LINE__, \
        .function = __func__ \
    }; \
    snprintf(__res.message, 256, fmt, ##__VA_ARGS__); \
    __res; \
})

// Type aliases
typedef uint16_t node_id_t;
typedef uint64_t execution_id_t;
typedef uint32_t rule_id_t;

// Timing utilities
uint64_t time_now_ms(void);
uint64_t time_now_us(void);
double time_diff_us(const struct timespec *start, const struct timespec *end);
void timespec_now(struct timespec *ts);

// Memory utilities
void* safe_malloc(size_t size);
void* safe_calloc(size_t nmemb, size_t size);
void* safe_realloc(void *ptr, size_t size);
void safe_free(void *ptr);

// String utilities
char* free_strdup(const char *s);
int safe_strncpy(char *dst, const char *src, size_t dst_size);

// Hash utilities
uint32_t hash_u32(uint32_t x);
uint64_t hash_u64(uint64_t x);
uint32_t hash_string(const char *str);

// Macros
#define ROOLE_MIN(a, b) ((a) < (b) ? (a) : (b))
#define ROOLE_MAX(a, b) ((a) > (b) ? (a) : (b))
#define ROOLE_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#endif // ROOLE_COMMON_H