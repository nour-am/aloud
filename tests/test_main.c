#define _POSIX_C_SOURCE 200809L
#include "../src/audio_out.h"
#include "../src/canvas.h"
#include "../src/decoder.h"
#include "../src/icons.h"
#include "../src/library.h"
#include "../src/mp4lite.h"
#include "../src/platform.h"
#include "../src/player.h"
#include "../src/power.h"
#include "../src/store.h"
#include "../src/text.h"
#include "../src/ui.h"

#include <inttypes.h>
#include <signal.h>
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

static int read_n(Decoder *d, int16_t *out, int frames) {
    int total = 0, n;
    while (total < frames && (n = decoder_read_frames(d, out + total * 2, frames - total)) > 0) {
        total += n;
    }
    return total;
}

static void test_mp4lite_reader(const char *fixture_dir) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/library/Moby-Dick.m4b", fixture_dir);

    Mp4Lite *m = NULL;
    Mp4LiteInfo mi;
    CHECK(mp4lite_open(&m, path, &mi) == 0 && m, "mp4lite opens an ffmpeg-made M4B");
    CHECK(mi.timescale > 0 && mi.asc_len >= 2 && mi.sample_count > 400, "mp4lite track info");
    CHECK(mp4lite_heap_bytes(m) < 64 * 1024, "mp4lite memory is independent of length");
    int64_t pts = 0;
    CHECK(mp4lite_seek(m, (int64_t)mi.timescale * 6, &pts) == 0, "mp4lite seek");
    CHECK(pts <= (int64_t)mi.timescale * 6 && pts > (int64_t)mi.timescale * 6 - 2048,
          "mp4lite seek lands on the frame holding the target");
    mp4lite_close(m);

    /* Same PCM as libavformat, sample for sample, when read from the start. */
    Decoder *lite = NULL, *ref = NULL;
    CHECK(decoder_open(&lite, path) == 0 && decoder_uses_mp4lite(lite), "M4B uses mp4lite");
    CHECK(decoder_open_ex(&ref, path, DECODER_FLAG_NO_MP4LITE) == 0 && !decoder_uses_mp4lite(ref),
          "libavformat path still available");
    CHECK(decoder_duration_ms(lite) == decoder_duration_ms(ref), "same duration both paths");
    static int16_t a[44100 * 2 * 2], b[44100 * 2 * 2];
    int na = read_n(lite, a, 44100 * 2), nb = read_n(ref, b, 44100 * 2);
    CHECK(na == nb && na == 44100 * 2, "same frame count both paths");
    CHECK(memcmp(a, b, (size_t)na * 4) == 0, "bit-identical PCM both paths");
    CHECK(decoder_seek_ms(lite, 6000) == 0 && decoder_seek_ms(ref, 6000) == 0, "seek both");
    int64_t pl = decoder_position_ms(lite);
    CHECK(pl >= 5950 && pl <= 6050, "mp4lite position after seek");
    CHECK(read_n(lite, a, 4096) == 4096, "mp4lite decodes after seek");
    CHECK(decoder_seek_ms(lite, 99999999) == 0 && read_n(lite, a, 4096) < 4096, "seek past end is EOF");
    decoder_close(lite);
    decoder_close(ref);

    /* Fragmented MP4 is out of scope for mp4lite: must fall back, not fail. */
    snprintf(path, sizeof(path), "%s/tmp/fragmented.m4a", fixture_dir);
    Decoder *frag = NULL;
    CHECK(decoder_open(&frag, path) == 0 && !decoder_uses_mp4lite(frag), "fragmented MP4 falls back");
    CHECK(read_n(frag, a, 44100) == 44100, "fragmented MP4 still plays");
    decoder_close(frag);

    /* Truncated M4B (download cut short): plays what exists, then ends cleanly. */
    snprintf(path, sizeof(path), "%s/tmp/truncated.m4b", fixture_dir);
    Decoder *trunc = NULL;
    if (decoder_open(&trunc, path) == 0 && trunc) {
        int total = 0, n;
        while ((n = decoder_read_frames(trunc, a, 4096)) > 0) total += n;
        CHECK(total > 44100, "truncated M4B plays its first seconds");
        decoder_close(trunc);
    }
}

/* Capturing AudioOut: records the first 2048 frames written after a reset so tests
 * can inspect the start-of-playback ramp. */
