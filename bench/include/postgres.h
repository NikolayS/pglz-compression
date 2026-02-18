/*
 * Redirect to postgres_fe.h for standalone builds.
 * pg_lzcompress.c includes either postgres.h or postgres_fe.h depending
 * on FRONTEND. We define FRONTEND, so this should not be reached, but
 * just in case:
 */
#include "postgres_fe.h"
