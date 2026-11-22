/*
 * sl_config.h - flat "key = value" configuration file.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SL_CONFIG_H
#define SL_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define SL_CFG_MAX_KV 64

typedef struct {
    char key[48];
    char value[160];
} sl_kv_t;

typedef struct {
    sl_kv_t kv[SL_CFG_MAX_KV];
    int     count;
} sl_config_t;

/* Returns 0 on success, -1 if the file cannot be read. Unknown keys are
 * kept so that a newer config file still loads on an older binary. */
int sl_config_load(sl_config_t *c, const char *path);

const char *sl_config_str(const sl_config_t *c, const char *key,
                          const char *fallback);
uint64_t    sl_config_u64(const sl_config_t *c, const char *key,
                          uint64_t fallback);
uint32_t    sl_config_u32(const sl_config_t *c, const char *key,
                          uint32_t fallback);
/* Same, but the value may carry a unit suffix: "3s", "900ms", "250us". */
uint64_t    sl_config_duration_ns(const sl_config_t *c, const char *key,
                                  uint64_t fallback_ns);

#endif /* SL_CONFIG_H */
