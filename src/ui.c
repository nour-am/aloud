#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "icons.h"

#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const int kUiSpeedOptions[UI_SPEED_OPTION_COUNT] = {
    75, 100, 110, 125, 150, 175, 200, 250, 300
};

const int kUiSleepOptions[UI_SLEEP_OPTION_COUNT] = {
    SLEEP_OFF, 5, 15, 30, 45, 60, SLEEP_END_OF_CHAPTER
};

void ui_format_clock_ms(int64_t ms, bool neg_prefix, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (ms < 0) ms = 0;
    int64_t total_sec = (ms + 500) / 1000;
    int64_t hrs = total_sec / 3600;
    int64_t mins = (total_sec % 3600) / 60;
    int64_t secs = total_sec % 60;
    const char *sign = neg_prefix ? "-" : "";
    if (hrs > 0) {
        snprintf(out, cap, "%s%" PRId64 ":%02" PRId64 ":%02" PRId64, sign, hrs, mins, secs);
    } else {
        snprintf(out, cap, "%s%02" PRId64 ":%02" PRId64, sign, mins, secs);
    }
}

void ui_format_hours_left_ms(int64_t ms, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (ms < 0) ms = 0;
    int64_t total_min = (ms + 30000) / 60000;
    int64_t hrs = total_min / 60;
    int64_t mins = total_min % 60;
    if (hrs > 0) {
        snprintf(out, cap, "%" PRId64 " h %" PRId64 " m left", hrs, mins);
    } else if (mins > 0) {
        snprintf(out, cap, "%" PRId64 " m left", mins);
    } else {
        snprintf(out, cap, "< 1 m left");
    }
}

static void format_speed_label(int speed_x100, bool with_prefix, char *out, size_t cap) {
    int whole = speed_x100 / 100;
    int frac = speed_x100 % 100;
    const char *pre = with_prefix ? "Speed " : "";
    /* Multiply sign U+00D7 = \xC3\x97 */
    if (frac == 0) {
        snprintf(out, cap, "%s%d.0\xC3\x97", pre, whole);
    } else if (frac % 10 == 0) {
        snprintf(out, cap, "%s%d.%d\xC3\x97", pre, whole, frac / 10);
    } else {
        snprintf(out, cap, "%s%d.%02d\xC3\x97", pre, whole, frac);
    }
}

static void format_sleep_button_label(int sleep_mode, int64_t sleep_rem_ms, char *out, size_t cap) {
    if (sleep_mode == SLEEP_OFF) {
        snprintf(out, cap, "Sleep Off");
    } else if (sleep_mode == SLEEP_END_OF_CHAPTER) {
        if (sleep_rem_ms > 0) {
            int mins = (int)((sleep_rem_ms + 59999) / 60000);
            snprintf(out, cap, "Sleep Ch. (%d m)", mins);
        } else {
            snprintf(out, cap, "Sleep Ch.");
        }
    } else {
        int mins = (sleep_rem_ms > 0) ? (int)((sleep_rem_ms + 59999) / 60000) : sleep_mode;
        snprintf(out, cap, "Sleep %d m", mins);
    }
}

int ui_init(Ui *ui, Canvas *canvas, TextSystem *text, Library *library,
            Player *player, bool has_volume_control) {
    if (!ui || !canvas || !text || !library || !player) return -1;
    memset(ui, 0, sizeof(*ui));
    ui->canvas = canvas;
    ui->text = text;
    ui->library = library;
    ui->player = player;
    ui->has_volume_control = has_volume_control;
    ui->screen = UI_SCREEN_LIBRARY;
    ui->sheet = UI_SHEET_NONE;
    ui->last_drawn_second = -1;
    ui->last_drawn_sleep_min = -1;
    snprintf(ui->library_dir, sizeof(ui->library_dir), "/mnt/us/audiobooks");
    snprintf(ui->browse_dir, sizeof(ui->browse_dir), "/mnt/us");
    return 0;
}

void ui_free(Ui *ui) {
    if (!ui) return;
    free(ui->cached_player_cover);
    ui->cached_player_cover = NULL;
}

static void clear_hit_boxes(Ui *ui) {
    memset(&ui->hit_header_left, 0, sizeof(AloudRect));
    memset(&ui->hit_header_center, 0, sizeof(AloudRect));
    memset(&ui->hit_header_right, 0, sizeof(AloudRect));
    memset(&ui->hit_page_prev, 0, sizeof(AloudRect));
    memset(&ui->hit_page_next, 0, sizeof(AloudRect));
    memset(ui->hit_list_rows, 0, sizeof(ui->hit_list_rows));
    memset(ui->hit_folder_select, 0, sizeof(ui->hit_folder_select));
    ui->hit_list_row_count = 0;
    memset(&ui->hit_use_current_dir, 0, sizeof(AloudRect));

    memset(&ui->hit_scrubber, 0, sizeof(AloudRect));
    memset(&ui->hit_prev_chap, 0, sizeof(AloudRect));
    memset(&ui->hit_skip_back, 0, sizeof(AloudRect));
    memset(&ui->hit_play_pause, 0, sizeof(AloudRect));
    memset(&ui->hit_skip_fwd, 0, sizeof(AloudRect));
    memset(&ui->hit_next_chap, 0, sizeof(AloudRect));

    memset(&ui->hit_speed_btn, 0, sizeof(AloudRect));
    memset(&ui->hit_sleep_btn, 0, sizeof(AloudRect));
    memset(&ui->hit_vol_down, 0, sizeof(AloudRect));
    memset(&ui->hit_vol_up, 0, sizeof(AloudRect));

    memset(&ui->hit_sheet_box, 0, sizeof(AloudRect));
    memset(&ui->hit_sheet_close, 0, sizeof(AloudRect));
    memset(ui->hit_sheet_opts, 0, sizeof(ui->hit_sheet_opts));
    ui->hit_sheet_opt_count = 0;
}

static bool is_ignored_system_folder(const char *name) {
    if (!name || name[0] == '\0' || name[0] == '.') return true;
    static const char *const kIgnore[] = {
        "$RECYCLE.BIN", "System Volume Information", "lost+found",
        "system", "extensions", "koreader", "LARK", "libkh",
        "mrpackages", "update.bin.tmp.partial", NULL
    };
    for (int i = 0; kIgnore[i]; i++) {
        if (strcasecmp(name, kIgnore[i]) == 0) return true;
    }
    return false;
}

/* Fast 2-level count of audio files & child subdirectories inside dir_path (no FFmpeg open). */
static void inspect_folder_contents(const char *dir_path, int *out_audio, int *out_subdirs) {
    int audio = 0;
    int subdirs = 0;
    DIR *d = opendir(dir_path);
    if (!d) {
        if (out_audio) *out_audio = 0;
        if (out_subdirs) *out_subdirs = 0;
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (lib_is_supported_audio_ext(de->d_name)) {
            audio++;
            continue;
        }
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (!is_ignored_system_folder(de->d_name)) {
                subdirs++;
            }
            /* Also check 1 level deeper so folders of multi-file MP3 books count as audio! */
            DIR *sub = opendir(child);
            if (sub) {
                struct dirent *sde;
                while ((sde = readdir(sub)) != NULL) {
                    if (sde->d_name[0] != '.' && lib_is_supported_audio_ext(sde->d_name)) {
                        audio++;
                    }
                }
                closedir(sub);
            }
        }
    }
    closedir(d);
    if (out_audio) *out_audio = audio;
    if (out_subdirs) *out_subdirs = subdirs;
}

static int cmp_folder_entries(const void *a, const void *b) {
    const UiFolderEntry *fa = (const UiFolderEntry *)a;
    const UiFolderEntry *fb = (const UiFolderEntry *)b;
    if (fa->is_parent != fb->is_parent) {
        return fa->is_parent ? -1 : 1;
    }
    bool a_has = (fa->audio_count > 0);
    bool b_has = (fb->audio_count > 0);
    if (a_has != b_has) {
        return a_has ? -1 : 1;
    }
    return strcasecmp(fa->name, fb->name);
}

static void scan_browse_dir(Ui *ui, const char *requested_dir) {
    if (!ui) return;
    char norm[1024];
    if (requested_dir && requested_dir[0]) {
        snprintf(norm, sizeof(norm), "%s", requested_dir);
    } else if (ui->library_dir[0]) {
        snprintf(norm, sizeof(norm), "%s", ui->library_dir);
    } else {
        snprintf(norm, sizeof(norm), "/mnt/us");
    }
    size_t len = strlen(norm);
    while (len > 1 && norm[len - 1] == '/') {
        norm[--len] = '\0';
    }

    /* If norm doesn't exist yet on disk, walk up to its parent directory. */
    struct stat st;
    if (stat(norm, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char *slash = strrchr(norm, '/');
        if (slash && slash != norm) {
            *slash = '\0';
        } else if (access("/mnt/us", F_OK) == 0) {
            snprintf(norm, sizeof(norm), "/mnt/us");
        } else {
            snprintf(norm, sizeof(norm), ".");
        }
    }

    snprintf(ui->browse_dir, sizeof(ui->browse_dir), "%s", norm);
    ui->folder_count = 0;
    ui->folder_page = 0;

    /* Add ".. (Parent folder)" if not at root "/" or "/mnt/us". */
    if (strcmp(norm, "/") != 0 && strcmp(norm, ".") != 0 && strcmp(norm, "/mnt/us") != 0) {
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s", norm);
        char *slash = strrchr(parent, '/');
        if (slash) {
            if (slash == parent) parent[1] = '\0';
            else *slash = '\0';
            UiFolderEntry *e = &ui->folders[ui->folder_count++];
            memset(e, 0, sizeof(*e));
            snprintf(e->name, sizeof(e->name), ".. (Up to %s)", parent);
            snprintf(e->fullpath, sizeof(e->fullpath), "%s", parent);
            e->is_parent = true;
        }
    }

    /* If browsing /mnt/us and /mnt/us/audiobooks doesn't exist yet, offer it first. */
    if (strcmp(norm, "/mnt/us") == 0 && access("/mnt/us/audiobooks", F_OK) != 0 &&
        ui->folder_count < UI_MAX_FOLDERS) {
        UiFolderEntry *e = &ui->folders[ui->folder_count++];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "audiobooks (Default)");
        snprintf(e->fullpath, sizeof(e->fullpath), "/mnt/us/audiobooks");
    }

    DIR *d = opendir(norm);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL && ui->folder_count < UI_MAX_FOLDERS) {
            if (is_ignored_system_folder(de->d_name)) continue;
            char full[1024];
            snprintf(full, sizeof(full), "%s/%s",
                     (strcmp(norm, "/") == 0) ? "" : norm, de->d_name);
            struct stat cst;
            if (stat(full, &cst) != 0 || !S_ISDIR(cst.st_mode)) continue;

            UiFolderEntry *e = &ui->folders[ui->folder_count++];
            memset(e, 0, sizeof(*e));
            snprintf(e->name, sizeof(e->name), "%s", de->d_name);
            snprintf(e->fullpath, sizeof(e->fullpath), "%s", full);
            inspect_folder_contents(full, &e->audio_count, &e->subdir_count);
        }
        closedir(d);
    }

    if (ui->folder_count > 1) {
        qsort(ui->folders, (size_t)ui->folder_count, sizeof(UiFolderEntry), cmp_folder_entries);
    }
}

void ui_set_storage(Ui *ui, Store *store, const char *state_dir, const char *library_dir) {
    if (!ui) return;
    ui->store = store;
    if (state_dir) {
        snprintf(ui->state_dir, sizeof(ui->state_dir), "%s", state_dir);
    }
    if (library_dir && library_dir[0]) {
        snprintf(ui->library_dir, sizeof(ui->library_dir), "%s", library_dir);
    }
}