static int16_t g_cap_buf[2048 * 2];
static int g_cap_frames;
static int cap_write(AudioOut *ao, const int16_t *frames, int n) {
    (void)ao;
    int keep = 2048 - g_cap_frames;
    if (keep > n) keep = n;
    if (keep > 0) {
        memcpy(g_cap_buf + g_cap_frames * 2, frames, (size_t)keep * 2 * sizeof(int16_t));
        g_cap_frames += keep;
    }
    return n;
}
static void cap_close(AudioOut *ao) { free(ao); }

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

    /* Playback start must ramp in from silence (no click / glitch at playback start). */
    {
        AudioOut *cap = calloc(1, sizeof(*cap));
        cap->write_s16le = cap_write;
        cap->close = cap_close;
        Player *p2 = NULL;
        CHECK(player_create(&p2, &store, state_dir, NULL) == 0, "player_create (ramp test)");
        player_set_sim_audio(p2, cap);
        /* Seek after play (play may rewind 2 s) to the exact start of file 2, so the
         * decoder opens fresh instead of priming an MP3 bit reservoir mid-file. */
        int64_t f1_start = lib.books[0].files[0].duration_ms;
        CHECK(player_load_book(p2, &lib.books[0]) == 0 && player_play(p2) == 0 &&
              player_seek_book_ms(p2, f1_start) == 0, "ramp test: load, play, seek to file 2");
        g_cap_frames = 0;
        for (int i = 0; i < 8 && g_cap_frames < 2048; i++) {
            (void)player_step_frames_sync(p2, 2048);
        }
        int peak_late = 0;
        for (int i = 1500 * 2; i < g_cap_frames * 2; i++) {
            int v = abs(g_cap_buf[i]);
            if (v > peak_late) peak_late = v;
        }
        CHECK(g_cap_frames >= 2048 && g_cap_buf[0] == 0 && g_cap_buf[1] == 0 && peak_late > 1000,
              "playback starts at silence and ramps in (first=%d,%d peak_after_ramp=%d)",
              g_cap_buf[0], g_cap_buf[1], peak_late);
        int peak_early = 0;
        for (int i = 0; i < 40 * 2; i++) {
            int v = abs(g_cap_buf[i]);
            if (v > peak_early) peak_early = v;
        }
        CHECK(peak_early * 10 < peak_late, "first ~1 ms is attenuated (%d vs %d)", peak_early, peak_late);
        player_destroy(p2); /* closes cap */
    }

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

    /* Arabic fallback font + NFC Hamza composition + contextual joining check. */
    int w_ar_nfc = text_measure_width(ts, FONT_SERIF_BOLD, 36, "أبي الذي أكره", TEXT_FLAG_NONE);
    int w_ar_nfd = text_measure_width(ts, FONT_SERIF_BOLD, 36, "ا\xD9\x94" "بي الذي ا\xD9\x94" "كره", TEXT_FLAG_NONE);
    CHECK(w_ar_nfc > 80 && w_ar_nfc == w_ar_nfd,
          "Arabic NFC & NFD titles shape identically (%d == %d)", w_ar_nfc, w_ar_nfd);

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

    /* Tap the Kindred row -> navigates to Player screen. Pick it by title: rows are
     * ordered by last-played second, which earlier tests make timing-dependent (a
     * nearly-finished 12 s book in row 0 made later checks flaky). */
    int kindred_row = -1;
    for (int i = 0; i < (int)lib.count; i++) {
        if (strstr(lib.books[i].title, "Kindred")) kindred_row = i;
    }
    CHECK(kindred_row >= 0 && kindred_row < ui.hit_list_row_count, "Kindred row present");
    AloudInputEvent tap_row = {
        .type = ALOUD_EV_TAP,
        .x = ui.hit_list_rows[kindred_row].x + 100,
        .y = ui.hit_list_rows[kindred_row].y + 50
    };
    CHECK(ui_handle_event(&ui, tap_row, &rect, &wfm), "tap row 0 handled");
    CHECK(ui.screen == UI_SCREEN_PLAYER, "navigated to Player screen");
    {
        PlayerSnapshot chosen;
        player_get_snapshot(player, &chosen);
        CHECK(chosen.book && strstr(chosen.book->title, "Kindred"), "opened Kindred");
    }

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

    /* Calling ui_on_player_tick when the displayed second hasn't changed is a no-op;
     * advancing 1 second triggers a fast DU partial refresh of the progress block. */
    CHECK(!ui_on_player_tick(&ui, &rect, &wfm), "second tick skipped when displayed second unchanged");
    for (int i = 0; i < 25; i++) {
        (void)player_step_frames_sync(player, PLAYER_CHUNK_FRAMES);
    }
    CHECK(ui_on_player_tick(&ui, &rect, &wfm) && wfm == ALOUD_WFM_DU,
          "1-second playback advance refreshes progress block with DU");

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

    /* Drag/swipe across the scrubber bar -> seeks immediately to release X and updates with DU. */
    AloudInputEvent drag_scrub = {
        .type = ALOUD_EV_SWIPE_RIGHT,
        .start_x = ui.hit_scrubber.x + 20,
        .start_y = ui.hit_scrubber.y + ui.hit_scrubber.h / 2,
        .x = ui.hit_scrubber.x + (ui.hit_scrubber.w * 3) / 4,
        .y = ui.hit_scrubber.y + ui.hit_scrubber.h / 2
    };
    CHECK(ui_handle_event(&ui, drag_scrub, &rect, &wfm) && wfm == ALOUD_WFM_DU,
          "scrubber drag seeks immediately and refreshes progress with DU");
    player_get_snapshot(player, &snap);
    CHECK(snap.chapter_pos_ms > (snap.chapter_dur_ms * 60) / 100,
          "scrubber drag jumped to ~75%% of chapter (%lld / %lld ms)",
          (long long)snap.chapter_pos_ms, (long long)snap.chapter_dur_ms);

    /* Position persistence across Library round-trips (regression: Back used to zero
     * every book's in-memory position and re-sort the Book array under the player). */
    {
        ui_set_storage(&ui, &store, state_dir, lib_dir);
        player_get_snapshot(player, &snap);
        CHECK(snap.playing && snap.book_pos_ms > 1000, "playing Kindred mid-book before Back");
        int64_t live_ms = snap.book_pos_ms;
        char kindred_key[512];
        snprintf(kindred_key, sizeof(kindred_key), "%s", snap.book->key);

        /* Re-opening the loaded book must keep the live position and keep playing. */
        CHECK(player_load_book(player, (Book *)snap.book) == 0, "reload same book");
        player_get_snapshot(player, &snap);
        CHECK(snap.playing && snap.book_pos_ms == live_ms,
              "reloading the same book keeps playing at the live position (%lld vs %lld)",
              (long long)snap.book_pos_ms, (long long)live_ms);

        const StoreEntry *moby_before = store_get(&store, "Moby-Dick.m4b");
        int64_t moby_ms = moby_before ? moby_before->position_ms : -1;
        CHECK(moby_ms > 0, "Moby-Dick has a saved position (%lld)", (long long)moby_ms);

        AloudInputEvent tap_back = {
            .type = ALOUD_EV_TAP,
            .x = ui.hit_header_left.x + ui.hit_header_left.w / 2,
            .y = ui.hit_header_left.y + ui.hit_header_left.h / 2
        };
        CHECK(ui_handle_event(&ui, tap_back, &rect, &wfm) && ui.screen == UI_SCREEN_LIBRARY,
              "Back returns to Library");

        int krow = -1, mrow = -1;
        for (int i = 0; i < (int)lib.count; i++) {
            if (strcmp(lib.books[i].key, kindred_key) == 0) krow = i;
            if (strcmp(lib.books[i].key, "Moby-Dick.m4b") == 0) mrow = i;
        }
        CHECK(krow >= 0 && mrow >= 0, "both books still listed after Back");
        CHECK(lib.books[krow].saved_book_pos_ms > live_ms - 1500 &&
                  lib.books[krow].saved_book_pos_ms <= live_ms,
              "Library shows Kindred at its live position (%lld vs %lld)",
              (long long)lib.books[krow].saved_book_pos_ms, (long long)live_ms);
        CHECK(lib.books[mrow].saved_file_pos_ms == moby_ms,
              "other books keep their saved position after Back (%lld vs %lld)",
              (long long)lib.books[mrow].saved_file_pos_ms, (long long)moby_ms);
        const StoreEntry *moby_after = store_get(&store, "Moby-Dick.m4b");
        CHECK(moby_after && moby_after->position_ms == moby_ms,
              "Moby-Dick's stored position is untouched");

        /* Store on disk agrees: reload it fresh. */
        static Store disk; /* large: keep off the stack */
        store_init(&disk);
        CHECK(store_load(&disk, state_dir) == 0, "reload positions.tsv");
        const StoreEntry *kd = store_get(&disk, kindred_key);
        CHECK(kd && lib_book_pos_from_file(&lib.books[krow], kd->file_index, kd->position_ms) ==
                        lib.books[krow].saved_book_pos_ms,
              "positions.tsv holds Kindred's live position");

        AloudInputEvent tap_k = {
            .type = ALOUD_EV_TAP,
            .x = ui.hit_list_rows[krow].x + 100,
            .y = ui.hit_list_rows[krow].y + 50
        };
        CHECK(ui_handle_event(&ui, tap_k, &rect, &wfm) && ui.screen == UI_SCREEN_PLAYER,
              "reopen Kindred from Library");
        player_get_snapshot(player, &snap);
        CHECK(snap.book && strcmp(snap.book->key, kindred_key) == 0 &&
                  snap.book_pos_ms == lib.books[krow].saved_book_pos_ms,
              "reopened Kindred at its saved position (%lld)", (long long)snap.book_pos_ms);
    }

    /* Tap Chapters button -> navigates to Chapters screen. */
    AloudInputEvent tap_chaps = {
        .type = ALOUD_EV_TAP,
        .x = ui.hit_header_right.x + 20,
        .y = ui.hit_header_right.y + 20
    };
    CHECK(ui_handle_event(&ui, tap_chaps, &rect, &wfm), "tap chapters");
    CHECK(ui.screen == UI_SCREEN_CHAPTERS, "on Chapters screen");

    /* Test store_set_library_dir persistence and UI folder picker screen. */
    ui_set_storage(&ui, &store, state_dir, lib_dir);
    ui.screen = UI_SCREEN_LIBRARY;
    ui_render_full(&ui, false, &rect, &wfm);
    CHECK(ui.hit_header_center.w > 0, "Library header has Folder picker button");

    AloudInputEvent tap_folder_btn = {
        .type = ALOUD_EV_TAP,
        .x = ui.hit_header_center.x + ui.hit_header_center.w / 2,
        .y = ui.hit_header_center.y + ui.hit_header_center.h / 2
    };
    CHECK(ui_handle_event(&ui, tap_folder_btn, &rect, &wfm), "tap Folder button opens folder picker");
    CHECK(ui.screen == UI_SCREEN_FOLDERS, "navigated to UI_SCREEN_FOLDERS");
    CHECK(ui.folder_count > 0, "folder picker discovered subdirectories in %s", ui.browse_dir);

    /* Tap 'Use Current' or 'Use Folder' to select the library directory and persist it. */
    ui_open_folder_picker(&ui, lib_dir);
    ui_render_full(&ui, false, &rect, &wfm);
    AloudInputEvent tap_use_cur = {
        .type = ALOUD_EV_TAP,
        .x = ui.hit_use_current_dir.x + ui.hit_use_current_dir.w / 2,
        .y = ui.hit_use_current_dir.y + ui.hit_use_current_dir.h / 2
    };
    CHECK(ui_handle_event(&ui, tap_use_cur, &rect, &wfm), "tap Use Current selects folder");
    CHECK(ui.screen == UI_SCREEN_LIBRARY, "returned to Library after choosing folder");
    CHECK(store_get_library_dir(&store) != NULL &&
          strcmp(store_get_library_dir(&store), lib_dir) == 0,
          "selected library_dir persisted in Store (%s)",
          store_get_library_dir(&store) ? store_get_library_dir(&store) : "NULL");

    Store reloaded_store;
    store_init(&reloaded_store);
    CHECK(store_load(&reloaded_store, state_dir) == 0, "reload store with library_dir.txt");
    CHECK(store_get_library_dir(&reloaded_store) != NULL &&
          strcmp(store_get_library_dir(&reloaded_store), lib_dir) == 0,
          "reloaded store preserved library_dir (%s)",
          store_get_library_dir(&reloaded_store) ? store_get_library_dir(&reloaded_store) : "NULL");

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

