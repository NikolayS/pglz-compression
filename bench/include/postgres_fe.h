/*
 * Minimal postgres_fe.h stub for standalone pglz compilation.
 * Provides just enough definitions to compile pg_lzcompress.c outside Postgres.
 */
#ifndef POSTGRES_FE_H
#define POSTGRES_FE_H

#define FRONTEND 1

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Basic Postgres types */
typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

/* Macros that pg_lzcompress.c needs */
#ifndef Min
#define Min(x, y)       ((x) < (y) ? (x) : (y))
#endif

#ifndef Max
#define Max(x, y)       ((x) > (y) ? (x) : (y))
#endif

#ifndef unlikely
#define unlikely(x)     __builtin_expect((x) != 0, 0)
#endif

#ifndef PGDLLIMPORT
#define PGDLLIMPORT
#endif

/* memcpy / memcmp are in string.h, already included */

#endif /* POSTGRES_FE_H */