void ui_open_folder_picker(Ui *ui, const char *start_dir) {
    if (!ui) return;
    const char *base = start_dir;
    if (!base || !base[0]) {
        /* Start at parent of current library_dir (e.g. /mnt/us) so the user sees all
         * top-level folders on their Kindle USB drive right away! */
        static char parent_buf[1024];
        if (ui->library_dir[0]) {
            snprintf(parent_buf, sizeof(parent_buf), "%s", ui->library_dir);
            char *slash = strrchr(parent_buf, '/');
            if (slash && slash != parent_buf) {
                *slash = '\0';
                base = parent_buf;
            } else {
                base = ui->library_dir;
            }
        } else {
            base = "/mnt/us";
        }
    }
    scan_browse_dir(ui, base);
    ui->screen = UI_SCREEN_FOLDERS;
    ui->sheet = UI_SHEET_NONE;
}

static void select_library_folder(Ui *ui, const char *chosen_dir) {
    if (!ui || !chosen_dir || !chosen_dir[0]) return;
    (void)mkdir(chosen_dir, 0755);
    snprintf(ui->library_dir, sizeof(ui->library_dir), "%s", chosen_dir);
    if (ui->store) {
        (void)store_set_library_dir(ui->store, ui->state_dir[0] ? ui->state_dir : NULL, chosen_dir);
    }
    if (ui->library) {
        /* lib_scan frees and rebuilds the Book array the player and cover cache point into. */
        player_unload_book(ui->player);
        ui->cached_player_book = NULL;
        (void)lib_scan(ui->library, chosen_dir,
                       ui->state_dir[0] ? ui->state_dir : NULL, ui->store);
    }
    ui->lib_page = 0;
    ui->screen = UI_SCREEN_LIBRARY;
    ui->sheet = UI_SHEET_NONE;
}

/* Draw a cover (or refined typographic bookplate placeholder when no art exists). */
static void draw_book_cover_box(Ui *ui, const Book *b, int x, int y, int side, bool is_large) {
    Canvas *c = ui->canvas;
    int rule_px = canvas_mm_to_px(c, 0.25f);
    if (rule_px < 1) rule_px = 1;

    if (b && b->cover_gray) {
        if (is_large) {
            if (ui->cached_player_book != b || ui->cached_player_cover_size != side || !ui->cached_player_cover) {
                free(ui->cached_player_cover);
                ui->cached_player_cover = malloc((size_t)side * (size_t)side);
                if (ui->cached_player_cover) {
                    canvas_scale_and_dither_gray(b->cover_gray, LIB_THUMB_SIZE, LIB_THUMB_SIZE,
                                                 ui->cached_player_cover, side, side);
                    ui->cached_player_book = b;
                    ui->cached_player_cover_size = side;
                }
            }
            if (ui->cached_player_cover) {
                canvas_blit_gray(c, x, y, ui->cached_player_cover, side, side, side);
            }
        } else {
            uint8_t *thumb = malloc((size_t)side * (size_t)side);
            if (thumb) {
                canvas_scale_and_dither_gray(b->cover_gray, LIB_THUMB_SIZE, LIB_THUMB_SIZE,
                                             thumb, side, side);
                canvas_blit_gray(c, x, y, thumb, side, side, side);
                free(thumb);
            }
        }
        canvas_stroke_rect(c, x, y, side, side, rule_px, TONE_BLACK);
        return;
    }

    /* Placeholder per DESIGN.md: title set in the serif on a hairline frame. */
    canvas_fill_rect(c, x, y, side, side, TONE_WHITE);
    canvas_stroke_rect(c, x, y, side, side, rule_px, TONE_BLACK);
    int inset = canvas_mm_to_px(c, is_large ? 2.5f : 1.0f);
    canvas_stroke_rect(c, x + inset, y + inset, side - inset * 2, side - inset * 2,
                       rule_px, TONE_RULE_GRAY);

    if (!b) return;
    if (is_large) {
        int pad = inset + canvas_mm_to_px(c, 4.0f);
        int inner_w = side - pad * 2;
        int title_px = canvas_mm_to_px(c, 4.2f);
        int author_px = canvas_mm_to_px(c, 2.8f);
        int ty = y + side / 3;
        int th = text_draw_wrapped(ui->text, c, FONT_SERIF_BOLD, title_px,
                                   x + pad, ty, inner_w, 3, canvas_mm_to_px(c, 1.0f),
                                   b->title, TONE_BLACK, TEXT_ALIGN_CENTER);
        int ry = ty + th + canvas_mm_to_px(c, 3.0f);
        canvas_hline(c, x + side / 3, ry, side / 3, rule_px, TONE_RULE_GRAY);
        text_draw_line(ui->text, c, FONT_SANS_REGULAR, author_px,
                       x + pad, ry + canvas_mm_to_px(c, 2.5f), inner_w,
                       b->author, TONE_INK_GRAY, TEXT_ALIGN_CENTER);
    } else {
        /* Small thumbnail monogram in Literata Serif Bold. */
        char mono[8] = {0};
        mono[0] = (b->title[0] != '\0') ? b->title[0] : 'A';
        int fpx = side / 2;
        int lh = text_line_height(ui->text, FONT_SERIF_BOLD, fpx);
        text_draw_line(ui->text, c, FONT_SERIF_BOLD, fpx,
                       x, y + (side - lh) / 2, side, mono, TONE_BLACK, TEXT_ALIGN_CENTER);
    }
}

static void render_pagination_footer(Ui *ui, int y, int h, int cur_page, int total_pages) {
    Canvas *c = ui->canvas;
    int margin = canvas_mm_to_px(c, 5.5f);
    int rule_px = canvas_mm_to_px(c, 0.25f);
    canvas_hline(c, margin, y, c->width - margin * 2, rule_px, TONE_RULE_GRAY);

    int font_px = canvas_mm_to_px(c, 3.3f);
    int lh = text_line_height(ui->text, FONT_SANS_REGULAR, font_px);
    int ty = y + (h - lh) / 2;

    char page_lbl[64];
    snprintf(page_lbl, sizeof(page_lbl), "%d / %d", cur_page + 1, total_pages);
    text_draw_line(ui->text, c, FONT_SANS_REGULAR, font_px,
                   margin, ty, c->width - margin * 2, page_lbl,
                   TONE_INK_GRAY, TEXT_ALIGN_CENTER | TEXT_FLAG_TABULAR);

    int btn_w = canvas_mm_to_px(c, 22.0f);
    int icon_sz = canvas_mm_to_px(c, 4.0f);
    int iy = y + (h - icon_sz) / 2;

    ui->hit_page_prev = (AloudRect){margin, y, btn_w, h};
    ui->hit_page_next = (AloudRect){c->width - margin - btn_w, y, btn_w, h};

    uint8_t prev_tone = (cur_page > 0) ? TONE_BLACK : TONE_RULE_GRAY;
    uint8_t next_tone = (cur_page + 1 < total_pages) ? TONE_BLACK : TONE_RULE_GRAY;

    icon_draw(c, ICON_CHEVRON_LEFT, margin + canvas_mm_to_px(c, 1.0f), iy, icon_sz, prev_tone);
    text_draw_line(ui->text, c, FONT_SANS_REGULAR, font_px,
                   margin + icon_sz + canvas_mm_to_px(c, 2.0f), ty, btn_w - icon_sz,
                   "Prev", prev_tone, TEXT_ALIGN_LEFT);

    text_draw_line(ui->text, c, FONT_SANS_REGULAR, font_px,
                   c->width - margin - btn_w, ty, btn_w - icon_sz - canvas_mm_to_px(c, 2.0f),
                   "Next", next_tone, TEXT_ALIGN_RIGHT);
    icon_draw(c, ICON_CHEVRON_RIGHT, c->width - margin - icon_sz - canvas_mm_to_px(c, 1.0f),
              iy, icon_sz, next_tone);
}

