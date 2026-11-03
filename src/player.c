#define _POSIX_C_SOURCE 200809L
#include "player.h"

#include "sonic.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct Player {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    bool thread_running;
    bool quit_requested;

    int notify_pipe[2];

    Store *store;
    char state_dir[1024];
    char helper_dir[1024];

    Book *book;
    int file_index;
    int64_t file_pos_ms;
    int64_t book_pos_ms;
    int last_chapter_index;

    Decoder *dec;
    sonicStream sonic;
    AudioOut *ao;
    bool using_sim_ao;
    bool ao_busy;

    bool playing;
    bool paused_once; /* True after a pause so resume rewinds 2 s (D6) */
    bool finished;

    int speed_x100;
    int volume_pct;

    int sleep_mode;       /* SLEEP_OFF, >0 minutes, or SLEEP_END_OF_CHAPTER */
    int64_t sleep_rem_ms; /* Remaining wall-clock ms until sleep pause */

    int64_t paused_idle_ms;
    int64_t since_last_save_ms;
    int last_notified_second;
    bool dirty_save;
    bool just_seeked;
    int fade_in_left; /* frames of start-of-audio ramp still to apply */

    char toast[256];

    /* Preallocated hot-path buffers (zero malloc in audio thread per Conventions). */
    int16_t decode_buf[PLAYER_CHUNK_FRAMES * DECODER_OUT_CHANNELS];
    int16_t sonic_buf[PLAYER_CHUNK_FRAMES * 4 * DECODER_OUT_CHANNELS];
    int16_t write_buf[PLAYER_CHUNK_FRAMES * DECODER_OUT_CHANNELS];
};

static int64_t now_epoch_sec(void) {
    return (int64_t)time(NULL);
}

static void notify_main_locked(Player *p) {
    if (p->notify_pipe[1] >= 0) {
        uint8_t b = 1;
        (void)write(p->notify_pipe[1], &b, 1);
    }
}

/* Book position the listener has actually heard: the decoder runs ahead of the
 * speaker by the sink's buffered audio (FIFO + helper queue), so while playing we
 * subtract that delay. Saving the decode position instead would skip that audio
 * after a restart. */
static int64_t heard_book_pos_locked(const Player *p) {
    int64_t pos = p->book_pos_ms;
    if (p->playing && !p->just_seeked && p->ao && p->ao->delay_ms) {
        int64_t delay_book = (p->ao->delay_ms(p->ao) * p->speed_x100) / 100;
        pos = (pos > delay_book) ? pos - delay_book : 0;
    }
    return pos;
}

static void update_in_memory_position_locked(Player *p) {
    if (!p->book || !p->store) return;
    int64_t epoch = now_epoch_sec();
    int64_t book_ms = heard_book_pos_locked(p);
    int file_index = p->file_index;
    int64_t file_ms = p->file_pos_ms;
    if (book_ms != p->book_pos_ms) {
        lib_file_pos_from_book(p->book, book_ms, &file_index, &file_ms);
    }
    store_set(p->store, p->book->key, file_index, file_ms,
              p->speed_x100, epoch, p->finished);
    p->book->saved_file_index = file_index;
    p->book->saved_file_pos_ms = file_ms;
    p->book->saved_book_pos_ms = book_ms;
    p->book->saved_speed_x100 = p->speed_x100;
    p->book->updated_epoch = epoch;
    p->book->finished = p->finished;
}

static void save_position_locked(Player *p) {
    if (!p->book || !p->store) return;
    update_in_memory_position_locked(p);
    if (p->state_dir[0]) {
        (void)store_save(p->store, p->state_dir);
    }
    p->dirty_save = false;
    p->since_last_save_ms = 0;
}

static void mark_position_dirty_locked(Player *p) {
    if (!p->book || !p->store) return;
    update_in_memory_position_locked(p);
    if (p->playing && p->thread_running) {
        /* While playing, coalesce bursts of scrubber/skip taps: the audio thread
         * persists within 2 s, so the UI thread never blocks on a FAT32 fsync per tap. */
        p->dirty_save = true;
        p->since_last_save_ms = 0;
    } else {
        /* Paused (or no audio thread): nothing else will flush soon, so persist now. */
        save_position_locked(p);
    }
}

