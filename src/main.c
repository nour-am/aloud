#define _POSIX_C_SOURCE 200809L
#include "audio_out.h"
#include "canvas.h"
#include "decoder.h"
#include "library.h"
#include "platform.h"
#include "player.h"
#include "power.h"
#include "store.h"
#include "text.h"
#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_quit_signal = 0;
static int g_sig_pipe[2] = {-1, -1};

static void on_signal(int sig) {
    (void)sig;
    g_quit_signal = 1;
    if (g_sig_pipe[1] >= 0) {
        uint8_t b = 1;
        (void)write(g_sig_pipe[1], &b, 1);
    }
}

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)(ts.tv_nsec / 1000000LL);
}

/* Standalone on-device 440 Hz tone + screensaver + deferSuspend verifier (Roadmap Step 1). */
static int run_tone_test(const char *helper_dir, int seconds) {
    if (seconds <= 0) seconds = 10;
    printf("Aloud audio & power test: playing 440 Hz tone for %d s...\n", seconds);
    printf("Press the Kindle power button during playback to test screensaver + deferSuspend.\n");
    fflush(stdout);

    PowerMonitor pm;
    power_init(&pm);
    (void)power_start(&pm);

    AudioOut *ao = NULL;
    char err_msg[256] = {0};
    int rc = audio_out_open_kindle(&ao, helper_dir, err_msg, sizeof(err_msg));
    if (rc < 0 || !ao) {
        fprintf(stderr, "Audio open failed (%d): %s\n", rc, err_msg[0] ? err_msg : "unknown");
        power_stop(&pm);
        return 1;
    }

    const int rate = DECODER_OUT_RATE;
    const int chunk = 1024;
    int16_t pcm[1024 * DECODER_OUT_CHANNELS];
    int total_frames = rate * seconds;
    int written = 0;
    double phase = 0.0;
    const double step = (2.0 * 3.14159265358979323846 * 440.0) / (double)rate;

    while (written < total_frames && !g_quit_signal) {
        int n = total_frames - written;
        if (n > chunk) n = chunk;
        for (int i = 0; i < n; i++) {
            int16_t s = (int16_t)lround(sin(phase) * 10000.0);
            pcm[i * 2] = s;
            pcm[i * 2 + 1] = s;
            phase += step;
        }
        if (ao->write_s16le(ao, pcm, n) < 0) {
            fprintf(stderr, "Audio write interrupted\n");
            break;
        }
        written += n;

        bool repaint = false;
        (void)power_handle_readable(&pm, true, (int64_t)(seconds * 1000), &repaint);
    }

    audio_out_close(ao);
    power_stop(&pm);
    printf("Aloud audio test finished (%d frames written).\n", written);
    return 0;
}

/* Render all reference screens at a specific resolution/DPI for simulator & golden tests. */
static int export_profile_screens(const char *prefix, int w, int h, int dpi,
                                  const char *lib_dir, const char *state_dir,
                                  const char *font_dir, const char *out_dir) {
    mkdir(out_dir, 0755);

    Store store;
    store_init(&store);
    (void)store_load(&store, state_dir);

    Library lib;
    lib_init(&lib);
    (void)lib_scan(&lib, lib_dir, state_dir, &store);

    TextSystem *ts = NULL;
    int rc = text_init(&ts, font_dir);
    if (rc < 0) {
        lib_free(&lib);
        return rc;
    }

    Canvas canvas;
    if (canvas_init(&canvas, w, h, dpi) < 0) {
        text_free(ts);
        lib_free(&lib);
        return -ENOMEM;
    }

    Player *player = NULL;
    if (player_create(&player, &store, state_dir, NULL) < 0) {
        canvas_free(&canvas);
        text_free(ts);
        lib_free(&lib);
        return -ENOMEM;
    }
    AudioOut *sim_ao = NULL;
    if (audio_out_open_sim(&sim_ao, NULL) == 0) {
        player_set_sim_audio(player, sim_ao);
    }

    Ui ui;
    ui_init(&ui, &canvas, ts, &lib, player, true);

    char path[1024];
    AloudRect rect;
    AloudWaveform wfm;

    /* 1. Library screen (populated). */
    ui.screen = UI_SCREEN_LIBRARY;
    ui.sheet = UI_SHEET_NONE;
    ui_render_full(&ui, true, &rect, &wfm);
    snprintf(path, sizeof(path), "%s/%s_library.png", out_dir, prefix);
    platform_write_png(&canvas, path);

    /* Load the first book for Player / Chapters / Sheets screens. */
    if (lib.count > 0) {
        player_load_book(player, &lib.books[0]);
        player_set_speed(player, 125);
        player_set_sleep_timer(player, 30);
        /* Seek into Chapter 2 so scrubber and elapsed/remaining look realistic. */
        if (lib.books[0].chapter_count > 1) {
            int64_t target = lib.books[0].chapters[1].start_book_ms +
                             (lib.books[0].chapters[1].duration_ms * 38) / 100;
            player_seek_book_ms(player, target);
        }
    }

    /* 2. Player screen (Now Playing). */
    ui.screen = UI_SCREEN_PLAYER;
    ui.sheet = UI_SHEET_NONE;
    ui_render_full(&ui, true, &rect, &wfm);
    snprintf(path, sizeof(path), "%s/%s_player.png", out_dir, prefix);
    platform_write_png(&canvas, path);

    /* 3. Chapters screen. */
    ui.screen = UI_SCREEN_CHAPTERS;
    ui.sheet = UI_SHEET_NONE;
    ui_render_full(&ui, true, &rect, &wfm);
    snprintf(path, sizeof(path), "%s/%s_chapters.png", out_dir, prefix);
    platform_write_png(&canvas, path);

    /* 4. Speed bottom sheet. */
    ui.screen = UI_SCREEN_PLAYER;
    ui.sheet = UI_SHEET_SPEED;
    ui_render_full(&ui, false, &rect, &wfm);
    snprintf(path, sizeof(path), "%s/%s_sheet_speed.png", out_dir, prefix);
    platform_write_png(&canvas, path);

    /* 5. Sleep bottom sheet. */
    ui.screen = UI_SCREEN_PLAYER;
    ui.sheet = UI_SHEET_SLEEP;
    ui_render_full(&ui, false, &rect, &wfm);
    snprintf(path, sizeof(path), "%s/%s_sheet_sleep.png", out_dir, prefix);
    platform_write_png(&canvas, path);

    /* 6. Empty library screen. */
    Library empty_lib;
    lib_init(&empty_lib);
    ui.library = &empty_lib;
    ui.screen = UI_SCREEN_LIBRARY;
    ui.sheet = UI_SHEET_NONE;
    ui_render_full(&ui, false, &rect, &wfm);
    snprintf(path, sizeof(path), "%s/%s_empty.png", out_dir, prefix);
    platform_write_png(&canvas, path);

    ui_free(&ui);
    player_destroy(player);
    canvas_free(&canvas);
    text_free(ts);
    lib_free(&lib);
    return 0;
}

