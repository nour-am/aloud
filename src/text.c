#define _POSIX_C_SOURCE 200809L
#include "text.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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
    bool from_fallback;
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
    bool mapped; /* ttf_data is an mmap of the file, not a heap copy */
} LoadedFont;

/* CJK fallback: one face for regular slots, one for bold. */
enum { CJK_REGULAR = 0, CJK_BOLD = 1, CJK_COUNT = 2 };

struct TextSystem {
    LoadedFont fonts[FONT_COUNT];
    LoadedFont fallback_fonts[FONT_COUNT];
    LoadedFont cjk_fonts[CJK_COUNT];
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

/* Large fallback fonts (CJK, 2.5 MB each) are memory-mapped instead of copied to the
 * heap: the kernel pages in only the glyph outlines actually drawn, and clean pages
 * can be dropped under memory pressure, so an all-Latin library costs almost nothing. */
static int load_font_mapped(LoadedFont *lf, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 64 || st.st_size > 32 * 1024 * 1024) {
        close(fd);
        return -EINVAL;
    }
    size_t len = (size_t)st.st_size;
    void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* the mapping keeps the file referenced */
    if (map == MAP_FAILED) return -ENOMEM;

    uint8_t *data = map;
    int off = -1;
    if (text_validate_ttf_bytes(data, len)) {
        off = stbtt_GetFontOffsetForIndex(data, 0);
    }
    if (off < 0 || !stbtt_InitFont(&lf->info, data, off)) {
        munmap(map, len);
        return -EINVAL;
    }
    lf->ttf_data = data;
    lf->ttf_len = len;
    lf->loaded = true;
    lf->mapped = true;
    return 0;
}