static void update_book_pos_and_chapter_locked(Player *p) {
    if (!p->book) return;
    p->book_pos_ms = lib_book_pos_from_file(p->book, p->file_index, p->file_pos_ms);
    int ch = lib_chapter_at_pos(p->book, p->book_pos_ms);
    bool crossed_chapter = (p->last_chapter_index >= 0 && ch != p->last_chapter_index);
    if (crossed_chapter) {
        /* Chapter boundary crossed: persist listening position (D8). */
        p->last_chapter_index = ch;
        save_position_locked(p);
    } else {
        p->last_chapter_index = ch;
    }

    if (p->sleep_mode == SLEEP_END_OF_CHAPTER && p->book->chapter_count > 0) {
        if (crossed_chapter && p->playing) {
            p->sleep_rem_ms = 0;
            return;
        }
        int cidx = (ch >= 0 && (size_t)ch < p->book->chapter_count) ? ch : 0;
        int64_t ch_end = p->book->chapters[cidx].start_book_ms + p->book->chapters[cidx].duration_ms;
        int64_t rem_book_ms = ch_end - p->book_pos_ms;
        if (rem_book_ms < 0) rem_book_ms = 0;
        /* Convert remaining book time to wall-clock time at current speed. */
        p->sleep_rem_ms = (rem_book_ms * 100) / (p->speed_x100 > 0 ? p->speed_x100 : 100);
    }
}

static int open_current_file_locked(Player *p, int64_t seek_ms) {
    if (p->dec) {
        decoder_close(p->dec);
        p->dec = NULL;
    }
    if (!p->book || p->book->file_count == 0) {
        return -EINVAL;
    }
    if (p->file_index < 0) p->file_index = 0;
    if ((size_t)p->file_index >= p->book->file_count) {
        p->file_index = (int)p->book->file_count - 1;
    }

    const BookFile *bf = &p->book->files[p->file_index];
    int rc = decoder_open(&p->dec, bf->fullpath);
    if (rc < 0) {
        snprintf(p->toast, sizeof(p->toast),
                 "Can't play '%s' \u2014 skipped", bf->title[0] ? bf->title : "file");
        return rc;
    }
    if (seek_ms > 0) {
        (void)decoder_seek_ms(p->dec, seek_ms);
    }
    p->file_pos_ms = decoder_position_ms(p->dec);
    if (p->sonic) {
        sonicFlushStream(p->sonic);
        while (sonicSamplesAvailable(p->sonic) > 0) {
            sonicReadShortFromStream(p->sonic, p->sonic_buf, PLAYER_CHUNK_FRAMES);
        }
    }
    update_book_pos_and_chapter_locked(p);
    return 0;
}

static int ensure_audio_out_locked(Player *p) {
    if (p->ao) return 0;
    char helper_dir[1024];
    snprintf(helper_dir, sizeof(helper_dir), "%s", p->helper_dir);
    p->ao_busy = true;
    pthread_mutex_unlock(&p->mu);

    AudioOut *new_ao = NULL;
    char err_msg[256] = {0};
    int rc = audio_out_open_kindle(&new_ao, helper_dir, err_msg, sizeof(err_msg));

    pthread_mutex_lock(&p->mu);
    p->ao_busy = false;
    if (rc < 0 || !new_ao) {
        if (err_msg[0]) {
            snprintf(p->toast, sizeof(p->toast), "%s", err_msg);
        } else {
            snprintf(p->toast, sizeof(p->toast), "No audio output \u2014 connect Bluetooth headphones");
        }
        return (rc < 0) ? rc : -ENOTCONN;
    }
    if (p->ao) {
        audio_out_close(new_ao);
        return 0;
    }
    p->ao = new_ao;
    return 0;
}

