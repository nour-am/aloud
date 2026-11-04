#ifndef ALOUD_POWER_H
#define ALOUD_POWER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    POWER_EVT_NONE = 0,
    POWER_EVT_GOING_TO_SCREENSAVER,
    POWER_EVT_OUT_OF_SCREENSAVER,
    POWER_EVT_READY_TO_SUSPEND,
    POWER_EVT_WAKEUP_FROM_SUSPEND,
    POWER_EVT_SUSPENDING,
    POWER_EVT_CHARGING /* a cable was plugged in (USB drive mode may follow) */
} PowerEventType;

typedef struct {
    pid_t wait_pid;
    int event_fd;
    bool in_screensaver;
    bool charging_event; /* set on POWER_EVT_CHARGING; the caller clears it */
    bool prevent_screensaver_fallback;
    bool prevent_screensaver_active;
    char line_buf[512];
    size_t line_len;
} PowerMonitor;

void power_init(PowerMonitor *pm);

/* Start the long-lived `lipc-wait-event` child process on Kindle.
 * Returns 0 on success (or if lipc-wait-event is not present on host). */
int power_start(PowerMonitor *pm);

/* Stop the child process and restore any preventScreenSaver state. */
void power_stop(PowerMonitor *pm);

/* Parse a single line emitted by `lipc-wait-event -m com.lab126.powerd ...`.
 * Exposed for unit testing. */
PowerEventType power_parse_event_line(const char *line);

/* Compute the deferSuspend duration in seconds given playback state and remaining
 * sleep timer milliseconds. Returns 0 when paused (meaning: do NOT defer suspend). */
int power_compute_defer_seconds(bool is_playing, int64_t sleep_remaining_ms);

/* Read pending data from event_fd, handle readyToSuspend (by calling
 * `lipc-set-prop com.lab126.powerd deferSuspend <sec>` when playing), and
 * set *out_needs_full_repaint = true if waking from screensaver. */
int power_handle_readable(PowerMonitor *pm, bool is_playing,
                          int64_t sleep_remaining_ms, bool *out_needs_full_repaint);

/* Optional fallback for Broadcom devices if configured: update preventScreenSaver
 * when play/pause state changes. */
void power_on_playback_changed(PowerMonitor *pm, bool is_playing);

#endif /* ALOUD_POWER_H */
