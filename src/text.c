#define _POSIX_C_SOURCE 200809L
#include "text.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma clang diagnostic pop

#define GLYPH_CACHE_CAP 384

typedef struct {
    bool valid;
    FontId font;
    int size_px;
    uint32_t codepoint;
    int glyph_index;
    int advance_px;
    int xoff;
    int yoff;
    int w;
    int h;
    uint8_t *bitmap;
    uint64_t last_used;
} CachedGlyph;

typedef struct {
    uint8_t *ttf_data;
    size_t ttf_len;
    stbtt_fontinfo info;
    bool loaded;
} LoadedFont;

struct TextSystem {
    LoadedFont fonts[FONT_COUNT];
    CachedGlyph cache[GLYPH_CACHE_CAP];
    uint64_t use_clock;
};

bool text_validate_ttf_bytes(const uint8_t *data, size_t len) {
    if (!data || len < 64) {
        return false;
    }
    /* Check SFNT version tag: 0x00010000 (TrueType), 'true', or 'OTTO' (CFF). */
    uint32_t tag = ((uint32_t)data[0] << 24) |
                   ((uint32_t)data[1] << 16) |
                   ((uint32_t)data[2] << 8)  |
                   (uint32_t)data[3];
    if (tag != 0x00010000U && tag != 0x74727565U && tag != 0x4F54544FU) {
        return false;
    }
    uint16_t num_tables = (uint16_t)(((uint16_t)data[4] << 8) | data[5]);
    if (num_tables == 0 || num_tables > 128) {
        return false;
    }
    size_t dir_end = 12U + (size_t)num_tables * 16U;
    if (dir_end > len) {
        return false;
    }
    /* Verify every table offset + length stays within bounds so stb_truetype
     * can never read out of bounds on a truncated or malformed font file. */
    for (uint16_t i = 0; i < num_tables; i++) {
        const uint8_t *rec = data + 12U + (size_t)i * 16U;
        uint32_t offset = ((uint32_t)rec[8] << 24) |
                          ((uint32_t)rec[9] << 16) |
                          ((uint32_t)rec[10] << 8) |
                          (uint32_t)rec[11];
        uint32_t length = ((uint32_t)rec[12] << 24) |
                          ((uint32_t)rec[13] << 16) |
                          ((uint32_t)rec[14] << 8) |
                          (uint32_t)rec[15];
        if ((uint64_t)offset + (uint64_t)length > (uint64_t)len) {
            return false;
        }
    }
    int off = stbtt_GetFontOffsetForIndex(data, 0);
    if (off < 0 || (size_t)off >= len) {
        return false;
    }
    stbtt_fontinfo tmp;
    if (!stbtt_InitFont(&tmp, data, off)) {
        return false;
    }
    return true;
}

static int load_one_font(LoadedFont *lf, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -ENOENT;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -EIO;
    }
    long sz = ftell(fp);
    if (sz < 64 || sz > 8 * 1024 * 1024) {
        fclose(fp);
        return -EINVAL;
    }
    rewind(fp);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(fp);
        return -ENOMEM;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return -EIO;
    }
    fclose(fp);

    if (!text_validate_ttf_bytes(buf, (size_t)sz)) {
        free(buf);
        return -EINVAL;
    }
    int off = stbtt_GetFontOffsetForIndex(buf, 0);
    if (!stbtt_InitFont(&lf->info, buf, off)) {
        free(buf);
        return -EINVAL;
    }
    lf->ttf_data = buf;
    lf->ttf_len = (size_t)sz;
    lf->loaded = true;
    return 0;
}

int text_init(TextSystem **out, const char *font_dir) {
    if (!out || !font_dir) return -EINVAL;
    *out = NULL;

    TextSystem *ts = calloc(1, sizeof(*ts));
    if (!ts) return -ENOMEM;

    static const char *const kFiles[FONT_COUNT] = {
        "Literata-Regular.ttf",
        "Literata-SemiBold.ttf",
        "Inter-Regular.ttf",
        "Inter-SemiBold.ttf"
    };

    for (int i = 0; i < FONT_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", font_dir, kFiles[i]);
        int rc = load_one_font(&ts->fonts[i], path);
        if (rc < 0) {
            text_free(ts);
            return rc;
        }
    }

    *out = ts;
    return 0;
}