/* Core decode -> sonic -> fade -> output step (called with p->mu held). */
static int step_frames_locked(Player *p, int max_out_frames) {
    if (!p->playing || !p->book) {
        return 0;
    }
    if (!p->dec) {
        if (open_current_file_locked(p, p->file_pos_ms) < 0) {
            p->playing = false;
            notify_main_locked(p);
            return -EIO;
        }
    }
    if (!p->ao) {
        if (ensure_audio_out_locked(p) < 0) {
            p->playing = false;
            notify_main_locked(p);
            return -ENOTCONN;
        }
        if (!p->playing) {
            return 0;
        }
    }

    if (max_out_frames <= 0 || max_out_frames > PLAYER_CHUNK_FRAMES) {
        max_out_frames = PLAYER_CHUNK_FRAMES;
    }

    int avail = sonicSamplesAvailable(p->sonic);
    while (avail < max_out_frames) {
        int dec_frames = decoder_read_frames(p->dec, p->decode_buf, PLAYER_CHUNK_FRAMES);
        if (dec_frames > 0) {
            sonicWriteShortToStream(p->sonic, p->decode_buf, dec_frames);
            p->file_pos_ms = decoder_position_ms(p->dec);
            avail = sonicSamplesAvailable(p->sonic);
        } else {
            /* EOF or corrupt tail on current file: flush sonic and advance to next file. */
            sonicFlushStream(p->sonic);
            avail = sonicSamplesAvailable(p->sonic);
            if (avail > 0) {
                break;
            }
            if ((size_t)(p->file_index + 1) < p->book->file_count) {
                p->file_index++;
                p->file_pos_ms = 0;
                if (p->sleep_mode == SLEEP_END_OF_CHAPTER) {
                    (void)open_current_file_locked(p, 0);
                    p->sleep_rem_ms = 0;
                    p->sleep_mode = SLEEP_OFF;
                    p->playing = false;
                    p->paused_once = true;
                    save_position_locked(p);
                    notify_main_locked(p);
                    return 0;
                }
                /* Skip any unreadable files in a multi-file folder gracefully. */
                while (open_current_file_locked(p, 0) < 0) {
                    if ((size_t)(p->file_index + 1) < p->book->file_count) {
                        p->file_index++;
                    } else {
                        p->playing = false;
                        save_position_locked(p);
                        notify_main_locked(p);
                        return 0;
                    }
                }
                save_position_locked(p);
                notify_main_locked(p);
                continue;
            } else {
                /* Reached end of book. */
                p->playing = false;
                p->finished = true;
                p->file_pos_ms = decoder_duration_ms(p->dec);
                update_book_pos_and_chapter_locked(p);
                save_position_locked(p);
                notify_main_locked(p);
                return 0;
            }
        }
    }

    int to_read = (avail < max_out_frames) ? avail : max_out_frames;
    if (to_read <= 0) {
        return 0;
    }

    int got = sonicReadShortFromStream(p->sonic, p->sonic_buf, to_read);
    if (got <= 0) {
        return 0;
    }

    /* Software volume + 10-second sleep timer linear fade-out (DESIGN.md & D6). */
    float gain = (float)p->volume_pct / 100.0f;
    bool sleep_active = (p->sleep_mode != SLEEP_OFF);
    if (sleep_active && p->sleep_rem_ms <= PLAYER_FADE_WINDOW_MS) {
        float fade = (p->sleep_rem_ms > 0)
                         ? ((float)p->sleep_rem_ms / (float)PLAYER_FADE_WINDOW_MS)
                         : 0.0f;
        gain *= fade;
    }

    int total_samples = got * DECODER_OUT_CHANNELS;
    if (p->fade_in_left > 0) {
        /* Ramp in after start/resume/seek: starting mid-waveform at full amplitude
         * is an audible click ("glitch") on headphones. */
        for (int f = 0; f < got; f++) {
            float g = gain;
            if (p->fade_in_left > 0) {
                g *= 1.0f - (float)p->fade_in_left / (float)PLAYER_FADE_IN_FRAMES;
                p->fade_in_left--;
            }
            for (int c = 0; c < DECODER_OUT_CHANNELS; c++) {
                int i = f * DECODER_OUT_CHANNELS + c;
                int32_t v = (int32_t)lroundf((float)p->sonic_buf[i] * g);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                p->write_buf[i] = (int16_t)v;
            }
        }
    } else if (fabsf(gain - 1.0f) > 0.001f) {
        for (int i = 0; i < total_samples; i++) {
            int32_t v = (int32_t)lroundf((float)p->sonic_buf[i] * gain);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            p->write_buf[i] = (int16_t)v;
        }
    } else {
        memcpy(p->write_buf, p->sonic_buf, (size_t)total_samples * sizeof(int16_t));
    }

    /* Release p->mu while writing PCM to the audio sink so the UI thread never blocks
     * waiting for the audio pipe to drain. */
    AudioOut *ao = p->ao;
    p->ao_busy = true;
    pthread_mutex_unlock(&p->mu);

    int wrc = ao->write_s16le(ao, p->write_buf, got);

    pthread_mutex_lock(&p->mu);
    p->ao_busy = false;
    if (wrc < 0) {
        if (!p->using_sim_ao && p->ao) {
            audio_out_close(p->ao);
            p->ao = NULL;
        }
        snprintf(p->toast, sizeof(p->toast),
                 "No audio output \u2014 connect Bluetooth headphones");
        p->playing = false;
        save_position_locked(p);
        notify_main_locked(p);
        return wrc;
    }

    int64_t wall_ms = ((int64_t)got * 1000) / DECODER_OUT_RATE;
    if (wall_ms < 1) wall_ms = 1;

    p->just_seeked = false;
    update_book_pos_and_chapter_locked(p);

    /* Advance autosave counter (every 60 s while playing, or after 2 s if a seek dirtied the position). */
    p->since_last_save_ms += wall_ms;
    if (p->since_last_save_ms >= PLAYER_AUTOSAVE_INTERVAL_MS ||
        (p->dirty_save && p->since_last_save_ms >= 2000)) {
        save_position_locked(p);
    }

    /* Notify UI every 1 second when the displayed second changes. */
    int cur_sec = (int)(p->book_pos_ms / 1000);
    if (cur_sec != p->last_notified_second) {
        p->last_notified_second = cur_sec;
        notify_main_locked(p);
    }

    /* Advance sleep timer countdown. */
    if (sleep_active) {
        if (p->sleep_mode > 0) {
            p->sleep_rem_ms -= wall_ms;
        }
        if (p->sleep_rem_ms <= 0) {
            p->sleep_rem_ms = 0;
            p->sleep_mode = SLEEP_OFF;
            p->playing = false;
            p->paused_once = true;
            save_position_locked(p);
            notify_main_locked(p);
        }
    }

    return got;
}

