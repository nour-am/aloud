#define _POSIX_C_SOURCE 200809L
#include "canvas.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#pragma clang diagnostic pop

int canvas_init(Canvas *c, int width, int height, int dpi) {
    if (!c || width <= 0 || height <= 0) {
        return -EINVAL;
    }
    memset(c, 0, sizeof(*c));
    c->width = width;
    c->height = height;
    c->dpi = (dpi > 0) ? dpi : 300;
    c->pixels = malloc((size_t)width * (size_t)height);
    if (!c->pixels) {
        return -ENOMEM;
    }
    memset(c->pixels, TONE_WHITE, (size_t)width * (size_t)height);
    c->dirty = (AloudRect){0, 0, width, height};
    return 0;
}

void canvas_free(Canvas *c) {
    if (!c) return;
    free(c->pixels);
    c->pixels = NULL;
    c->width = 0;
    c->height = 0;
}

int canvas_mm_to_px(const Canvas *c, float mm) {
    int dpi = (c && c->dpi > 0) ? c->dpi : 300;
    int px = (int)lroundf((mm * (float)dpi) / 25.4f);
    return (px < 1 && mm > 0.0f) ? 1 : px;
}

void canvas_reset_dirty(Canvas *c) {
    if (!c) return;
    c->dirty = (AloudRect){0, 0, 0, 0};
}

AloudRect canvas_rect_union(AloudRect a, AloudRect b) {
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    int x1 = (a.x < b.x) ? a.x : b.x;
    int y1 = (a.y < b.y) ? a.y : b.y;
    int x2 = ((a.x + a.w) > (b.x + b.w)) ? (a.x + a.w) : (b.x + b.w);
    int y2 = ((a.y + a.h) > (b.y + b.h)) ? (a.y + a.h) : (b.y + b.h);
    return (AloudRect){x1, y1, x2 - x1, y2 - y1};
}

bool canvas_rect_contains(AloudRect r, int x, int y) {
    return r.w > 0 && r.h > 0 &&
           x >= r.x && x < (r.x + r.w) &&
           y >= r.y && y < (r.y + r.h);
}

void canvas_mark_dirty(Canvas *c, int x, int y, int w, int h) {
    if (!c || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->width) w = c->width - x;
    if (y + h > c->height) h = c->height - y;
    if (w <= 0 || h <= 0) return;
    c->dirty = canvas_rect_union(c->dirty, (AloudRect){x, y, w, h});
}

void canvas_clear(Canvas *c, uint8_t tone) {
    if (!c || !c->pixels) return;
    memset(c->pixels, tone, (size_t)c->width * (size_t)c->height);
    c->dirty = (AloudRect){0, 0, c->width, c->height};
}

void canvas_fill_rect(Canvas *c, int x, int y, int w, int h, uint8_t tone) {
    if (!c || !c->pixels || w <= 0 || h <= 0) return;
    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > c->width) ? c->width : (x + w);
    int y1 = (y + h > c->height) ? c->height : (y + h);
    if (x0 >= x1 || y0 >= y1) return;

    int rw = x1 - x0;
    for (int py = y0; py < y1; py++) {
        memset(c->pixels + (size_t)py * (size_t)c->width + x0, tone, (size_t)rw);
    }
    canvas_mark_dirty(c, x0, y0, rw, y1 - y0);
}

void canvas_stroke_rect(Canvas *c, int x, int y, int w, int h, int thickness, uint8_t tone) {
    if (thickness <= 0 || w <= 0 || h <= 0) return;
    canvas_fill_rect(c, x, y, w, thickness, tone);
    canvas_fill_rect(c, x, y + h - thickness, w, thickness, tone);
    canvas_fill_rect(c, x, y, thickness, h, tone);
    canvas_fill_rect(c, x + w - thickness, y, thickness, h, tone);
}

void canvas_hline(Canvas *c, int x, int y, int w, int thickness, uint8_t tone) {
    canvas_fill_rect(c, x, y, w, thickness, tone);
}

