#ifndef ALOUD_DECODER_H
#define ALOUD_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DECODER_OUT_RATE 44100
#define DECODER_OUT_CHANNELS 2
#define DECODER_MAX_CHAPTERS 512

typedef struct {
    char title[256];
    int64_t start_ms;
    int64_t end_ms;
} DecoderChapter;

typedef struct {
    char title[256];
    char album[256];
    char artist[256];
    char album_artist[256];
    int track_number;
    int64_t duration_ms;
    DecoderChapter chapters[DECODER_MAX_CHAPTERS];
    size_t chapter_count;
    bool has_cover;
} DecoderProbeInfo;

typedef struct Decoder Decoder;

/* Probe metadata, duration, and embedded chapters without decoding the stream.
 * If cover_out != NULL and the file has an attached picture, allocates *cover_out
 * (caller frees) and sets *cover_len. */
int decoder_probe(const char *path, DecoderProbeInfo *info,
                  uint8_t **cover_out, size_t *cover_len);

/* Open a media file for decoding to S16LE, 44100 Hz, stereo PCM. */
int decoder_open(Decoder **out, const char *path);

#define DECODER_FLAG_NO_MP4LITE 1u /* force libavformat (tests, ALOUD_NO_MP4LITE=1) */
int decoder_open_ex(Decoder **out, const char *path, unsigned flags);

/* True when the file is read by the low-memory MP4 reader rather than libavformat. */
bool decoder_uses_mp4lite(const Decoder *d);

/* Close and free all FFmpeg contexts. */
void decoder_close(Decoder *d);

/* Read up to max_frames stereo S16LE frames (each frame = 2 * int16_t = 4 bytes)
 * into pcm_out.
 * Returns:
 *   > 0 : number of stereo frames written
 *   0   : clean EOF
 *   < 0 : error (or corrupt stream tail after returning all valid frames) */
int decoder_read_frames(Decoder *d, int16_t *pcm_out, int max_frames);

/* Seek to target_ms from the beginning of the file. Clamps to [0, duration_ms]. */
int decoder_seek_ms(Decoder *d, int64_t target_ms);

/* Current decoded position in milliseconds. */
int64_t decoder_position_ms(const Decoder *d);

/* Total file duration in milliseconds. */
int64_t decoder_duration_ms(const Decoder *d);

#endif /* ALOUD_DECODER_H */