static void render_library_screen(Ui *ui) {
    Canvas *c = ui->canvas;
    canvas_clear(c, TONE_WHITE);

    int margin = canvas_mm_to_px(c, 5.5f);
    int header_h = canvas_mm_to_px(c, 13.0f);
    int footer_h = canvas_mm_to_px(c, 12.5f);
    int rule_px = canvas_mm_to_px(c, 0.28f);
    if (rule_px < 1) rule_px = 1;

    /* Header: "Library" (sans, bold) left; "[Folder: ...]" center; "✕ Exit" right. */
    int hdr_font = canvas_mm_to_px(c, 4.6f);
    int sub_font = canvas_mm_to_px(c, 3.3f);
    int hdr_lh = text_line_height(ui->text, FONT_SANS_BOLD, hdr_font);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, hdr_font,
                   margin, (header_h - hdr_lh) / 2, c->width / 3,
                   "Library", TONE_BLACK, TEXT_ALIGN_LEFT);

    /* Center header button: Change Audiobook Folder. */
    const char *dir_base = strrchr(ui->library_dir, '/');
    dir_base = (dir_base && dir_base[1]) ? (dir_base + 1) : ui->library_dir;
    char folder_btn_lbl[128];
    snprintf(folder_btn_lbl, sizeof(folder_btn_lbl), "Folder: %s", dir_base);
    int fbtn_font = canvas_mm_to_px(c, 2.8f);
    int fbtn_lh = text_line_height(ui->text, FONT_SANS_BOLD, fbtn_font);
    int fbtn_tw = text_measure_width(ui->text, FONT_SANS_BOLD, fbtn_font, folder_btn_lbl, 0);
    int fbtn_w = fbtn_tw + canvas_mm_to_px(c, 6.0f);
    int max_fbtn_w = (c->width * 42) / 100;
    if (fbtn_w > max_fbtn_w) fbtn_w = max_fbtn_w;
    if (fbtn_w < canvas_mm_to_px(c, 26.0f)) fbtn_w = canvas_mm_to_px(c, 26.0f);
    int fbtn_h = canvas_mm_to_px(c, 7.8f);
    int fbtn_x = (c->width - fbtn_w) / 2;
    int fbtn_y = (header_h - fbtn_h) / 2;
    ui->hit_header_center = (AloudRect){fbtn_x, 0, fbtn_w, header_h};
    canvas_stroke_round_rect(c, fbtn_x, fbtn_y, fbtn_w, fbtn_h,
                             canvas_mm_to_px(c, 1.4f), rule_px, TONE_BLACK);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, fbtn_font,
                   fbtn_x + canvas_mm_to_px(c, 2.0f), fbtn_y + (fbtn_h - fbtn_lh) / 2,
                   fbtn_w - canvas_mm_to_px(c, 4.0f),
                   folder_btn_lbl, TONE_BLACK, TEXT_ALIGN_CENTER);

    int exit_w = canvas_mm_to_px(c, 20.0f);
    ui->hit_header_right = (AloudRect){c->width - margin - exit_w, 0, exit_w, header_h};
    int sub_lh = text_line_height(ui->text, FONT_SANS_BOLD, sub_font);
    int close_sz = canvas_mm_to_px(c, 3.4f);
    int exit_lbl_w = text_measure_width(ui->text, FONT_SANS_BOLD, sub_font, "Exit", 0);
    int exit_total_w = close_sz + canvas_mm_to_px(c, 1.8f) + exit_lbl_w;
    int exit_x = c->width - margin - exit_total_w;
    icon_draw(c, ICON_CLOSE, exit_x, (header_h - close_sz) / 2, close_sz, TONE_BLACK);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, sub_font,
                   exit_x + close_sz + canvas_mm_to_px(c, 1.8f),
                   (header_h - sub_lh) / 2, exit_lbl_w + 8,
                   "Exit", TONE_BLACK, TEXT_ALIGN_LEFT);

    canvas_hline(c, margin, header_h - rule_px, c->width - margin * 2, rule_px, TONE_BLACK);

    int body_y = header_h;
    int body_h = c->height - header_h - footer_h;
    ui->region_list_body = (AloudRect){0, body_y, c->width, c->height - body_y};

    if (ui->library->count == 0) {
        int empty_title_px = canvas_mm_to_px(c, 4.8f);
        int empty_sub_px = canvas_mm_to_px(c, 3.2f);
        int cy = body_y + body_h / 2 - canvas_mm_to_px(c, 12.0f);
        text_draw_line(ui->text, c, FONT_SERIF_BOLD, empty_title_px,
                       margin, cy, c->width - margin * 2,
                       "No audiobooks in this folder", TONE_BLACK, TEXT_ALIGN_CENTER);
        char empty_msg[1100];
        snprintf(empty_msg, sizeof(empty_msg),
                 "Current folder: %s", ui->library_dir);
        text_draw_line(ui->text, c, FONT_SANS_REGULAR, empty_sub_px,
                       margin, cy + canvas_mm_to_px(c, 8.0f), c->width - margin * 2,
                       empty_msg, TONE_INK_GRAY, TEXT_ALIGN_CENTER);

        int pick_w = canvas_mm_to_px(c, 64.0f);
        if (pick_w > c->width - margin * 4) pick_w = c->width - margin * 4;
        int pick_h = canvas_mm_to_px(c, 11.0f);
        int pick_x = (c->width - pick_w) / 2;
        int pick_y = cy + canvas_mm_to_px(c, 16.5f);
        ui->hit_use_current_dir = (AloudRect){pick_x, pick_y, pick_w, pick_h};
        canvas_fill_round_rect(c, pick_x, pick_y, pick_w, pick_h,
                               canvas_mm_to_px(c, 1.8f), TONE_BLACK);
        int pick_lh = text_line_height(ui->text, FONT_SANS_BOLD, sub_font);
        text_draw_line(ui->text, c, FONT_SANS_BOLD, sub_font,
                       pick_x, pick_y + (pick_h - pick_lh) / 2, pick_w,
                       "Choose Audiobook Folder...", TONE_WHITE, TEXT_ALIGN_CENTER);

        render_pagination_footer(ui, c->height - footer_h, footer_h, 0, 1);
        return;
    }

    int target_row_h = canvas_mm_to_px(c, 24.0f);
    int rows_per_page = body_h / target_row_h;
    if (rows_per_page < 3) rows_per_page = 3;
    if (rows_per_page > 10) rows_per_page = 10;
    ui->lib_rows_per_page = rows_per_page;

    int total_pages = ((int)ui->library->count + rows_per_page - 1) / rows_per_page;
    if (total_pages < 1) total_pages = 1;
    if (ui->lib_page < 0) ui->lib_page = 0;
    if (ui->lib_page >= total_pages) ui->lib_page = total_pages - 1;

    int row_h = body_h / rows_per_page;
    int thumb_sz = canvas_mm_to_px(c, 18.0f);
    if (thumb_sz > row_h - canvas_mm_to_px(c, 4.0f)) {
        thumb_sz = row_h - canvas_mm_to_px(c, 4.0f);
    }

    int title_px = canvas_mm_to_px(c, 3.8f);
    int author_px = canvas_mm_to_px(c, 2.9f);
    int meta_px = canvas_mm_to_px(c, 2.6f);

    size_t start_idx = (size_t)(ui->lib_page * rows_per_page);
    ui->hit_list_row_count = 0;

    for (int r = 0; r < rows_per_page; r++) {
        size_t bidx = start_idx + (size_t)r;
        if (bidx >= ui->library->count) break;
        const Book *b = &ui->library->books[bidx];

        int ry = body_y + r * row_h;
        ui->hit_list_rows[ui->hit_list_row_count++] = (AloudRect){0, ry, c->width, row_h};

        int tx = margin;
        int ty = ry + (row_h - thumb_sz) / 2;
        draw_book_cover_box(ui, b, tx, ty, thumb_sz, false);

        int text_x = tx + thumb_sz + canvas_mm_to_px(c, 4.0f);
        int text_w = c->width - margin - text_x;
        int cur_y = ty;

        int th = text_draw_wrapped(ui->text, c, FONT_SERIF_BOLD, title_px,
                                   text_x, cur_y, text_w, 2, canvas_mm_to_px(c, 0.5f),
                                   b->title, TONE_BLACK, TEXT_ALIGN_LEFT);
        cur_y += th + canvas_mm_to_px(c, 0.8f);

        text_draw_line(ui->text, c, FONT_SANS_REGULAR, author_px,
                       text_x, cur_y, text_w, b->author, TONE_INK_GRAY, TEXT_ALIGN_LEFT);

        /* Progress bar + status line at bottom of the row block. */
        int meta_lh = text_line_height(ui->text, FONT_SANS_REGULAR, meta_px);
        int meta_y = ty + thumb_sz - meta_lh;
        int bar_w = canvas_mm_to_px(c, 22.0f);
        int bar_h = canvas_mm_to_px(c, 1.1f);
        if (bar_h < 2) bar_h = 2;
        int bar_y = meta_y + (meta_lh - bar_h) / 2;

        int pct = 0;
        if (b->total_duration_ms > 0) {
            pct = (int)((b->saved_book_pos_ms * 100) / b->total_duration_ms);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }

        canvas_fill_rect(c, text_x, bar_y, bar_w, bar_h, TONE_RULE_GRAY);
        if (b->finished) {
            canvas_fill_rect(c, text_x, bar_y, bar_w, bar_h, TONE_BLACK);
        } else if (pct > 0) {
            int fw = (bar_w * pct) / 100;
            if (fw < 2) fw = 2;
            canvas_fill_rect(c, text_x, bar_y, fw, bar_h, TONE_BLACK);
        }

        char status[128];
        if (b->finished) {
            snprintf(status, sizeof(status), "Finished");
        } else if (b->saved_book_pos_ms <= 0 && b->updated_epoch == 0) {
            char rem[64];
            ui_format_hours_left_ms(b->total_duration_ms, rem, sizeof(rem));
            snprintf(status, sizeof(status), "New \xC2\xB7 %s", rem);
        } else {
            int64_t rem_ms = b->total_duration_ms - b->saved_book_pos_ms;
            if (b->saved_speed_x100 > 0) {
                rem_ms = (rem_ms * 100) / b->saved_speed_x100;
            }
            char rem[64];
            ui_format_hours_left_ms(rem_ms, rem, sizeof(rem));
            snprintf(status, sizeof(status), "%d%% \xC2\xB7 %s", pct, rem);
        }

        int status_x = text_x + bar_w + canvas_mm_to_px(c, 2.8f);
        text_draw_line(ui->text, c, FONT_SANS_REGULAR, meta_px,
                       status_x, meta_y, c->width - margin - status_x,
                       status, TONE_INK_GRAY, TEXT_ALIGN_LEFT | TEXT_FLAG_TABULAR);

        if (r + 1 < rows_per_page) {
            canvas_hline(c, margin, ry + row_h - 1, c->width - margin * 2, 1, TONE_RULE_GRAY);
        }
    }

    render_pagination_footer(ui, c->height - footer_h, footer_h, ui->lib_page, total_pages);
}

static void render_player_progress_block(Ui *ui, const PlayerSnapshot *snap,
                                         int x, int y, int w) {
    Canvas *c = ui->canvas;
    const Book *b = snap->book;
    int rule_px = canvas_mm_to_px(c, 0.3f);
    if (rule_px < 1) rule_px = 1;

    int ch_title_px = canvas_mm_to_px(c, 3.4f);
    int ch_sub_px = canvas_mm_to_px(c, 2.7f);
    int time_px = canvas_mm_to_px(c, 2.9f);
    int book_px = canvas_mm_to_px(c, 2.7f);

    int h_est = canvas_mm_to_px(c, 27.0f);
    ui->region_progress = (AloudRect){x, y, w, h_est};
    canvas_fill_rect(c, x, y, w, h_est, TONE_WHITE);

    const char *ch_name = "Chapter 1";
    int ch_num = 1;
    int ch_tot = 1;
    if (b && b->chapter_count > 0) {
        int idx = snap->chapter_index;
        if (idx < 0) idx = 0;
        if ((size_t)idx >= b->chapter_count) idx = (int)b->chapter_count - 1;
        ch_name = b->chapters[idx].title;
        ch_num = idx + 1;
        ch_tot = (int)b->chapter_count;
    }

    int cur_y = y;
    text_draw_line(ui->text, c, FONT_SANS_BOLD, ch_title_px,
                   x, cur_y, w, ch_name, TONE_BLACK, TEXT_ALIGN_CENTER);
    cur_y += text_line_height(ui->text, FONT_SANS_BOLD, ch_title_px) + canvas_mm_to_px(c, 0.6f);

    char ch_of[64];
    snprintf(ch_of, sizeof(ch_of), "Chapter %d of %d", ch_num, ch_tot);
    text_draw_line(ui->text, c, FONT_SANS_REGULAR, ch_sub_px,
                   x, cur_y, w, ch_of, TONE_INK_GRAY, TEXT_ALIGN_CENTER | TEXT_FLAG_TABULAR);
    cur_y += text_line_height(ui->text, FONT_SANS_REGULAR, ch_sub_px) + canvas_mm_to_px(c, 2.2f);

    /* Chapter scrubber: 13 mm tall touch target centered on a crisp track + black fill + dot. */
    int scrub_touch_h = canvas_mm_to_px(c, 13.0f);
    int track_y = cur_y + canvas_mm_to_px(c, 2.0f);
    int track_h = canvas_mm_to_px(c, 0.8f);
    if (track_h < 2) track_h = 2;

    ui->hit_scrubber = (AloudRect){x, track_y - scrub_touch_h / 2, w, scrub_touch_h};
    canvas_fill_rect(c, x, track_y, w, track_h, TONE_RULE_GRAY);

    int fill_w = 0;
    if (snap->chapter_dur_ms > 0) {
        fill_w = (int)((snap->chapter_pos_ms * (int64_t)w) / snap->chapter_dur_ms);
        if (fill_w < 0) fill_w = 0;
        if (fill_w > w) fill_w = w;
    }
    if (fill_w > 0) {
        canvas_fill_rect(c, x, track_y, fill_w, track_h, TONE_BLACK);
    }
    int dot_r = canvas_mm_to_px(c, 1.5f);
    int dot_x = x + fill_w;
    if (dot_x < x + dot_r) dot_x = x + dot_r;
    if (dot_x > x + w - dot_r) dot_x = x + w - dot_r;
    canvas_fill_circle(c, dot_x, track_y + track_h / 2, dot_r, TONE_BLACK);

    cur_y = track_y + track_h + canvas_mm_to_px(c, 1.8f);

    char elapsed_str[32], rem_str[32];
    ui_format_clock_ms(snap->chapter_pos_ms, false, elapsed_str, sizeof(elapsed_str));
    int64_t ch_rem = snap->chapter_dur_ms - snap->chapter_pos_ms;
    if (ch_rem < 0) ch_rem = 0;
    ui_format_clock_ms(ch_rem, true, rem_str, sizeof(rem_str));

    text_draw_line(ui->text, c, FONT_SANS_REGULAR, time_px,
                   x, cur_y, w / 2, elapsed_str, TONE_BLACK, TEXT_ALIGN_LEFT | TEXT_FLAG_TABULAR);
    text_draw_line(ui->text, c, FONT_SANS_REGULAR, time_px,
                   x + w / 2, cur_y, w - w / 2, rem_str, TONE_INK_GRAY, TEXT_ALIGN_RIGHT | TEXT_FLAG_TABULAR);
    cur_y += text_line_height(ui->text, FONT_SANS_REGULAR, time_px) + canvas_mm_to_px(c, 1.6f);

    /* Book progress line: "Book 42% · 6 h 2 m left" (scaled by playback speed). */
    if (b) {
        int pct = (b->total_duration_ms > 0)
                      ? (int)((snap->book_pos_ms * 100) / b->total_duration_ms)
                      : 0;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        int64_t book_rem = b->total_duration_ms - snap->book_pos_ms;
        if (snap->speed_x100 > 0) {
            book_rem = (book_rem * 100) / snap->speed_x100;
        }
        char hrs_left[64];
        ui_format_hours_left_ms(book_rem, hrs_left, sizeof(hrs_left));
        char book_line[128];
        snprintf(book_line, sizeof(book_line), "Book %d%% \xC2\xB7 %s", pct, hrs_left);
        text_draw_line(ui->text, c, FONT_SANS_REGULAR, book_px,
                       x, cur_y, w, book_line, TONE_INK_GRAY, TEXT_ALIGN_CENTER | TEXT_FLAG_TABULAR);
        cur_y += text_line_height(ui->text, FONT_SANS_REGULAR, book_px);
    }

    ui->region_progress.h = cur_y - y + 2;
}

