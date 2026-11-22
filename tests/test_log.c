/*
 * test_log.c - event log framing, CRC and round trip.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sentinel.h"
#include "sl_log.h"

#include <stdio.h>
#include <stdlib.h>
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

static const char *tmp_path(char *buf, size_t cap, const char *tag)
{
    snprintf(buf, cap, "/tmp/sentinel-test-%s-%d.slg", tag, (int)getpid());
    return buf;
}

static void test_round_trip(void)
{
    char path[128];
    sl_log_t *l;
    sl_log_reader_t rd;
    sl_rec_t hdr;
    uint8_t payload[SL_LOG_MAX_PAYLOAD];
    size_t plen;
    int jobs = 0, misses = 0, modes = 0, rc;

    tmp_path(path, sizeof(path), "roundtrip");
    l = sl_log_create(path, 1);
    CHECK(l != NULL, "log create");
    CHECK(sl_log_start(l) == 0, "log start");

    for (int i = 0; i < 500; i++) {
        sl_log_job(l, 0, (uint32_t)i, 100000u + (uint64_t)i, 900000u);
    }
    sl_log_miss(l, 0, 7, 120000u, 1020000u);
    sl_log_mode(l, SL_MODE_NOMINAL, SL_MODE_DEGRADED, SL_RS_CRITICAL_MISS, 0);
    sl_log_ctl(l, SL_CTL_RESET, 0, 1);
    sl_log_stop(l); /* flushes and closes */

    CHECK(sl_log_reader_open(&rd, path) == 0, "reader open");
    while ((rc = sl_log_reader_next(&rd, &hdr, payload, sizeof(payload), &plen)) == 1) {
        switch (hdr.type) {
        case SL_EV_JOB: jobs++; break;
        case SL_EV_MISS: misses++; break;
        case SL_EV_MODE: modes++; break;
        default: break;
        }
    }
    CHECK(rc == 0, "reader stopped with rc %d", rc);
    CHECK(rd.bad_records == 0, "corrupt records: %llu",
          (unsigned long long)rd.bad_records);
    CHECK(jobs == 500, "jobs %d", jobs);
    CHECK(misses == 1, "misses %d", misses);
    CHECK(modes == 1, "modes %d", modes);
    sl_log_reader_close(&rd);
    unlink(path);
}

static void test_decimation(void)
{
    char path[128];
    sl_log_t *l;
    sl_log_reader_t rd;
    sl_rec_t hdr;
    uint8_t payload[SL_LOG_MAX_PAYLOAD];
    size_t plen;
    int jobs = 0, misses = 0;

    tmp_path(path, sizeof(path), "decimation");
    l = sl_log_create(path, 10);
    CHECK(sl_log_start(l) == 0, "log start");
    for (int i = 0; i < 1000; i++) {
        sl_log_job(l, 0, (uint32_t)i, 100000u, 900000u);
    }
    sl_log_miss(l, 0, 3, 1u, 900001u);
    sl_log_stop(l);

    CHECK(sl_log_reader_open(&rd, path) == 0, "reader open");
    while (sl_log_reader_next(&rd, &hdr, payload, sizeof(payload), &plen) == 1) {
        if (hdr.type == SL_EV_JOB) jobs++;
        if (hdr.type == SL_EV_MISS) misses++;
    }
    CHECK(jobs == 100, "decimated jobs %d (expected 100)", jobs);
    CHECK(misses == 1, "misses must never be decimated (%d)", misses);
    sl_log_reader_close(&rd);
    unlink(path);
}

static void test_corruption_is_detected(void)
{
    char path[128];
    sl_log_t *l;
    sl_log_reader_t rd;
    sl_rec_t hdr;
    uint8_t payload[SL_LOG_MAX_PAYLOAD];
    size_t plen;
    FILE *f;
    long pos;
    int corrupted = 0;

    /* Keep the record count small: the log goes through a 2 ms batching
     * writer, and the test wants a file it can corrupt in place. */
    tmp_path(path, sizeof(path), "corrupt");
    l = sl_log_create(path, 1);
    CHECK(sl_log_start(l) == 0, "log start");
    for (int i = 0; i < 20; i++) {
        sl_log_job(l, 0, (uint32_t)i, 100000u, 900000u);
    }
    sl_log_stop(l);

    f = fopen(path, "r+b");
    CHECK(f != NULL, "reopen for corruption");
    if (f == NULL) {
        return;
    }
    /* Flip a byte inside the payload of the first record. */
    pos = (long)sizeof(sl_rec_t) + 3;
    fseek(f, pos, SEEK_SET);
    {
        int c = fgetc(f);
        fseek(f, pos, SEEK_SET);
        fputc(c ^ 0xFF, f);
    }
    fclose(f);

    CHECK(sl_log_reader_open(&rd, path) == 0, "reader open");
    while (sl_log_reader_next(&rd, &hdr, payload, sizeof(payload), &plen) == 1) {
        corrupted++;
    }
    CHECK(rd.bad_records >= 1, "corruption not detected");
    CHECK(corrupted == 19, "wrong survivor count %d", corrupted);
    sl_log_reader_close(&rd);
    unlink(path);
}

static void test_truncated_tail(void)
{
    char path[128];
    sl_log_t *l;
    sl_log_reader_t rd;
    sl_rec_t hdr;
    uint8_t payload[SL_LOG_MAX_PAYLOAD];
    size_t plen;
    long size;
    int n = 0;

    tmp_path(path, sizeof(path), "truncated");
    l = sl_log_create(path, 1);
    CHECK(sl_log_start(l) == 0, "log start");
    for (int i = 0; i < 30; i++) {
        sl_log_job(l, 0, (uint32_t)i, 100000u, 900000u);
    }
    sl_log_stop(l);

    /* Simulate a power cut in the middle of the last record. */
    {
        FILE *f = fopen(path, "r+b");
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        ftruncate(fileno(f), size - 5);
        fclose(f);
    }

    CHECK(sl_log_reader_open(&rd, path) == 0, "reader open");
    while (sl_log_reader_next(&rd, &hdr, payload, sizeof(payload), &plen) == 1) {
        n++;
    }
    CHECK(n == 29, "truncated tail handling: %d records", n);
    sl_log_reader_close(&rd);
    unlink(path);
}

int main(void)
{
    test_round_trip();
    test_decimation();
    test_corruption_is_detected();
    test_truncated_tail();

    printf("test_log: %s (%d failures)\n", failures ? "FAILED" : "ok", failures);
    return failures ? 1 : 0;
}
