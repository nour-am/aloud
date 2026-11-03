#ifndef ALOUD_AUDIO_OUT_H
#define ALOUD_AUDIO_OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIO_BACKEND_NONE = 0,
    AUDIO_BACKEND_SIM_NULL,
    AUDIO_BACKEND_SIM_WAV,
    AUDIO_BACKEND_KINDLE_ALSA,
    AUDIO_BACKEND_KINDLE_GST
} AudioBackendType;

typedef struct AudioOut AudioOut;

struct AudioOut {
    AudioBackendType type;
    void *impl;

    int (*write_s16le)(AudioOut *ao, const int16_t *frames, int frame_count);
    int (*set_paused)(AudioOut *ao, bool paused);
    int (*flush)(AudioOut *ao);
    int64_t (*delay_ms)(AudioOut *ao);
    void (*close)(AudioOut *ao);
};

/* Host simulator / test backend: if wav_path is non-NULL, writes a valid 44.1 kHz
 * stereo S16LE WAV file; otherwise acts as a zero-overhead null sink that counts frames. */
int audio_out_open_sim(AudioOut **out, const char *wav_path);
int64_t audio_out_sim_frames_written(const AudioOut *ao);

/* Kindle backends (compiled into the Kindle build, safe stubs on host if needed). */
int audio_out_open_alsa(AudioOut **out, char *err_msg, size_t err_cap);
int audio_out_open_gst(AudioOut **out, const char *helper_dir, char *err_msg, size_t err_cap);

/* Auto-select Kindle audio backend per D6:
 * 1. If /proc/asound/cards lists a real sound card (Kindle Touch), open raw ALSA.
 * 2. Otherwise open the GStreamer mixersink helper (Bluetooth Kindles). */
int audio_out_open_kindle(AudioOut **out, const char *helper_dir, char *err_msg, size_t err_cap);

void audio_out_close(AudioOut *ao);

#endif /* ALOUD_AUDIO_OUT_H */