static void render_play_button_only(Ui *ui, bool playing) {
    Canvas *c = ui->canvas;
    AloudRect r = ui->hit_play_pause;
    if (r.w <= 0 || r.h <= 0) return;

    canvas_fill_rect(c, r.x, r.y, r.w, r.h, TONE_WHITE);
    int cx = r.x + r.w / 2;
    int cy = r.y + r.h / 2;
    int rad = r.w / 2;
    canvas_fill_circle(c, cx, cy, rad, TONE_BLACK);

    int icon_sz = (r.w * 46) / 100;
    icon_draw(c, playing ? ICON_PAUSE : ICON_PLAY,
              cx - icon_sz / 2, cy - icon_sz / 2, icon_sz, TONE_WHITE);
    ui->region_play_btn = r;
}

static void render_transport_row(Ui *ui, const PlayerSnapshot *snap, int x, int y, int w) {
    Canvas *c = ui->canvas;
    int play_d = canvas_mm_to_px(c, 18.0f); /* 18 mm primary play disc per DESIGN.md */
    int target_d = canvas_mm_to_px(c, 11.5f); /* >= 9 mm secondary targets */
    int icon_sz = canvas_mm_to_px(c, 6.2f);
    int skip_icon_sz = canvas_mm_to_px(c, 8.2f);
    int skip_lbl_px = canvas_mm_to_px(c, 2.3f);

    int cy = y + play_d / 2;
    int slot_w = w / 5;

    int cx0 = x + slot_w * 0 + slot_w / 2;
    int cx1 = x + slot_w * 1 + slot_w / 2;
    int cx2 = x + w / 2;
    int cx3 = x + slot_w * 3 + slot_w / 2;
    int cx4 = x + slot_w * 4 + slot_w / 2;

    ui->hit_prev_chap = (AloudRect){cx0 - target_d / 2, cy - target_d / 2, target_d, target_d};
    ui->hit_skip_back = (AloudRect){cx1 - target_d / 2, cy - target_d / 2, target_d, target_d};
    ui->hit_play_pause = (AloudRect){cx2 - play_d / 2, cy - play_d / 2, play_d, play_d};
    ui->hit_skip_fwd  = (AloudRect){cx3 - target_d / 2, cy - target_d / 2, target_d, target_d};
    ui->hit_next_chap = (AloudRect){cx4 - target_d / 2, cy - target_d / 2, target_d, target_d};

    icon_draw(c, ICON_PREV_CHAPTER, cx0 - icon_sz / 2, cy - icon_sz / 2, icon_sz, TONE_BLACK);

    icon_draw(c, ICON_SKIP_BACK, cx1 - skip_icon_sz / 2, cy - skip_icon_sz / 2, skip_icon_sz, TONE_BLACK);
    int slh = text_line_height(ui->text, FONT_SANS_BOLD, skip_lbl_px);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, skip_lbl_px,
                   cx1 - skip_icon_sz / 2, cy - slh / 2 + canvas_mm_to_px(c, 0.4f),
                   skip_icon_sz, "30", TONE_BLACK, TEXT_ALIGN_CENTER | TEXT_FLAG_TABULAR);

    render_play_button_only(ui, snap->playing);

    icon_draw(c, ICON_SKIP_FWD, cx3 - skip_icon_sz / 2, cy - skip_icon_sz / 2, skip_icon_sz, TONE_BLACK);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, skip_lbl_px,
                   cx3 - skip_icon_sz / 2, cy - slh / 2 + canvas_mm_to_px(c, 0.4f),
                   skip_icon_sz, "30", TONE_BLACK, TEXT_ALIGN_CENTER | TEXT_FLAG_TABULAR);

    icon_draw(c, ICON_NEXT_CHAPTER, cx4 - icon_sz / 2, cy - icon_sz / 2, icon_sz, TONE_BLACK);
}

static void render_player_bottom_bar(Ui *ui, const PlayerSnapshot *snap, int x, int y, int w, int h) {
    Canvas *c = ui->canvas;
    ui->region_bottom_bar = (AloudRect){x, y, w, h};
    canvas_fill_rect(c, x, y, w, h, TONE_WHITE);

    int rule_px = canvas_mm_to_px(c, 0.25f);
    if (rule_px < 1) rule_px = 1;
    canvas_hline(c, x, y, w, rule_px, TONE_RULE_GRAY);

    int btn_h = canvas_mm_to_px(c, 9.2f);
    int by = y + (h - btn_h) / 2;
    int font_px = canvas_mm_to_px(c, 3.0f);
    int lh = text_line_height(ui->text, FONT_SANS_BOLD, font_px);
    int ty = by + (btn_h - lh) / 2;
    int rad = canvas_mm_to_px(c, 1.5f);

    char spd_lbl[32];
    format_speed_label(snap->speed_x100, true, spd_lbl, sizeof(spd_lbl));
    char slp_lbl[32];
    format_sleep_button_label(snap->sleep_mode, snap->sleep_rem_ms, slp_lbl, sizeof(slp_lbl));

    int gap = canvas_mm_to_px(c, 2.0f);
    int col_w = (w - gap * 2) / 3;

    ui->hit_speed_btn = (AloudRect){x, by, col_w, btn_h};
    canvas_stroke_round_rect(c, ui->hit_speed_btn.x, ui->hit_speed_btn.y,
                             ui->hit_speed_btn.w, ui->hit_speed_btn.h, rad, rule_px, TONE_BLACK);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, font_px,
                   ui->hit_speed_btn.x, ty, ui->hit_speed_btn.w,
                   spd_lbl, TONE_BLACK, TEXT_ALIGN_CENTER | TEXT_FLAG_TABULAR);

    ui->hit_sleep_btn = (AloudRect){x + col_w + gap, by, col_w, btn_h};
    bool sleep_on = (snap->sleep_mode != SLEEP_OFF);
    if (sleep_on) {
        canvas_fill_round_rect(c, ui->hit_sleep_btn.x, ui->hit_sleep_btn.y,
                               ui->hit_sleep_btn.w, ui->hit_sleep_btn.h, rad, TONE_BLACK);
    } else {
        canvas_stroke_round_rect(c, ui->hit_sleep_btn.x, ui->hit_sleep_btn.y,
                                 ui->hit_sleep_btn.w, ui->hit_sleep_btn.h, rad, rule_px, TONE_BLACK);
    }
    text_draw_line(ui->text, c, FONT_SANS_BOLD, font_px,
                   ui->hit_sleep_btn.x, ty, ui->hit_sleep_btn.w,
                   slp_lbl, sleep_on ? TONE_WHITE : TONE_BLACK,
                   TEXT_ALIGN_CENTER | TEXT_FLAG_TABULAR);

    if (ui->has_volume_control) {
        int vx = x + (col_w + gap) * 2;
        int vw = w - (col_w + gap) * 2;
        int step_w = canvas_mm_to_px(c, 9.0f);
        if (step_w * 2 > vw - 10) step_w = vw / 3;

        ui->hit_vol_down = (AloudRect){vx, by, step_w, btn_h};
        ui->hit_vol_up   = (AloudRect){vx + vw - step_w, by, step_w, btn_h};

        canvas_stroke_round_rect(c, vx, by, vw, btn_h, rad, rule_px, TONE_BLACK);
        canvas_vline(c, vx + step_w, by, btn_h, rule_px, TONE_RULE_GRAY);
        canvas_vline(c, vx + vw - step_w, by, btn_h, rule_px, TONE_RULE_GRAY);

        text_draw_line(ui->text, c, FONT_SANS_BOLD, font_px,
                       vx, ty, step_w, "\xE2\x88\x92", TONE_BLACK, TEXT_ALIGN_CENTER);
        char vol_str[16];
        snprintf(vol_str, sizeof(vol_str), "%d%%", snap->volume_pct);
        text_draw_line(ui->text, c, FONT_SANS_REGULAR, canvas_mm_to_px(c, 2.5f),
                       vx + step_w, by + (btn_h - text_line_height(ui->text, FONT_SANS_REGULAR, canvas_mm_to_px(c, 2.5f))) / 2,
                       vw - step_w * 2, vol_str, TONE_INK_GRAY, TEXT_ALIGN_CENTER | TEXT_FLAG_TABULAR);
        text_draw_line(ui->text, c, FONT_SANS_BOLD, font_px,
                       vx + vw - step_w, ty, step_w, "+", TONE_BLACK, TEXT_ALIGN_CENTER);
    }
}

static void render_active_sheet(Ui *ui, const PlayerSnapshot *snap) {
    if (ui->sheet == UI_SHEET_NONE) return;
    Canvas *c = ui->canvas;
    int margin = canvas_mm_to_px(c, 5.5f);
    int rule_px = canvas_mm_to_px(c, 0.35f);
    if (rule_px < 1) rule_px = 1;

    int sheet_h = canvas_mm_to_px(c, 52.0f);
    if (sheet_h > (c->height * 55) / 100) {
        sheet_h = (c->height * 55) / 100;
    }
    int sy = c->height - sheet_h;
    ui->hit_sheet_box = (AloudRect){0, sy, c->width, sheet_h};

    canvas_fill_rect(c, 0, sy, c->width, sheet_h, TONE_WHITE);
    canvas_hline(c, 0, sy, c->width, rule_px * 2, TONE_BLACK);

    int hdr_h = canvas_mm_to_px(c, 11.5f);
    int hdr_px = canvas_mm_to_px(c, 3.6f);
    int hdr_lh = text_line_height(ui->text, FONT_SANS_BOLD, hdr_px);
    const char *title = (ui->sheet == UI_SHEET_SPEED) ? "Playback speed" : "Sleep timer";
    text_draw_line(ui->text, c, FONT_SANS_BOLD, hdr_px,
                   margin, sy + (hdr_h - hdr_lh) / 2, c->width - margin * 2,
                   title, TONE_BLACK, TEXT_ALIGN_LEFT);

    int close_box = canvas_mm_to_px(c, 10.0f);
    ui->hit_sheet_close = (AloudRect){c->width - margin - close_box, sy, close_box, hdr_h};
    int csz = canvas_mm_to_px(c, 3.6f);
    icon_draw(c, ICON_CLOSE,
              ui->hit_sheet_close.x + (close_box - csz) / 2,
              sy + (hdr_h - csz) / 2, csz, TONE_BLACK);

    canvas_hline(c, margin, sy + hdr_h, c->width - margin * 2, 1, TONE_RULE_GRAY);

    int grid_y = sy + hdr_h + canvas_mm_to_px(c, 3.0f);
    int grid_h = sheet_h - hdr_h - canvas_mm_to_px(c, 6.0f);
    int grid_w = c->width - margin * 2;
    int gap = canvas_mm_to_px(c, 2.4f);
    int rad = canvas_mm_to_px(c, 1.6f);
    int opt_px = canvas_mm_to_px(c, 3.1f);
    int opt_lh = text_line_height(ui->text, FONT_SANS_BOLD, opt_px);

    if (ui->sheet == UI_SHEET_SPEED) {
        int cols = 3, rows = 3;
        int bw = (grid_w - gap * (cols - 1)) / cols;
        int bh = (grid_h - gap * (rows - 1)) / rows;
        ui->hit_sheet_opt_count = UI_SPEED_OPTION_COUNT;
        for (int i = 0; i < UI_SPEED_OPTION_COUNT; i++) {
            int row = i / cols;
            int col = i % cols;
            int bx = margin + col * (bw + gap);
            int by = grid_y + row * (bh + gap);
            ui->hit_sheet_opts[i] = (AloudRect){bx, by, bw, bh};

            bool active = (snap->speed_x100 == kUiSpeedOptions[i]);
            if (active) {
                canvas_fill_round_rect(c, bx, by, bw, bh, rad, TONE_BLACK);
            } else {
                canvas_stroke_round_rect(c, bx, by, bw, bh, rad, 1, TONE_BLACK);
            }
            char lbl[24];
            format_speed_label(kUiSpeedOptions[i], false, lbl, sizeof(lbl));
            text_draw_line(ui->text, c, FONT_SANS_BOLD, opt_px,
                           bx, by + (bh - opt_lh) / 2, bw,
                           lbl, active ? TONE_WHITE : TONE_BLACK,
                           TEXT_ALIGN_CENTER | TEXT_FLAG_TABULAR);
        }
    } else if (ui->sheet == UI_SHEET_SLEEP) {
        int cols = 3, rows = 3;
        int bw = (grid_w - gap * (cols - 1)) / cols;
        int bh = (grid_h - gap * (rows - 1)) / rows;
        ui->hit_sheet_opt_count = UI_SLEEP_OPTION_COUNT;
        for (int i = 0; i < UI_SLEEP_OPTION_COUNT; i++) {
            int row = i / cols;
            int col = i % cols;
            int bx = margin + col * (bw + gap);
            int by = grid_y + row * (bh + gap);
            /* Make "End of chapter" (last item) span the remaining columns cleanly. */
            int item_w = (i == UI_SLEEP_OPTION_COUNT - 1) ? (grid_w - col * (bw + gap)) : bw;
            ui->hit_sheet_opts[i] = (AloudRect){bx, by, item_w, bh};

            int mode = kUiSleepOptions[i];
            bool active = (snap->sleep_mode == mode);
            if (active) {
                canvas_fill_round_rect(c, bx, by, item_w, bh, rad, TONE_BLACK);
            } else {
                canvas_stroke_round_rect(c, bx, by, item_w, bh, rad, 1, TONE_BLACK);
            }
            char lbl[32];
            if (mode == SLEEP_OFF) snprintf(lbl, sizeof(lbl), "Off");
            else if (mode == SLEEP_END_OF_CHAPTER) snprintf(lbl, sizeof(lbl), "End of chapter");
            else snprintf(lbl, sizeof(lbl), "%d min", mode);

            text_draw_line(ui->text, c, FONT_SANS_BOLD, opt_px,
                           bx, by + (bh - opt_lh) / 2, item_w,
                           lbl, active ? TONE_WHITE : TONE_BLACK,
                           TEXT_ALIGN_CENTER | TEXT_FLAG_TABULAR);
        }
    }
}

