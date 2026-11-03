#define _POSIX_C_SOURCE 200809L
#include "audio_out.h"
#include "decoder.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The named FIFO is drained on pause/seek (drain_fifo_nonblocking), so its size no
 * longer adds pause latency. Make it big enough (~185 ms) that a slow e-ink render on
 * the single Cortex-A8 core cannot starve mixersink right after playback starts. The
 * anonymous-pipe fallback cannot be drained, so it stays small for snappy pauses. */
#define GST_FIFO_BYTES 32768
#define GST_PIPE_BYTES 8192
/* Estimated latency of the helper's `queue max-size-time=100ms` + mixersink. */
#define GST_QUEUE_MS 100

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    (void)nanosleep(&ts, NULL);
}

/* Bluetooth Kindles have no ALSA card; audio goes through Lab126's proprietary
 * GStreamer `mixersink` element at 44100 Hz S16LE stereo (ARCHITECTURE.md D6,
 * RESEARCH.md §6.2-6.3). Because our main binary is static musl, we spawn the
 * tiny dynamically linked `aloud-sink` helper (or fallback to `gst-launch-*`)
 * and feed PCM over its stdin pipe. */

typedef struct {
    pid_t pid;
    int pcm_fd;
    int ctrl_fd;
    int status_fd;
    char fifo_path[64];
    int64_t frames_written;  /* since this helper started (includes pre-roll) */
    bool paused;
    char helper_dir[512];
    int64_t measured_delay_ms; /* -1 = unknown */
    int64_t delay_checked_ms;  /* monotonic time of the last POS query */
    bool delay_logged;
    bool stop_sent;
    bool have_helper;
    char helper_path[1024];
    atomic_bool needs_restart; /* set by flush (UI thread), acted on by the writer */
    /* Serialises helper restarts (audio thread) against control traffic and FIFO
     * draining (UI thread): the writer itself is the only thread replacing pcm_fd. */
    pthread_mutex_t ctrl_mu;
} GstAudioImpl;

/* Stopped helpers are reaped without blocking the UI thread (see gst_teardown_child). */
static pid_t g_pending_reap[8];

static void reap_pending_nonblocking(void) {
    for (int i = 0; i < 8; i++) {
        if (g_pending_reap[i] > 0 && waitpid(g_pending_reap[i], NULL, WNOHANG) != 0) {
            g_pending_reap[i] = 0;
        }
    }
}

static void add_pending_reap(pid_t pid) {
    reap_pending_nonblocking();
    for (int i = 0; i < 8; i++) {
        if (g_pending_reap[i] <= 0) {
            g_pending_reap[i] = pid;
            return;
        }
    }
    kill(pid, SIGKILL); /* table full: never leak, never block */
    (void)waitpid(pid, NULL, 0);
}

static int64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void close_inherited_fds_except(int keep1, int keep2) {
    for (int fd = 3; fd < 64; fd++) {
        if (fd != keep1 && fd != keep2) {
            close(fd);
        }
    }
}

static void reap_child_bounded(pid_t pid, int wait_ms_total) {
    if (pid <= 0) return;
    int status = 0;
    int elapsed = 0;
    while (elapsed < wait_ms_total) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r != 0) return;
        sleep_ms(10);
        elapsed += 10;
    }
    kill(pid, SIGKILL);
    for (int i = 0; i < 5; i++) {
        if (waitpid(pid, &status, WNOHANG) != 0) return;
        sleep_ms(5);
    }
}