static void *audio_thread_main(void *arg) {
    Player *p = (Player *)arg;
    pthread_mutex_lock(&p->mu);
    while (!p->quit_requested) {
        if (!p->playing) {
            pthread_cond_wait(&p->cv, &p->mu);
            continue;
        }
        /* Step 512 frames (~11.6 ms) per slice so pause/seek commands on the UI
         * thread take effect almost instantaneously without waiting on a big chunk. */
        (void)step_frames_locked(p, 512);
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}

int player_create(Player **out, Store *store, const char *state_dir, const char *helper_dir) {
    if (!out) return -EINVAL;
    *out = NULL;

    Player *p = calloc(1, sizeof(*p));
    if (!p) return -ENOMEM;

    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->notify_pipe[0] = -1;
    p->notify_pipe[1] = -1;
    if (pipe(p->notify_pipe) == 0) {
        for (int i = 0; i < 2; i++) {
            int fl = fcntl(p->notify_pipe[i], F_GETFL, 0);
            if (fl >= 0) (void)fcntl(p->notify_pipe[i], F_SETFL, fl | O_NONBLOCK);
        }
    }

    p->store = store;
    if (state_dir) snprintf(p->state_dir, sizeof(p->state_dir), "%s", state_dir);
    if (helper_dir) snprintf(p->helper_dir, sizeof(p->helper_dir), "%s", helper_dir);
    p->speed_x100 = 100;
    p->volume_pct = 100;
    p->last_chapter_index = -1;
    p->last_notified_second = -1;

    p->sonic = sonicCreateStream(DECODER_OUT_RATE, DECODER_OUT_CHANNELS);
    if (!p->sonic) {
        player_destroy(p);
        return -ENOMEM;
    }
    sonicSetSpeed(p->sonic, 1.0f);

    *out = p;
    return 0;
}

void player_destroy(Player *p) {
    if (!p) return;
    player_stop_thread(p);
    pthread_mutex_lock(&p->mu);
    if (p->playing || p->dirty_save) {
        p->playing = false;
        save_position_locked(p);
    }
    if (p->dec) {
        decoder_close(p->dec);
        p->dec = NULL;
    }
    if (p->ao) {
        audio_out_close(p->ao);
        p->ao = NULL;
    }
    if (p->sonic) {
        sonicDestroyStream(p->sonic);
        p->sonic = NULL;
    }
    if (p->notify_pipe[0] >= 0) close(p->notify_pipe[0]);
    if (p->notify_pipe[1] >= 0) close(p->notify_pipe[1]);
    pthread_mutex_unlock(&p->mu);

    pthread_cond_destroy(&p->cv);
    pthread_mutex_destroy(&p->mu);
    free(p);
}

void player_set_sim_audio(Player *p, AudioOut *ao) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    if (p->ao) {
        audio_out_close(p->ao);
    }
    p->ao = ao;
    p->using_sim_ao = (ao != NULL);
    pthread_mutex_unlock(&p->mu);
}