static void render_player_screen(Ui *ui) {
    Canvas *c = ui->canvas;
    canvas_clear(c, TONE_WHITE);

    PlayerSnapshot snap;
    player_get_snapshot(ui->player, &snap);
    const Book *b = snap.book;

    int margin = canvas_mm_to_px(c, 5.5f);
    int header_h = canvas_mm_to_px(c, 12.5f);
    int bottom_h = canvas_mm_to_px(c, 14.0f);
    int rule_px = canvas_mm_to_px(c, 0.28f);
    if (rule_px < 1) rule_px = 1;

    /* Top bar: "‹ Library" left, "Chapters" right (DESIGN.md). */
    int nav_px = canvas_mm_to_px(c, 3.4f);
    int nav_lh = text_line_height(ui->text, FONT_SANS_BOLD, nav_px);
    int chev_sz = canvas_mm_to_px(c, 4.0f);
    int nav_w = canvas_mm_to_px(c, 28.0f);

    ui->hit_header_left = (AloudRect){margin, 0, nav_w, header_h};
    icon_draw(c, ICON_CHEVRON_LEFT, margin, (header_h - chev_sz) / 2, chev_sz, TONE_BLACK);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, nav_px,
                   margin + chev_sz + canvas_mm_to_px(c, 1.2f),
                   (header_h - nav_lh) / 2, nav_w - chev_sz,
                   "Library", TONE_BLACK, TEXT_ALIGN_LEFT);

    ui->hit_header_right = (AloudRect){c->width - margin - nav_w, 0, nav_w, header_h};
    int chap_lbl_w = text_measure_width(ui->text, FONT_SANS_BOLD, nav_px, "Chapters", 0);
    int chap_x = c->width - margin - chev_sz - canvas_mm_to_px(c, 1.2f) - chap_lbl_w;
    text_draw_line(ui->text, c, FONT_SANS_BOLD, nav_px,
                   chap_x, (header_h - nav_lh) / 2, chap_lbl_w + 6,
                   "Chapters", TONE_BLACK, TEXT_ALIGN_LEFT);
    icon_draw(c, ICON_CHEVRON_RIGHT, c->width - margin - chev_sz,
              (header_h - chev_sz) / 2, chev_sz, TONE_BLACK);

    canvas_hline(c, margin, header_h - rule_px, c->width - margin * 2, rule_px, TONE_RULE_GRAY);

    bool is_landscape = (c->width > c->height);
    int title_px = canvas_mm_to_px(c, 4.3f);
    int author_px = canvas_mm_to_px(c, 3.1f);
    int play_d = canvas_mm_to_px(c, 18.0f);

    if (!is_landscape) {
        int content_w = c->width - margin * 2;
        int transport_y = c->height - bottom_h - play_d - canvas_mm_to_px(c, 3.0f);

        int title_lh = text_line_height(ui->text, FONT_SERIF_BOLD, title_px);
        int author_lh = text_line_height(ui->text, FONT_SANS_REGULAR, author_px);
        int prog_need_h = canvas_mm_to_px(c, 25.0f);
        int meta_need_h = title_lh * 2 + author_lh + canvas_mm_to_px(c, 11.0f);

        /* Cover art: up to 52% of screen width, clamped so all controls breathe on 4:3 panels. */
        int cover_sz = (c->width * 52) / 100;
        int max_cover = (transport_y - header_h) - meta_need_h - prog_need_h;
        if (cover_sz > max_cover) cover_sz = max_cover;
        if (cover_sz < canvas_mm_to_px(c, 22.0f)) cover_sz = canvas_mm_to_px(c, 22.0f);

        int cur_y = header_h + canvas_mm_to_px(c, 3.0f);
        draw_book_cover_box(ui, b, (c->width - cover_sz) / 2, cur_y, cover_sz, true);
        cur_y += cover_sz + canvas_mm_to_px(c, 2.8f);

        if (b) {
            int th = text_draw_wrapped(ui->text, c, FONT_SERIF_BOLD, title_px,
                                       margin, cur_y, content_w, 2, canvas_mm_to_px(c, 0.5f),
                                       b->title, TONE_BLACK, TEXT_ALIGN_CENTER);
            cur_y += th + canvas_mm_to_px(c, 0.8f);
            text_draw_line(ui->text, c, FONT_SANS_REGULAR, author_px,
                           margin, cur_y, content_w, b->author, TONE_INK_GRAY, TEXT_ALIGN_CENTER);
            cur_y += author_lh + canvas_mm_to_px(c, 2.0f);
        }

        int prog_avail = transport_y - cur_y;
        int prog_y = cur_y;
        if (prog_avail > prog_need_h) {
            prog_y = cur_y + (prog_avail - prog_need_h) / 3;
        }
        render_player_progress_block(ui, &snap, margin, prog_y, content_w);

        /* Single-line toast message area above the transport row (DESIGN.md). */
        if (snap.toast[0] != '\0') {
            int toast_px = canvas_mm_to_px(c, 2.7f);
            int toast_h = canvas_mm_to_px(c, 6.0f);
            int toast_y = transport_y - toast_h - canvas_mm_to_px(c, 1.5f);
            canvas_fill_round_rect(c, margin, toast_y, content_w, toast_h,
                                   canvas_mm_to_px(c, 1.2f), TONE_WHITE);
            canvas_stroke_round_rect(c, margin, toast_y, content_w, toast_h,
                                     canvas_mm_to_px(c, 1.2f), 1, TONE_INK_GRAY);
            int tlh = text_line_height(ui->text, FONT_SANS_REGULAR, toast_px);
            text_draw_line(ui->text, c, FONT_SANS_REGULAR, toast_px,
                           margin + canvas_mm_to_px(c, 2.0f), toast_y + (toast_h - tlh) / 2,
                           content_w - canvas_mm_to_px(c, 4.0f),
                           snap.toast, TONE_BLACK, TEXT_ALIGN_CENTER);
        }

        render_transport_row(ui, &snap, margin, transport_y, content_w);
        render_player_bottom_bar(ui, &snap, margin, c->height - bottom_h, content_w, bottom_h);
    } else {
        /* Landscape two-column layout: cover + book title/author left, controls right (D11). */
        int body_y = header_h + canvas_mm_to_px(c, 3.5f);
        int body_h = c->height - header_h - canvas_mm_to_px(c, 4.0f);
        int col_gap = canvas_mm_to_px(c, 6.0f);
        int left_w = (c->width - margin * 2 - col_gap) * 42 / 100;
        int right_x = margin + left_w + col_gap;
        int right_w = c->width - margin - right_x;

        int cover_sz = (left_w * 78) / 100;
        if (cover_sz > (body_h * 64) / 100) cover_sz = (body_h * 64) / 100;
        int cover_x = margin + (left_w - cover_sz) / 2;
        int l_y = body_y + canvas_mm_to_px(c, 2.0f);
        draw_book_cover_box(ui, b, cover_x, l_y, cover_sz, true);
        l_y += cover_sz + canvas_mm_to_px(c, 3.0f);

        if (b) {
            int th = text_draw_wrapped(ui->text, c, FONT_SERIF_BOLD, title_px,
                                       margin, l_y, left_w, 2, canvas_mm_to_px(c, 0.5f),
                                       b->title, TONE_BLACK, TEXT_ALIGN_CENTER);
            l_y += th + canvas_mm_to_px(c, 1.0f);
            text_draw_line(ui->text, c, FONT_SANS_REGULAR, author_px,
                           margin, l_y, left_w, b->author, TONE_INK_GRAY, TEXT_ALIGN_CENTER);
        }

        canvas_vline(c, margin + left_w + col_gap / 2, header_h + canvas_mm_to_px(c, 3.0f),
                     c->height - header_h - canvas_mm_to_px(c, 6.0f), 1, TONE_RULE_GRAY);

        int r_y = body_y + canvas_mm_to_px(c, 4.0f);
        render_player_progress_block(ui, &snap, right_x, r_y, right_w);

        int transport_y = c->height - bottom_h - play_d - canvas_mm_to_px(c, 4.0f);
        if (snap.toast[0] != '\0') {
            int toast_px = canvas_mm_to_px(c, 2.6f);
            int toast_h = canvas_mm_to_px(c, 6.0f);
            int toast_y = transport_y - toast_h - canvas_mm_to_px(c, 1.5f);
            int tlh = text_line_height(ui->text, FONT_SANS_REGULAR, toast_px);
            text_draw_line(ui->text, c, FONT_SANS_REGULAR, toast_px,
                           right_x, toast_y + (toast_h - tlh) / 2, right_w,
                           snap.toast, TONE_BLACK, TEXT_ALIGN_CENTER);
        }
        render_transport_row(ui, &snap, right_x, transport_y, right_w);
        render_player_bottom_bar(ui, &snap, right_x, c->height - bottom_h, right_w, bottom_h);
    }

    render_active_sheet(ui, &snap);

    ui->last_drawn_second = (int)(snap.book_pos_ms / 1000);
    ui->last_drawn_sleep_min = (snap.sleep_rem_ms > 0) ? (int)((snap.sleep_rem_ms + 59999) / 60000) : 0;
    ui->last_drawn_playing = snap.playing;
}

