#ifndef ALOUD_MP4LITE_H
#define ALOUD_MP4LITE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal reader for the audio track of a plain (non-fragmented) MP4/M4A/M4B.
 *
 * Why: libavformat's mov demuxer keeps ~36 bytes of RAM per audio frame (AVIndexEntry +
 * per-sample timing) for as long as the file is open, and twice that while opening. An
 * 8.6 h AAC audiobook is 1.33 M frames, i.e. ~50 MB held and ~100 MB peak on a 512 MB
 * Kindle that is also running the stock reader (ARCHITECTURE.md D14). This reader keeps
 * only the small run-length tables (stts, stsc, stco) in memory and streams the per-sample
 * size table (stsz) from the file through a 4 KiB window, so memory is independent of
 * book length.
 *
 * Scope is deliberately narrow: first sound track, AAC (`mp4a` + `esds`), at most one
 * edit, no fragments. Anything else returns an error so the caller falls back to
 * libavformat, which handles every odd file. All sizes read from the file are bounds-
 * checked; this code never trusts the container. */

typedef struct Mp4Lite Mp4Lite;

typedef struct {
    uint32_t timescale;       /* media timescale: units of pts/duration below */
    int64_t duration;         /* presentation duration in timescale units */
    int sample_rate;          /* from the sample entry (AAC config may refine it) */
    int channels;
    uint8_t asc[64];          /* AudioSpecificConfig, the decoder's extradata */
    size_t asc_len;
    uint32_t sample_count;
} Mp4LiteInfo;

/* Returns 0 on success, negative errno if the file is not something this reader handles
 * (the caller should then use libavformat). */
int mp4lite_open(Mp4Lite **out, const char *path, Mp4LiteInfo *info);
void mp4lite_close(Mp4Lite *m);

/* Size and presentation timestamp of the next sample. Returns 1 if there is one, 0 at
 * end of track, negative on a damaged table. A negative pts marks encoder priming that
 * the decoder should drop. */
int mp4lite_peek(Mp4Lite *m, uint32_t *size, int64_t *pts);

/* Read the sample described by the last mp4lite_peek() into dst (at least `size`
 * bytes) and advance. Returns 0, or negative if the file is short/unreadable. */
int mp4lite_read(Mp4Lite *m, uint8_t *dst, uint32_t size);

/* Position on the last sample whose pts <= target_pts (every AAC frame is a sync
 * sample). Returns 0 and sets *out_pts to that sample's pts, or negative on error. */
int mp4lite_seek(Mp4Lite *m, int64_t target_pts, int64_t *out_pts);

/* Bytes of heap this reader holds (for tests and the --bench report). */
size_t mp4lite_heap_bytes(const Mp4Lite *m);

#endif /* ALOUD_MP4LITE_H */
