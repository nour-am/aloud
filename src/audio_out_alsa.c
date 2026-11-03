#define _POSIX_C_SOURCE 200809L
#include "audio_out.h"
#include "decoder.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <sound/asound.h>

/* Raw kernel ALSA backend for the Kindle Touch (/dev/snd/pcmC0D0p) — no
 * libasound.so needed in the static musl binary (ARCHITECTURE.md D6). */

typedef struct {
    int fd;
    bool paused;
} AlsaAudioImpl;

static void param_init(struct snd_pcm_hw_params *p) {
    memset(p, 0, sizeof(*p));
    for (size_t i = 0; i < sizeof(p->masks) / sizeof(p->masks[0]); i++) {
        memset(&p->masks[i], 0xFF, sizeof(p->masks[i]));
    }
    for (size_t i = 0; i < sizeof(p->intervals) / sizeof(p->intervals[0]); i++) {
        p->intervals[i].min = 0;
        p->intervals[i].max = ~0U;
    }
    p->rmask = ~0U;
}

static void param_set_mask(struct snd_pcm_hw_params *p, int var, unsigned bit) {
    struct snd_mask *m = &p->masks[var - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(m, 0, sizeof(*m));
    m->bits[bit >> 5] |= (1U << (bit & 31U));
}

static void param_set_int(struct snd_pcm_hw_params *p, int var, unsigned val) {
    struct snd_interval *iv = &p->intervals[var - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    memset(iv, 0, sizeof(*iv));
    iv->min = val;
    iv->max = val;
    iv->integer = 1;
}

static int alsa_write_s16le(AudioOut *ao, const int16_t *frames, int frame_count) {
    if (!ao || !ao->impl || !frames || frame_count < 0) return -EINVAL;
    AlsaAudioImpl *im = (AlsaAudioImpl *)ao->impl;
    if (frame_count == 0) return 0;

    int written = 0;
    while (written < frame_count) {
        struct snd_xferi xfer;
        memset(&xfer, 0, sizeof(xfer));
        xfer.buf = (void *)(frames + (size_t)written * DECODER_OUT_CHANNELS);
        xfer.frames = (snd_pcm_uframes_t)(frame_count - written);
        if (ioctl(im->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer) < 0) {
            if (errno == EINTR) continue;
            if (errno == EPIPE || errno == ESTRPIPE) {
                /* Recover from underrun/suspend by re-preparing the stream. */
                (void)ioctl(im->fd, SNDRV_PCM_IOCTL_PREPARE);
                continue;
            }
            return -errno;
        }
        written += (int)xfer.result;
    }
    return written;
}

static int alsa_set_paused(AudioOut *ao, bool paused) {
    if (!ao || !ao->impl) return -EINVAL;
    AlsaAudioImpl *im = (AlsaAudioImpl *)ao->impl;
    if (im->paused == paused) return 0;
    im->paused = paused;
    int arg = paused ? 1 : 0;
    if (ioctl(im->fd, SNDRV_PCM_IOCTL_PAUSE, &arg) < 0) {
        if (paused) {
            (void)ioctl(im->fd, SNDRV_PCM_IOCTL_DROP);
        } else {
            (void)ioctl(im->fd, SNDRV_PCM_IOCTL_PREPARE);
        }
    }
    return 0;
}

static int alsa_flush(AudioOut *ao) {
    if (!ao || !ao->impl) return -EINVAL;
    AlsaAudioImpl *im = (AlsaAudioImpl *)ao->impl;
    (void)ioctl(im->fd, SNDRV_PCM_IOCTL_DROP);
    (void)ioctl(im->fd, SNDRV_PCM_IOCTL_PREPARE);
    return 0;
}

static int64_t alsa_delay_ms(AudioOut *ao) {
    if (!ao || !ao->impl) return 0;
    AlsaAudioImpl *im = (AlsaAudioImpl *)ao->impl;
    snd_pcm_sframes_t delay = 0;
    if (ioctl(im->fd, SNDRV_PCM_IOCTL_DELAY, &delay) == 0 && delay > 0) {
        return ((int64_t)delay * 1000) / DECODER_OUT_RATE;
    }
    return 50;
}

static void alsa_close(AudioOut *ao) {
    if (!ao) return;
    AlsaAudioImpl *im = (AlsaAudioImpl *)ao->impl;
    if (im) {
        if (im->fd >= 0) {
            (void)ioctl(im->fd, SNDRV_PCM_IOCTL_DROP);
            close(im->fd);
        }
        free(im);
    }
    free(ao);
}

static bool has_alsa_soundcard(void) {
    FILE *fp = fopen("/proc/asound/cards", "r");
    if (!fp) return false;
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0 || strstr(buf, "no soundcards") != NULL) {
        return false;
    }
    return access("/dev/snd/pcmC0D0p", W_OK) == 0;
}

int audio_out_open_alsa(AudioOut **out, char *err_msg, size_t err_cap) {
    if (!out) return -EINVAL;
    *out = NULL;
    if (err_msg && err_cap > 0) err_msg[0] = '\0';

    if (!has_alsa_soundcard()) {
        return -ENODEV;
    }

    int fd = open("/dev/snd/pcmC0D0p", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (err_msg && err_cap > 0) snprintf(err_msg, err_cap, "Cannot open ALSA PCM device");
        return -errno;
    }

    struct snd_pcm_hw_params hw;
    param_init(&hw);
    param_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    param_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    param_set_int(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, DECODER_OUT_CHANNELS);
    param_set_int(&hw, SNDRV_PCM_HW_PARAM_RATE, DECODER_OUT_RATE);

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0 ||
        ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, 0) < 0) {
        int err = errno;
        close(fd);
        if (err_msg && err_cap > 0) snprintf(err_msg, err_cap, "ALSA hardware parameter setup failed");
        return -err;
    }

    AudioOut *ao = calloc(1, sizeof(*ao));
    AlsaAudioImpl *im = calloc(1, sizeof(*im));
    if (!ao || !im) {
        free(ao);
        free(im);
        close(fd);
        return -ENOMEM;
    }
    im->fd = fd;
    ao->type = AUDIO_BACKEND_KINDLE_ALSA;
    ao->impl = im;
    ao->write_s16le = alsa_write_s16le;
    ao->set_paused = alsa_set_paused;
    ao->flush = alsa_flush;
    ao->delay_ms = alsa_delay_ms;
    ao->close = alsa_close;
    *out = ao;
    return 0;
}

#else /* !__linux__ */

int audio_out_open_alsa(AudioOut **out, char *err_msg, size_t err_cap) {
    if (out) *out = NULL;
    if (err_msg && err_cap > 0) snprintf(err_msg, err_cap, "ALSA only supported on Linux");
    return -ENODEV;
}

#endif /* __linux__ */

int audio_out_open_kindle(AudioOut **out, const char *helper_dir, char *err_msg, size_t err_cap) {
    int rc = audio_out_open_alsa(out, err_msg, err_cap);
    if (rc == 0) {
        return 0;
    }
    return audio_out_open_gst(out, helper_dir, err_msg, err_cap);
}