void canvas_vline(Canvas *c, int x, int y, int h, int thickness, uint8_t tone) {
    canvas_fill_rect(c, x, y, thickness, h, tone);
}

/* Snap an anti-aliased edge value to the 16-level e-ink palette so curves look crisp. */
static inline uint8_t quantize_eink16(int v) {
    if (v <= 0) return 0x00;
    if (v >= 255) return 0xFF;
    int idx = (v + 8) / 17;
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    return (uint8_t)(idx * 17);
}

static void blend_pixel_aa(Canvas *c, int px, int py, uint8_t tone, float cov) {
    if (px < 0 || px >= c->width || py < 0 || py >= c->height || cov <= 0.0f) return;
    if (cov >= 1.0f) {
        c->pixels[(size_t)py * (size_t)c->width + px] = tone;
        return;
    }
    uint8_t bg = c->pixels[(size_t)py * (size_t)c->width + px];
    int blended = (int)lroundf((float)bg * (1.0f - cov) + (float)tone * cov);
    c->pixels[(size_t)py * (size_t)c->width + px] = quantize_eink16(blended);
}

static float sd_rounded_box(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = fabsf(px - cx) - (hw - r);
    float qy = fabsf(py - cy) - (hh - r);
    float mx = (qx > 0.0f) ? qx : 0.0f;
    float my = (qy > 0.0f) ? qy : 0.0f;
    float in_d = (qx > qy) ? qx : qy;
    if (in_d > 0.0f) in_d = 0.0f;
    return sqrtf(mx * mx + my * my) + in_d - r;
}

void canvas_fill_round_rect(Canvas *c, int x, int y, int w, int h, int radius, uint8_t tone) {
    if (!c || !c->pixels || w <= 0 || h <= 0) return;
    if (radius <= 0) {
        canvas_fill_rect(c, x, y, w, h, tone);
        return;
    }
    int max_r = (w < h ? w : h) / 2;
    if (radius > max_r) radius = max_r;

    float cx = (float)x + (float)w * 0.5f;
    float cy = (float)y + (float)h * 0.5f;
    float hw = (float)w * 0.5f;
    float hh = (float)h * 0.5f;
    float r = (float)radius;

    int x0 = (x < 0) ? 0 : x;
    int y0 = (y < 0) ? 0 : y;
    int x1 = (x + w > c->width) ? c->width : (x + w);
    int y1 = (y + h > c->height) ? c->height : (y + h);

    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float d = sd_rounded_box((float)px + 0.5f, (float)py + 0.5f, cx, cy, hw, hh, r);
            float cov = 0.5f - d;
            if (cov > 0.0f) {
                blend_pixel_aa(c, px, py, tone, cov > 1.0f ? 1.0f : cov);
            }
        }
    }
    canvas_mark_dirty(c, x0, y0, x1 - x0, y1 - y0);
}

void canvas_stroke_round_rect(Canvas *c, int x, int y, int w, int h, int radius, int thickness, uint8_t tone) {
    if (!c || !c->pixels || w <= 0 || h <= 0 || thickness <= 0) return;
    if (radius <= 0) {
        canvas_stroke_rect(c, x, y, w, h, thickness, tone);
        return;
    }
    int max_r = (w < h ? w : h) / 2;
    if (radius > max_r) radius = max_r;

    float cx = (float)x + (float)w * 0.5f;
    float cy = (float)y + (float)h * 0.5f;
    float hw = (float)w * 0.5f;
    float hh = (float)h * 0.5f;
    float r = (float)radius;
    float t = (float)thickness;

    int x0 = (x - 1 < 0) ? 0 : (x - 1);
    int y0 = (y - 1 < 0) ? 0 : (y - 1);
    int x1 = (x + w + 1 > c->width) ? c->width : (x + w + 1);
    int y1 = (y + h + 1 > c->height) ? c->height : (y + h + 1);

    for (int py = y0; py < y1; py++) {
        for (int px = x0; px < x1; px++) {
            float d_out = sd_rounded_box((float)px + 0.5f, (float)py + 0.5f, cx, cy, hw, hh, r);
            float d_in = d_out + t;
            float cov_out = 0.5f - d_out;
            float cov_in = 0.5f - d_in;
            if (cov_out < 0.0f) cov_out = 0.0f;
            if (cov_out > 1.0f) cov_out = 1.0f;
            if (cov_in < 0.0f) cov_in = 0.0f;
            if (cov_in > 1.0f) cov_in = 1.0f;
            float cov = cov_out - cov_in;
            if (cov > 0.0f) {
                blend_pixel_aa(c, px, py, tone, cov);
            }
        }
    }
    canvas_mark_dirty(c, x0, y0, x1 - x0, y1 - y0);
}