static int run_cmd_capture_timeout(char *const argv[], char *out, size_t out_cap, int timeout_ms) {
    if (out && out_cap > 0) out[0] = '\0';
    int pfd[2];
    if (pipe(pfd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
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
        close_inherited_fds_except(-1, -1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    int fl = fcntl(pfd[0], F_GETFL, 0);
    if (fl >= 0) (void)fcntl(pfd[0], F_SETFL, fl | O_NONBLOCK);

    size_t total = 0;
    int elapsed = 0;
    while (elapsed < timeout_ms) {
        struct pollfd pf = {.fd = pfd[0], .events = POLLIN};
        int pr = poll(&pf, 1, 25);
        elapsed += 25;
        if (pr > 0) {
            if (pf.revents & POLLIN) {
                char discard[128];
                char *dst = (out && total + 1 < out_cap) ? (out + total) : discard;
                size_t cap = (out && total + 1 < out_cap) ? (out_cap - 1 - total) : sizeof(discard);
                ssize_t n = read(pfd[0], dst, cap);
                if (n > 0) {
                    if (dst != discard) total += (size_t)n;
                    continue;
                }
            }
            if (pf.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                break;
            }
        }
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            close(pfd[0]);
            if (out && out_cap > 0) out[total] = '\0';
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
    }
    close(pfd[0]);
    if (out && out_cap > 0) out[total] = '\0';
    reap_child_bounded(pid, 30);
    return -1;
}

/* Best-effort Bluetooth wake/reconnect check via lipc (strictly timeout-bounded
 * so it can never block the audio thread if btfd is slow). */
static void try_wake_bt_nonblocking(void) {
    if (access("/usr/bin/lipc-set-prop", X_OK) == 0) {
        char *ensure_cmd[] = {"/usr/bin/lipc-set-prop", "-i", "com.lab126.btfd", "ensureBTconnection", "1", NULL};
        (void)run_cmd_capture_timeout(ensure_cmd, NULL, 0, 150);
    }

    if (access("/usr/bin/lipc-get-prop", X_OK) != 0) {
        return;
    }

    char out[256];
    char *bt_cmd[] = {"/usr/bin/lipc-get-prop", "com.lab126.btfd", "BTconnectedDevName", NULL};
    if (run_cmd_capture_timeout(bt_cmd, out, sizeof(out), 150) == 0) {
        for (size_t i = strlen(out); i > 0 && (out[i - 1] == '\n' || out[i - 1] == '\r' || out[i - 1] == ' '); i--) {
            out[i - 1] = '\0';
        }
        if (out[0] != '\0' && strcmp(out, "none") != 0 && strcmp(out, "NULL") != 0) {
            return;
        }
    }

    /* Check if a paired MAC is known in /var/local/zbluetooth/bt_config.conf and
     * attempt a fast non-blocking `Connect <MAC>` (D6). */
    FILE *fp = fopen("/var/local/zbluetooth/bt_config.conf", "r");
    if (fp) {
        char line[256];
        char mac[32] = {0};
        while (fgets(line, sizeof(line), fp)) {
            for (size_t i = 0; line[i] && i + 17 <= strlen(line); i++) {
                if (line[i + 2] == ':' && line[i + 5] == ':' &&
                    line[i + 8] == ':' && line[i + 11] == ':' && line[i + 14] == ':') {
                    memcpy(mac, line + i, 17);
                    mac[17] = '\0';
                    break;
                }
            }
            if (mac[0]) break;
        }
        fclose(fp);
        if (mac[0] && access("/usr/bin/lipc-set-prop", X_OK) == 0) {
            char *conn_cmd[] = {"/usr/bin/lipc-set-prop", "com.lab126.btfd", "Connect", mac, NULL};
            (void)run_cmd_capture_timeout(conn_cmd, NULL, 0, 150);
        }
    }
}

static int send_ctrl_cmd(GstAudioImpl *im, const char *cmd, char *reply, size_t reply_cap) {
    if (im->ctrl_fd < 0) return -ENOTSUP;
    if (im->status_fd >= 0) {
        /* Discard replies that arrived after an earlier command's 80 ms wait. */
        char stale[64];
        while (read(im->status_fd, stale, sizeof(stale)) > 0) {
        }
    }
    size_t len = strlen(cmd);
    if (write(im->ctrl_fd, cmd, len) != (ssize_t)len) {
        return -EIO;
    }
    if (reply && reply_cap > 0 && im->status_fd >= 0) {
        struct pollfd pfd = {.fd = im->status_fd, .events = POLLIN};
        if (poll(&pfd, 1, 80) > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(im->status_fd, reply, reply_cap - 1);
            if (n > 0) {
                reply[n] = '\0';
                return 0;
            }
        }
    }
    return 0;
}

static void drain_fifo_nonblocking(int fd) {
    if (fd < 0) return;
    uint8_t discard[4096];
    while (read(fd, discard, sizeof(discard)) > 0) {
    }
}


/* Write raw PCM into the current helper's pipe. Only the audio thread calls this. */
static int write_pcm(GstAudioImpl *im, const int16_t *frames, int frame_count) {
    const uint8_t *ptr = (const uint8_t *)frames;
    size_t remaining = (size_t)frame_count * DECODER_OUT_CHANNELS * sizeof(int16_t);
    int stall_polls = 0;
    while (remaining > 0) {
        if (atomic_load(&im->needs_restart)) {
            return frame_count; /* flushed mid-chunk: the rest is stale audio anyway */
        }
        ssize_t n = write(im->pcm_fd, ptr, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = {.fd = im->pcm_fd, .events = POLLOUT};
                int pr = poll(&pfd, 1, 200);
                if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    return -EPIPE;
                }
                if (pr == 0 && ++stall_polls >= 8) {
                    /* Pipe stayed full for > 1.6 s: mixersink is not draining
                     * (e.g. Bluetooth headphones not connected). Fail cleanly. */
                    return -ENOTCONN;
                }
                continue;
            }
            return -errno;
        }
        stall_polls = 0;
        ptr += n;
        remaining -= (size_t)n;
    }
    im->frames_written += frame_count;
    return frame_count;
}

/* Stop the current helper without waiting for it: close our PCM writer so its
 * filesrc sees EOF, then STOP, on which it pauses and tears its mixer stream down.
 * Caller holds ctrl_mu (or is the only thread left). */
static void gst_teardown_child(GstAudioImpl *im) {
    if (im->pcm_fd >= 0) {
        if (im->fifo_path[0]) drain_fifo_nonblocking(im->pcm_fd);
        close(im->pcm_fd);
        im->pcm_fd = -1;
    }
    if (im->ctrl_fd >= 0) {
        if (!im->stop_sent && im->pid > 0) (void)write(im->ctrl_fd, "STOP\n", 5);
        close(im->ctrl_fd);
        im->ctrl_fd = -1;
    }
    if (im->status_fd >= 0) {
        close(im->status_fd);
        im->status_fd = -1;
    }
    if (im->pid > 0) {
        add_pending_reap(im->pid);
        im->pid = 0;
    }
    if (im->fifo_path[0]) {
        unlink(im->fifo_path);
        im->fifo_path[0] = '\0';
    }
    im->paused = false;
    im->frames_written = 0;
    im->measured_delay_ms = -1;
    im->delay_checked_ms = 0;
}

/* Start a helper (or gst-launch fallback) and fill im's pid/fds. */
static int gst_spawn(GstAudioImpl *im, char *err_msg, size_t err_cap) {
    reap_pending_nonblocking();
    bool have_helper = im->have_helper;
    const char *helper_path = im->helper_path;

    /* Create a real named FIFO in /tmp so GStreamer's `filesrc location=...`
     * sees a proper FIFO filesystem node (verified in LARK/KinAmp on Kindle).
     * Opening with O_RDWR | O_NONBLOCK succeeds immediately without waiting
     * for the child reader, and allows draining buffered bytes on pause/seek. */
    const char *fifo_path = "/tmp/aloud_audio.fifo";
    unlink(fifo_path);
    int pcm_fd = -1;
    int pcm_pipe[2] = {-1, -1};
    bool use_fifo = false;
    if (mkfifo(fifo_path, 0666) == 0) {
        pcm_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (pcm_fd >= 0) {
            use_fifo = true;
#ifdef F_SETPIPE_SZ
            (void)fcntl(pcm_fd, F_SETPIPE_SZ, GST_FIFO_BYTES);
#endif
        } else {
            unlink(fifo_path);
        }
    }
    if (!use_fifo) {
        if (pipe(pcm_pipe) != 0) {
            return -errno;
        }
        pcm_fd = pcm_pipe[1];
#ifdef F_SETPIPE_SZ
        (void)fcntl(pcm_fd, F_SETPIPE_SZ, GST_PIPE_BYTES);
#endif
    }

    int ctrl_pipe[2] = {-1, -1};
    int status_pipe[2] = {-1, -1};
    if (have_helper) {
        if (pipe(ctrl_pipe) != 0 || pipe(status_pipe) != 0) {
            int err = errno;
            if (use_fifo) { close(pcm_fd); unlink(fifo_path); }
            else { close(pcm_pipe[0]); close(pcm_pipe[1]); }
            if (ctrl_pipe[0] >= 0) { close(ctrl_pipe[0]); close(ctrl_pipe[1]); }
            return -err;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        if (use_fifo) { close(pcm_fd); unlink(fifo_path); }
        else { close(pcm_pipe[0]); close(pcm_pipe[1]); }
        if (ctrl_pipe[0] >= 0) { close(ctrl_pipe[0]); close(ctrl_pipe[1]); }
        if (status_pipe[0] >= 0) { close(status_pipe[0]); close(status_pipe[1]); }
        return -err;
    }

    if (pid == 0) {
        if (use_fifo) {
            close(pcm_fd);
        } else {
            close(pcm_pipe[1]);
            dup2(pcm_pipe[0], STDIN_FILENO);
            close(pcm_pipe[0]);
        }

        if (have_helper) {
            close(ctrl_pipe[1]);
            close(status_pipe[0]);
            close_inherited_fds_except(ctrl_pipe[0], status_pipe[1]);
            char cfd_str[16], sfd_str[16];
            snprintf(cfd_str, sizeof(cfd_str), "%d", ctrl_pipe[0]);
            snprintf(sfd_str, sizeof(sfd_str), "%d", status_pipe[1]);
            if (use_fifo) {
                execl(helper_path, "aloud-sink", "--fifo", fifo_path,
                      "--ctrl-fd", cfd_str, "--status-fd", sfd_str, (char *)NULL);
            } else {
                execl(helper_path, "aloud-sink",
                      "--ctrl-fd", cfd_str, "--status-fd", sfd_str, (char *)NULL);
            }
            fprintf(stderr, "aloud: cannot exec helper %s: %s\n", helper_path, strerror(errno));
        }

        close_inherited_fds_except(-1, -1);

        char loc_arg[128];
        snprintf(loc_arg, sizeof(loc_arg), "location=%s", use_fifo ? fifo_path : "/dev/stdin");

        /* Fallback if helper could not start: firmware gst-launch CLI (D6). */
        if (access("/usr/lib/libgstreamer-1.0.so.0", F_OK) == 0) {
            execlp("gst-launch-1.0", "gst-launch-1.0", "-q",
                   "filesrc", loc_arg, "!",
                   "audio/x-raw,format=S16LE,layout=interleaved,rate=44100,channels=2", "!",
                   "queue", "!", "mixersink", (char *)NULL);
        }
        execlp("gst-launch-0.10", "gst-launch-0.10", "-q",
               "filesrc", loc_arg, "!",
               "audio/x-raw-int,endianness=1234,signed=true,width=16,depth=16,rate=44100,channels=2", "!",
               "queue", "!", "mixersink", (char *)NULL);
        execlp("gst-launch", "gst-launch", "-q",
               "filesrc", loc_arg, "!",
               "audio/x-raw-int,endianness=1234,signed=true,width=16,depth=16,rate=44100,channels=2", "!",
               "queue", "!", "mixersink", (char *)NULL);
        _exit(127);
    }

    if (!use_fifo && pcm_pipe[0] >= 0) {
        close(pcm_pipe[0]);
    }
    int fl = fcntl(pcm_fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(pcm_fd, F_SETFL, fl | O_NONBLOCK);

    if (have_helper) {
        close(ctrl_pipe[0]);
        close(status_pipe[1]);
        int cfl = fcntl(ctrl_pipe[1], F_GETFL, 0);
        if (cfl >= 0) (void)fcntl(ctrl_pipe[1], F_SETFL, cfl | O_NONBLOCK);
        int sfl = fcntl(status_pipe[0], F_GETFL, 0);
        if (sfl >= 0) (void)fcntl(status_pipe[0], F_SETFL, sfl | O_NONBLOCK);
    }

    im->pid = pid;
    im->pcm_fd = pcm_fd;
    im->ctrl_fd = have_helper ? ctrl_pipe[1] : -1;
    im->status_fd = have_helper ? status_pipe[0] : -1;
    im->fifo_path[0] = '\0';
    if (use_fifo) {
        snprintf(im->fifo_path, sizeof(im->fifo_path), "%s", fifo_path);
    }
    im->paused = false;
    im->frames_written = 0;
    im->measured_delay_ms = -1;
    im->delay_checked_ms = 0;
    im->delay_logged = false;
    im->stop_sent = false;

    /* If using the helper, wait briefly (max 400 ms) for its READY/OK banner so we
     * detect missing GStreamer or broken mixersink immediately. */
    if (!have_helper) {
        fprintf(stderr, "aloud: using gst-launch (no audio helper in %s)\n", im->helper_dir);
    }
    if (have_helper && im->status_fd >= 0) {
        struct pollfd pfd = {.fd = im->status_fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 400);
        char banner[128] = {0};
        ssize_t n = -1;
        if (pr > 0 && (pfd.revents & POLLIN)) {
            n = read(im->status_fd, banner, sizeof(banner) - 1);
        }
        if (pr > 0 && n <= 0) {
            /* Status pipe closed (HUP or EOF) with no banner. Either the helper could not
             * exec and the child fell through to gst-launch (still running), or the
             * helper started and died. A dying process is reaped within moments. */
            int status = 0;
            pid_t r = 0;
            for (int i = 0; i < 10 && r == 0; i++) {
                r = waitpid(im->pid, &status, WNOHANG);
                if (r == 0) sleep_ms(10);
            }
            if (r == im->pid) {
                if (WIFSIGNALED(status)) {
                    fprintf(stderr, "aloud: audio helper %s died (signal %d)\n", helper_path,
                            WTERMSIG(status));
                } else {
                    fprintf(stderr, "aloud: audio helper %s exited (status %d)\n", helper_path,
                            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
                }
                im->pid = 0; /* already reaped */
                gst_teardown_child(im);
                if (im->have_helper) {
                    /* Never leave the user without sound: use the firmware gst-launch
                     * for the rest of this session. */
                    fprintf(stderr, "aloud: retrying audio with gst-launch\n");
                    im->have_helper = false;
                    return gst_spawn(im, err_msg, err_cap);
                }
                if (err_msg && err_cap > 0) {
                    snprintf(err_msg, err_cap, "Connect Bluetooth headphones in Kindle Settings");
                }
                return -EIO;
            }
            /* Child alive: it is gst-launch, which has no control channel. Pause and
             * seek stop that process instead of sending commands nobody reads. */
            fprintf(stderr, "aloud: audio helper did not start; using gst-launch fallback\n");
            close(im->ctrl_fd);
            close(im->status_fd);
            im->ctrl_fd = -1;
            im->status_fd = -1;
        } else if (pr > 0) {
            if (n > 0 && strncmp(banner, "ERR", 3) != 0 && !im->delay_logged) {
                fprintf(stderr, "aloud: audio helper started (%s)\n", helper_path);
            }
            if (n <= 0 || strncmp(banner, "ERR", 3) == 0) {
                fprintf(stderr, "aloud: audio helper failed: %s\n", n > 0 ? banner : "no banner");
                if (err_msg && err_cap > 0) {
                    snprintf(err_msg, err_cap, "Connect Bluetooth headphones in Kindle Settings");
                }
                gst_teardown_child(im);
                return -EIO;
            }
        }
    }

    /* Pre-roll ~120 ms of silence: mixersink/A2DP start-up (stream setup, clock slaving,
     * first underrun) then lands on silence instead of clipping the first syllable. */
    static const int16_t zeros[1024 * DECODER_OUT_CHANNELS];
    for (int left = (DECODER_OUT_RATE * 120) / 1000; left > 0; left -= 1024) {
        if (write_pcm(im, zeros, left < 1024 ? left : 1024) < 0) break;
    }
    return 0;
}

static int gst_write_s16le(AudioOut *ao, const int16_t *frames, int frame_count) {
    if (!ao || !ao->impl || !frames || frame_count < 0) return -EINVAL;
    GstAudioImpl *im = (GstAudioImpl *)ao->impl;
    if (frame_count == 0) return 0;

    if (atomic_load(&im->needs_restart)) {
        /* A seek flushed the sink: replace the helper so none of the audio the mixer
         * daemon buffered for the old position is ever heard. Runs on the audio
         * thread, so the UI never waits for the ~0.3 s helper start. */
        pthread_mutex_lock(&im->ctrl_mu);
        atomic_store(&im->needs_restart, false);
        gst_teardown_child(im);
        int rc = gst_spawn(im, NULL, 0);
        pthread_mutex_unlock(&im->ctrl_mu);
        if (rc < 0) return rc;
    }
    if (im->pcm_fd < 0) return -EPIPE;
    return write_pcm(im, frames, frame_count);
}

/* End the current stream right now; the writer starts a fresh one on its next chunk.
 * On device, PAUSED alone left ~5 s of audio playing out of the Lab126 mixer daemon,
 * whereas ending the stream (as seeks do) drops it. Caller holds ctrl_mu.
 *
 * Our end of the PCM pipe is atomically replaced by /dev/null (dup2): the helper's
 * filesrc then sees EOF, so its PAUSED -> NULL teardown cannot block on read(), and
 * the audio thread can never be caught writing to a closed or reused descriptor. */
static void end_stream_now_locked(GstAudioImpl *im) {
    if (im->pcm_fd >= 0) {
        if (im->fifo_path[0]) drain_fifo_nonblocking(im->pcm_fd);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, im->pcm_fd);
            close(devnull);
        }
    }
    if (im->ctrl_fd >= 0) {
        if (!im->stop_sent) (void)write(im->ctrl_fd, "STOP\n", 5);
        im->stop_sent = true;
    } else if (im->pid > 0) {
        /* gst-launch has no control channel and would play its queue out on EOF. */
        kill(im->pid, SIGKILL);
    }
    atomic_store(&im->needs_restart, true);
}

static int gst_set_paused(AudioOut *ao, bool paused) {
    if (!ao || !ao->impl) return -EINVAL;
    GstAudioImpl *im = (GstAudioImpl *)ao->impl;
    pthread_mutex_lock(&im->ctrl_mu);
    if (atomic_load(&im->needs_restart) || im->paused == paused) {
        /* Pending restart: the old helper is already silenced, the new one starts
         * playing on the next write. */
        pthread_mutex_unlock(&im->ctrl_mu);
        return 0;
    }
    if (paused) {
        end_stream_now_locked(im); /* resume restarts it (it rewinds 2 s anyway) */
    } else {
        im->paused = false;
        if (im->ctrl_fd >= 0) {
            char reply[64];
            (void)send_ctrl_cmd(im, "PLAY\n", reply, sizeof(reply));
        }
    }
    pthread_mutex_unlock(&im->ctrl_mu);
    return 0;
}

static int gst_flush(AudioOut *ao) {
    if (!ao || !ao->impl) return -EINVAL;
    GstAudioImpl *im = (GstAudioImpl *)ao->impl;
    pthread_mutex_lock(&im->ctrl_mu);
    if (!atomic_load(&im->needs_restart)) {
        end_stream_now_locked(im);
    }
    pthread_mutex_unlock(&im->ctrl_mu);
    return 0;
}

static int64_t gst_delay_ms(AudioOut *ao) {
    if (!ao || !ao->impl) return GST_QUEUE_MS;
    GstAudioImpl *im = (GstAudioImpl *)ao->impl;
    if (pthread_mutex_trylock(&im->ctrl_mu) != 0) {
        return GST_QUEUE_MS; /* helper restarting: nothing meaningful buffered */
    }
    int64_t now = mono_ms();
    if (im->ctrl_fd >= 0 && !im->paused && !atomic_load(&im->needs_restart) &&
        now - im->delay_checked_ms >= 1000) {
        /* Ask the pipeline what it has played; everything written beyond that is still
         * buffered (FIFO + queue + mixer daemon). At most one round-trip per second. */
        im->delay_checked_ms = now;
        char reply[64] = {0};
        int64_t pos_ms = 0;
        if (send_ctrl_cmd(im, "POS\n", reply, sizeof(reply)) == 0 &&
            sscanf(reply, "POS %" SCNd64, &pos_ms) == 1 && pos_ms > 0) {
            int64_t written_ms = (im->frames_written * 1000) / DECODER_OUT_RATE;
            int64_t d = written_ms - pos_ms;
            im->measured_delay_ms = (d >= 0 && d <= 10000) ? d : -1;
            if (!im->delay_logged) {
                /* One line per helper start: on-device evidence of real sink latency. */
                fprintf(stderr, "aloud: sink delay written=%" PRId64 " ms played=%" PRId64
                                " ms -> %" PRId64 " ms\n", written_ms, pos_ms, d);
                im->delay_logged = true;
            }
        }
    }
    int64_t result = im->measured_delay_ms;
    if (result < 0) {
        /* Fallback: bytes still in the FIFO are exact; add the helper queue estimate. */
        int queued = 0;
        result = GST_QUEUE_MS;
        if (im->fifo_path[0] && ioctl(im->pcm_fd, FIONREAD, &queued) == 0 && queued > 0) {
            int bytes_per_sec = DECODER_OUT_RATE * DECODER_OUT_CHANNELS * (int)sizeof(int16_t);
            result += ((int64_t)queued * 1000) / bytes_per_sec;
        }
    }
    pthread_mutex_unlock(&im->ctrl_mu);
    return result;
}

static void gst_close(AudioOut *ao) {
    if (!ao) return;
    GstAudioImpl *im = (GstAudioImpl *)ao->impl;
    if (im) {
        gst_teardown_child(im);
        /* Give stopped helpers a moment to close their mixer streams, then make sure
         * none outlives us. Close happens only when idle or exiting. */
        for (int i = 0; i < 8; i++) {
            if (g_pending_reap[i] > 0) {
                reap_child_bounded(g_pending_reap[i], 300);
                g_pending_reap[i] = 0;
            }
        }
        pthread_mutex_destroy(&im->ctrl_mu);
        free(im);
    }
    free(ao);
}

int audio_out_open_gst(AudioOut **out, const char *helper_dir, char *err_msg, size_t err_cap) {
    if (!out) return -EINVAL;
    *out = NULL;
    if (err_msg && err_cap > 0) err_msg[0] = '\0';

    try_wake_bt_nonblocking();

    AudioOut *ao = calloc(1, sizeof(*ao));
    GstAudioImpl *im = calloc(1, sizeof(*im));
    if (!ao || !im) {
        free(ao);
        free(im);
        return -ENOMEM;
    }
    im->pid = 0;
    im->pcm_fd = -1;
    im->ctrl_fd = -1;
    im->status_fd = -1;
    im->measured_delay_ms = -1;
    atomic_init(&im->needs_restart, false);
    pthread_mutex_init(&im->ctrl_mu, NULL);

    /* Pick armhf vs armel helper using KOReader's ld-linux-armhf.so.3 check (D6). */
    bool is_armhf = (access("/lib/ld-linux-armhf.so.3", F_OK) == 0);
    const char *dir = (helper_dir && helper_dir[0]) ? helper_dir : "/mnt/us/extensions/aloud/bin";
    snprintf(im->helper_dir, sizeof(im->helper_dir), "%s", dir);
    snprintf(im->helper_path, sizeof(im->helper_path), "%s/%s",
             dir, is_armhf ? "aloud-sink-armhf" : "aloud-sink-armel");
    im->have_helper = (access(im->helper_path, X_OK) == 0);
    if (!im->have_helper) {
        snprintf(im->helper_path, sizeof(im->helper_path), "%s/aloud-sink", dir);
        im->have_helper = (access(im->helper_path, X_OK) == 0);
    }

    ao->type = AUDIO_BACKEND_KINDLE_GST;
    ao->impl = im;
    ao->write_s16le = gst_write_s16le;
    ao->set_paused = gst_set_paused;
    ao->flush = gst_flush;
    ao->delay_ms = gst_delay_ms;
    ao->close = gst_close;

    int rc = gst_spawn(im, err_msg, err_cap);
    if (rc < 0) {
        gst_close(ao);
        return rc;
    }
    *out = ao;
    return 0;
}