int player_start_thread(Player *p) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    if (p->thread_running) {
        pthread_mutex_unlock(&p->mu);
        return 0;
    }
    p->quit_requested = false;
    int rc = pthread_create(&p->thread, NULL, audio_thread_main, p);
    if (rc == 0) {
        p->thread_running = true;
    }
    pthread_mutex_unlock(&p->mu);
    return -rc;
}

void player_stop_thread(Player *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    if (!p->thread_running) {
        pthread_mutex_unlock(&p->mu);
        return;
    }
    p->quit_requested = true;
    p->playing = false;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);

    pthread_join(p->thread, NULL);

    pthread_mutex_lock(&p->mu);
    p->thread_running = false;
    pthread_mutex_unlock(&p->mu);
}

int player_notify_fd(const Player *p) {
    return p ? p->notify_pipe[0] : -1;
}

void player_clear_notify(Player *p) {
    if (!p || p->notify_pipe[0] < 0) return;
    uint8_t buf[64];
    while (read(p->notify_pipe[0], buf, sizeof(buf)) > 0) {
    }
}

int player_load_book(Player *p, Book *book) {
    if (!p || !book) return -EINVAL;
    pthread_mutex_lock(&p->mu);

    if (p->book == book) {
        /* Re-opening the loaded book (e.g. Library -> same row) must not reset the
         * live position to the last autosave or stop playback. */
        pthread_mutex_unlock(&p->mu);
        return 0;
    }
    if (p->book) {
        /* Save previous book before switching (D8), then silence it. */
        save_position_locked(p);
        if (p->playing && p->ao && p->ao->set_paused) {
            (void)p->ao->set_paused(p->ao, true);
        }
    }

    p->playing = false;
    p->just_seeked = false;
    p->book = book;
    p->file_index = book->saved_file_index;
    p->file_pos_ms = book->saved_file_pos_ms;
    p->finished = book->finished;
    if (p->finished && book->total_duration_ms > 0 &&
        book->saved_book_pos_ms >= book->total_duration_ms - 1000) {
        /* Re-opening a finished book starts ready at the beginning. */
        p->file_index = 0;
        p->file_pos_ms = 0;
        p->finished = false;
    }
    /* Resuming a book after reopening rewinds 2 s, exactly like resuming after a pause. */
    p->paused_once = (p->file_index > 0 || p->file_pos_ms > 0);
    p->speed_x100 = (book->saved_speed_x100 >= 50 && book->saved_speed_x100 <= 300)
                        ? book->saved_speed_x100
                        : 100;
    sonicSetSpeed(p->sonic, (float)p->speed_x100 / 100.0f);

    p->last_chapter_index = -1;
    int rc = open_current_file_locked(p, p->file_pos_ms);
    pthread_mutex_unlock(&p->mu);
    return rc;
}