static void render_chapters_screen(Ui *ui) {
    Canvas *c = ui->canvas;
    canvas_clear(c, TONE_WHITE);

    PlayerSnapshot snap;
    player_get_snapshot(ui->player, &snap);
    const Book *b = snap.book;

    int margin = canvas_mm_to_px(c, 5.5f);
    int header_h = canvas_mm_to_px(c, 13.0f);
    int footer_h = canvas_mm_to_px(c, 12.5f);
    int rule_px = canvas_mm_to_px(c, 0.28f);
    if (rule_px < 1) rule_px = 1;

    int nav_px = canvas_mm_to_px(c, 3.4f);
    int nav_lh = text_line_height(ui->text, FONT_SANS_BOLD, nav_px);
    int chev_sz = canvas_mm_to_px(c, 4.0f);
    int nav_w = canvas_mm_to_px(c, 28.0f);

    ui->hit_header_left = (AloudRect){margin, 0, nav_w, header_h};
    icon_draw(c, ICON_CHEVRON_LEFT, margin, (header_h - chev_sz) / 2, chev_sz, TONE_BLACK);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, nav_px,
                   margin + chev_sz + canvas_mm_to_px(c, 1.2f),
                   (header_h - nav_lh) / 2, nav_w - chev_sz,
                   "Player", TONE_BLACK, TEXT_ALIGN_LEFT);

    int hdr_px = canvas_mm_to_px(c, 4.2f);
    int hdr_lh = text_line_height(ui->text, FONT_SANS_BOLD, hdr_px);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, hdr_px,
                   margin + nav_w, (header_h - hdr_lh) / 2, c->width - (margin + nav_w) * 2,
                   "Chapters", TONE_BLACK, TEXT_ALIGN_CENTER);

    canvas_hline(c, margin, header_h - rule_px, c->width - margin * 2, rule_px, TONE_BLACK);

    int body_y = header_h;
    int body_h = c->height - header_h - footer_h;
    ui->region_list_body = (AloudRect){0, body_y, c->width, c->height - body_y};

    if (!b || b->chapter_count == 0) {
        render_pagination_footer(ui, c->height - footer_h, footer_h, 0, 1);
        return;
    }

    int target_row_h = canvas_mm_to_px(c, 12.5f);
    int rows_per_page = body_h / target_row_h;
    if (rows_per_page < 5) rows_per_page = 5;
    if (rows_per_page > 14) rows_per_page = 14;
    ui->chap_rows_per_page = rows_per_page;

    int total_pages = ((int)b->chapter_count + rows_per_page - 1) / rows_per_page;
    if (total_pages < 1) total_pages = 1;
    if (ui->chap_page < 0) ui->chap_page = 0;
    if (ui->chap_page >= total_pages) ui->chap_page = total_pages - 1;

    int row_h = body_h / rows_per_page;
    int row_px = canvas_mm_to_px(c, 3.3f);
    int row_lh = text_line_height(ui->text, FONT_SANS_REGULAR, row_px);
    int dur_w = canvas_mm_to_px(c, 22.0f);
    int bar_w = canvas_mm_to_px(c, 1.3f);
    if (bar_w < 3) bar_w = 3;

    size_t start_idx = (size_t)(ui->chap_page * rows_per_page);
    ui->hit_list_row_count = 0;

    for (int r = 0; r < rows_per_page; r++) {
        size_t cidx = start_idx + (size_t)r;
        if (cidx >= b->chapter_count) break;
        const BookChapter *ch = &b->chapters[cidx];

        int ry = body_y + r * row_h;
        ui->hit_list_rows[ui->hit_list_row_count++] = (AloudRect){0, ry, c->width, row_h};

        bool is_current = ((int)cidx == snap.chapter_index);
        if (is_current) {
            /* Current chapter in bold with a black left bar (DESIGN.md). */
            canvas_fill_rect(c, margin, ry + canvas_mm_to_px(c, 2.0f),
                             bar_w, row_h - canvas_mm_to_px(c, 4.0f), TONE_BLACK);
        }

        int tx = margin + (is_current ? (bar_w + canvas_mm_to_px(c, 2.8f)) : 0);
        int tw = c->width - margin - dur_w - tx - canvas_mm_to_px(c, 2.0f);
        int ty = ry + (row_h - row_lh) / 2;

        text_draw_line(ui->text, c, is_current ? FONT_SANS_BOLD : FONT_SANS_REGULAR,
                       row_px, tx, ty, tw, ch->title, TONE_BLACK, TEXT_ALIGN_LEFT);

        char dur_str[32];
        ui_format_clock_ms(ch->duration_ms, false, dur_str, sizeof(dur_str));
        text_draw_line(ui->text, c, is_current ? FONT_SANS_BOLD : FONT_SANS_REGULAR,
                       row_px, c->width - margin - dur_w, ty, dur_w,
                       dur_str, is_current ? TONE_BLACK : TONE_INK_GRAY,
                       TEXT_ALIGN_RIGHT | TEXT_FLAG_TABULAR);

        if (r + 1 < rows_per_page) {
            canvas_hline(c, margin, ry + row_h - 1, c->width - margin * 2, 1, TONE_RULE_GRAY);
        }
    }

    render_pagination_footer(ui, c->height - footer_h, footer_h, ui->chap_page, total_pages);
}

static void render_folders_screen(Ui *ui) {
    Canvas *c = ui->canvas;
    canvas_clear(c, TONE_WHITE);

    if (ui->browse_dir[0] == '\0') {
        scan_browse_dir(ui, "/mnt/us");
    }

    int margin = canvas_mm_to_px(c, 5.5f);
    int header_h = canvas_mm_to_px(c, 13.0f);
    int banner_h = canvas_mm_to_px(c, 14.5f);
    int footer_h = canvas_mm_to_px(c, 12.5f);
    int rule_px = canvas_mm_to_px(c, 0.28f);
    if (rule_px < 1) rule_px = 1;

    /* Top bar: "‹ Library" left, "Choose Audiobook Folder" center, "✕ Exit" right. */
    int nav_px = canvas_mm_to_px(c, 3.2f);
    int nav_lh = text_line_height(ui->text, FONT_SANS_BOLD, nav_px);
    int chev_sz = canvas_mm_to_px(c, 3.8f);
    int nav_w = canvas_mm_to_px(c, 24.0f);

    ui->hit_header_left = (AloudRect){margin, 0, nav_w, header_h};
    icon_draw(c, ICON_CHEVRON_LEFT, margin, (header_h - chev_sz) / 2, chev_sz, TONE_BLACK);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, nav_px,
                   margin + chev_sz + canvas_mm_to_px(c, 1.0f),
                   (header_h - nav_lh) / 2, nav_w - chev_sz,
                   "Library", TONE_BLACK, TEXT_ALIGN_LEFT);

    int hdr_px = canvas_mm_to_px(c, 3.8f);
    int hdr_lh = text_line_height(ui->text, FONT_SANS_BOLD, hdr_px);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, hdr_px,
                   margin + nav_w, (header_h - hdr_lh) / 2, c->width - (margin + nav_w) * 2,
                   "Choose Audiobook Folder", TONE_BLACK, TEXT_ALIGN_CENTER);

    int exit_w = canvas_mm_to_px(c, 18.0f);
    ui->hit_header_right = (AloudRect){c->width - margin - exit_w, 0, exit_w, header_h};
    int close_sz = canvas_mm_to_px(c, 3.4f);
    int exit_lbl_w = text_measure_width(ui->text, FONT_SANS_BOLD, nav_px, "Exit", 0);
    int exit_total_w = close_sz + canvas_mm_to_px(c, 1.6f) + exit_lbl_w;
    int exit_x = c->width - margin - exit_total_w;
    icon_draw(c, ICON_CLOSE, exit_x, (header_h - close_sz) / 2, close_sz, TONE_BLACK);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, nav_px,
                   exit_x + close_sz + canvas_mm_to_px(c, 1.6f),
                   (header_h - nav_lh) / 2, exit_lbl_w + 8,
                   "Exit", TONE_BLACK, TEXT_ALIGN_LEFT);

    canvas_hline(c, margin, header_h - rule_px, c->width - margin * 2, rule_px, TONE_BLACK);

    /* Current folder banner with "Use This Folder" button. */
    int by = header_h;
    int use_w = canvas_mm_to_px(c, 34.0f);
    int use_h = canvas_mm_to_px(c, 9.0f);
    int use_x = c->width - margin - use_w;
    int use_y = by + (banner_h - use_h) / 2;
    ui->hit_use_current_dir = (AloudRect){use_x, by, use_w, banner_h};
    canvas_fill_round_rect(c, use_x, use_y, use_w, use_h,
                           canvas_mm_to_px(c, 1.5f), TONE_BLACK);
    int btn_px = canvas_mm_to_px(c, 2.9f);
    int btn_lh = text_line_height(ui->text, FONT_SANS_BOLD, btn_px);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, btn_px,
                   use_x, use_y + (use_h - btn_lh) / 2, use_w,
                   "Use Current", TONE_WHITE, TEXT_ALIGN_CENTER);

    int path_w = use_x - margin - canvas_mm_to_px(c, 3.0f);
    int lbl_px = canvas_mm_to_px(c, 2.6f);
    int path_px = canvas_mm_to_px(c, 3.2f);
    text_draw_line(ui->text, c, FONT_SANS_REGULAR, lbl_px,
                   margin, by + canvas_mm_to_px(c, 1.8f), path_w,
                   "Tap 'Use Folder' on any directory where your audiobooks are downloaded:",
                   TONE_INK_GRAY, TEXT_ALIGN_LEFT);
    text_draw_line(ui->text, c, FONT_SANS_BOLD, path_px,
                   margin, by + canvas_mm_to_px(c, 6.8f), path_w,
                   ui->browse_dir, TONE_BLACK, TEXT_ALIGN_LEFT);

    canvas_hline(c, margin, header_h + banner_h - 1, c->width - margin * 2, 1, TONE_BLACK);

    int body_y = header_h + banner_h;
    int body_h = c->height - body_y - footer_h;
    ui->region_list_body = (AloudRect){0, body_y, c->width, c->height - body_y};

    int target_row_h = canvas_mm_to_px(c, 15.0f);
    int rows_per_page = body_h / target_row_h;
    if (rows_per_page < 4) rows_per_page = 4;
    if (rows_per_page > 12) rows_per_page = 12;
    ui->folder_rows_per_page = rows_per_page;

    int total_pages = (ui->folder_count + rows_per_page - 1) / rows_per_page;
    if (total_pages < 1) total_pages = 1;
    if (ui->folder_page < 0) ui->folder_page = 0;
    if (ui->folder_page >= total_pages) ui->folder_page = total_pages - 1;

    int row_h = body_h / rows_per_page;
    int name_px = canvas_mm_to_px(c, 3.5f);
    int meta_px = canvas_mm_to_px(c, 2.7f);
    int name_lh = text_line_height(ui->text, FONT_SANS_BOLD, name_px);
    int meta_lh = text_line_height(ui->text, FONT_SANS_REGULAR, meta_px);
    int sel_w = canvas_mm_to_px(c, 28.0f);
    int sel_h = canvas_mm_to_px(c, 8.6f);

    int start_idx = ui->folder_page * rows_per_page;
    ui->hit_list_row_count = 0;

    for (int r = 0; r < rows_per_page; r++) {
        int fidx = start_idx + r;
        if (fidx >= ui->folder_count) break;
        const UiFolderEntry *fe = &ui->folders[fidx];

        int ry = body_y + r * row_h;
        ui->hit_list_rows[ui->hit_list_row_count] = (AloudRect){0, ry, c->width, row_h};

        bool is_active_lib = (strcmp(fe->fullpath, ui->library_dir) == 0);
        int tx = margin;
        int tw = c->width - margin * 2 - (fe->is_parent ? 0 : (sel_w + canvas_mm_to_px(c, 3.0f)));
        int ty = ry + (row_h - (name_lh + meta_lh + canvas_mm_to_px(c, 0.8f))) / 2;

        text_draw_line(ui->text, c, FONT_SANS_BOLD, name_px,
                       tx, ty, tw, fe->name, TONE_BLACK, TEXT_ALIGN_LEFT);

        char sub_info[256];
        if (fe->is_parent) {
            snprintf(sub_info, sizeof(sub_info), "Go up to parent directory (%s)", fe->fullpath);
        } else if (fe->audio_count > 0 && fe->subdir_count > 0) {
            snprintf(sub_info, sizeof(sub_info), "%d audio file%s \xC2\xB7 %d subfolder%s (%s)",
                     fe->audio_count, fe->audio_count == 1 ? "" : "s",
                     fe->subdir_count, fe->subdir_count == 1 ? "" : "s", fe->fullpath);
        } else if (fe->audio_count > 0) {
            snprintf(sub_info, sizeof(sub_info), "%d audio file%s (%s)",
                     fe->audio_count, fe->audio_count == 1 ? "" : "s", fe->fullpath);
        } else if (fe->subdir_count > 0) {
            snprintf(sub_info, sizeof(sub_info), "%d subfolder%s \xC2\xB7 tap row to open (%s)",
                     fe->subdir_count, fe->subdir_count == 1 ? "" : "s", fe->fullpath);
        } else {
            snprintf(sub_info, sizeof(sub_info), "%s", fe->fullpath);
        }
        text_draw_line(ui->text, c, FONT_SANS_REGULAR, meta_px,
                       tx, ty + name_lh + canvas_mm_to_px(c, 0.8f), tw,
                       sub_info, TONE_INK_GRAY, TEXT_ALIGN_LEFT);

        if (!fe->is_parent) {
            int sx = c->width - margin - sel_w;
            int sy = ry + (row_h - sel_h) / 2;
            ui->hit_folder_select[ui->hit_list_row_count] = (AloudRect){sx, ry, sel_w, row_h};
            if (is_active_lib || fe->audio_count > 0) {
                canvas_fill_round_rect(c, sx, sy, sel_w, sel_h,
                                       canvas_mm_to_px(c, 1.4f), TONE_BLACK);
                text_draw_line(ui->text, c, FONT_SANS_BOLD, btn_px,
                               sx, sy + (sel_h - btn_lh) / 2, sel_w,
                               is_active_lib ? "Selected" : "Use Folder",
                               TONE_WHITE, TEXT_ALIGN_CENTER);
            } else {
                canvas_stroke_round_rect(c, sx, sy, sel_w, sel_h,
                                         canvas_mm_to_px(c, 1.4f), rule_px, TONE_BLACK);
                text_draw_line(ui->text, c, FONT_SANS_BOLD, btn_px,
                               sx, sy + (sel_h - btn_lh) / 2, sel_w,
                               "Use Folder", TONE_BLACK, TEXT_ALIGN_CENTER);
            }
        }

        ui->hit_list_row_count++;
        if (r + 1 < rows_per_page) {
            canvas_hline(c, margin, ry + row_h - 1, c->width - margin * 2, 1, TONE_RULE_GRAY);
        }
    }

    render_pagination_footer(ui, c->height - footer_h, footer_h, ui->folder_page, total_pages);
}

