#define _POSIX_C_SOURCE 200809L
#include "platform.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#pragma clang diagnostic pop

void touch_decoder_init(TouchDecoder *td, int min_x, int max_x, int min_y, int max_y,
                        int view_w, int view_h, int dpi, uint8_t canonical_rota,
                        bool swap_axes, bool mirror_x, bool mirror_y, bool is_oasis) {
    if (!td) return;
    memset(td, 0, sizeof(*td));
    td->min_x = min_x;
    td->max_x = (max_x > min_x) ? max_x : (min_x + ((view_w > 0) ? view_w - 1 : 1000));
    td->min_y = min_y;
    td->max_y = (max_y > min_y) ? max_y : (min_y + ((view_h > 0) ? view_h - 1 : 1000));
    td->view_w = (view_w > 0) ? view_w : 1236;
    td->view_h = (view_h > 0) ? view_h : 1648;
    td->dpi = (dpi > 0) ? dpi : 300;
    td->canonical_rota = canonical_rota & 3U;
    td->touch_swap_axes = swap_axes;
    td->touch_mirror_x = mirror_x;
    td->touch_mirror_y = mirror_y;
    td->is_oasis = is_oasis;

    /* Per D11 / KUAL Next: if raw touch axis aspect ratio disagrees with the
     * upright panel aspect ratio and swap_axes wasn't set yet, swap axes. */
    int raw_w = td->max_x - td->min_x;
    int raw_h = td->max_y - td->min_y;
    bool upright_landscape = (td->canonical_rota & 1U) ? (td->view_h > td->view_w)
                                                       : (td->view_w > td->view_h);
    bool raw_landscape = (raw_w > raw_h);
    if (raw_w != raw_h && td->view_w != td->view_h && raw_landscape != upright_landscape) {
        td->touch_swap_axes = !td->touch_swap_axes;
    }
}

void touch_map_coords(const TouchDecoder *td, int raw_x, int raw_y,
                      int *out_x, int *out_y) {
    if (!td) {
        if (out_x) *out_x = 0;
        if (out_y) *out_y = 0;
        return;
    }

    float span_x = (float)(td->max_x - td->min_x);
    float span_y = (float)(td->max_y - td->min_y);
    float u = (span_x > 0.0f) ? ((float)(raw_x - td->min_x) / span_x) : 0.0f;
    float v = (span_y > 0.0f) ? ((float)(raw_y - td->min_y) / span_y) : 0.0f;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;

    if (td->touch_swap_axes) {
        float tmp = u;
        u = v;
        v = tmp;
    }
    if (td->touch_mirror_x) u = 1.0f - u;
    if (td->touch_mirror_y) v = 1.0f - v;

    float ru = u;
    float rv = v;
    switch (td->canonical_rota & 3U) {
        case 0: /* UR */
            ru = u;
            rv = v;
            break;
        case 1: /* CW (90 deg) */
            ru = 1.0f - v;
            rv = u;
            break;
        case 2: /* UD (180 deg) */
            ru = 1.0f - u;
            rv = 1.0f - v;
            break;
        case 3: /* CCW (270 deg) */
            ru = v;
            rv = 1.0f - u;
            break;
    }

    int sx = (int)lroundf(ru * (float)(td->view_w - 1));
    int sy = (int)lroundf(rv * (float)(td->view_h - 1));
    if (sx < 0) sx = 0;
    if (sx >= td->view_w) sx = td->view_w - 1;
    if (sy < 0) sy = 0;
    if (sy >= td->view_h) sy = td->view_h - 1;

    if (out_x) *out_x = sx;
    if (out_y) *out_y = sy;
}