static int seek_book_locked(Player *p, int64_t target_book_ms) {
    if (!p->book) return -EINVAL;
    if (target_book_ms < 0) target_book_ms = 0;
    if (p->book->total_duration_ms > 0 && target_book_ms > p->book->total_duration_ms) {
        target_book_ms = p->book->total_duration_ms;
    }

    int target_file = 0;
    int64_t target_fpos = 0;
    lib_file_pos_from_book(p->book, target_book_ms, &target_file, &target_fpos);

    int rc = 0;
    if (!p->dec || target_file != p->file_index) {
        p->file_index = target_file;
        rc = open_current_file_locked(p, target_fpos);
    } else {
        rc = decoder_seek_ms(p->dec, target_fpos);
        p->file_pos_ms = target_fpos;
        if (p->sonic) {
            sonicFlushStream(p->sonic);
            while (sonicSamplesAvailable(p->sonic) > 0) {
                sonicReadShortFromStream(p->sonic, p->sonic_buf, PLAYER_CHUNK_FRAMES);
            }
        }
        update_book_pos_and_chapter_locked(p);
    }

    /* Flush any pre-buffered audio in the sink FIFO immediately so the new seek
     * position plays right away without waiting for old samples to drain. */
    if (p->ao && p->ao->flush) {
        (void)p->ao->flush(p->ao);
    }

    if (target_book_ms < p->book->total_duration_ms) {
        p->finished = false;
    }
    p->just_seeked = true;
    p->fade_in_left = PLAYER_FADE_IN_FRAMES;
    mark_position_dirty_locked(p);
    return rc;
}

int player_play(Player *p) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    if (!p->book) {
        pthread_mutex_unlock(&p->mu);
        return -EINVAL;
    }
    if (p->playing) {
        pthread_mutex_unlock(&p->mu);
        return 0;
    }

    /* Resume rewinds 2 s (standard audiobook behaviour, also absorbs pipe-latency
     * position error — ARCHITECTURE.md D6). */
    if (p->paused_once && p->book_pos_ms > 0) {
        int64_t rew = p->book_pos_ms - PLAYER_RESUME_REWIND_MS;
        if (rew < 0) rew = 0;
        (void)seek_book_locked(p, rew);
    }

    /* Only open audio synchronously when running single-threaded (e.g. unit tests);
     * when the background audio thread is active, it opens audio asynchronously in
     * step_frames_locked() so tapping Play never blocks the UI thread! */
    if (!p->thread_running && !p->ao) {
        int rc = ensure_audio_out_locked(p);
        if (rc < 0) {
            pthread_mutex_unlock(&p->mu);
            return rc;
        }
    }
    if (p->ao && p->ao->set_paused) {
        (void)p->ao->set_paused(p->ao, false);
    }

    p->playing = true;
    p->paused_idle_ms = 0;
    p->fade_in_left = PLAYER_FADE_IN_FRAMES;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

int player_pause(Player *p) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    if (!p->playing) {
        pthread_mutex_unlock(&p->mu);
        return 0;
    }
    /* Measure the heard position before set_paused() drains the sink buffers. */
    int64_t heard = heard_book_pos_locked(p);
    p->playing = false;
    p->paused_once = true;
    p->paused_idle_ms = 0;
    if (p->ao && p->ao->set_paused) {
        (void)p->ao->set_paused(p->ao, true);
    }
    if (p->book && heard < p->book_pos_ms) {
        int fi = p->file_index;
        int64_t fpos = p->file_pos_ms;
        lib_file_pos_from_book(p->book, heard, &fi, &fpos);
        if (fi == p->file_index) {
            /* Same file: the decoder is reseeked on resume anyway (2 s rewind). */
            p->file_pos_ms = fpos;
            p->book_pos_ms = heard;
        }
    }
    save_position_locked(p);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