void ui_render_full(Ui *ui, bool force_flashing, AloudRect *out_rect, AloudWaveform *out_wfm) {
    if (!ui || !ui->canvas) return;
    clear_hit_boxes(ui);
    canvas_reset_dirty(ui->canvas);

    switch (ui->screen) {
        case UI_SCREEN_LIBRARY:
            render_library_screen(ui);
            break;
        case UI_SCREEN_PLAYER:
            render_player_screen(ui);
            break;
        case UI_SCREEN_CHAPTERS:
            render_chapters_screen(ui);
            break;
        case UI_SCREEN_FOLDERS:
            render_folders_screen(ui);
            break;
    }

    ui->full_repaint_counter++;
    /* DESIGN.md refresh policy: flashing GC16 every 6th full repaint or when a cover is drawn. */
    bool has_cover = (ui->screen == UI_SCREEN_PLAYER ||
                      (ui->screen == UI_SCREEN_LIBRARY && ui->library->count > 0));
    bool flash = force_flashing || has_cover || (ui->full_repaint_counter % 6 == 0);

    if (out_rect) *out_rect = (AloudRect){0, 0, ui->canvas->width, ui->canvas->height};
    if (out_wfm) *out_wfm = flash ? ALOUD_WFM_GC16_FLASH : ALOUD_WFM_GL16;
}

static bool change_page(Ui *ui, int delta, AloudRect *out_rect, AloudWaveform *out_wfm) {
    if (ui->screen == UI_SCREEN_LIBRARY) {
        if (ui->library->count == 0 || ui->lib_rows_per_page <= 0) return false;
        int total = ((int)ui->library->count + ui->lib_rows_per_page - 1) / ui->lib_rows_per_page;
        int next = ui->lib_page + delta;
        if (next < 0 || next >= total) return false;
        ui->lib_page = next;
        clear_hit_boxes(ui);
        render_library_screen(ui);
        if (out_rect) *out_rect = ui->region_list_body;
        if (out_wfm) *out_wfm = ALOUD_WFM_GL16;
        return true;
    }
    if (ui->screen == UI_SCREEN_CHAPTERS) {
        PlayerSnapshot snap;
        player_get_snapshot(ui->player, &snap);
        if (!snap.book || snap.book->chapter_count == 0 || ui->chap_rows_per_page <= 0) return false;
        int total = ((int)snap.book->chapter_count + ui->chap_rows_per_page - 1) / ui->chap_rows_per_page;
        int next = ui->chap_page + delta;
        if (next < 0 || next >= total) return false;
        ui->chap_page = next;
        clear_hit_boxes(ui);
        render_chapters_screen(ui);
        if (out_rect) *out_rect = ui->region_list_body;
        if (out_wfm) *out_wfm = ALOUD_WFM_GL16;
        return true;
    }
    if (ui->screen == UI_SCREEN_FOLDERS) {
        if (ui->folder_count == 0 || ui->folder_rows_per_page <= 0) return false;
        int total = (ui->folder_count + ui->folder_rows_per_page - 1) / ui->folder_rows_per_page;
        int next = ui->folder_page + delta;
        if (next < 0 || next >= total) return false;
        ui->folder_page = next;
        clear_hit_boxes(ui);
        render_folders_screen(ui);
        if (out_rect) *out_rect = ui->region_list_body;
        if (out_wfm) *out_wfm = ALOUD_WFM_GL16;
        return true;
    }
    return false;
}