/* GStreamer backend process handling, using a fake `aloud-sink` that consumes the
 * FIFO and logs every command. Verifies pause really reaches the helper, seeks
 * (flush) replace the helper on the next write, and nothing hangs or leaks. */
static int count_lines_with(const char *path, const char *needle) {
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, needle)) n++;
    }
    fclose(fp);
    return n;
}

static void test_gst_backend_with_fake_helper(const char *fixture_dir) {
    char dir[1024], helper[1100], logp[1100];
    snprintf(dir, sizeof(dir), "%s/tmp/fakehelper", fixture_dir);
    mkdir(dir, 0755);
    snprintf(helper, sizeof(helper), "%s/aloud-sink", dir);
    snprintf(logp, sizeof(logp), "%s/log.txt", dir);
    remove(logp);
    FILE *fp = fopen(helper, "w");
    CHECK(fp != NULL, "write fake helper");
    fprintf(fp,
            "#!/bin/bash\n"
            "trap '' PIPE\n"
            "LOG='%s'\n"
            "while [ $# -gt 0 ]; do case $1 in --fifo) F=$2; shift;; --ctrl-fd) C=$2; shift;;"
            " --status-fd) S=$2; shift;; esac; shift; done\n"
            "echo START >> \"$LOG\"\n"
            "cat \"$F\" > /dev/null & CAT=$!\n"
            "echo OK >&$S\n"
            "while read -r line <&$C; do echo \"$line\" >> \"$LOG\";\n"
            "  case $line in POS) echo 'POS 0' >&$S;; STOP) break;; *) echo OK >&$S;; esac\n"
            "done\n"
            "kill $CAT 2>/dev/null; echo EXIT >> \"$LOG\"\n",
            logp);
    fclose(fp);
    chmod(helper, 0755);

    AudioOut *ao = NULL;
    char err[256] = {0};
    CHECK(audio_out_open_gst(&ao, dir, err, sizeof(err)) == 0 && ao, "open gst backend (%s)", err);
    static int16_t pcm[4096 * 2];
    for (int i = 0; i < 20; i++) {
        CHECK(ao->write_s16le(ao, pcm, 4096) == 4096, "write PCM to helper");
    }
    /* Pause ends the stream at once (the mixer daemon would otherwise play seconds of
     * buffered audio); resume starts a fresh helper on the next write. */
    CHECK(ao->set_paused(ao, true) == 0, "pause");
    usleep(300000);
    CHECK(count_lines_with(logp, "STOP") == 1 && count_lines_with(logp, "EXIT") == 1,
          "pause stops the helper (stops=%d exits=%d)", count_lines_with(logp, "STOP"),
          count_lines_with(logp, "EXIT"));
    CHECK(ao->set_paused(ao, false) == 0, "resume");
    CHECK(ao->write_s16le(ao, pcm, 4096) == 4096, "write after resume");
    usleep(300000);
    CHECK(count_lines_with(logp, "START") == 2, "resume started a fresh helper");

    /* Seek: flush ends the stream; the next write starts a new one. */
    CHECK(ao->flush(ao) == 0, "flush");
    CHECK(ao->set_paused(ao, false) == 0, "resume during pending restart is a no-op");
    CHECK(ao->write_s16le(ao, pcm, 4096) == 4096, "write after flush");
    usleep(300000);
    CHECK(count_lines_with(logp, "START") == 3, "flush replaced the helper (starts=%d)",
          count_lines_with(logp, "START"));
    CHECK(count_lines_with(logp, "STOP") == 2 && count_lines_with(logp, "EXIT") == 2,
          "old helper was stopped exactly once and exited");
    (void)ao->delay_ms(ao);

    audio_out_close(ao);
    usleep(200000);
    CHECK(count_lines_with(logp, "EXIT") == 3, "close stops the current helper");
    struct stat st;
    CHECK(stat("/tmp/aloud_audio.fifo", &st) != 0, "FIFO removed on close");

    /* Helper present but not executable as a program (what we saw on device): the
     * backend must notice the gst-launch fallback, and pause/flush/close must still
     * work and never hang. */
    fp = fopen(helper, "w");
    CHECK(fp != NULL, "write broken helper");
    fputs("\x7f" "ELF-not-really", fp);
    fclose(fp);
    chmod(helper, 0755);
    ao = NULL;
    CHECK(audio_out_open_gst(&ao, dir, err, sizeof(err)) == 0 && ao, "open with broken helper");
    CHECK(ao->set_paused(ao, true) == 0 && ao->flush(ao) == 0 && ao->set_paused(ao, false) == 0,
          "pause/flush/resume on fallback path");
    audio_out_close(ao);
    CHECK(stat("/tmp/aloud_audio.fifo", &st) != 0, "FIFO removed after fallback close");
}

int main(int argc, char **argv) {
    const char *fixture_dir = (argc > 1) ? argv[1] : "build/test-fixtures";
    const char *font_dir = (argc > 2) ? argv[2] : "third_party/fonts";
    signal(SIGPIPE, SIG_IGN); /* as src/main.c does: a dead helper must not kill us */

    test_natural_sort();
    test_store_persistence_and_recovery(fixture_dir);
    test_decoder_m4b_and_corrupt(fixture_dir);
    test_mp4lite_reader(fixture_dir);
    test_library_and_player_engine(fixture_dir);
    test_power_and_touch_decoders();
    test_gst_backend_with_fake_helper(fixture_dir);
    test_ui_and_golden_screens(fixture_dir, font_dir);

    if (g_tests_failed > 0) {
        fprintf(stderr, "FAILED: %d / %d checks failed\n", g_tests_failed, g_tests_run);
        return 1;
    }
    printf("PASS: all %d checks passed.\n", g_tests_run);
    return 0;
}