static void unload_font(LoadedFont *lf) {
    if (!lf->ttf_data) return;
    if (lf->mapped) {
        munmap(lf->ttf_data, lf->ttf_len);
    } else {
        free(lf->ttf_data);
    }
    memset(lf, 0, sizeof(*lf));
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
    static const char *const kFallbackFiles[FONT_COUNT] = {
        "NotoNaskhArabic-Regular.ttf",
        "NotoNaskhArabic-Bold.ttf",
        "NotoSansArabicUI-Regular.ttf",
        "NotoSansArabicUI-Bold.ttf"
    };

    for (int i = 0; i < FONT_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", font_dir, kFiles[i]);
        int rc = load_one_font(&ts->fonts[i], path);
        if (rc < 0) {
            text_free(ts);
            return rc;
        }
        snprintf(path, sizeof(path), "%s/%s", font_dir, kFallbackFiles[i]);
        (void)load_one_font(&ts->fallback_fonts[i], path);
    }
    /* Optional: without them CJK characters draw as "?" like any missing glyph. */
    static const char *const kCjkFiles[CJK_COUNT] = {
        "NotoSansCJK-Regular.otf",
        "NotoSansCJK-Bold.otf"
    };
    for (int i = 0; i < CJK_COUNT; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", font_dir, kCjkFiles[i]);
        (void)load_font_mapped(&ts->cjk_fonts[i], path);
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
        unload_font(&ts->fonts[i]);
        unload_font(&ts->fallback_fonts[i]);
    }
    for (int i = 0; i < CJK_COUNT; i++) {
        unload_font(&ts->cjk_fonts[i]);
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

/* Arabic contextual joining (U+0621..U+064A -> U+FE80..U+FEFC) + RTL visual ordering. */
typedef struct {
    uint8_t join; /* 0 = non-joining, 1 = right-joining, 2 = dual-joining */
    uint16_t forms[4]; /* {isolated, final, initial, medial} */
} ArabicForm;

static const ArabicForm kArabicForms[0x064A - 0x0621 + 1] = {
    /* U+0621 */ {0, {0xFE80, 0x0621, 0x0621, 0x0621}},
    /* U+0622 */ {1, {0xFE81, 0xFE82, 0xFE81, 0xFE82}},
    /* U+0623 */ {1, {0xFE83, 0xFE84, 0xFE83, 0xFE84}},
    /* U+0624 */ {1, {0xFE85, 0xFE86, 0xFE85, 0xFE86}},
    /* U+0625 */ {1, {0xFE87, 0xFE88, 0xFE87, 0xFE88}},
    /* U+0626 */ {2, {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}},
    /* U+0627 */ {1, {0xFE8D, 0xFE8E, 0xFE8D, 0xFE8E}},
    /* U+0628 */ {2, {0xFE8F, 0xFE90, 0xFE91, 0xFE92}},
    /* U+0629 */ {1, {0xFE93, 0xFE94, 0xFE93, 0xFE94}},
    /* U+062A */ {2, {0xFE95, 0xFE96, 0xFE97, 0xFE98}},
    /* U+062B */ {2, {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}},
    /* U+062C */ {2, {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}},
    /* U+062D */ {2, {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}},
    /* U+062E */ {2, {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}},
    /* U+062F */ {1, {0xFEA9, 0xFEAA, 0xFEA9, 0xFEAA}},
    /* U+0630 */ {1, {0xFEAB, 0xFEAC, 0xFEAB, 0xFEAC}},
    /* U+0631 */ {1, {0xFEAD, 0xFEAE, 0xFEAD, 0xFEAE}},
    /* U+0632 */ {1, {0xFEAF, 0xFEB0, 0xFEAF, 0xFEB0}},
    /* U+0633 */ {2, {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}},
    /* U+0634 */ {2, {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}},
    /* U+0635 */ {2, {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}},
    /* U+0636 */ {2, {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}},
    /* U+0637 */ {2, {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}},
    /* U+0638 */ {2, {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}},
    /* U+0639 */ {2, {0xFEC9, 0xFECA, 0xFECB, 0xFECC}},
    /* U+063A */ {2, {0xFECD, 0xFECE, 0xFECF, 0xFED0}},
    /* U+063B */ {0, {0x063B, 0x063B, 0x063B, 0x063B}},
    /* U+063C */ {0, {0x063C, 0x063C, 0x063C, 0x063C}},
    /* U+063D */ {0, {0x063D, 0x063D, 0x063D, 0x063D}},
    /* U+063E */ {0, {0x063E, 0x063E, 0x063E, 0x063E}},
    /* U+063F */ {0, {0x063F, 0x063F, 0x063F, 0x063F}},
    /* U+0640 */ {2, {0x0640, 0x0640, 0x0640, 0x0640}},
    /* U+0641 */ {2, {0xFED1, 0xFED2, 0xFED3, 0xFED4}},
    /* U+0642 */ {2, {0xFED5, 0xFED6, 0xFED7, 0xFED8}},
    /* U+0643 */ {2, {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}},
    /* U+0644 */ {2, {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}},
    /* U+0645 */ {2, {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}},
    /* U+0646 */ {2, {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}},
    /* U+0647 */ {2, {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}},
    /* U+0648 */ {1, {0xFEED, 0xFEEE, 0xFEED, 0xFEEE}},
    /* U+0649 */ {1, {0xFEEF, 0xFEF0, 0xFEEF, 0xFEF0}},
    /* U+064A */ {2, {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}},
};

static bool is_arabic_cp(uint32_t cp) {
    return (cp >= 0x0600 && cp <= 0x06FF) ||
           (cp >= 0xFB50 && cp <= 0xFDFF) ||
           (cp >= 0xFE70 && cp <= 0xFEFF);
}

static uint8_t arabic_join_type(uint32_t cp) {
    if (cp >= 0x0621 && cp <= 0x064A) {
        return kArabicForms[cp - 0x0621].join;
    }
    return 0;
}

static uint32_t lam_alef_ligature(uint32_t next_cp, bool join_prev) {
    switch (next_cp) {
        case 0x0622: return join_prev ? 0xFEF6U : 0xFEF5U;
        case 0x0623: return join_prev ? 0xFEF8U : 0xFEF7U;
        case 0x0625: return join_prev ? 0xFEFAU : 0xFEF9U;
        case 0x0627: return join_prev ? 0xFEFCU : 0xFEFBU;
        default: return 0;
    }
}

static size_t decode_and_shape(const char *utf8, uint32_t *out, size_t cap) {
    if (!utf8 || !out || cap == 0) return 0;
    uint32_t raw[512];
    size_t n = 0;
    bool has_arabic = false;
    const char *p = utf8;
    while (*p && n < 512 && n < cap) {
        uint32_t cp = utf8_next(&p);
        if (!cp) break;
        if (is_arabic_cp(cp)) has_arabic = true;
        /* NFC compose Arabic base + combining Madda/Hamza (e.g. macOS NFD filenames). */
        if (n > 0) {
            uint32_t prev = raw[n - 1];
            if (prev == 0x0627 && cp == 0x0653) { raw[n - 1] = 0x0622; continue; }
            if (prev == 0x0627 && cp == 0x0654) { raw[n - 1] = 0x0623; continue; }
            if (prev == 0x0627 && cp == 0x0655) { raw[n - 1] = 0x0625; continue; }
            if (prev == 0x0648 && cp == 0x0654) { raw[n - 1] = 0x0624; continue; }
            if ((prev == 0x064A || prev == 0x0649) && cp == 0x0654) { raw[n - 1] = 0x0626; continue; }
        }
        /* Skip non-spacing Arabic harakat/tashkeel so letter joining is uninterrupted. */
        if ((cp >= 0x064B && cp <= 0x065F) || cp == 0x0670) {
            continue;
        }
        raw[n++] = cp;
    }
    if (!has_arabic) {
        memcpy(out, raw, n * sizeof(uint32_t));
        return n;
    }

    /* Contextual joining + Lam-Alef ligatures. */
    uint32_t shaped[512];
    size_t sn = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = raw[i];
        uint8_t jt = arabic_join_type(cp);
        if (jt == 0) {
            if (cp == 0x0621) cp = 0xFE80U;
            shaped[sn++] = cp;
            continue;
        }
        bool join_prev = (i > 0 && arabic_join_type(raw[i - 1]) == 2);
        if (cp == 0x0644 && i + 1 < n) {
            uint32_t lig = lam_alef_ligature(raw[i + 1], join_prev);
            if (lig != 0) {
                shaped[sn++] = lig;
                i++; /* consumed Alef */
                continue;
            }
        }
        bool join_next = (jt == 2 && i + 1 < n && arabic_join_type(raw[i + 1]) >= 1);
        int form_idx = (join_prev ? 1 : 0) | (join_next ? 2 : 0);
        shaped[sn++] = kArabicForms[cp - 0x0621].forms[form_idx];
    }

    /* Visual Bidi ordering: reverse each RTL span (from an Arabic glyph through the last
     * Arabic glyph in that span, including trailing ellipsis if attached to Arabic),
     * then restore LTR order for any embedded ASCII digit/word runs inside the span. */
    size_t i = 0;
    while (i < sn) {
        if (!is_arabic_cp(shaped[i])) {
            i++;
            continue;
        }
        size_t start = i;
        size_t end = i;
        for (size_t j = i + 1; j < sn; j++) {
            if (is_arabic_cp(shaped[j])) {
                end = j;
            } else if (shaped[j] == 0x2026U && end + 1 == j) {
                end = j; /* keep ellipsis attached to end of Arabic text */
            } else if (shaped[j] != ' ' && shaped[j] != '-' && shaped[j] != ':' &&
                       !(shaped[j] >= '0' && shaped[j] <= '9')) {
                break;
            }
        }
        /* Reverse [start .. end]. */
        for (size_t l = start, r = end; l < r; l++, r--) {
            uint32_t tmp = shaped[l];
            shaped[l] = shaped[r];
            shaped[r] = tmp;
        }
        /* Restore LTR order for embedded ASCII digit runs inside [start .. end]. */
        size_t k = start;
        while (k <= end) {
            if (shaped[k] >= '0' && shaped[k] <= '9') {
                size_t d_start = k;
                while (k + 1 <= end && (shaped[k + 1] >= '0' && shaped[k + 1] <= '9')) {
                    k++;
                }
                for (size_t l = d_start, r = k; l < r; l++, r--) {
                    uint32_t tmp = shaped[l];
                    shaped[l] = shaped[r];
                    shaped[r] = tmp;
                }
            }
            k++;
        }
        i = end + 1;
    }

    memcpy(out, shaped, sn * sizeof(uint32_t));
    return sn;
}

static const LoadedFont *get_font(const TextSystem *ts, FontId font) {
    if (!ts || font < 0 || font >= FONT_COUNT) return NULL;
    return ts->fonts[font].loaded ? &ts->fonts[font] : NULL;
}

static const LoadedFont *get_fallback_font(const TextSystem *ts, FontId font) {
    if (!ts || font < 0 || font >= FONT_COUNT) return NULL;
    return ts->fallback_fonts[font].loaded ? &ts->fallback_fonts[font] : NULL;
}

static const LoadedFont *get_cjk_font(const TextSystem *ts, FontId font) {
    if (!ts) return NULL;
    bool bold = (font == FONT_SERIF_BOLD || font == FONT_SANS_BOLD);
    const LoadedFont *lf = &ts->cjk_fonts[bold ? CJK_BOLD : CJK_REGULAR];
    if (lf->loaded) return lf;
    lf = &ts->cjk_fonts[bold ? CJK_REGULAR : CJK_BOLD];
    return lf->loaded ? lf : NULL;
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

    const LoadedFont *active_lf = lf;
    bool from_fb = false;
    int gidx = stbtt_FindGlyphIndex(&lf->info, (int)cp);
    if (gidx == 0 && cp != ' ') {
        const LoadedFont *chain[2] = { get_fallback_font(ts, font), get_cjk_font(ts, font) };
        for (int k = 0; k < 2 && gidx == 0; k++) {
            if (!chain[k]) continue;
            int fb_idx = stbtt_FindGlyphIndex(&chain[k]->info, (int)cp);
            if (fb_idx != 0) {
                active_lf = chain[k];
                gidx = fb_idx;
                from_fb = true;
            }
        }
        if (gidx == 0) {
            gidx = stbtt_FindGlyphIndex(&lf->info, '?');
        }
    }

    /* Fallback faces are scaled to the primary font's em size, not its line height:
     * Noto's tall ascender/descender metrics would otherwise draw Arabic and CJK
     * visibly smaller than the Latin text beside them. */
    float scale = stbtt_ScaleForPixelHeight(&lf->info, (float)size_px);
    if (from_fb) {
        float em_px = scale / stbtt_ScaleForMappingEmToPixels(&lf->info, 1.0f);
        scale = stbtt_ScaleForMappingEmToPixels(&active_lf->info, em_px);
    }
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&active_lf->info, gidx, &adv, &lsb);

    int w = 0, h = 0, xoff = 0, yoff = 0;
    uint8_t *bmp = NULL;
    if (cp != ' ' && cp != '\t') {
        bmp = stbtt_GetGlyphBitmap(&active_lf->info, scale, scale, gidx, &w, &h, &xoff, &yoff);
    }

    g->valid = true;
    g->font = font;
    g->size_px = size_px;
    g->codepoint = cp;
    g->glyph_index = gidx;
    g->from_fallback = from_fb;
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

    uint32_t cps[512];
    size_t n = decode_and_shape(utf8, cps, 512);

    int total_x = 0;
    int prev_gidx = 0;
    bool prev_fb = false;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = cps[i];
        bool is_digit = (cp >= '0' && cp <= '9');
        CachedGlyph *g = get_cached_glyph(ts, font, size_px, cp);
        if (!g) continue;

        if (prev_gidx && !tabular && !prev_fb && !g->from_fallback) {
            int kern = stbtt_GetGlyphKernAdvance(&lf->info, prev_gidx, g->glyph_index);
            total_x += (int)lroundf((float)kern * scale);
        }
        total_x += (tabular && is_digit) ? tab_adv : g->advance_px;
        prev_gidx = (tabular && is_digit) ? 0 : g->glyph_index;
        prev_fb = g->from_fallback;
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

    uint32_t cps[512];
    size_t n = decode_and_shape(utf8, cps, 512);

    int pen_x = x;
    int prev_gidx = 0;
    bool prev_fb = false;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = cps[i];
        bool is_digit = (cp >= '0' && cp <= '9');
        CachedGlyph *g = get_cached_glyph(ts, font, size_px, cp);
        if (!g) continue;

        if (prev_gidx && !tabular && !prev_fb && !g->from_fallback) {
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
        prev_fb = g->from_fallback;
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

static bool is_cjk_cp(uint32_t cp) {
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

/* Closing punctuation and the long-vowel mark must not start a line (basic kinsoku). */
static bool cjk_no_line_start(uint32_t cp) {
    switch (cp) {
        case 0x3001: case 0x3002: case 0x3009: case 0x300B: case 0x300D: case 0x300F:
        case 0x3011: case 0x3015: case 0x30FC: case 0xFF01: case 0xFF09: case 0xFF0C:
        case 0xFF0E: case 0xFF1A: case 0xFF1B: case 0xFF1F:
            return true;
        default:
            return false;
    }
}

/* End of the next unbreakable unit starting at s (s is not a space): a run of
 * non-CJK characters up to a space, newline or CJK character, or a single CJK
 * character with any punctuation that must stay attached to it. CJK text has no
 * spaces, so without this a Japanese or Chinese title could never wrap. */
static const char *wrap_unit_end(const char *s) {
    const char *q = s;
    uint32_t cp = utf8_next(&q);
    if (is_cjk_cp(cp)) {
        for (;;) {
            const char *r = q;
            uint32_t n = utf8_next(&r);
            if (n == 0 || !cjk_no_line_start(n)) break;
            q = r;
        }
        return q;
    }
    while (*q && *q != ' ' && *q != '\n') {
        const char *r = q;
        if (is_cjk_cp(utf8_next(&r))) break;
        q = r;
    }
    return q;
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
            const char *word_end = wrap_unit_end(scan);
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
            const char *word_end = wrap_unit_end(p);
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