bool ui_handle_event(Ui *ui, AloudInputEvent ev, AloudRect *out_rect, AloudWaveform *out_wfm) {
    if (!ui) return false;
    if (out_rect) *out_rect = (AloudRect){0, 0, 0, 0};
    if (out_wfm) *out_wfm = ALOUD_WFM_NONE;

    if (ev.type == ALOUD_EV_QUIT) {
        ui->quit_requested = true;
        return false;
    }

    /* Clear any active toast message on the next interaction (DESIGN.md). */
    PlayerSnapshot prev_snap;
    player_get_snapshot(ui->player, &prev_snap);
    bool had_toast = (prev_snap.toast[0] != '\0');
    if (had_toast) {
        player_clear_toast(ui->player);
    }

    if (ev.type == ALOUD_EV_SWIPE_LEFT || ev.type == ALOUD_EV_KEY_NEXT ||
        ev.type == ALOUD_EV_SWIPE_RIGHT || ev.type == ALOUD_EV_KEY_PREV) {
        if (ui->screen == UI_SCREEN_PLAYER && ui->sheet == UI_SHEET_NONE) {
            /* If the user dragged/swiped across the scrubber bar, seek to the release X! */
            if ((ev.type == ALOUD_EV_SWIPE_LEFT || ev.type == ALOUD_EV_SWIPE_RIGHT) &&
                (canvas_rect_contains(ui->hit_scrubber, ev.start_x, ev.start_y) ||
                 canvas_rect_contains(ui->hit_scrubber, ev.x, ev.y))) {
                PlayerSnapshot snap;
                player_get_snapshot(ui->player, &snap);
                if (snap.book && snap.book->chapter_count > 0 && ui->hit_scrubber.w > 0) {
                    int rel_x = ev.x - ui->hit_scrubber.x;
                    if (rel_x < 0) rel_x = 0;
                    if (rel_x > ui->hit_scrubber.w) rel_x = ui->hit_scrubber.w;
                    const BookChapter *ch = &snap.book->chapters[snap.chapter_index];
                    int64_t new_ch_pos = ((int64_t)rel_x * ch->duration_ms) / ui->hit_scrubber.w;
                    player_seek_book_ms(ui->player, ch->start_book_ms + new_ch_pos);
                    player_get_snapshot(ui->player, &snap);
                    ui->last_drawn_second = (int)(snap.book_pos_ms / 1000);
                    render_player_progress_block(ui, &snap,
                                                 ui->region_progress.x, ui->region_progress.y,
                                                 ui->region_progress.w);
                    if (out_rect) *out_rect = ui->region_progress;
                    if (out_wfm) *out_wfm = ALOUD_WFM_DU;
                    return true;
                }
            }
            if (ev.type == ALOUD_EV_KEY_NEXT || ev.type == ALOUD_EV_KEY_PREV) {
                /* Hardware page-turn forward/back = skip +/- 30 s in the player (D11). */
                player_seek_relative_ms(ui->player, (ev.type == ALOUD_EV_KEY_NEXT) ? 30000 : -30000);
                PlayerSnapshot snap;
                player_get_snapshot(ui->player, &snap);
                ui->last_drawn_second = (int)(snap.book_pos_ms / 1000);
                render_player_progress_block(ui, &snap,
                                             ui->region_progress.x, ui->region_progress.y,
                                             ui->region_progress.w);
                if (out_rect) *out_rect = ui->region_progress;
                if (out_wfm) *out_wfm = ALOUD_WFM_DU;
                return true;
            }
            return false;
        }
        int delta = (ev.type == ALOUD_EV_SWIPE_LEFT || ev.type == ALOUD_EV_KEY_NEXT) ? +1 : -1;
        return change_page(ui, delta, out_rect, out_wfm);
    }

    if (ev.type != ALOUD_EV_TAP) {
        return false;
    }

    int tx = ev.x;
    int ty = ev.y;

    /* 1. Active bottom sheet on Player screen. */
    if (ui->screen == UI_SCREEN_PLAYER && ui->sheet != UI_SHEET_NONE) {
        AloudRect sheet_rect = ui->hit_sheet_box;
        if (canvas_rect_contains(ui->hit_sheet_close, tx, ty) ||
            !canvas_rect_contains(ui->hit_sheet_box, tx, ty)) {
            ui->sheet = UI_SHEET_NONE;
            clear_hit_boxes(ui);
            render_player_screen(ui);
            if (out_rect) *out_rect = sheet_rect;
            if (out_wfm) *out_wfm = ALOUD_WFM_GL16;
            return true;
        }
        for (int i = 0; i < ui->hit_sheet_opt_count; i++) {
            if (canvas_rect_contains(ui->hit_sheet_opts[i], tx, ty)) {
                if (ui->sheet == UI_SHEET_SPEED) {
                    player_set_speed(ui->player, kUiSpeedOptions[i]);
                } else if (ui->sheet == UI_SHEET_SLEEP) {
                    player_set_sleep_timer(ui->player, kUiSleepOptions[i]);
                }
                ui->sheet = UI_SHEET_NONE;
                clear_hit_boxes(ui);
                render_player_screen(ui);
                if (out_rect) *out_rect = canvas_rect_union(sheet_rect, ui->region_progress);
                if (out_wfm) *out_wfm = ALOUD_WFM_GL16;
                return true;
            }
        }
        return false;
    }

    /* 1b. Folder picker screen taps. */
    if (ui->screen == UI_SCREEN_FOLDERS) {
        if (canvas_rect_contains(ui->hit_header_right, tx, ty)) {
            ui->quit_requested = true;
            return false;
        }
        if (canvas_rect_contains(ui->hit_header_left, tx, ty)) {
            ui->screen = UI_SCREEN_LIBRARY;
            ui_render_full(ui, false, out_rect, out_wfm);
            return true;
        }
        if (canvas_rect_contains(ui->hit_use_current_dir, tx, ty)) {
            char chosen[1024];
            snprintf(chosen, sizeof(chosen), "%s", ui->browse_dir);
            select_library_folder(ui, chosen);
            ui_render_full(ui, true, out_rect, out_wfm);
            return true;
        }
        if (canvas_rect_contains(ui->hit_page_prev, tx, ty)) {
            return change_page(ui, -1, out_rect, out_wfm);
        }
        if (canvas_rect_contains(ui->hit_page_next, tx, ty)) {
            return change_page(ui, +1, out_rect, out_wfm);
        }
        for (int i = 0; i < ui->hit_list_row_count; i++) {
            int fidx = ui->folder_page * ui->folder_rows_per_page + i;
            if (fidx >= ui->folder_count) break;
            char target_path[1024];
            snprintf(target_path, sizeof(target_path), "%s", ui->folders[fidx].fullpath);
            bool is_parent = ui->folders[fidx].is_parent;
            int subdirs = ui->folders[fidx].subdir_count;

            /* Tapping the "Use Folder" button on the right selects this folder immediately. */
            if (!is_parent && canvas_rect_contains(ui->hit_folder_select[i], tx, ty)) {
                select_library_folder(ui, target_path);
                ui_render_full(ui, true, out_rect, out_wfm);
                return true;
            }
            /* Tapping the row navigates into it if it has subfolders (or is '..'),
             * or selects it directly if it's a leaf folder! */
            if (canvas_rect_contains(ui->hit_list_rows[i], tx, ty)) {
                if (is_parent || subdirs > 0) {
                    scan_browse_dir(ui, target_path);
                    ui_render_full(ui, false, out_rect, out_wfm);
                } else {
                    select_library_folder(ui, target_path);
                    ui_render_full(ui, true, out_rect, out_wfm);
                }
                return true;
            }
        }
        return false;
    }

    /* 2. Library screen taps. */
    if (ui->screen == UI_SCREEN_LIBRARY) {
        if (canvas_rect_contains(ui->hit_header_right, tx, ty)) {
            ui->quit_requested = true;
            return false;
        }
        if (canvas_rect_contains(ui->hit_header_center, tx, ty) ||
            (ui->library->count == 0 && canvas_rect_contains(ui->hit_use_current_dir, tx, ty))) {
            ui_open_folder_picker(ui, NULL);
            ui_render_full(ui, false, out_rect, out_wfm);
            return true;
        }
        if (canvas_rect_contains(ui->hit_page_prev, tx, ty)) {
            return change_page(ui, -1, out_rect, out_wfm);
        }
        if (canvas_rect_contains(ui->hit_page_next, tx, ty)) {
            return change_page(ui, +1, out_rect, out_wfm);
        }
        for (int i = 0; i < ui->hit_list_row_count; i++) {
            if (canvas_rect_contains(ui->hit_list_rows[i], tx, ty)) {
                size_t bidx = (size_t)(ui->lib_page * ui->lib_rows_per_page + i);
                if (bidx < ui->library->count) {
                    player_load_book(ui->player, &ui->library->books[bidx]);
                    ui->screen = UI_SCREEN_PLAYER;
                    ui->sheet = UI_SHEET_NONE;
                    ui_render_full(ui, false, out_rect, out_wfm);
                    return true;
                }
            }
        }
        return false;
    }

    /* 3. Chapters screen taps. */
    if (ui->screen == UI_SCREEN_CHAPTERS) {
        if (canvas_rect_contains(ui->hit_header_left, tx, ty)) {
            ui->screen = UI_SCREEN_PLAYER;
            ui_render_full(ui, false, out_rect, out_wfm);
            return true;
        }
        if (canvas_rect_contains(ui->hit_page_prev, tx, ty)) {
            return change_page(ui, -1, out_rect, out_wfm);
        }
        if (canvas_rect_contains(ui->hit_page_next, tx, ty)) {
            return change_page(ui, +1, out_rect, out_wfm);
        }
        for (int i = 0; i < ui->hit_list_row_count; i++) {
            if (canvas_rect_contains(ui->hit_list_rows[i], tx, ty)) {
                int cidx = ui->chap_page * ui->chap_rows_per_page + i;
                player_seek_chapter(ui->player, cidx);
                ui->screen = UI_SCREEN_PLAYER;
                ui_render_full(ui, false, out_rect, out_wfm);
                return true;
            }
        }
        return false;
    }

    /* 4. Player screen taps. */
    if (ui->screen == UI_SCREEN_PLAYER) {
        if (canvas_rect_contains(ui->hit_header_left, tx, ty)) {
            /* Pause + unload (both persist the position) before re-sorting: the sort
             * moves Book structs, so the player must not keep a pointer into the array. */
            player_pause(ui->player);
            player_unload_book(ui->player);
            ui->cached_player_book = NULL;
            lib_sync_store(ui->library, ui->store);
            ui->screen = UI_SCREEN_LIBRARY;
            ui->lib_page = 0;
            ui_render_full(ui, false, out_rect, out_wfm);
            return true;
        }
        if (canvas_rect_contains(ui->hit_header_right, tx, ty)) {
            PlayerSnapshot snap;
            player_get_snapshot(ui->player, &snap);
            ui->screen = UI_SCREEN_CHAPTERS;
            ui->chap_page = (ui->chap_rows_per_page > 0)
                                ? (snap.chapter_index / ui->chap_rows_per_page)
                                : 0;
            ui_render_full(ui, false, out_rect, out_wfm);
            return true;
        }

        if (canvas_rect_contains(ui->hit_play_pause, tx, ty)) {
            player_toggle_play(ui->player);
            PlayerSnapshot snap;
            player_get_snapshot(ui->player, &snap);
            ui->last_drawn_playing = snap.playing;
            if (snap.toast[0] != '\0' || had_toast) {
                clear_hit_boxes(ui);
                render_player_screen(ui);
                if (out_rect) *out_rect = (AloudRect){0, 0, ui->canvas->width, ui->canvas->height};
                if (out_wfm) *out_wfm = ALOUD_WFM_GL16;
                return true;
            }
            /* Surgical DU refresh on the play/pause button (+ progress if rewind happened). */
            render_play_button_only(ui, snap.playing);
            ui->last_drawn_second = (int)(snap.book_pos_ms / 1000);
            render_player_progress_block(ui, &snap,
                                         ui->region_progress.x, ui->region_progress.y,
                                         ui->region_progress.w);
            if (out_rect) *out_rect = canvas_rect_union(ui->region_play_btn, ui->region_progress);
            if (out_wfm) *out_wfm = ALOUD_WFM_DU;
            return true;
        }

        if (canvas_rect_contains(ui->hit_scrubber, tx, ty)) {
            PlayerSnapshot snap;
            player_get_snapshot(ui->player, &snap);
            if (snap.book && snap.book->chapter_count > 0 && ui->hit_scrubber.w > 0) {
                int rel_x = tx - ui->hit_scrubber.x;
                if (rel_x < 0) rel_x = 0;
                if (rel_x > ui->hit_scrubber.w) rel_x = ui->hit_scrubber.w;
                const BookChapter *ch = &snap.book->chapters[snap.chapter_index];
                int64_t new_ch_pos = ((int64_t)rel_x * ch->duration_ms) / ui->hit_scrubber.w;
                player_seek_book_ms(ui->player, ch->start_book_ms + new_ch_pos);
                player_get_snapshot(ui->player, &snap);
                ui->last_drawn_second = (int)(snap.book_pos_ms / 1000);
                render_player_progress_block(ui, &snap,
                                             ui->region_progress.x, ui->region_progress.y,
                                             ui->region_progress.w);
                if (out_rect) *out_rect = ui->region_progress;
                if (out_wfm) *out_wfm = ALOUD_WFM_DU;
                return true;
            }
        }

        if (canvas_rect_contains(ui->hit_skip_back, tx, ty) ||
            canvas_rect_contains(ui->hit_skip_fwd, tx, ty) ||
            canvas_rect_contains(ui->hit_prev_chap, tx, ty) ||
            canvas_rect_contains(ui->hit_next_chap, tx, ty)) {
            if (canvas_rect_contains(ui->hit_skip_back, tx, ty)) {
                player_seek_relative_ms(ui->player, -30000);
            } else if (canvas_rect_contains(ui->hit_skip_fwd, tx, ty)) {
                player_seek_relative_ms(ui->player, +30000);
            } else if (canvas_rect_contains(ui->hit_prev_chap, tx, ty)) {
                player_prev_chapter(ui->player);
            } else {
                player_next_chapter(ui->player);
            }
            PlayerSnapshot snap;
            player_get_snapshot(ui->player, &snap);
            ui->last_drawn_second = (int)(snap.book_pos_ms / 1000);
            render_player_progress_block(ui, &snap,
                                         ui->region_progress.x, ui->region_progress.y,
                                         ui->region_progress.w);
            if (out_rect) *out_rect = ui->region_progress;
            if (out_wfm) *out_wfm = ALOUD_WFM_DU;
            return true;
        }

        if (canvas_rect_contains(ui->hit_speed_btn, tx, ty)) {
            ui->sheet = UI_SHEET_SPEED;
            clear_hit_boxes(ui);
            render_player_screen(ui);
            if (out_rect) *out_rect = ui->hit_sheet_box;
            if (out_wfm) *out_wfm = ALOUD_WFM_GL16;
            return true;
        }

        if (canvas_rect_contains(ui->hit_sleep_btn, tx, ty)) {
            ui->sheet = UI_SHEET_SLEEP;
            clear_hit_boxes(ui);
            render_player_screen(ui);
            if (out_rect) *out_rect = ui->hit_sheet_box;
            if (out_wfm) *out_wfm = ALOUD_WFM_GL16;
            return true;
        }

        if (ui->has_volume_control &&
            (canvas_rect_contains(ui->hit_vol_down, tx, ty) ||
             canvas_rect_contains(ui->hit_vol_up, tx, ty))) {
            PlayerSnapshot snap;
            player_get_snapshot(ui->player, &snap);
            int delta = canvas_rect_contains(ui->hit_vol_up, tx, ty) ? +10 : -10;
            player_set_volume(ui->player, snap.volume_pct + delta);
            player_get_snapshot(ui->player, &snap);
            render_player_bottom_bar(ui, &snap,
                                     ui->region_bottom_bar.x, ui->region_bottom_bar.y,
                                     ui->region_bottom_bar.w, ui->region_bottom_bar.h);
            if (out_rect) *out_rect = ui->region_bottom_bar;
            if (out_wfm) *out_wfm = ALOUD_WFM_DU;
            return true;
        }
    }

    return false;
}

bool ui_on_player_tick(Ui *ui, AloudRect *out_rect, AloudWaveform *out_wfm) {
    if (!ui || ui->screen != UI_SCREEN_PLAYER || ui->sheet != UI_SHEET_NONE) {
        return false;
    }
    PlayerSnapshot snap;
    player_get_snapshot(ui->player, &snap);

    /* If the background audio thread changed playing state (e.g. failed to open sink,
     * sleep timer expired, or finished book) or posted a toast, repaint the Player screen! */
    if (snap.playing != ui->last_drawn_playing || snap.toast[0] != '\0') {
        clear_hit_boxes(ui);
        render_player_screen(ui);
        if (out_rect) *out_rect = (AloudRect){0, 0, ui->canvas->width, ui->canvas->height};
        if (out_wfm) *out_wfm = ALOUD_WFM_GL16;
        return true;
    }

    int cur_sec = (int)(snap.book_pos_ms / 1000);
    int cur_sleep_min = (snap.sleep_rem_ms > 0) ? (int)((snap.sleep_rem_ms + 59999) / 60000) : 0;

    /* Skip refresh completely when the displayed second and sleep timer minute are unchanged. */
    if (cur_sec == ui->last_drawn_second &&
        cur_sleep_min == ui->last_drawn_sleep_min) {
        return false;
    }

    ui->last_drawn_second = cur_sec;
    render_player_progress_block(ui, &snap,
                                 ui->region_progress.x, ui->region_progress.y,
                                 ui->region_progress.w);
    AloudRect dirty = ui->region_progress;

    if (cur_sleep_min != ui->last_drawn_sleep_min) {
        ui->last_drawn_sleep_min = cur_sleep_min;
        render_player_bottom_bar(ui, &snap,
                                 ui->region_bottom_bar.x, ui->region_bottom_bar.y,
                                 ui->region_bottom_bar.w, ui->region_bottom_bar.h);
        dirty = canvas_rect_union(dirty, ui->region_bottom_bar);
    }

    if (out_rect) *out_rect = dirty;
    if (out_wfm) *out_wfm = ALOUD_WFM_DU;
    return true;
}
