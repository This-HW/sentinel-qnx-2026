/*
 * sl_config.c - minimal configuration reader.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n')) {
        *--e = '\0';
    }
    return s;
}

int sl_config_load(sl_config_t *c, const char *path)
{
    FILE *f;
    char  line[256];

    memset(c, 0, sizeof(*c));
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *p = trim(line);
        char *eq;
        if (*p == '\0' || *p == '#' || *p == ';' || *p == '[') {
            continue;
        }
        eq = strchr(p, '=');
        if (eq == NULL || c->count >= SL_CFG_MAX_KV) {
            continue;
        }
        *eq = '\0';
        snprintf(c->kv[c->count].key, sizeof(c->kv[c->count].key), "%s",
                 trim(p));
        snprintf(c->kv[c->count].value, sizeof(c->kv[c->count].value), "%s",
                 trim(eq + 1));
        c->count++;
    }
    fclose(f);
    return 0;
}

const char *sl_config_str(const sl_config_t *c, const char *key,
                          const char *fallback)
{
    int i;
    for (i = 0; i < c->count; i++) {
        if (strcmp(c->kv[i].key, key) == 0) {
            return c->kv[i].value;
        }
    }
    return fallback;
}

uint64_t sl_config_u64(const sl_config_t *c, const char *key, uint64_t fallback)
{
    const char *v = sl_config_str(c, key, NULL);
    char *end = NULL;
    unsigned long long n;

    if (v == NULL || *v == '\0') {
        return fallback;
    }
    errno = 0;
    n = strtoull(v, &end, 0);
    if (errno != 0 || end == v) {
        return fallback;
    }
    return (uint64_t)n;
}

uint32_t sl_config_u32(const sl_config_t *c, const char *key, uint32_t fallback)
{
    return (uint32_t)sl_config_u64(c, key, fallback);
}

uint64_t sl_config_duration_ns(const sl_config_t *c, const char *key,
                               uint64_t fallback_ns)
{
    const char *v = sl_config_str(c, key, NULL);
    char *end = NULL;
    double n;

    if (v == NULL || *v == '\0') {
        return fallback_ns;
    }
    n = strtod(v, &end);
    if (end == v) {
        return fallback_ns;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (strncmp(end, "ms", 2) == 0) {
        n *= 1e6;
    } else if (strncmp(end, "us", 2) == 0) {
        n *= 1e3;
    } else if (strncmp(end, "ns", 2) == 0) {
        n *= 1.0;
    } else if (*end == 's' || *end == '\0') {
        n *= 1e9;
    }
    if (n < 0.0) {
        return fallback_ns;
    }
    return (uint64_t)n;
}