int player_toggle_play(Player *p) {
    if (!p) return -EINVAL;
    bool is_playing;
    pthread_mutex_lock(&p->mu);
    is_playing = p->playing;
    pthread_mutex_unlock(&p->mu);
    return is_playing ? player_pause(p) : player_play(p);
}

int player_seek_book_ms(Player *p, int64_t target_book_ms) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    int rc = seek_book_locked(p, target_book_ms);
    pthread_mutex_unlock(&p->mu);
    return rc;
}

int player_seek_relative_ms(Player *p, int64_t delta_ms) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    int rc = seek_book_locked(p, p->book_pos_ms + delta_ms);
    pthread_mutex_unlock(&p->mu);
    return rc;
}

int player_seek_chapter(Player *p, int chapter_index) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    if (!p->book || p->book->chapter_count == 0) {
        pthread_mutex_unlock(&p->mu);
        return -EINVAL;
    }
    if (chapter_index < 0) chapter_index = 0;
    if ((size_t)chapter_index >= p->book->chapter_count) {
        chapter_index = (int)p->book->chapter_count - 1;
    }
    int64_t target = p->book->chapters[chapter_index].start_book_ms;
    int rc = seek_book_locked(p, target);
    pthread_mutex_unlock(&p->mu);
    return rc;
}

int player_prev_chapter(Player *p) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    if (!p->book || p->book->chapter_count == 0) {
        pthread_mutex_unlock(&p->mu);
        return -EINVAL;
    }
    int ch = lib_chapter_at_pos(p->book, p->book_pos_ms);
    int64_t ch_start = p->book->chapters[ch].start_book_ms;
    int64_t into_ch = p->book_pos_ms - ch_start;

    /* Per DESIGN.md: "Previous chapter" within the first 3 s of a chapter goes
     * to the chapter before; otherwise restarts the current one. */
    int target_ch = (into_ch <= 3000 && ch > 0) ? (ch - 1) : ch;
    int rc = seek_book_locked(p, p->book->chapters[target_ch].start_book_ms);
    pthread_mutex_unlock(&p->mu);
    return rc;
}

int player_next_chapter(Player *p) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    if (!p->book || p->book->chapter_count == 0) {
        pthread_mutex_unlock(&p->mu);
        return -EINVAL;
    }
    int ch = lib_chapter_at_pos(p->book, p->book_pos_ms);
    if ((size_t)(ch + 1) < p->book->chapter_count) {
        int rc = seek_book_locked(p, p->book->chapters[ch + 1].start_book_ms);
        pthread_mutex_unlock(&p->mu);
        return rc;
    }
    pthread_mutex_unlock(&p->mu);
    return 0;
}

int player_set_speed(Player *p, int speed_x100) {
    if (!p) return -EINVAL;
    if (speed_x100 < 50) speed_x100 = 50;
    if (speed_x100 > 300) speed_x100 = 300;
    pthread_mutex_lock(&p->mu);
    p->speed_x100 = speed_x100;
    if (p->sonic) {
        sonicSetSpeed(p->sonic, (float)speed_x100 / 100.0f);
    }
    update_book_pos_and_chapter_locked(p);
    save_position_locked(p);
    pthread_mutex_unlock(&p->mu);
    return 0;
}

int player_set_volume(Player *p, int volume_pct) {
    if (!p) return -EINVAL;
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 150) volume_pct = 150;
    pthread_mutex_lock(&p->mu);
    p->volume_pct = volume_pct;
    pthread_mutex_unlock(&p->mu);
    return 0;
}