void text_free(TextSystem *ts) {
    if (!ts) return;
    for (int i = 0; i < GLYPH_CACHE_CAP; i++) {
        if (ts->cache[i].bitmap) {
            stbtt_FreeBitmap(ts->cache[i].bitmap, NULL);
        }
    }
    for (int i = 0; i < FONT_COUNT; i++) {
        free(ts->fonts[i].ttf_data);
    }
    free(ts);
}

/* Decode one UTF-8 codepoint and advance *p. Invalid sequences return U+FFFD. */
static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)(*p);
    if (!*s) return 0;
    if (s[0] < 0x80) {
        *p += 1;
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
        *p += 2;
        return (cp >= 0x80) ? cp : 0xFFFDU;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(s[0] & 0x0F) << 12) |
                      ((uint32_t)(s[1] & 0x3F) << 6) |
                      (uint32_t)(s[2] & 0x3F);
        *p += 3;
        return (cp >= 0x800) ? cp : 0xFFFDU;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        uint32_t cp = ((uint32_t)(s[0] & 0x07) << 18) |
                      ((uint32_t)(s[1] & 0x3F) << 12) |
                      ((uint32_t)(s[2] & 0x3F) << 6) |
                      (uint32_t)(s[3] & 0x3F);
        *p += 4;
        return (cp >= 0x10000 && cp <= 0x10FFFF) ? cp : 0xFFFDU;
    }
    *p += 1;
    return 0xFFFDU;
}

static const LoadedFont *get_font(const TextSystem *ts, FontId font) {
    if (!ts || font < 0 || font >= FONT_COUNT) return NULL;
    return ts->fonts[font].loaded ? &ts->fonts[font] : NULL;
}

int text_ascent(TextSystem *ts, FontId font, int size_px) {
    const LoadedFont *lf = get_font(ts, font);
    if (!lf || size_px <= 0) return size_px;
    float scale = stbtt_ScaleForPixelHeight(&lf->info, (float)size_px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&lf->info, &asc, &desc, &gap);
    return (int)lroundf((float)asc * scale);
}

int text_line_height(TextSystem *ts, FontId font, int size_px) {
    const LoadedFont *lf = get_font(ts, font);
    if (!lf || size_px <= 0) return size_px;
    float scale = stbtt_ScaleForPixelHeight(&lf->info, (float)size_px);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&lf->info, &asc, &desc, &gap);
    int h = (int)lroundf((float)(asc - desc + gap) * scale);
    return (h > size_px) ? h : size_px;
}

static CachedGlyph *get_cached_glyph(TextSystem *ts, FontId font, int size_px, uint32_t cp) {
    const LoadedFont *lf = get_font(ts, font);
    if (!lf || size_px <= 0) return NULL;

    for (int i = 0; i < GLYPH_CACHE_CAP; i++) {
        CachedGlyph *g = &ts->cache[i];
        if (g->valid && g->font == font && g->size_px == size_px && g->codepoint == cp) {
            g->last_used = ++ts->use_clock;
            return g;
        }
    }

    int slot = 0;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < GLYPH_CACHE_CAP; i++) {
        if (!ts->cache[i].valid) {
            slot = i;
            break;
        }
        if (ts->cache[i].last_used < oldest) {
            oldest = ts->cache[i].last_used;
            slot = i;
        }
    }

    CachedGlyph *g = &ts->cache[slot];
    if (g->bitmap) {
        stbtt_FreeBitmap(g->bitmap, NULL);
        g->bitmap = NULL;
    }

    float scale = stbtt_ScaleForPixelHeight(&lf->info, (float)size_px);
    int gidx = stbtt_FindGlyphIndex(&lf->info, (int)cp);
    if (gidx == 0 && cp != ' ') {
        gidx = stbtt_FindGlyphIndex(&lf->info, '?');
    }

    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&lf->info, gidx, &adv, &lsb);

    int w = 0, h = 0, xoff = 0, yoff = 0;
    uint8_t *bmp = NULL;
    if (cp != ' ' && cp != '\t') {
        bmp = stbtt_GetGlyphBitmap(&lf->info, scale, scale, gidx, &w, &h, &xoff, &yoff);
    }

    g->valid = true;
    g->font = font;
    g->size_px = size_px;
    g->codepoint = cp;
    g->glyph_index = gidx;
    g->advance_px = (int)lroundf((float)adv * scale);
    g->xoff = xoff;
    g->yoff = yoff;
    g->w = w;
    g->h = h;
    g->bitmap = bmp;
    g->last_used = ++ts->use_clock;
    return g;
}

