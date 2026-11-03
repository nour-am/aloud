#define _POSIX_C_SOURCE 200809L
#include "../src/audio_out.h"
#include "../src/canvas.h"
#include "../src/decoder.h"
#include "../src/icons.h"
#include "../src/library.h"
#include "../src/platform.h"
#include "../src/player.h"
#include "../src/power.h"
#include "../src/store.h"
#include "../src/text.h"
#include "../src/ui.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define CHECK(cond, ...) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        fprintf(stderr, "FAIL [%s:%d]: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        return; \
    } \
} while (0)

static void test_natural_sort(void) {
    CHECK(lib_natural_cmp("Chapter 2.mp3", "Chapter 10.mp3") < 0, "2 should sort before 10");
    CHECK(lib_natural_cmp("02 The River.mp3", "10 The Fire.mp3") < 0, "02 should sort before 10");
    CHECK(lib_natural_cmp("Disc 1/Track 9.mp3", "Disc 2/Track 1.mp3") < 0, "Disc 1 before Disc 2");
    CHECK(lib_natural_cmp("Book A", "book b") < 0, "case-insensitive alphabetical");
    CHECK(lib_names_differ_in_number("02_foo.mp3", "10_foo.mp3"), "numbers differ");
    CHECK(!lib_names_differ_in_number("Prologue.mp3", "Epilogue.mp3"), "no numbers differ");
    CHECK(lib_is_supported_audio_ext("book.m4b"), "m4b supported");
    CHECK(lib_is_supported_audio_ext("TRACK.MP3"), "MP3 supported");
    CHECK(!lib_is_supported_audio_ext(".hidden.mp3"), "hidden dotfiles ignored");
    CHECK(!lib_is_supported_audio_ext("notes.txt"), "txt ignored");
}

static void test_store_persistence_and_recovery(const char *fixture_dir) {
    char sdir[1024];
    snprintf(sdir, sizeof(sdir), "%s/store_test", fixture_dir);
    mkdir(sdir, 0755);

    char pfile[1024], bfile[1024];
    snprintf(pfile, sizeof(pfile), "%s/positions.tsv", sdir);
    snprintf(bfile, sizeof(bfile), "%s/positions.tsv.bak", sdir);
    unlink(pfile);
    unlink(bfile);

    Store s1;
    store_init(&s1);
    CHECK(store_set(&s1, "Book One.m4b", 0, 123456, 125, 1700000100, false) == 0, "set 1");
    CHECK(store_set(&s1, "Author - Folder", 3, 654321, 150, 1700000200, true) == 0, "set 2");
    CHECK(store_save(&s1, sdir) == 0, "first store_save");

    Store s2;
    CHECK(store_load(&s2, sdir) == 0, "store_load primary");
    CHECK(!s2.recovered_from_bak, "primary was valid");
    const StoreEntry *e1 = store_get(&s2, "Book One.m4b");
    CHECK(e1 != NULL && e1->position_ms == 123456 && e1->speed_x100 == 125, "entry 1 match");

    /* Second save copies valid primary to positions.tsv.bak, then writes new primary. */
    CHECK(store_set(&s2, "Book One.m4b", 0, 222222, 150, 1700000300, false) == 0, "update 1");
    CHECK(store_save(&s2, sdir) == 0, "second store_save");

    /* Simulate power-loss corruption of primary positions.tsv (truncated mid-line). */
    FILE *fp = fopen(pfile, "w");
    CHECK(fp != NULL, "open primary to corrupt");
    fputs("# aloud-positions-v1\nBook One.m4b\t0\t999", fp);
    fclose(fp);

    Store s3;
    CHECK(store_load(&s3, sdir) == 0, "store_load should recover from .bak");
    CHECK(s3.recovered_from_bak, "recovered_from_bak flag set");
    const StoreEntry *er = store_get(&s3, "Book One.m4b");
    CHECK(er != NULL && er->position_ms == 123456, "recovered previous good position from .bak");
}

