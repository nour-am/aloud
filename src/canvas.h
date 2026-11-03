#ifndef ALOUD_CANVAS_H
#define ALOUD_CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Four UI tones per DESIGN.md principle 3. */
#define TONE_BLACK     0x00
#define TONE_INK_GRAY  0x55
#define TONE_RULE_GRAY 0xAA
#define TONE_WHITE     0xFF

typedef struct {
    int x;
    int y;
    int w;
    int h;
} AloudRect;

typedef struct {
    int width;
    int height;
    int dpi;
    uint8_t *pixels;
    AloudRect dirty;
} Canvas;

int canvas_init(Canvas *c, int width, int height, int dpi);
void canvas_free(Canvas *c);

/* Convert physical millimetres to pixels using the panel DPI. */
int canvas_mm_to_px(const Canvas *c, float mm);

void canvas_reset_dirty(Canvas *c);
void canvas_mark_dirty(Canvas *c, int x, int y, int w, int h);
AloudRect canvas_rect_union(AloudRect a, AloudRect b);
bool canvas_rect_contains(AloudRect r, int x, int y);

void canvas_clear(Canvas *c, uint8_t tone);
void canvas_fill_rect(Canvas *c, int x, int y, int w, int h, uint8_t tone);
void canvas_stroke_rect(Canvas *c, int x, int y, int w, int h, int thickness, uint8_t tone);
void canvas_hline(Canvas *c, int x, int y, int w, int thickness, uint8_t tone);
void canvas_vline(Canvas *c, int x, int y, int h, int thickness, uint8_t tone);
void canvas_fill_round_rect(Canvas *c, int x, int y, int w, int h, int radius, uint8_t tone);
void canvas_stroke_round_rect(Canvas *c, int x, int y, int w, int h, int radius, int thickness, uint8_t tone);
void canvas_fill_circle(Canvas *c, int cx, int cy, int radius, uint8_t tone);

/* Blend an 8-bit alpha coverage mask (0 = transparent, 255 = opaque) with tone. */
void canvas_blend_alpha(Canvas *c, int dst_x, int dst_y,
                        const uint8_t *alpha, int w, int h, int stride, uint8_t tone);

/* Blit an 8-bit grayscale buffer (such as a pre-dithered cover) onto the canvas. */
void canvas_blit_gray(Canvas *c, int dst_x, int dst_y,
                      const uint8_t *gray, int w, int h, int stride);

/* Decode a JPEG/PNG image from memory, center-crop to square, box-filter downscale
 * to target_size x target_size, and Floyd-Steinberg dither to the 16-level e-ink
 * palette (0x00, 0x11, ..., 0xFF). Caller frees *out_gray. */
int canvas_decode_and_dither_cover(const uint8_t *img_bytes, size_t img_len,
                                   int target_size, uint8_t **out_gray);

/* Scale an already-decoded 8-bit grayscale square image to target_size x target_size
 * and Floyd-Steinberg dither to the 16-level e-ink palette. */
void canvas_scale_and_dither_gray(const uint8_t *src, int src_w, int src_h,
                                  uint8_t *dst, int dst_w, int dst_h);

#endif /* ALOUD_CANVAS_H */