void canvas_fill_circle(Canvas *c, int cx, int cy, int radius, uint8_t tone) {
    if (!c || !c->pixels || radius <= 0) return;
    canvas_fill_round_rect(c, cx - radius, cy - radius, radius * 2, radius * 2, radius, tone);
}

void canvas_blend_alpha(Canvas *c, int dst_x, int dst_y,
                        const uint8_t *alpha, int w, int h, int stride, uint8_t tone) {
    if (!c || !c->pixels || !alpha || w <= 0 || h <= 0) return;
    int x0 = (dst_x < 0) ? 0 : dst_x;
    int y0 = (dst_y < 0) ? 0 : dst_y;
    int x1 = (dst_x + w > c->width) ? c->width : (dst_x + w);
    int y1 = (dst_y + h > c->height) ? c->height : (dst_y + h);
    if (x0 >= x1 || y0 >= y1) return;

    for (int py = y0; py < y1; py++) {
        const uint8_t *srow = alpha + (size_t)(py - dst_y) * (size_t)stride;
        uint8_t *drow = c->pixels + (size_t)py * (size_t)c->width;
        for (int px = x0; px < x1; px++) {
            uint8_t a = srow[px - dst_x];
            if (a == 0) continue;
            if (a == 255) {
                drow[px] = tone;
            } else {
                int bg = drow[px];
                int blended = (bg * (255 - a) + (int)tone * a + 127) / 255;
                drow[px] = quantize_eink16(blended);
            }
        }
    }
    canvas_mark_dirty(c, x0, y0, x1 - x0, y1 - y0);
}

void canvas_blit_gray(Canvas *c, int dst_x, int dst_y,
                      const uint8_t *gray, int w, int h, int stride) {
    if (!c || !c->pixels || !gray || w <= 0 || h <= 0) return;
    int x0 = (dst_x < 0) ? 0 : dst_x;
    int y0 = (dst_y < 0) ? 0 : dst_y;
    int x1 = (dst_x + w > c->width) ? c->width : (dst_x + w);
    int y1 = (dst_y + h > c->height) ? c->height : (dst_y + h);
    if (x0 >= x1 || y0 >= y1) return;

    int copy_w = x1 - x0;
    for (int py = y0; py < y1; py++) {
        memcpy(c->pixels + (size_t)py * (size_t)c->width + x0,
               gray + (size_t)(py - dst_y) * (size_t)stride + (x0 - dst_x),
               (size_t)copy_w);
    }
    canvas_mark_dirty(c, x0, y0, copy_w, y1 - y0);
}