static void test_decoder_m4b_and_corrupt(const char *fixture_dir) {
    char m4b_path[1024];
    snprintf(m4b_path, sizeof(m4b_path), "%s/library/Moby-Dick.m4b", fixture_dir);

    DecoderProbeInfo info;
    uint8_t *cover = NULL;
    size_t cover_len = 0;
    CHECK(decoder_probe(m4b_path, &info, &cover, &cover_len) == 0, "probe Moby-Dick.m4b");
    CHECK(strcmp(info.album, "Moby-Dick") == 0, "album tag");
    CHECK(strcmp(info.artist, "Herman Melville") == 0, "artist tag");
    CHECK(info.chapter_count == 4, "4 embedded M4B chapters");
    CHECK(strstr(info.chapters[0].title, "Loomings") != NULL, "chapter 1 title");
    CHECK(info.duration_ms >= 11500 && info.duration_ms <= 12500, "duration ~12s");
    CHECK(info.has_cover && cover != NULL && cover_len > 100, "extracted embedded cover art");
    free(cover);

    Decoder *dec = NULL;
    CHECK(decoder_open(&dec, m4b_path) == 0 && dec != NULL, "decoder_open M4B");
    int16_t pcm[2048 * 2];
    int n = decoder_read_frames(dec, pcm, 2048);
    CHECK(n > 0, "decoded stereo PCM frames");
    CHECK(decoder_seek_ms(dec, 6000) == 0, "seek to 6000 ms");
    int64_t pos = decoder_position_ms(dec);
    CHECK(pos >= 5800 && pos <= 6200, "position after seek");
    n = decoder_read_frames(dec, pcm, 2048);
    CHECK(n > 0, "decoded frames after seek");
    decoder_close(dec);

    /* Fault tolerance: truncated/corrupt MP3 must never crash or assert. */
    char corrupt_path[1024];
    snprintf(corrupt_path, sizeof(corrupt_path), "%s/tmp/corrupt_truncated.mp3", fixture_dir);
    Decoder *cdec = NULL;
    if (decoder_open(&cdec, corrupt_path) == 0 && cdec) {
        int total = 0;
        while ((n = decoder_read_frames(cdec, pcm, 1024)) > 0) {
            total += n;
        }
        CHECK(total > 0, "decoded valid prefix of truncated MP3 before clean EOF");
        decoder_close(cdec);
    }
}

