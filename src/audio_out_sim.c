#define _POSIX_C_SOURCE 200809L
#include "audio_out.h"
#include "decoder.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *wav_fp;
    int64_t total_frames;
    bool paused;
} SimAudioImpl;

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_wav_header(FILE *fp, uint32_t data_bytes) {
    uint8_t hdr[44];
    memcpy(hdr + 0, "RIFF", 4);
    write_le32(hdr + 4, 36U + data_bytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    write_le32(hdr + 16, 16U);
    write_le16(hdr + 20, 1U); /* PCM */
    write_le16(hdr + 22, (uint16_t)DECODER_OUT_CHANNELS);
    write_le32(hdr + 24, (uint32_t)DECODER_OUT_RATE);
    write_le32(hdr + 28, (uint32_t)(DECODER_OUT_RATE * DECODER_OUT_CHANNELS * 2));
    write_le16(hdr + 32, (uint16_t)(DECODER_OUT_CHANNELS * 2));
    write_le16(hdr + 34, 16U);
    memcpy(hdr + 36, "data", 4);
    write_le32(hdr + 40, data_bytes);
    fseek(fp, 0, SEEK_SET);
    (void)fwrite(hdr, 1, sizeof(hdr), fp);
}

static int sim_write_s16le(AudioOut *ao, const int16_t *frames, int frame_count) {
    if (!ao || !ao->impl || !frames || frame_count < 0) return -EINVAL;
    SimAudioImpl *im = (SimAudioImpl *)ao->impl;
    if (frame_count == 0) return 0;

    if (im->wav_fp) {
        size_t bytes = (size_t)frame_count * DECODER_OUT_CHANNELS * sizeof(int16_t);
        if (fwrite(frames, 1, bytes, im->wav_fp) != bytes) {
            return -EIO;
        }
    }
    im->total_frames += frame_count;
    return frame_count;
}

static int sim_set_paused(AudioOut *ao, bool paused) {
    if (!ao || !ao->impl) return -EINVAL;
    SimAudioImpl *im = (SimAudioImpl *)ao->impl;
    im->paused = paused;
    return 0;
}

static int sim_flush(AudioOut *ao) {
    (void)ao;
    return 0;
}

static int64_t sim_delay_ms(AudioOut *ao) {
    (void)ao;
    return 0;
}

static void sim_close(AudioOut *ao) {
    if (!ao) return;
    SimAudioImpl *im = (SimAudioImpl *)ao->impl;
    if (im) {
        if (im->wav_fp) {
            uint32_t data_bytes = (uint32_t)(im->total_frames * DECODER_OUT_CHANNELS * 2);
            write_wav_header(im->wav_fp, data_bytes);
            fflush(im->wav_fp);
            fclose(im->wav_fp);
        }
        free(im);
    }
    free(ao);
}

int audio_out_open_sim(AudioOut **out, const char *wav_path) {
    if (!out) return -EINVAL;
    *out = NULL;

    AudioOut *ao = calloc(1, sizeof(*ao));
    SimAudioImpl *im = calloc(1, sizeof(*im));
    if (!ao || !im) {
        free(ao);
        free(im);
        return -ENOMEM;
    }

    if (wav_path && wav_path[0] != '\0') {
        im->wav_fp = fopen(wav_path, "wb");
        if (!im->wav_fp) {
            int err = errno;
            free(ao);
            free(im);
            return -err;
        }
        write_wav_header(im->wav_fp, 0);
        ao->type = AUDIO_BACKEND_SIM_WAV;
    } else {
        ao->type = AUDIO_BACKEND_SIM_NULL;
    }

    ao->impl = im;
    ao->write_s16le = sim_write_s16le;
    ao->set_paused = sim_set_paused;
    ao->flush = sim_flush;
    ao->delay_ms = sim_delay_ms;
    ao->close = sim_close;
    *out = ao;
    return 0;
}

int64_t audio_out_sim_frames_written(const AudioOut *ao) {
    if (!ao || !ao->impl) return 0;
    if (ao->type != AUDIO_BACKEND_SIM_NULL && ao->type != AUDIO_BACKEND_SIM_WAV) return 0;
    const SimAudioImpl *im = (const SimAudioImpl *)ao->impl;
    return im->total_frames;
}

void audio_out_close(AudioOut *ao) {
    if (!ao) return;
    if (ao->close) {
        ao->close(ao);
    } else {
        free(ao);
    }
}