int main(int argc, char **argv) {
    const char *library_dir = "/mnt/us/audiobooks";
    const char *state_dir = "/mnt/us/extensions/aloud/state";
    const char *font_dir = "/mnt/us/extensions/aloud/fonts";
    const char *helper_dir = "/mnt/us/extensions/aloud/bin";
    const char *export_screens_dir = NULL;
    bool tone_test = false;
    int tone_seconds = 10;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--library") == 0 && i + 1 < argc) {
            library_dir = argv[++i];
        } else if (strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
            state_dir = argv[++i];
        } else if (strcmp(argv[i], "--fonts") == 0 && i + 1 < argc) {
            font_dir = argv[++i];
        } else if (strcmp(argv[i], "--helpers") == 0 && i + 1 < argc) {
            helper_dir = argv[++i];
        } else if (strcmp(argv[i], "--export-screens") == 0 && i + 1 < argc) {
            export_screens_dir = argv[++i];
        } else if (strcmp(argv[i], "--tone-test") == 0) {
            tone_test = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                tone_seconds = atoi(argv[++i]);
            }
        }
    }

    if (pipe(g_sig_pipe) == 0) {
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(g_sig_pipe[i], F_GETFL, 0);
            if (fl >= 0) (void)fcntl(g_sig_pipe[i], F_SETFL, fl | O_NONBLOCK);
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (tone_test) {
        return run_tone_test(helper_dir, tone_seconds);
    }

    if (export_screens_dir) {
        /* Export PW5 portrait (1236x1648 @ 300), KT4 portrait (600x800 @ 167),
         * and PW5 landscape (1648x1236 @ 300) per AGENTS.md Roadmap Step 3. */
        int rc = export_profile_screens("pw5", 1236, 1648, 300,
                                        library_dir, state_dir, font_dir, export_screens_dir);
        if (rc == 0) {
            rc = export_profile_screens("kt4", 600, 800, 167,
                                        library_dir, state_dir, font_dir, export_screens_dir);
        }
        if (rc == 0) {
            rc = export_profile_screens("pw5_landscape", 1648, 1236, 300,
                                        library_dir, state_dir, font_dir, export_screens_dir);
        }
        return (rc == 0) ? 0 : 1;
    }

    Platform *plat = NULL;
    if (platform_init(&plat, 1236, 1648, 300) < 0) {
        fprintf(stderr, "aloud: failed to initialize display platform\n");
        return 1;
    }

    TextSystem *ts = NULL;
    if (text_init(&ts, font_dir) < 0) {
        fprintf(stderr, "aloud: failed to load fonts from %s\n", font_dir);
        platform_free(plat);
        return 1;
    }

    Canvas canvas;
    if (canvas_init(&canvas, platform_width(plat), platform_height(plat), platform_dpi(plat)) < 0) {
        text_free(ts);
        platform_free(plat);
        return 1;
    }

    Store store;
    store_init(&store);
    (void)store_load(&store, state_dir);

    Library lib;
    lib_init(&lib);
    (void)lib_scan(&lib, library_dir, state_dir, &store);

    Player *player = NULL;
    if (player_create(&player, &store, state_dir, helper_dir) < 0) {
        lib_free(&lib);
        canvas_free(&canvas);
        text_free(ts);
        platform_free(plat);
        return 1;
    }
    (void)player_start_thread(player);

    PowerMonitor power;
    power_init(&power);
    (void)power_start(&power);

    Ui ui;
    ui_init(&ui, &canvas, ts, &lib, player, platform_has_volume_control(plat));

    AloudRect init_rect;
    AloudWaveform init_wfm;
    ui_render_full(&ui, true, &init_rect, &init_wfm);
    platform_refresh(plat, &canvas, init_rect, init_wfm);

    int64_t last_loop_ms = monotonic_ms();
    bool has_been_playing = false;

    /* Main poll() event loop: zero wakeups when paused and idle (D7). */
    while (!g_quit_signal && !ui.quit_requested) {
        struct pollfd pfds[16];
        int nfds = 0;

        int input_fds[8];
        int n_in = platform_get_input_fds(plat, input_fds, 8);
        for (int i = 0; i < n_in; i++) {
            pfds[nfds++] = (struct pollfd){.fd = input_fds[i], .events = POLLIN};
        }

        int p_notify_fd = player_notify_fd(player);
        int notify_slot = -1;
        if (p_notify_fd >= 0) {
            notify_slot = nfds;
            pfds[nfds++] = (struct pollfd){.fd = p_notify_fd, .events = POLLIN};
        }

        int power_slot = -1;
        if (power.event_fd >= 0) {
            power_slot = nfds;
            pfds[nfds++] = (struct pollfd){.fd = power.event_fd, .events = POLLIN};
        }

        int sig_slot = -1;
        if (g_sig_pipe[0] >= 0) {
            sig_slot = nfds;
            pfds[nfds++] = (struct pollfd){.fd = g_sig_pipe[0], .events = POLLIN};
        }

        PlayerSnapshot snap;
        player_get_snapshot(player, &snap);
        power_on_playback_changed(&power, snap.playing);
        if (snap.playing) {
            has_been_playing = true;
        }

        /* Compute poll timeout:
         * - While playing: wake once per minute (60,000 ms).
         * - Just paused while audio helper may still be open: wake once after 2 min to close it.
         * - Otherwise paused & idle: timeout = -1 (infinite, zero CPU wakeups per D7). */
        int timeout_ms = -1;
        if (snap.playing) {
            timeout_ms = 60000;
        } else if (has_been_playing) {
            timeout_ms = PLAYER_IDLE_SINK_CLOSE_MS;
        }

        int pr = poll(pfds, (nfds_t)nfds, timeout_ms);
        int64_t now_ms = monotonic_ms();
        int64_t elapsed_ms = now_ms - last_loop_ms;
        last_loop_ms = now_ms;

        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pr == 0 && !snap.playing && has_been_playing) {
            player_housekeeping_tick(player, PLAYER_IDLE_SINK_CLOSE_MS + 1000);
            has_been_playing = false;
            continue;
        }

        player_housekeeping_tick(player, elapsed_ms);

        if (sig_slot >= 0 && (pfds[sig_slot].revents & POLLIN)) {
            break;
        }

        if (power_slot >= 0 && (pfds[power_slot].revents & (POLLIN | POLLHUP))) {
            bool wake_repaint = false;
            player_get_snapshot(player, &snap);
            (void)power_handle_readable(&power, snap.playing, snap.sleep_rem_ms, &wake_repaint);
            if (wake_repaint && !power.in_screensaver) {
                AloudRect r;
                AloudWaveform w;
                ui_render_full(&ui, true, &r, &w);
                platform_refresh(plat, &canvas, r, ALOUD_WFM_GC16_FLASH);
            }
        }

        if (notify_slot >= 0 && (pfds[notify_slot].revents & POLLIN)) {
            player_clear_notify(player);
            if (!power.in_screensaver) {
                AloudRect r;
                AloudWaveform w;
                if (ui_on_player_tick(&ui, &r, &w)) {
                    platform_refresh(plat, &canvas, r, w);
                }
            }
        }

        AloudInputEvent ev;
        while (platform_poll_event(plat, &ev)) {
            AloudRect r;
            AloudWaveform w;
            if (ui_handle_event(&ui, ev, &r, &w) && !power.in_screensaver) {
                platform_refresh(plat, &canvas, r, w);
            }
            if (ui.quit_requested) break;
        }
    }

    /* Always persist the current position before exiting (D8, D9). */
    (void)player_save_position_now(player);

    ui_free(&ui);
    power_stop(&power);
    player_destroy(player);
    lib_free(&lib);
    canvas_free(&canvas);
    text_free(ts);
    platform_free(plat);
    if (g_sig_pipe[0] >= 0) close(g_sig_pipe[0]);
    if (g_sig_pipe[1] >= 0) close(g_sig_pipe[1]);
    return 0;
}