bool touch_decoder_feed(TouchDecoder *td, uint16_t type, uint16_t code, int32_t value,
                        AloudInputEvent *out_ev) {
    if (!td || !out_ev) return false;
    memset(out_ev, 0, sizeof(*out_ev));

    if (type == ALOUD_EV_KEY) {
        if (code == ALOUD_BTN_TOUCH) {
            if (value == 1 && !td->contact_down) {
                td->pending_down = true;
            } else if (value == 0 && (td->contact_down || td->pending_down)) {
                /* KT4 never sends ABS_MT_TRACKING_ID -1 on lift; BTN_TOUCH 0 is the
                 * only lift signal on that model (RESEARCH.md §5, ARCHITECTURE.md D11). */
                td->pending_lift = true;
            }
            return false;
        }
        if (value == 1) {
            if (code == ALOUD_KEY_PAGEUP) {
                /* Oasis 1/2/3 reverses 104/109 compared to Voyage (RESEARCH.md §5). */
                out_ev->type = td->is_oasis ? ALOUD_EV_KEY_NEXT : ALOUD_EV_KEY_PREV;
                return true;
            }
            if (code == ALOUD_KEY_PAGEDOWN) {
                out_ev->type = td->is_oasis ? ALOUD_EV_KEY_PREV : ALOUD_EV_KEY_NEXT;
                return true;
            }
            if (code == ALOUD_KEY_HOME) {
                out_ev->type = ALOUD_EV_QUIT;
                return true;
            }
        }
        return false;
    }

    if (type == ALOUD_EV_ABS) {
        if (code == ALOUD_ABS_MT_SLOT) {
            td->cur_slot = value;
            return false;
        }
        if (td->cur_slot != 0) {
            return false;
        }
        switch (code) {
            case ALOUD_ABS_MT_TRACKING_ID:
                if (value >= 0) {
                    if (!td->contact_down) td->pending_down = true;
                } else {
                    if (td->contact_down || td->pending_down) td->pending_lift = true;
                }
                break;
            case ALOUD_ABS_MT_POSITION_X:
            case ALOUD_ABS_X:
                td->raw_x = value;
                td->have_x = true;
                break;
            case ALOUD_ABS_MT_POSITION_Y:
            case ALOUD_ABS_Y:
                td->raw_y = value;
                td->have_y = true;
                break;
            default:
                break;
        }
        return false;
    }

    if (type == ALOUD_EV_SYN) {
        if (code == ALOUD_SYN_MT_REPORT) {
            /* Protocol A packet boundary. */
            if (td->have_x || td->have_y) {
                td->mt_proto_a_seen_pos = true;
                if (!td->contact_down) td->pending_down = true;
            } else if (td->contact_down && !td->mt_proto_a_seen_pos) {
                /* Empty SYN_MT_REPORT packet signals lift in Protocol A. */
                td->pending_lift = true;
            }
            return false;
        }
        if (code == ALOUD_SYN_REPORT) {
            if (td->have_x || td->have_y || td->pending_down) {
                int sx = 0, sy = 0;
                touch_map_coords(td, td->raw_x, td->raw_y, &sx, &sy);
                if (!td->contact_down && td->pending_down) {
                    td->contact_down = true;
                    td->pending_down = false;
                    td->start_screen_x = sx;
                    td->start_screen_y = sy;
                }
                if (td->contact_down) {
                    td->last_screen_x = sx;
                    td->last_screen_y = sy;
                }
                td->have_x = false;
                td->have_y = false;
            }
            td->mt_proto_a_seen_pos = false;

            if (td->pending_lift) {
                bool was_down = td->contact_down;
                td->contact_down = false;
                td->pending_lift = false;
                td->pending_down = false;
                if (was_down) {
                    int dx = td->last_screen_x - td->start_screen_x;
                    int dy = td->last_screen_y - td->start_screen_y;
                    int swipe_px = (12 * td->dpi) / 25; /* ~12 mm */
                    if (swipe_px < 40) swipe_px = 40;
                    if (abs(dx) >= swipe_px && abs(dx) > (abs(dy) * 3) / 2) {
                        out_ev->type = (dx < 0) ? ALOUD_EV_SWIPE_LEFT : ALOUD_EV_SWIPE_RIGHT;
                    } else {
                        out_ev->type = ALOUD_EV_TAP;
                        out_ev->x = td->last_screen_x;
                        out_ev->y = td->last_screen_y;
                    }
                    return true;
                }
            }
        }
    }

    return false;
}

int platform_write_png(const Canvas *c, const char *png_path) {
    if (!c || !c->pixels || !png_path) return -EINVAL;
    int ok = stbi_write_png(png_path, c->width, c->height, 1, c->pixels, c->width);
    return ok ? 0 : -EIO;
}

#ifndef ALOUD_KINDLE_BUILD

struct Platform {
    int width;
    int height;
    int dpi;
    int refresh_count;
    AloudWaveform last_wfm;
    AloudRect last_rect;
};

int platform_init(Platform **out, int sim_w, int sim_h, int sim_dpi) {
    if (!out) return -EINVAL;
    Platform *p = calloc(1, sizeof(*p));
    if (!p) return -ENOMEM;
    p->width = (sim_w > 0) ? sim_w : 1236;
    p->height = (sim_h > 0) ? sim_h : 1648;
    p->dpi = (sim_dpi > 0) ? sim_dpi : 300;
    *out = p;
    return 0;
}

void platform_free(Platform *plat) {
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
    (void)plat;
    return true;
}

int platform_refresh(Platform *plat, const Canvas *c, AloudRect r, AloudWaveform wfm) {
    if (!plat || !c) return -EINVAL;
    plat->refresh_count++;
    plat->last_wfm = wfm;
    plat->last_rect = r;
    return 0;
}

int platform_get_input_fds(Platform *plat, int *fds_out, int max_fds) {
    (void)plat;
    (void)fds_out;
    (void)max_fds;
    return 0;
}

bool platform_poll_event(Platform *plat, AloudInputEvent *out_ev) {
    (void)plat;
    if (out_ev) memset(out_ev, 0, sizeof(*out_ev));
    return false;
}

#endif /* !ALOUD_KINDLE_BUILD */
