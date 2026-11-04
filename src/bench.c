#define _GNU_SOURCE
#include "bench.h"

#include "decoder.h"
#include "sonic.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

/* Measures what playing a given file costs on this device: memory to hold it open, CPU
 * per second of audio (decode + resample + speed change), and seek latency. Used to keep
 * background playback cheap enough that the stock reader never lags (ARCHITECTURE.md
 * D13). Mirrors the player's pipeline but writes to nowhere. */

static int64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static int64_t cpu_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* Resident and peak-resident set in KiB. Linux reports both; elsewhere only the peak. */
static void mem_kib(long *rss, long *hwm) {
    *rss = -1;
    *hwm = -1;
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "VmRSS:", 6) == 0) *rss = strtol(line + 6, NULL, 10);
            if (strncmp(line, "VmHWM:", 6) == 0) *hwm = strtol(line + 6, NULL, 10);
        }
        fclose(f);
        return;
    }
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
#ifdef __APPLE__
        *hwm = (long)(ru.ru_maxrss / 1024);
#else
        *hwm = (long)ru.ru_maxrss;
#endif
    }
}

static void print_mem(FILE *out, const char *label) {
    long rss, hwm;
    mem_kib(&rss, &hwm);
    fprintf(out, "%-26s rss=%ld KiB  peak=%ld KiB\n", label, rss, hwm);
}

static void print_meminfo(FILE *out) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0 || strncmp(line, "MemFree:", 8) == 0 ||
            strncmp(line, "MemAvailable:", 13) == 0 || strncmp(line, "Cached:", 7) == 0) {
            fprintf(out, "  %s", line);
        }
    }
    fclose(f);
}

/* Decode `audio_s` seconds of output at `speed` from `start_ms`; returns CPU percent of
 * real time (100 = one full core just to keep up), or -1 on error. */
static double run_pipeline(Decoder *d, sonicStream s, int64_t start_ms, int audio_s,
                           float speed, int16_t *buf, int16_t *obuf, int chunk) {
    if (decoder_seek_ms(d, start_ms) < 0) return -1;
    sonicFlushStream(s);
    while (sonicSamplesAvailable(s) > 0) sonicReadShortFromStream(s, obuf, chunk);
    sonicSetSpeed(s, speed);

    int64_t target = (int64_t)audio_s * DECODER_OUT_RATE;
    int64_t produced = 0;
    int64_t c0 = cpu_us();
    while (produced < target) {
        int n = decoder_read_frames(d, buf, chunk);
        if (n <= 0) break;
        sonicWriteShortToStream(s, buf, n);
        int got;
        while ((got = sonicReadShortFromStream(s, obuf, chunk)) > 0) {
            /* Software volume, as the player applies it. */
            for (int i = 0; i < got * DECODER_OUT_CHANNELS; i++) {
                obuf[i] = (int16_t)((obuf[i] * 3) / 4);
            }
            produced += got;
        }
    }
    int64_t c1 = cpu_us();
    if (produced <= 0) return -1;
    double out_s = (double)produced / DECODER_OUT_RATE;
    return (double)(c1 - c0) / 1e6 / out_s * 100.0;
}

int bench_run(const char *path, int audio_s, FILE *out) {
    if (!path || !out) return -EINVAL;
    if (audio_s <= 0) audio_s = 120;

    fprintf(out, "aloud bench: %s\n", path);
    print_meminfo(out);
    print_mem(out, "before open");

    int64_t w0 = mono_us(), c0 = cpu_us();
    Decoder *d = NULL;
    int rc = decoder_open(&d, path);
    int64_t w1 = mono_us(), c1 = cpu_us();
    if (rc < 0 || !d) {
        fprintf(out, "open failed: %d\n", rc);
        return 1;
    }
    int64_t dur = decoder_duration_ms(d);
    fprintf(out, "open: %.0f ms wall, %.0f ms cpu; duration %.2f h\n",
            (double)(w1 - w0) / 1000.0, (double)(c1 - c0) / 1000.0, (double)dur / 3600000.0);
    print_mem(out, "after open");

    int chunk = 2048;
    int16_t *buf = malloc((size_t)chunk * DECODER_OUT_CHANNELS * sizeof(int16_t));
    int16_t *obuf = malloc((size_t)chunk * DECODER_OUT_CHANNELS * sizeof(int16_t));
    sonicStream s = sonicCreateStream(DECODER_OUT_RATE, DECODER_OUT_CHANNELS);
    if (!buf || !obuf || !s) {
        fprintf(out, "out of memory\n");
        free(buf);
        free(obuf);
        if (s) sonicDestroyStream(s);
        decoder_close(d);
        return 1;
    }

    static const float speeds[] = {1.0f, 1.5f, 2.0f, 3.0f};
    int64_t mid = dur > 0 ? dur / 2 : 0;
    for (size_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        double pct = run_pipeline(d, s, mid, audio_s, speeds[i], buf, obuf, chunk);
        fprintf(out, "playback %.2fx: %5.2f%% of one CPU core\n", (double)speeds[i], pct);
    }

    /* Seeks spread over the whole book, each followed by the first chunk of audio the
     * listener would hear (what a skip or chapter jump actually costs). */
    int64_t worst = 0, total = 0;
    const int seeks = 12;
    for (int i = 0; i < seeks; i++) {
        int64_t tgt = dur > 0 ? (dur * (int64_t)(i * 7 % seeks)) / seeks : 0;
        int64_t t0 = mono_us();
        if (decoder_seek_ms(d, tgt) == 0) (void)decoder_read_frames(d, buf, chunk);
        int64_t dt = mono_us() - t0;
        total += dt;
        if (dt > worst) worst = dt;
    }
    fprintf(out, "seek+first audio: avg %.1f ms, worst %.1f ms\n",
            (double)total / seeks / 1000.0, (double)worst / 1000.0);
    print_mem(out, "after playback");

    sonicDestroyStream(s);
    free(buf);
    free(obuf);
    decoder_close(d);
    print_mem(out, "after close");
    return 0;
}
