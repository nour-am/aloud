#define _GNU_SOURCE
#include "platform.h"

#ifdef ALOUD_KINDLE_BUILD

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "fbink.h"

#define MAX_INPUT_DEVS 8

typedef struct {
    int fd;
    TouchDecoder td;
} KindleInputDev;

struct Platform {
    int fbfd;
    FBInkConfig fb_cfg;
    FBInkState fb_state;
    int width;
    int height;
    int dpi;
    bool has_volume;
    bool mtk_fast_mode_set;

    KindleInputDev devs[MAX_INPUT_DEVS];
    int dev_count;
    uint8_t *sub_buf;
    size_t sub_buf_cap;
};

static void open_single_input_dev(Platform *p, int fd, bool is_oasis) {
    if (fd < 0 || p->dev_count >= MAX_INPUT_DEVS) {
        if (fd >= 0) close(fd);
        return;
    }

    /* Grab input device exclusively so paused stock UI doesn't buffer touches (D9, D11). */
    (void)ioctl(fd, EVIOCGRAB, 1);

    int min_x = 0, max_x = p->width - 1;
    int min_y = 0, max_y = p->height - 1;
    struct input_absinfo abs_x, abs_y;
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0 && abs_x.maximum > abs_x.minimum) {
        min_x = abs_x.minimum;
        max_x = abs_x.maximum;
    } else if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0 && abs_x.maximum > abs_x.minimum) {
        min_x = abs_x.minimum;
        max_x = abs_x.maximum;
    }
    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0 && abs_y.maximum > abs_y.minimum) {
        min_y = abs_y.minimum;
        max_y = abs_y.maximum;
    } else if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0 && abs_y.maximum > abs_y.minimum) {
        min_y = abs_y.minimum;
        max_y = abs_y.maximum;
    }

    uint8_t rota = p->fb_state.current_rota & 3U;
    uint8_t canonical_rota = p->fb_state.rotation_map[rota] & 3U;

    KindleInputDev *kd = &p->devs[p->dev_count++];
    kd->fd = fd;
    touch_decoder_init(&kd->td, min_x, max_x, min_y, max_y,
                       p->width, p->height, p->dpi, canonical_rota,
                       p->fb_state.touch_swap_axes,
                       p->fb_state.touch_mirror_x,
                       p->fb_state.touch_mirror_y,
                       is_oasis);
}

static void scan_kindle_inputs(Platform *p) {
    bool is_oasis = (strstr(p->fb_state.device_name, "Oasis") != NULL);

    size_t count = 0;
    FBInkInputDevice *list = fbink_input_scan(
        INPUT_TOUCHSCREEN | INPUT_PAGINATION_BUTTONS | INPUT_HOME_BUTTON,
        INPUT_POWER_BUTTON | INPUT_SLEEP_COVER,
        NO_RECAP,
        &count);

    if (list) {
        for (size_t i = 0; i < count; i++) {
            if (list[i].matched && list[i].fd >= 0) {
                open_single_input_dev(p, list[i].fd, is_oasis);
            } else if (list[i].fd >= 0) {
                close(list[i].fd);
            }
        }
        free(list);
    }

    if (p->dev_count == 0) {
        /* Fallback scan of /dev/input/event0..7 (RESEARCH.md §5). */
        for (int i = 0; i < 8 && p->dev_count < MAX_INPUT_DEVS; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd >= 0) {
                open_single_input_dev(p, fd, is_oasis);
            }
        }
    }
}

int platform_init(Platform **out, int sim_w, int sim_h, int sim_dpi) {
    (void)sim_w;
    (void)sim_h;
    (void)sim_dpi;
    if (!out) return -EINVAL;
    *out = NULL;

    Platform *p = calloc(1, sizeof(*p));
    if (!p) return -ENOMEM;

    p->fbfd = fbink_open();
    if (p->fbfd < 0) {
        free(p);
        return -ENODEV;
    }

    memset(&p->fb_cfg, 0, sizeof(p->fb_cfg));
    p->fb_cfg.is_quiet = true;
    p->fb_cfg.ignore_alpha = true;
    p->fb_cfg.dithering_mode = HWD_PASSTHROUGH;

    if (fbink_init(p->fbfd, &p->fb_cfg) < 0) {
        fbink_close(p->fbfd);
        free(p);
        return -EIO;
    }

    fbink_get_state(&p->fb_cfg, &p->fb_state);
    p->width = (p->fb_state.view_width > 0) ? (int)p->fb_state.view_width : 1236;
    p->height = (p->fb_state.view_height > 0) ? (int)p->fb_state.view_height : 1648;
    p->dpi = (p->fb_state.screen_dpi > 0) ? (int)p->fb_state.screen_dpi : 300;
    p->has_volume = true; /* Software volume is always controllable in Aloud (D4/D6). */

    /* On MediaTek Kindles, disable auto-REAGL promotion so partial updates stay fast
     * and non-flashing (RESEARCH.md §3 & §5, ARCHITECTURE.md D5). */
    if (p->fb_state.is_mtk) {
        if (fbink_mtk_toggle_auto_reagl(p->fbfd, false) == 0) {
            p->mtk_fast_mode_set = true;
        }
    }

    scan_kindle_inputs(p);
    *out = p;
    return 0;
}