void canvas_scale_and_dither_gray(const uint8_t *src, int src_w, int src_h,
                                  uint8_t *dst, int dst_w, int dst_h) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

    float *err = calloc((size_t)(dst_w + 2) * (size_t)(dst_h + 1), sizeof(float));
    /* First pass: box-filter downscale (or bilinear when upscaling). */
    for (int dy = 0; dy < dst_h; dy++) {
        float sy0 = ((float)dy * (float)src_h) / (float)dst_h;
        float sy1 = ((float)(dy + 1) * (float)src_h) / (float)dst_h;
        int iy0 = (int)floorf(sy0);
        int iy1 = (int)ceilf(sy1);
        if (iy0 < 0) iy0 = 0;
        if (iy1 > src_h) iy1 = src_h;
        if (iy1 <= iy0) iy1 = iy0 + 1;

        for (int dx = 0; dx < dst_w; dx++) {
            float sx0 = ((float)dx * (float)src_w) / (float)dst_w;
            float sx1 = ((float)(dx + 1) * (float)src_w) / (float)dst_w;
            int ix0 = (int)floorf(sx0);
            int ix1 = (int)ceilf(sx1);
            if (ix0 < 0) ix0 = 0;
            if (ix1 > src_w) ix1 = src_w;
            if (ix1 <= ix0) ix1 = ix0 + 1;

            int sum = 0;
            int count = 0;
            for (int sy = iy0; sy < iy1 && sy < src_h; sy++) {
                for (int sx = ix0; sx < ix1 && sx < src_w; sx++) {
                    sum += src[(size_t)sy * (size_t)src_w + sx];
                    count++;
                }
            }
            uint8_t val = (count > 0) ? (uint8_t)((sum + count / 2) / count) : 0xFF;
            dst[(size_t)dy * (size_t)dst_w + dx] = val;
        }
    }

    if (!err) {
        /* Fallback to direct quantization if allocation failed. */
        for (int i = 0; i < dst_w * dst_h; i++) {
            dst[i] = quantize_eink16(dst[i]);
        }
        return;
    }

    /* Second pass: Floyd-Steinberg error diffusion to the 16-level e-ink palette
     * (0x00, 0x11, ..., 0xFF). Hardware dithering is broken on PW5/KT (RESEARCH.md §5). */
    int stride = dst_w + 2;
    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            float old_px = (float)dst[(size_t)y * (size_t)dst_w + x] + err[(size_t)y * (size_t)stride + (x + 1)];
            if (old_px < 0.0f) old_px = 0.0f;
            if (old_px > 255.0f) old_px = 255.0f;
            uint8_t new_px = quantize_eink16((int)lroundf(old_px));
            dst[(size_t)y * (size_t)dst_w + x] = new_px;
            float q_err = old_px - (float)new_px;

            err[(size_t)y * (size_t)stride + (x + 2)]       += q_err * (7.0f / 16.0f);
            err[(size_t)(y + 1) * (size_t)stride + x]       += q_err * (3.0f / 16.0f);
            err[(size_t)(y + 1) * (size_t)stride + (x + 1)] += q_err * (5.0f / 16.0f);
            err[(size_t)(y + 1) * (size_t)stride + (x + 2)] += q_err * (1.0f / 16.0f);
        }
    }
    free(err);
}

int canvas_decode_and_dither_cover(const uint8_t *img_bytes, size_t img_len,
                                   int target_size, uint8_t **out_gray) {
    if (!img_bytes || img_len == 0 || target_size <= 0 || !out_gray) {
        return -EINVAL;
    }
    *out_gray = NULL;

    int w = 0, h = 0, comp = 0;
    stbi_uc *rgba = stbi_load_from_memory(img_bytes, (int)img_len, &w, &h, &comp, 3);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        return -EINVAL;
    }

    /* Center-crop to square and convert to Rec. 709 luma. */
    int side = (w < h) ? w : h;
    int ox = (w - side) / 2;
    int oy = (h - side) / 2;

    uint8_t *crop = malloc((size_t)side * (size_t)side);
    if (!crop) {
        stbi_image_free(rgba);
        return -ENOMEM;
    }

    for (int y = 0; y < side; y++) {
        for (int x = 0; x < side; x++) {
            const stbi_uc *p = rgba + ((size_t)(oy + y) * (size_t)w + (ox + x)) * 3;
            int luma = (p[0] * 54 + p[1] * 183 + p[2] * 19 + 128) >> 8;
            crop[(size_t)y * (size_t)side + x] = (uint8_t)luma;
        }
    }
    stbi_image_free(rgba);

    uint8_t *dithered = malloc((size_t)target_size * (size_t)target_size);
    if (!dithered) {
        free(crop);
        return -ENOMEM;
    }

    canvas_scale_and_dither_gray(crop, side, side, dithered, target_size, target_size);
    free(crop);

    *out_gray = dithered;
    return 0;
}
