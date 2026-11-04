#ifndef ALOUD_UI_H
#define ALOUD_UI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "canvas.h"
#include "library.h"
#include "platform.h"
#include "player.h"
#include "text.h"

typedef enum {
    UI_SCREEN_LIBRARY = 0,
    UI_SCREEN_PLAYER,
    UI_SCREEN_CHAPTERS,
    UI_SCREEN_FOLDERS
} UiScreen;

typedef enum {
    UI_SHEET_NONE = 0,
    UI_SHEET_SPEED,
    UI_SHEET_SLEEP
} UiSheet;

#define UI_SPEED_OPTION_COUNT 9
#define UI_SLEEP_OPTION_COUNT 7
#define UI_MAX_FOLDERS 48

extern const int kUiSpeedOptions[UI_SPEED_OPTION_COUNT];
extern const int kUiSleepOptions[UI_SLEEP_OPTION_COUNT];

typedef struct {
    char name[256];
    char fullpath[1024];
    int audio_count;
    int subdir_count;
    bool is_parent;
} UiFolderEntry;

typedef struct {
    Canvas *canvas;
    TextSystem *text;
    Library *library;
    Player *player;
    Store *store;
    char state_dir[1024];
    char library_dir[1024];
    bool has_volume_control;

    UiScreen screen;
    UiSheet sheet;

    int lib_page;
    int lib_rows_per_page;
    int chap_page;
    int chap_rows_per_page;

    /* Folder picker state. */
    char browse_dir[1024];
    UiFolderEntry folders[UI_MAX_FOLDERS];
    int folder_count;
    int folder_page;
    int folder_rows_per_page;

    /* Cached scaled/dithered covers so redraws don't re-dither on partial updates. */
    const Book *cached_player_book;
    int cached_player_cover_size;
    uint8_t *cached_player_cover;

    /* Layout hit-boxes for the current screen (computed during render, used by tap handler). */
    AloudRect hit_header_left;
    AloudRect hit_header_center;
    AloudRect hit_header_right;
    AloudRect hit_page_prev;
    AloudRect hit_page_next;
    AloudRect hit_list_rows[16];
    AloudRect hit_folder_select[16];
    int hit_list_row_count;
    AloudRect hit_use_current_dir;

    AloudRect hit_scrubber;
    AloudRect hit_prev_chap;
    AloudRect hit_skip_back;
    AloudRect hit_play_pause;
    AloudRect hit_skip_fwd;
    AloudRect hit_next_chap;

    AloudRect hit_speed_btn;
    AloudRect hit_sleep_btn;
    AloudRect hit_vol_down;
    AloudRect hit_vol_up;

    AloudRect hit_sheet_box;
    AloudRect hit_sheet_close;
    AloudRect hit_sheet_opts[12];
    int hit_sheet_opt_count;

    /* Sub-regions for surgical partial refreshes (DESIGN.md refresh policy). */
    AloudRect region_progress;
    AloudRect region_play_btn;
    AloudRect region_bottom_bar;
    AloudRect region_list_body;

    /* Refresh policy state. */
    int full_repaint_counter;
    int last_drawn_second;
    int last_drawn_sleep_min;
    bool last_drawn_playing;
    bool quit_requested;
    bool background_requested; /* "Play in background" tapped; main.c clears it */
} Ui;

int ui_init(Ui *ui, Canvas *canvas, TextSystem *text, Library *library,
            Player *player, bool has_volume_control);
void ui_free(Ui *ui);

/* Attach persistent Store + state_dir + current library_dir so the UI can prompt
 * the user to choose/change their audiobook download directory. */
void ui_set_storage(Ui *ui, Store *store, const char *state_dir, const char *library_dir);

/* Open the interactive audiobook directory picker starting at start_dir (or /mnt/us). */
void ui_open_folder_picker(Ui *ui, const char *start_dir);

/* Render the full current screen and populate *out_rect and *out_wfm according
 * to DESIGN.md's refresh policy. */
void ui_render_full(Ui *ui, bool force_flashing, AloudRect *out_rect, AloudWaveform *out_wfm);

/* Handle a user input event (tap, swipe, hardware key). Returns true if the screen
 * needs a refresh, setting *out_rect and *out_wfm to the minimal update region. */
bool ui_handle_event(Ui *ui, AloudInputEvent ev, AloudRect *out_rect, AloudWaveform *out_wfm);

/* Called on audio thread notifications / minute ticks. Updates only the progress
 * region with WFM_DU when the displayed minute changes (never per second). */
bool ui_on_player_tick(Ui *ui, AloudRect *out_rect, AloudWaveform *out_wfm);

/* Format helpers exposed for unit testing. */
void ui_format_clock_ms(int64_t ms, bool neg_prefix, char *out, size_t cap);
void ui_format_hours_left_ms(int64_t ms, char *out, size_t cap);

#endif /* ALOUD_UI_H */
