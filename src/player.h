#ifndef ALOUD_PLAYER_H
#define ALOUD_PLAYER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_out.h"
#include "decoder.h"
#include "library.h"
#include "store.h"

#define SLEEP_OFF 0
#define SLEEP_END_OF_CHAPTER (-1)
#define PLAYER_CHUNK_FRAMES 2048
#define PLAYER_FADE_WINDOW_MS 10000
#define PLAYER_FADE_IN_FRAMES 882 /* 20 ms at 44.1 kHz */
#define PLAYER_IDLE_SINK_CLOSE_MS 120000
#define PLAYER_RESUME_REWIND_MS 2000
#define PLAYER_AUTOSAVE_INTERVAL_MS 60000

typedef struct {
    const Book *book;
    int file_index;
    int64_t file_pos_ms;
    int64_t book_pos_ms;
    int chapter_index;
    int64_t chapter_pos_ms;
    int64_t chapter_dur_ms;

    bool playing;
    bool finished;
    int speed_x100;       /* 75..300 */
    int volume_pct;       /* 0..150 (default 100) */
    int sleep_mode;       /* SLEEP_OFF (0), >0 minutes, or SLEEP_END_OF_CHAPTER (-1) */
    int64_t sleep_rem_ms; /* >0 when sleep timer is active, 0 when off */
    char toast[256];
} PlayerSnapshot;

typedef struct Player Player;

int player_create(Player **out, Store *store, const char *state_dir, const char *helper_dir);
void player_destroy(Player *p);

/* Configure a custom audio backend factory (e.g. simulator WAV/null sink).
 * Takes ownership of ao if non-NULL. */
void player_set_sim_audio(Player *p, AudioOut *ao);

/* Start/stop the background audio thread (for live runtime; unit tests can call
 * player_step_frames_sync directly without spawning a thread). */
int player_start_thread(Player *p);
void player_stop_thread(Player *p);

/* File descriptor that becomes readable when the audio thread changes state,
 * finishes a chapter/book, triggers a toast, or hits a minute/sleep milestone. */
int player_notify_fd(const Player *p);
void player_clear_notify(Player *p);

/* Load a book and position at its saved offset (does not auto-play). */
int player_load_book(Player *p, Book *book);

/* Transport controls (each triggers atomic position save when appropriate per D8). */
int player_play(Player *p);
int player_pause(Player *p);
int player_toggle_play(Player *p);
int player_seek_book_ms(Player *p, int64_t target_book_ms);
int player_seek_relative_ms(Player *p, int64_t delta_ms);
int player_seek_chapter(Player *p, int chapter_index);
int player_prev_chapter(Player *p);
int player_next_chapter(Player *p);

int player_set_speed(Player *p, int speed_x100);
int player_set_volume(Player *p, int volume_pct);
int player_set_sleep_timer(Player *p, int sleep_mode); /* 0, 5, 15, 30, 45, 60, or -1 */

/* Advance idle housekeeping (closes audio helper after 2 min paused, autosaves
 * every 60 s while playing). */
void player_housekeeping_tick(Player *p, int64_t elapsed_wall_ms);

/* Synchronous decode+sonic+output step for deterministic unit testing and simulator.
 * Processes up to max_out_frames output frames. Returns >0 frames written, 0 on paused/EOF, <0 on error. */
int player_step_frames_sync(Player *p, int max_out_frames);

void player_get_snapshot(Player *p, PlayerSnapshot *snap);
void player_clear_toast(Player *p);
int player_save_position_now(Player *p);
/* Save and drop the current book. Must be called before the Library's Book array is
 * rebuilt (lib_scan/lib_free), since the player holds a pointer into it. */
void player_unload_book(Player *p);

#endif /* ALOUD_PLAYER_H */