static void test_library_and_player_engine(const char *fixture_dir) {
    char lib_dir[1024], state_dir[1024];
    snprintf(lib_dir, sizeof(lib_dir), "%s/library", fixture_dir);
    snprintf(state_dir, sizeof(state_dir), "%s/engine_state", fixture_dir);
    mkdir(state_dir, 0755);

    Store store;
    store_init(&store);
    /* Mark Kindred as most-recently-played so it sorts first. */
    store_set(&store, "Octavia E. Butler - Kindred", 0, 500, 100, 1800000000, false);

    Library lib;
    lib_init(&lib);
    CHECK(lib_scan(&lib, lib_dir, state_dir, &store) == 0, "lib_scan");
    CHECK(lib.count == 3, "discovered 3 books (M4B, MP3 folder, FLAC)");
    CHECK(strcmp(lib.books[0].title, "Kindred") == 0, "most-recently-played book sorted first");

    const Book *kindred = &lib.books[0];
    CHECK(kindred->file_count == 3, "3 MP3 files in Kindred");
    /* Verify natural sort: "01 Prologue.mp3", "2 The River.mp3", "10 The Fire.mp3" */
    CHECK(strstr(kindred->files[0].relpath, "01") != NULL, "file 0 is 01");
    CHECK(strstr(kindred->files[1].relpath, "2 ") != NULL, "file 1 is 2 (before 10!)");
    CHECK(strstr(kindred->files[2].relpath, "10") != NULL, "file 2 is 10");
    CHECK(kindred->cover_gray != NULL, "folder cover.png decoded and dithered");

    /* Test Player across multi-file book, speed change, seek clamping, and sleep timer fade. */
    Player *p = NULL;
    CHECK(player_create(&p, &store, state_dir, NULL) == 0, "player_create");
    char wav_out[1024];
    snprintf(wav_out, sizeof(wav_out), "%s/tmp/sim_out.wav", fixture_dir);
    AudioOut *sim_ao = NULL;
    CHECK(audio_out_open_sim(&sim_ao, wav_out) == 0, "audio_out_open_sim WAV");
    player_set_sim_audio(p, sim_ao);

    CHECK(player_load_book(p, &lib.books[0]) == 0, "player_load_book Kindred");
    CHECK(player_play(p) == 0, "player_play");

    /* Seek near the end of file 0 and step frames to verify seamless file 0 -> file 1 transition. */
    int64_t f0_dur = lib.books[0].files[0].duration_ms;
    CHECK(player_seek_book_ms(p, f0_dur - 250) == 0, "seek near end of file 0");
    for (int i = 0; i < 40; i++) {
        (void)player_step_frames_sync(p, 2048);
    }
    PlayerSnapshot snap;
    player_get_snapshot(p, &snap);
    CHECK(snap.file_index >= 1, "advanced seamlessly from file 0 to file 1");

    /* Test Previous Chapter 3-second rule (DESIGN.md). */
    CHECK(player_seek_book_ms(p, lib.books[0].chapters[1].start_book_ms + 3500) == 0, "seek 3.5s into ch 2");
    CHECK(player_prev_chapter(p) == 0, "prev_chapter >3s restarts current chapter");
    player_get_snapshot(p, &snap);
    CHECK(snap.chapter_index == 1 && snap.chapter_pos_ms < 500, "restarted chapter 2");
    CHECK(player_prev_chapter(p) == 0, "prev_chapter <=3s goes to chapter 1");
    player_get_snapshot(p, &snap);
    CHECK(snap.chapter_index == 0, "went back to chapter 1");

    /* Test pause + 2-second rewind on resume (ARCHITECTURE.md D6). */
    CHECK(player_seek_book_ms(p, 5000) == 0, "seek to 5000 ms");
    CHECK(player_pause(p) == 0, "pause at 5000 ms");
    CHECK(player_play(p) == 0, "resume rewinds 2000 ms");
    player_get_snapshot(p, &snap);
    CHECK(snap.book_pos_ms >= 2800 && snap.book_pos_ms <= 3200, "rewound ~2s on resume");

    /* Test 2.0x speed + sleep timer fade-out to auto-pause. */
    CHECK(player_set_speed(p, 200) == 0, "set speed 2.0x");
    CHECK(player_set_sleep_timer(p, 5) == 0, "set 5 min sleep timer");
    /* Fast-forward sleep test by setting End of Chapter and seeking 1s before chapter end. */
    CHECK(player_set_sleep_timer(p, SLEEP_END_OF_CHAPTER) == 0, "set sleep End of Chapter");
    int64_t ch0_end = lib.books[0].chapters[0].start_book_ms + lib.books[0].chapters[0].duration_ms;
    CHECK(player_seek_book_ms(p, ch0_end - 400) == 0, "seek 400ms before chapter end");
    for (int i = 0; i < 30; i++) {
        (void)player_step_frames_sync(p, 1024);
    }
    player_get_snapshot(p, &snap);
    CHECK(!snap.playing && snap.sleep_mode == SLEEP_OFF, "sleep timer faded and paused playback");
    CHECK(audio_out_sim_frames_written(sim_ao) > 44100, "WAV output received >1s of frames");

    player_destroy(p);
    lib_free(&lib);
}

