/*
 * test_config.c - configuration parsing.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sl_config.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                       \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
            failures++;                                                       \
        }                                                                     \
    } while (0)

int main(void)
{
    char path[128];
    sl_config_t cfg;
    FILE *f;

    snprintf(path, sizeof(path), "/tmp/sentinel-test-%d.conf", (int)getpid());
    f = fopen(path, "w");
    fprintf(f,
            "# comment\n"
            "\n"
            "log = /tmp/run.slg\n"
            "priority=30\n"
            "  risk_watch_ppm  =  2000  \n"
            "recover_after = 3s\n"
            "miss_window = 500ms\n"
            "tiny = 250us\n"
            "big = 12000\n"
            "quoted = \"a b\"\n");
    fclose(f);

    CHECK(sl_config_load(&cfg, path) == 0, "load");
    CHECK(strcmp(sl_config_str(&cfg, "log", "x"), "/tmp/run.slg") == 0, "log value");
    CHECK(sl_config_u32(&cfg, "priority", 0) == 30, "priority");
    CHECK(sl_config_u32(&cfg, "risk_watch_ppm", 0) == 2000, "watch");
    CHECK(sl_config_duration_ns(&cfg, "recover_after", 0) == 3000000000ull,
          "recover_after %llu",
          (unsigned long long)sl_config_duration_ns(&cfg, "recover_after", 0));
    CHECK(sl_config_duration_ns(&cfg, "miss_window", 0) == 500000000ull,
          "miss_window");
    CHECK(sl_config_duration_ns(&cfg, "tiny", 0) == 250000ull, "us suffix");
    CHECK(sl_config_duration_ns(&cfg, "big", 0) == 12000000000000ull,
          "a bare number means seconds");
    CHECK(sl_config_u32(&cfg, "missing", 77) == 77, "default for missing key");
    CHECK(strcmp(sl_config_str(&cfg, "missing", "dflt"), "dflt") == 0,
          "string default");

    /* A missing file is reported, not fatal. */
    CHECK(sl_config_load(&cfg, "/nonexistent/sentinel.conf") == -1,
          "missing file must return -1");

    unlink(path);
    printf("test_config: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
