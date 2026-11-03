#ifndef ALOUD_TEXT_H
#define ALOUD_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "canvas.h"

typedef enum {
    FONT_SERIF_REGULAR = 0,
    FONT_SERIF_BOLD    = 1,
    FONT_SANS_REGULAR  = 2,
    FONT_SANS_BOLD     = 3,
    FONT_COUNT         = 4
} FontId;

#define TEXT_FLAG_NONE    0
#define TEXT_FLAG_TABULAR (1 << 0)
#define TEXT_ALIGN_LEFT   (0 << 1)
#define TEXT_ALIGN_CENTER (1 << 1)
#define TEXT_ALIGN_RIGHT  (2 << 1)
#define TEXT_ALIGN_MASK   (3 << 1)

typedef struct TextSystem TextSystem;

/* Load the 4 bundled OFL static TTF fonts from font_dir:
 * Literata-Regular.ttf, Literata-SemiBold.ttf, Inter-Regular.ttf, Inter-SemiBold.ttf.
 * Validates each TTF header before passing to stb_truetype (RESEARCH.md §5). */
int text_init(TextSystem **out, const char *font_dir);
void text_free(TextSystem *ts);

/* Validate raw font bytes before passing to stb_truetype. Exposed for unit tests. */
bool text_validate_ttf_bytes(const uint8_t *data, size_t len);

int text_line_height(TextSystem *ts, FontId font, int size_px);
int text_ascent(TextSystem *ts, FontId font, int size_px);

int text_measure_width(TextSystem *ts, FontId font, int size_px,
                       const char *utf8, unsigned flags);

/* Draw a single line of UTF-8 text with top of line at y.
 * If max_w > 0 and the string is wider than max_w, truncates cleanly with "…".
 * IfTEXT_ALIGN_CENTER or TEXT_ALIGN_RIGHT is set in flags, aligns within [x, x + max_w]. */
int text_draw_line(TextSystem *ts, Canvas *c, FontId font, int size_px,
                   int x, int y, int max_w, const char *utf8,
                   uint8_t tone, unsigned flags);

/* Word-wrap UTF-8 text into at most max_lines lines of width max_w, truncating the
 * last line with "…" if needed. Returns the total vertical height in pixels. */
int text_draw_wrapped(TextSystem *ts, Canvas *c, FontId font, int size_px,
                      int x, int y, int max_w, int max_lines, int line_gap_px,
                      const char *utf8, uint8_t tone, unsigned flags);

#endif /* ALOUD_TEXT_H */