void platform_free(Platform *plat) {
    if (!plat) return;
    for (int i = 0; i < plat->dev_count; i++) {
        if (plat->devs[i].fd >= 0) {
            (void)ioctl(plat->devs[i].fd, EVIOCGRAB, 0);
            close(plat->devs[i].fd);
        }
    }
    if (plat->mtk_fast_mode_set) {
        (void)fbink_mtk_toggle_auto_reagl(plat->fbfd, true);
    }
    if (plat->fbfd >= 0) {
        fbink_close(plat->fbfd);
    }
    free(plat->sub_buf);
    free(plat);
}

int platform_width(const Platform *plat) {
    return plat ? plat->width : 1236;
}

int platform_height(const Platform *plat) {
    return plat ? plat->height : 1648;
}

int platform_dpi(const Platform *plat) {
    return plat ? plat->dpi : 300;
}

bool platform_has_volume_control(const Platform *plat) {
    return plat ? plat->has_volume : true;
}

int platform_refresh(Platform *plat, const Canvas *c, AloudRect r, AloudWaveform wfm) {
    if (!plat || !c || !c->pixels || wfm == ALOUD_WFM_NONE) return 0;

    int x0 = (r.x < 0) ? 0 : r.x;
    int y0 = (r.y < 0) ? 0 : r.y;
    int x1 = (r.x + r.w > c->width) ? c->width : (r.x + r.w);
    int y1 = (r.y + r.h > c->height) ? c->height : (r.y + r.h);
    if (x0 >= x1 || y0 >= y1) return 0;

    int rw = x1 - x0;
    int rh = y1 - y0;

    FBInkConfig cfg = plat->fb_cfg;
    cfg.no_refresh = true;
    cfg.ignore_alpha = true;
    cfg.dithering_mode = HWD_PASSTHROUGH;

    /* Copy the dirty rectangle into a packed buffer (or pass c->pixels directly
     * when updating full width). */
    const uint8_t *raw_ptr = NULL;
    size_t raw_len = (size_t)rw * (size_t)rh;
    if (x0 == 0 && rw == c->width) {
        raw_ptr = c->pixels + (size_t)y0 * (size_t)c->width;
    } else {
        if (plat->sub_buf_cap < raw_len) {
            uint8_t *nb = realloc(plat->sub_buf, raw_len);
            if (!nb) return -ENOMEM;
            plat->sub_buf = nb;
            plat->sub_buf_cap = raw_len;
        }
        for (int row = 0; row < rh; row++) {
            memcpy(plat->sub_buf + (size_t)row * (size_t)rw,
                   c->pixels + (size_t)(y0 + row) * (size_t)c->width + x0,
                   (size_t)rw);
        }
        raw_ptr = plat->sub_buf;
    }

    (void)fbink_print_raw_data(plat->fbfd, raw_ptr, rw, rh, raw_len,
                               (short)x0, (short)y0, &cfg);

    cfg.no_refresh = false;
    switch (wfm) {
        case ALOUD_WFM_DU:
            cfg.wfm_mode = WFM_DU;
            cfg.is_flashing = false;
            break;
        case ALOUD_WFM_GL16:
            cfg.wfm_mode = WFM_GL16;
            cfg.is_flashing = false;
            break;
        case ALOUD_WFM_GC16_FLASH:
        default:
            cfg.wfm_mode = WFM_GC16;
            cfg.is_flashing = true;
            break;
    }

    (void)fbink_refresh(plat->fbfd, (uint32_t)y0, (uint32_t)x0,
                        (uint32_t)rw, (uint32_t)rh, &cfg);
    return 0;
}

int platform_get_input_fds(Platform *plat, int *fds_out, int max_fds) {
    if (!plat || !fds_out || max_fds <= 0) return 0;
    int n = (plat->dev_count < max_fds) ? plat->dev_count : max_fds;
    for (int i = 0; i < n; i++) {
        fds_out[i] = plat->devs[i].fd;
    }
    return n;
}

bool platform_poll_event(Platform *plat, AloudInputEvent *out_ev) {
    if (!plat || !out_ev) return false;
    for (int i = 0; i < plat->dev_count; i++) {
        KindleInputDev *kd = &plat->devs[i];
        struct input_event ie;
        while (read(kd->fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie)) {
            if (touch_decoder_feed(&kd->td, ie.type, ie.code, ie.value, out_ev)) {
                return true;
            }
        }
    }
    return false;
}

#endif /* ALOUD_KINDLE_BUILD */