static void test_power_and_touch_decoders(void) {
    /* Powerd event parsing and deferSuspend calculation (D12). */
    CHECK(power_parse_event_line("com.lab126.powerd goingToScreenSaver 2") == POWER_EVT_GOING_TO_SCREENSAVER, "parse screensaver");
    CHECK(power_parse_event_line("com.lab126.powerd outOfScreenSaver 1") == POWER_EVT_OUT_OF_SCREENSAVER, "parse wake");
    CHECK(power_parse_event_line("com.lab126.powerd readyToSuspend 10") == POWER_EVT_READY_TO_SUSPEND, "parse readyToSuspend");
    CHECK(power_compute_defer_seconds(false, 0) == 0, "paused -> never defer suspend");
    CHECK(power_compute_defer_seconds(true, 0) == 3600, "playing -> defer 3600s");
    CHECK(power_compute_defer_seconds(true, 300000) == 360, "playing with 5m sleep -> defer 360s");

    /* Multitouch Protocol B tap & swipe. */
    TouchDecoder td;
    touch_decoder_init(&td, 0, 1235, 0, 1647, 1236, 1648, 300, 0, false, false, false, false);
    AloudInputEvent ev;
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_ABS, ALOUD_ABS_MT_SLOT, 0, &ev), "slot 0");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_ABS, ALOUD_ABS_MT_TRACKING_ID, 42, &ev), "trk id");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_ABS, ALOUD_ABS_MT_POSITION_X, 600, &ev), "x");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_ABS, ALOUD_ABS_MT_POSITION_Y, 800, &ev), "y");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_SYN, ALOUD_SYN_REPORT, 0, &ev), "syn down");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_ABS, ALOUD_ABS_MT_TRACKING_ID, -1, &ev), "trk lift");
    CHECK(touch_decoder_feed(&td, ALOUD_EV_SYN, ALOUD_SYN_REPORT, 0, &ev), "syn lift emits tap");
    CHECK(ev.type == ALOUD_EV_TAP && ev.x == 600 && ev.y == 800, "tap coords (600, 800)");

    /* KT4 quirk: never sends ABS_MT_TRACKING_ID -1, only BTN_TOUCH 0 on lift (RESEARCH.md §5). */
    touch_decoder_init(&td, 0, 599, 0, 799, 600, 800, 167, 0, false, false, false, false);
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_KEY, ALOUD_BTN_TOUCH, 1, &ev), "btn_touch 1");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_ABS, ALOUD_ABS_MT_POSITION_X, 500, &ev), "x start");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_ABS, ALOUD_ABS_MT_POSITION_Y, 400, &ev), "y start");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_SYN, ALOUD_SYN_REPORT, 0, &ev), "syn 1");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_ABS, ALOUD_ABS_MT_POSITION_X, 100, &ev), "x swipe left");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_SYN, ALOUD_SYN_REPORT, 0, &ev), "syn 2");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_KEY, ALOUD_BTN_TOUCH, 0, &ev), "KT4 btn_touch 0 lift");
    CHECK(touch_decoder_feed(&td, ALOUD_EV_SYN, ALOUD_SYN_REPORT, 0, &ev), "syn 3 emits swipe");
    CHECK(ev.type == ALOUD_EV_SWIPE_LEFT, "KT4 swipe left decoded");

    /* Multitouch Protocol A (ABS_MT_POSITION_X/Y + SYN_MT_REPORT). */
    touch_decoder_init(&td, 0, 4095, 0, 4095, 600, 800, 167, 0, false, false, false, false);
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_ABS, ALOUD_ABS_MT_POSITION_X, 2048, &ev), "pA x");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_ABS, ALOUD_ABS_MT_POSITION_Y, 1024, &ev), "pA y");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_SYN, ALOUD_SYN_MT_REPORT, 0, &ev), "pA mt syn");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_SYN, ALOUD_SYN_REPORT, 0, &ev), "pA syn down");
    CHECK(!touch_decoder_feed(&td, ALOUD_EV_SYN, ALOUD_SYN_MT_REPORT, 0, &ev), "pA empty mt syn");
    CHECK(touch_decoder_feed(&td, ALOUD_EV_SYN, ALOUD_SYN_REPORT, 0, &ev), "pA lift emits tap");
    CHECK(ev.type == ALOUD_EV_TAP && ev.x >= 298 && ev.x <= 302 && ev.y >= 198 && ev.y <= 202,
          "Kindle Touch 0..4095 scaled to 600x800");

    /* Oasis vs Voyage page-turn key reversal (104 / 109). */
    touch_decoder_init(&td, 0, 100, 0, 100, 1264, 1680, 300, 0, false, false, false, true);
    CHECK(touch_decoder_feed(&td, ALOUD_EV_KEY, ALOUD_KEY_PAGEUP, 1, &ev) && ev.type == ALOUD_EV_KEY_NEXT,
          "Oasis 104 is page forward");
    touch_decoder_init(&td, 0, 100, 0, 100, 1072, 1448, 300, 0, false, false, false, false);
    CHECK(touch_decoder_feed(&td, ALOUD_EV_KEY, ALOUD_KEY_PAGEUP, 1, &ev) && ev.type == ALOUD_EV_KEY_PREV,
          "Voyage 104 is page back");
}

