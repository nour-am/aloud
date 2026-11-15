#define _POSIX_C_SOURCE 200809L
/* Scripted demo for the README video (simulator only, never built for Kindle).
 *
 * Drives the real UI and player through a short tour: open a book, play, change
 * speed, set a sleep timer, jump to a chapter, pause. Audio goes through the real
 * decoder, sonic and volume path into <out>/audio.wav. Every screen update is
 * written as a PNG and logged with its time on the audio clock, so
 * tools/make-demo-video.sh can rebuild the exact timeline with ffmpeg:
 *
 *   F <ms> <png> <waveform>   a screen update (waveform: du|gl16|gc16)
 *   T <ms> <x> <y>            a tap, drawn as a touch marker
 *   A <ms>                    playback starts: audio.wav begins here
 *   E <ms>                    end of the video
 */
#include "demo.h"

#include "audio_out.h"
#include "canvas.h"
#include "library.h"
#include "platform.h"
#include "player.h"
#include "store.h"
#include "text.h"
#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define DEMO_RATE 44100

typedef struct {
    Ui *ui;
    Player *player;
    Canvas *canvas;
    const char *out_dir;
    FILE *log;
    int64_t t_ms;   /* video clock */
    int frame_no;
} Demo;

static const char *wfm_tag(AloudWaveform w) {
    switch (w) {
        case ALOUD_WFM_DU: return "du";
        case ALOUD_WFM_GL16: return "gl16";
        case ALOUD_WFM_GC16_FLASH: return "gc16";
        default: return "none";
    }
}

static int frame(Demo *d, AloudWaveform w) {
    char name[64], path[1024];
    snprintf(name, sizeof(name), "f%04d.png", d->frame_no++);
    snprintf(path, sizeof(path), "%s/%s", d->out_dir, name);
    if (platform_write_png(d->canvas, path) != 0) return -EIO;
    fprintf(d->log, "F %lld %s %s\n", (long long)d->t_ms, name, wfm_tag(w));
    return 0;
}

static bool is_playing(Demo *d) {
    PlayerSnapshot snap;
    player_get_snapshot(d->player, &snap);
    return snap.playing;
}

/* Let ms pass. While playing, render that much audio in 250 ms steps and let the UI
 * redraw its clock exactly as the device main loop would. */
static void hold(Demo *d, int ms) {
    const int step_ms = 250;
    for (int done = 0; done < ms; done += step_ms) {
        int chunk = (ms - done < step_ms) ? ms - done : step_ms;
        if (is_playing(d)) {
            int want = (int)((int64_t)chunk * DEMO_RATE / 1000);
            int got = 0, idle = 0;
            while (got < want && idle < 64) {
                int n = player_step_frames_sync(d->player, want - got);
                if (n > 0) {
                    got += n;
                    idle = 0;
                } else if (n == 0) {
                    idle++;
                } else {
                    break;
                }
            }
        }
        d->t_ms += chunk;
        AloudRect r;
        AloudWaveform w;
        if (ui_on_player_tick(d->ui, &r, &w)) (void)frame(d, w);
    }
}

static void tap_rect(Demo *d, AloudRect r) {
    int x = r.x + r.w / 2, y = r.y + r.h / 2;
    bool was_playing = is_playing(d);
    fprintf(d->log, "T %lld %d %d\n", (long long)d->t_ms, x, y);
    hold(d, 350); /* the finger lands before the panel reacts */
    AloudRect out;
    AloudWaveform w = ALOUD_WFM_GL16;
    AloudInputEvent ev = { .type = ALOUD_EV_TAP, .x = x, .y = y };
    (void)ui_handle_event(d->ui, ev, &out, &w);
    if (!was_playing && is_playing(d)) {
        fprintf(d->log, "A %lld\n", (long long)d->t_ms);
    }
    (void)frame(d, w);
}

int demo_run(const char *lib_dir, const char *state_dir, const char *font_dir,
             const char *out_dir) {
    mkdir(out_dir, 0755);
    char path[1024];
    snprintf(path, sizeof(path), "%s/timeline.txt", out_dir);
    FILE *log = fopen(path, "w");
    if (!log) return -errno;

    Store store;
    store_init(&store);
    (void)store_load(&store, state_dir);
    Library lib;
    lib_init(&lib);
    (void)lib_scan(&lib, lib_dir, state_dir, &store);

    TextSystem *ts = NULL;
    Canvas canvas;
    Player *player = NULL;
    AudioOut *ao = NULL;
    int rc = text_init(&ts, font_dir);
    if (rc == 0) rc = canvas_init(&canvas, 1236, 1648, 300);
    if (rc == 0) rc = player_create(&player, &store, state_dir, NULL);
    if (rc == 0) {
        snprintf(path, sizeof(path), "%s/audio.wav", out_dir);
        rc = audio_out_open_sim(&ao, path);
    }
    if (rc != 0 || lib.count == 0) {
        fprintf(stderr, "aloud: demo setup failed (%d, %zu books)\n", rc, lib.count);
        fclose(log);
        return (rc != 0) ? rc : -ENOENT;
    }
    player_set_sim_audio(player, ao);

    Ui ui;
    ui_init(&ui, &canvas, ts, &lib, player, true);
    Demo d = { &ui, player, &canvas, out_dir, log, 0, 0 };
    AloudRect r;
    AloudWaveform w;

    ui_render_full(&ui, true, &r, &w);
    (void)frame(&d, ALOUD_WFM_GC16_FLASH);
    hold(&d, 1800);

    tap_rect(&d, ui.hit_list_rows[0]);      /* open the most recent book */
    hold(&d, 1200);
    tap_rect(&d, ui.hit_play_pause);        /* play */
    hold(&d, 9000);

    tap_rect(&d, ui.hit_speed_btn);         /* speed sheet */
    hold(&d, 900);
    tap_rect(&d, ui.hit_sheet_opts[4]);     /* 1.5x, pitch preserved */
    if (ui.sheet != UI_SHEET_NONE) {
        hold(&d, 600);
        tap_rect(&d, ui.hit_sheet_close);
    }
    hold(&d, 7000);

    tap_rect(&d, ui.hit_sleep_btn);         /* sleep timer sheet */
    hold(&d, 900);
    tap_rect(&d, ui.hit_sheet_opts[3]);     /* 30 min */
    if (ui.sheet != UI_SHEET_NONE) {
        hold(&d, 600);
        tap_rect(&d, ui.hit_sheet_close);
    }
    hold(&d, 2500);

    tap_rect(&d, ui.hit_header_right);      /* chapters */
    hold(&d, 2200);
    tap_rect(&d, ui.hit_list_rows[1]);      /* jump to chapter 2 */
    hold(&d, 6000);

    tap_rect(&d, ui.hit_play_pause);        /* pause */
    hold(&d, 1500);
    fprintf(log, "E %lld\n", (long long)d.t_ms);

    ui_free(&ui);
    player_destroy(player);
    canvas_free(&canvas);
    text_free(ts);
    lib_free(&lib);
    fclose(log);
    return 0;
}