int player_set_sleep_timer(Player *p, int sleep_mode) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    p->sleep_mode = sleep_mode;
    if (sleep_mode == SLEEP_OFF) {
        p->sleep_rem_ms = 0;
    } else if (sleep_mode > 0) {
        p->sleep_rem_ms = (int64_t)sleep_mode * 60LL * 1000LL;
    } else if (sleep_mode == SLEEP_END_OF_CHAPTER) {
        update_book_pos_and_chapter_locked(p);
    }
    pthread_mutex_unlock(&p->mu);
    return 0;
}

void player_housekeeping_tick(Player *p, int64_t elapsed_wall_ms) {
    if (!p || elapsed_wall_ms <= 0) return;
    pthread_mutex_lock(&p->mu);
    if (p->dirty_save && !p->playing) {
        save_position_locked(p);
    }
    if (!p->playing && !p->ao_busy && p->ao && !p->using_sim_ao) {
        p->paused_idle_ms += elapsed_wall_ms;
        /* After 2 min paused, stop the helper entirely so the mixer/BT path can idle (D6). */
        if (p->paused_idle_ms >= PLAYER_IDLE_SINK_CLOSE_MS) {
            audio_out_close(p->ao);
            p->ao = NULL;
        }
    }
    pthread_mutex_unlock(&p->mu);
}

int player_step_frames_sync(Player *p, int max_out_frames) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    int rc = step_frames_locked(p, max_out_frames);
    pthread_mutex_unlock(&p->mu);
    return rc;
}

void player_get_snapshot(Player *p, PlayerSnapshot *snap) {
    if (!snap) return;
    memset(snap, 0, sizeof(*snap));
    if (!p) return;

    pthread_mutex_lock(&p->mu);
    snap->book = p->book;
    snap->file_index = p->file_index;
    snap->file_pos_ms = p->file_pos_ms;
    snap->book_pos_ms = p->book_pos_ms;
    if (p->playing && !p->just_seeked && p->ao && p->ao->delay_ms) {
        int64_t delay_wall = p->ao->delay_ms(p->ao);
        int64_t delay_book = (delay_wall * p->speed_x100) / 100;
        if (snap->book_pos_ms > delay_book) {
            snap->book_pos_ms -= delay_book;
        }
    }
    if (p->book && p->book->chapter_count > 0) {
        int ch = lib_chapter_at_pos(p->book, snap->book_pos_ms);
        snap->chapter_index = ch;
        snap->chapter_pos_ms = snap->book_pos_ms - p->book->chapters[ch].start_book_ms;
        if (snap->chapter_pos_ms < 0) snap->chapter_pos_ms = 0;
        snap->chapter_dur_ms = p->book->chapters[ch].duration_ms;
    }
    snap->playing = p->playing;
    snap->finished = p->finished;
    snap->speed_x100 = p->speed_x100;
    snap->volume_pct = p->volume_pct;
    snap->sleep_mode = p->sleep_mode;
    snap->sleep_rem_ms = p->sleep_rem_ms;
    snprintf(snap->toast, sizeof(snap->toast), "%s", p->toast);
    pthread_mutex_unlock(&p->mu);
}

void player_clear_toast(Player *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    p->toast[0] = '\0';
    pthread_mutex_unlock(&p->mu);
}

void player_unload_book(Player *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    if (p->book) {
        if (p->playing && p->ao && p->ao->set_paused) {
            (void)p->ao->set_paused(p->ao, true);
        }
        p->playing = false;
        save_position_locked(p);
        if (p->dec) {
            decoder_close(p->dec);
            p->dec = NULL;
        }
        p->book = NULL;
        p->book_pos_ms = 0;
        p->file_pos_ms = 0;
        p->file_index = 0;
        p->sleep_mode = SLEEP_OFF;
        p->sleep_rem_ms = 0;
    }
    pthread_mutex_unlock(&p->mu);
}

int player_save_position_now(Player *p) {
    if (!p) return -EINVAL;
    pthread_mutex_lock(&p->mu);
    save_position_locked(p);
    pthread_mutex_unlock(&p->mu);
    return 0;
}