static int get_tabular_digit_advance(TextSystem *ts, FontId font, int size_px) {
    int max_adv = 0;
    for (uint32_t c = '0'; c <= '9'; c++) {
        CachedGlyph *g = get_cached_glyph(ts, font, size_px, c);
        if (g && g->advance_px > max_adv) {
            max_adv = g->advance_px;
        }
    }
    return max_adv;
}

int text_measure_width(TextSystem *ts, FontId font, int size_px,
                       const char *utf8, unsigned flags) {
    const LoadedFont *lf = get_font(ts, font);
    if (!lf || !utf8 || !*utf8 || size_px <= 0) return 0;

    float scale = stbtt_ScaleForPixelHeight(&lf->info, (float)size_px);
    bool tabular = (flags & TEXT_FLAG_TABULAR) != 0;
    int tab_adv = tabular ? get_tabular_digit_advance(ts, font, size_px) : 0;

    int total_x = 0;
    int prev_gidx = 0;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (!cp) break;
        bool is_digit = (cp >= '0' && cp <= '9');
        CachedGlyph *g = get_cached_glyph(ts, font, size_px, cp);
        if (!g) continue;

        if (prev_gidx && !tabular) {
            int kern = stbtt_GetGlyphKernAdvance(&lf->info, prev_gidx, g->glyph_index);
            total_x += (int)lroundf((float)kern * scale);
        }
        total_x += (tabular && is_digit) ? tab_adv : g->advance_px;
        prev_gidx = (tabular && is_digit) ? 0 : g->glyph_index;
    }
    return total_x;
}

static void render_string_raw(TextSystem *ts, Canvas *c, FontId font, int size_px,
                              int x, int y, const char *utf8, uint8_t tone, unsigned flags) {
    const LoadedFont *lf = get_font(ts, font);
    if (!lf || !c || !utf8 || !*utf8) return;

    float scale = stbtt_ScaleForPixelHeight(&lf->info, (float)size_px);
    int baseline = y + text_ascent(ts, font, size_px);
    bool tabular = (flags & TEXT_FLAG_TABULAR) != 0;
    int tab_adv = tabular ? get_tabular_digit_advance(ts, font, size_px) : 0;

    int pen_x = x;
    int prev_gidx = 0;
    const char *p = utf8;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (!cp) break;
        bool is_digit = (cp >= '0' && cp <= '9');
        CachedGlyph *g = get_cached_glyph(ts, font, size_px, cp);
        if (!g) continue;

        if (prev_gidx && !tabular) {
            int kern = stbtt_GetGlyphKernAdvance(&lf->info, prev_gidx, g->glyph_index);
            pen_x += (int)lroundf((float)kern * scale);
        }

        int draw_x = pen_x + g->xoff;
        if (tabular && is_digit && tab_adv > g->advance_px) {
            draw_x += (tab_adv - g->advance_px) / 2;
        }
        int draw_y = baseline + g->yoff;

        if (g->bitmap && g->w > 0 && g->h > 0) {
            canvas_blend_alpha(c, draw_x, draw_y, g->bitmap, g->w, g->h, g->w, tone);
        }

        pen_x += (tabular && is_digit) ? tab_adv : g->advance_px;
        prev_gidx = (tabular && is_digit) ? 0 : g->glyph_index;
    }
}

/* Truncate utf8 into out_buf so its rendered width is <= max_w, appending "…". */
static void fit_with_ellipsis(TextSystem *ts, FontId font, int size_px,
                              const char *utf8, int max_w, unsigned flags,
                              char *out_buf, size_t out_cap) {
    if (!utf8 || out_cap == 0) return;
    if (max_w <= 0 || text_measure_width(ts, font, size_px, utf8, flags) <= max_w) {
        snprintf(out_buf, out_cap, "%s", utf8);
        return;
    }

    const char *ellipsis = "\xE2\x80\xA6"; /* U+2026 */
    int ell_w = text_measure_width(ts, font, size_px, ellipsis, flags);
    if (ell_w >= max_w) {
        out_buf[0] = '\0';
        return;
    }

    size_t best_bytes = 0;
    const char *p = utf8;
    while (*p) {
        const char *next = p;
        (void)utf8_next(&next);
        size_t len = (size_t)(next - utf8);
        if (len + 4 >= out_cap) break;

        memcpy(out_buf, utf8, len);
        memcpy(out_buf + len, ellipsis, 4);
        if (text_measure_width(ts, font, size_px, out_buf, flags) <= max_w) {
            best_bytes = len;
            p = next;
        } else {
            break;
        }
    }

    /* Strip trailing space before ellipsis. */
    while (best_bytes > 0 && out_buf[best_bytes - 1] == ' ') {
        best_bytes--;
    }
    memcpy(out_buf, utf8, best_bytes);
    memcpy(out_buf + best_bytes, ellipsis, 4);
}

