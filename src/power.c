#define _POSIX_C_SOURCE 200809L
#include "power.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void power_init(PowerMonitor *pm) {
    if (!pm) return;
    memset(pm, 0, sizeof(*pm));
    pm->wait_pid = -1;
    pm->event_fd = -1;
    const char *env = getenv("ALOUD_PREVENT_SCREENSAVER");
    if (env && strcmp(env, "1") == 0) {
        pm->prevent_screensaver_fallback = true;
    }
}

static void close_inherited_fds(void) {
    for (int fd = 3; fd < 64; fd++) {
        close(fd);
    }
}

static void run_lipc_set_int(const char *prop, int val) {
    if (access("/usr/bin/lipc-set-prop", X_OK) != 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        char val_str[32];
        snprintf(val_str, sizeof(val_str), "%d", val);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close_inherited_fds();
        execl("/usr/bin/lipc-set-prop", "lipc-set-prop", "-i",
              "com.lab126.powerd", prop, val_str, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        for (int i = 0; i < 20; i++) {
            if (waitpid(pid, NULL, WNOHANG) != 0) return;
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000L};
            nanosleep(&ts, NULL);
        }
        kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, WNOHANG);
    }
}

int power_start(PowerMonitor *pm) {
    if (!pm) return -EINVAL;
    power_init(pm);

    if (access("/usr/bin/lipc-wait-event", X_OK) != 0) {
        return 0;
    }

    int pfd[2];
    if (pipe(pfd) != 0) {
        return -errno;
    }

    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        close(pfd[0]);
        close(pfd[1]);
        return -err;
    }

    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(pfd[1]);
        close_inherited_fds();
        execl("/usr/bin/lipc-wait-event", "lipc-wait-event", "-m", "-s", "0",
              "com.lab126.powerd",
              "goingToScreenSaver,outOfScreenSaver,exitingScreenSaver,readyToSuspend,wakeupFromSuspend,suspending",
              (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);
    int flags = fcntl(pfd[0], F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);
    }
    pm->wait_pid = pid;
    pm->event_fd = pfd[0];
    return 0;
}

void power_stop(PowerMonitor *pm) {
    if (!pm) return;
    if (pm->prevent_screensaver_active) {
        run_lipc_set_int("preventScreenSaver", 0);
        pm->prevent_screensaver_active = false;
    }
    if (pm->event_fd >= 0) {
        close(pm->event_fd);
        pm->event_fd = -1;
    }
    if (pm->wait_pid > 0) {
        kill(pm->wait_pid, SIGKILL);
        for (int i = 0; i < 10; i++) {
            if (waitpid(pm->wait_pid, NULL, WNOHANG) != 0) break;
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 5000000L};
            nanosleep(&ts, NULL);
        }
        pm->wait_pid = -1;
    }
}

PowerEventType power_parse_event_line(const char *line) {
    if (!line || !*line) return POWER_EVT_NONE;
    if (strstr(line, "goingToScreenSaver") != NULL) {
        return POWER_EVT_GOING_TO_SCREENSAVER;
    }
    if (strstr(line, "outOfScreenSaver") != NULL ||
        strstr(line, "exitingScreenSaver") != NULL) {
        return POWER_EVT_OUT_OF_SCREENSAVER;
    }
    if (strstr(line, "readyToSuspend") != NULL) {
        return POWER_EVT_READY_TO_SUSPEND;
    }
    if (strstr(line, "wakeupFromSuspend") != NULL) {
        return POWER_EVT_WAKEUP_FROM_SUSPEND;
    }
    if (strstr(line, "suspending") != NULL) {
        return POWER_EVT_SUSPENDING;
    }
    return POWER_EVT_NONE;
}

int power_compute_defer_seconds(bool is_playing, int64_t sleep_remaining_ms) {
    if (!is_playing) {
        /* While paused, let the Kindle suspend normally to save battery (D12). */
        return 0;
    }
    if (sleep_remaining_ms > 0) {
        int64_t secs = (sleep_remaining_ms / 1000) + 60;
        if (secs < 60) secs = 60;
        if (secs > 86400) secs = 86400;
        return (int)secs;
    }
    /* Playing with no sleep timer: defer suspend by 1 hour; powerd will emit
     * readyToSuspend again when that expires if still playing. */
    return 3600;
}

int power_handle_readable(PowerMonitor *pm, bool is_playing,
                          int64_t sleep_remaining_ms, bool *out_needs_full_repaint) {
    if (out_needs_full_repaint) *out_needs_full_repaint = false;
    if (!pm || pm->event_fd < 0) return 0;

    for (;;) {
        ssize_t n = read(pm->event_fd, pm->line_buf + pm->line_len,
                         sizeof(pm->line_buf) - 1 - pm->line_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -errno;
        }
        if (n == 0) {
            close(pm->event_fd);
            pm->event_fd = -1;
            break;
        }
        pm->line_len += (size_t)n;
        pm->line_buf[pm->line_len] = '\0';

        char *nl;
        while ((nl = strchr(pm->line_buf, '\n')) != NULL) {
            *nl = '\0';
            PowerEventType evt = power_parse_event_line(pm->line_buf);
            switch (evt) {
                case POWER_EVT_GOING_TO_SCREENSAVER:
                    pm->in_screensaver = true;
                    break;
                case POWER_EVT_OUT_OF_SCREENSAVER:
                case POWER_EVT_WAKEUP_FROM_SUSPEND:
                    pm->in_screensaver = false;
                    if (out_needs_full_repaint) {
                        *out_needs_full_repaint = true;
                    }
                    break;
                case POWER_EVT_READY_TO_SUSPEND: {
                    int defer_s = power_compute_defer_seconds(is_playing, sleep_remaining_ms);
                    if (defer_s > 0) {
                        run_lipc_set_int("deferSuspend", defer_s);
                    }
                    break;
                }
                case POWER_EVT_SUSPENDING:
                case POWER_EVT_NONE:
                    break;
            }
            size_t rem = pm->line_len - (size_t)(nl - pm->line_buf + 1);
            memmove(pm->line_buf, nl + 1, rem + 1);
            pm->line_len = rem;
        }
        if (pm->line_len >= sizeof(pm->line_buf) - 1) {
            pm->line_len = 0;
        }
    }
    return 0;
}

void power_on_playback_changed(PowerMonitor *pm, bool is_playing) {
    if (!pm || !pm->prevent_screensaver_fallback) return;
    if (is_playing != pm->prevent_screensaver_active) {
        run_lipc_set_int("preventScreenSaver", is_playing ? 1 : 0);
        pm->prevent_screensaver_active = is_playing;
    }
}