static void test_ui_and_golden_screens(const char *fixture_dir, const char *font_dir) {
    /* Font validation safety check (RESEARCH.md §5). */
    uint8_t bad_font[128] = {0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(!text_validate_ttf_bytes(bad_font, sizeof(bad_font)), "rejects non-TTF bytes");

    TextSystem *ts = NULL;
    CHECK(text_init(&ts, font_dir) == 0 && ts != NULL, "text_init loads Literata & Inter static TTFs");

    /* Tabular digits check: "11:11" and "08:49" must have identical pixel widths. */
    int w1 = text_measure_width(ts, FONT_SANS_REGULAR, 32, "11:11", TEXT_FLAG_TABULAR);
    int w2 = text_measure_width(ts, FONT_SANS_REGULAR, 32, "08:49", TEXT_FLAG_TABULAR);
    CHECK(w1 > 0 && w1 == w2, "tabular figures have identical width (%d vs %d)", w1, w2);

    char lib_dir[1024], state_dir[1024];
    snprintf(lib_dir, sizeof(lib_dir), "%s/library", fixture_dir);
    snprintf(state_dir, sizeof(state_dir), "%s/state", fixture_dir);

    Store store;
    store_init(&store);
    (void)store_load(&store, state_dir);

    Library lib;
    lib_init(&lib);
    CHECK(lib_scan(&lib, lib_dir, state_dir, &store) == 0 && lib.count == 3, "scan library for UI");

    Canvas canvas;
    CHECK(canvas_init(&canvas, 1236, 1648, 300) == 0, "canvas_init PW5");

    Player *player = NULL;
    CHECK(player_create(&player, &store, state_dir, NULL) == 0, "player_create for UI");
    AudioOut *sim_ao = NULL;
    CHECK(audio_out_open_sim(&sim_ao, NULL) == 0, "sim_ao");
    player_set_sim_audio(player, sim_ao);

    Ui ui;
    CHECK(ui_init(&ui, &canvas, ts, &lib, player, true) == 0, "ui_init");

    AloudRect rect;
    AloudWaveform wfm;
    ui_render_full(&ui, true, &rect, &wfm);
    CHECK(wfm == ALOUD_WFM_GC16_FLASH, "initial library render uses GC16 flash");
    CHECK(ui.hit_list_row_count == 3, "3 book rows clickable");

    /* Tap row 0 -> navigates to Player screen. */
    AloudInputEvent tap_row = {
        .type = ALOUD_EV_TAP,
        .x = ui.hit_list_rows[0].x + 100,
        .y = ui.hit_list_rows[0].y + 50
    };
    CHECK(ui_handle_event(&ui, tap_row, &rect, &wfm), "tap row 0 handled");
    CHECK(ui.screen == UI_SCREEN_PLAYER, "navigated to Player screen");

    /* Tap Play button -> surgical DU refresh on play button + progress (NOT full screen!). */
    AloudInputEvent tap_play = {
        .type = ALOUD_EV_TAP,
        .x = ui.hit_play_pause.x + ui.hit_play_pause.w / 2,
        .y = ui.hit_play_pause.y + ui.hit_play_pause.h / 2
    };
    CHECK(ui_handle_event(&ui, tap_play, &rect, &wfm), "tap play handled");
    CHECK(wfm == ALOUD_WFM_DU, "play toggle uses fast DU waveform");
    CHECK(rect.h < canvas.height / 2, "play toggle refreshes only partial region (%d px < %d px)",
          rect.h, canvas.height / 2);

    /* Calling ui_on_player_tick when the minute hasn't changed must be a no-op (DESIGN.md). */
    CHECK(!ui_on_player_tick(&ui, &rect, &wfm), "minute tick skipped when displayed minute unchanged");

    /* Tap Speed button -> opens Speed bottom sheet with GL16 partial update. */
    AloudInputEvent tap_speed = {
        .type = ALOUD_EV_TAP,
        .x = ui.hit_speed_btn.x + 20,
        .y = ui.hit_speed_btn.y + 20
    };
    CHECK(ui_handle_event(&ui, tap_speed, &rect, &wfm), "tap speed opens sheet");
    CHECK(ui.sheet == UI_SHEET_SPEED && wfm == ALOUD_WFM_GL16, "speed sheet opened with GL16");

    /* Tap 1.5x option (index 4 in kUiSpeedOptions) -> closes sheet and updates speed. */
    AloudInputEvent tap_15x = {
        .type = ALOUD_EV_TAP,
        .x = ui.hit_sheet_opts[4].x + 20,
        .y = ui.hit_sheet_opts[4].y + 20
    };
    CHECK(ui_handle_event(&ui, tap_15x, &rect, &wfm), "select 1.5x");
    PlayerSnapshot snap;
    player_get_snapshot(player, &snap);
    CHECK(ui.sheet == UI_SHEET_NONE && snap.speed_x100 == 150, "speed updated to 1.5x");

    /* Verify progress block never overlaps the transport row on PW5 (300 DPI). */
    CHECK(ui.region_progress.y + ui.region_progress.h <= ui.hit_play_pause.y,
          "PW5 progress block (%d..%d) does not overlap transport button (y=%d)",
          ui.region_progress.y, ui.region_progress.y + ui.region_progress.h, ui.hit_play_pause.y);

    /* Tap Chapters button -> navigates to Chapters screen. */
    AloudInputEvent tap_chaps = {
        .type = ALOUD_EV_TAP,
        .x = ui.hit_header_right.x + 20,
        .y = ui.hit_header_right.y + 20
    };
    CHECK(ui_handle_event(&ui, tap_chaps, &rect, &wfm), "tap chapters");
    CHECK(ui.screen == UI_SCREEN_CHAPTERS, "on Chapters screen");

    /* Also verify KT4 (600x800 @ 167 DPI) non-overlap. */
    Canvas kt4_canvas;
    CHECK(canvas_init(&kt4_canvas, 600, 800, 167) == 0, "kt4 canvas init");
    Ui kt4_ui;
    ui_init(&kt4_ui, &kt4_canvas, ts, &lib, player, true);
    kt4_ui.screen = UI_SCREEN_PLAYER;
    ui_render_full(&kt4_ui, false, &rect, &wfm);
    CHECK(kt4_ui.region_progress.y + kt4_ui.region_progress.h <= kt4_ui.hit_play_pause.y,
          "KT4 progress block (%d..%d) does not overlap transport button (y=%d)",
          kt4_ui.region_progress.y, kt4_ui.region_progress.y + kt4_ui.region_progress.h,
          kt4_ui.hit_play_pause.y);
    ui_free(&kt4_ui);
    canvas_free(&kt4_canvas);

    ui_free(&ui);
    player_destroy(player);
    canvas_free(&canvas);
    text_free(ts);
    lib_free(&lib);
}

int main(int argc, char **argv) {
    const char *fixture_dir = (argc > 1) ? argv[1] : "build/test-fixtures";
    const char *font_dir = (argc > 2) ? argv[2] : "third_party/fonts";

    test_natural_sort();
    test_store_persistence_and_recovery(fixture_dir);
    test_decoder_m4b_and_corrupt(fixture_dir);
    test_library_and_player_engine(fixture_dir);
    test_power_and_touch_decoders();
    test_ui_and_golden_screens(fixture_dir, font_dir);

    if (g_tests_failed > 0) {
        fprintf(stderr, "FAILED: %d / %d checks failed\n", g_tests_failed, g_tests_run);
        return 1;
    }
    printf("PASS: all %d checks passed.\n", g_tests_run);
    return 0;
}