int text_draw_line(TextSystem *ts, Canvas *c, FontId font, int size_px,
                   int x, int y, int max_w, const char *utf8,
                   uint8_t tone, unsigned flags) {
    if (!ts || !c || !utf8 || !*utf8 || size_px <= 0) return 0;

    char fitted[1024];
    fit_with_ellipsis(ts, font, size_px, utf8, max_w, flags, fitted, sizeof(fitted));
    int w = text_measure_width(ts, font, size_px, fitted, flags);

    int draw_x = x;
    unsigned align = flags & TEXT_ALIGN_MASK;
    if (max_w > 0) {
        if (align == TEXT_ALIGN_CENTER) {
            draw_x = x + (max_w - w) / 2;
        } else if (align == TEXT_ALIGN_RIGHT) {
            draw_x = x + max_w - w;
        }
    }

    render_string_raw(ts, c, font, size_px, draw_x, y, fitted, tone, flags);
    return w;
}

int text_draw_wrapped(TextSystem *ts, Canvas *c, FontId font, int size_px,
                      int x, int y, int max_w, int max_lines, int line_gap_px,
                      const char *utf8, uint8_t tone, unsigned flags) {
    if (!ts || !utf8 || !*utf8 || max_w <= 0 || max_lines <= 0 || size_px <= 0) {
        return 0;
    }

    int lh = text_line_height(ts, font, size_px);
    int cur_y = y;
    int lines_drawn = 0;

    const char *p = utf8;
    while (*p && lines_drawn < max_lines) {
        while (*p == ' ') p++;
        if (!*p) break;

        if (lines_drawn + 1 == max_lines) {
            if (c) {
                text_draw_line(ts, c, font, size_px, x, cur_y, max_w, p, tone, flags);
            }
            lines_drawn++;
            cur_y += lh + line_gap_px;
            break;
        }

        /* Greedily fit words onto the current line. */
        const char *scan = p;
        const char *best_end = NULL;
        while (*scan) {
            while (*scan == ' ') scan++;
            if (!*scan) break;
            const char *word_end = scan;
            while (*word_end && *word_end != ' ' && *word_end != '\n') {
                word_end++;
            }
            size_t candidate_len = (size_t)(word_end - p);
            char candidate[1024];
            if (candidate_len >= sizeof(candidate)) {
                candidate_len = sizeof(candidate) - 1;
            }
            memcpy(candidate, p, candidate_len);
            candidate[candidate_len] = '\0';

            if (text_measure_width(ts, font, size_px, candidate, flags) <= max_w) {
                best_end = word_end;
                scan = word_end;
                if (*scan == '\n') break;
            } else {
                break;
            }
        }

        char line_str[1024];
        if (best_end) {
            size_t len = (size_t)(best_end - p);
            if (len >= sizeof(line_str)) len = sizeof(line_str) - 1;
            memcpy(line_str, p, len);
            line_str[len] = '\0';
            p = best_end;
            if (*p == '\n') p++;
        } else {
            /* Single long word exceeds max_w: advance to next space and truncate line. */
            const char *word_end = p;
            while (*word_end && *word_end != ' ' && *word_end != '\n') word_end++;
            size_t len = (size_t)(word_end - p);
            if (len >= sizeof(line_str)) len = sizeof(line_str) - 1;
            memcpy(line_str, p, len);
            line_str[len] = '\0';
            p = word_end;
        }

        if (c) {
            text_draw_line(ts, c, font, size_px, x, cur_y, max_w, line_str, tone, flags);
        }
        lines_drawn++;
        cur_y += lh + line_gap_px;
    }

    return (lines_drawn > 0) ? (lines_drawn * lh + (lines_drawn - 1) * line_gap_px) : 0;
}
