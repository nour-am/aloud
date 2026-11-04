#ifndef ALOUD_BENCH_H
#define ALOUD_BENCH_H

#include <stdio.h>

/* Report open time, memory, CPU per second of audio at several speeds, and seek latency
 * for one audio file. Returns 0 on success. */
int bench_run(const char *path, int audio_s, FILE *out);

#endif /* ALOUD_BENCH_H */
