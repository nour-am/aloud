#define _POSIX_C_SOURCE 200809L
#include "audio_out.h"
#include "decoder.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
    int64_t frames_written;
    bool paused;
} GstAudioImpl;

static int run_cmd_capture(char *const argv[], char *out, size_t out_cap) {
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
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t total = 0;
    if (out && out_cap > 1) {
        ssize_t n;
        while (total + 1 < out_cap &&
               (n = read(pfd[0], out + total, out_cap - 1 - total)) > 0) {
            total += (size_t)n;
        }
        out[total] = '\0';
    }
    close(pfd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Check Bluetooth output state via lipc (RESEARCH.md §6.4, ARCHITECTURE.md D6).
 * Returns true if connected (or if lipc is absent on non-BT devices). */
static bool check_or_reconnect_bt(char *err_msg, size_t err_cap) {
    if (access("/usr/bin/lipc-get-prop", X_OK) != 0) {
        return true;
    }

    char out[512];
    char *bt_cmd[] = {"/usr/bin/lipc-get-prop", "com.lab126.btfd", "BTconnectedDevName", NULL};
    if (run_cmd_capture(bt_cmd, out, sizeof(out)) == 0) {
        /* Trim whitespace */
        for (size_t i = strlen(out); i > 0 && (out[i - 1] == '\n' || out[i - 1] == '\r' || out[i - 1] == ' '); i--) {
            out[i - 1] = '\0';
        }
        if (out[0] != '\0' && strcmp(out, "none") != 0 && strcmp(out, "NULL") != 0) {
            return true;
        }
    }

    char *am_cmd[] = {"/usr/bin/lipc-get-prop", "com.lab126.audiomgrd", "audioOutputConnected", NULL};
    if (run_cmd_capture(am_cmd, out, sizeof(out)) == 0 && atoi(out) > 0) {
        return true;
    }

    /* Check if a paired MAC is known in /var/local/zbluetooth/bt_config.conf and
     * attempt a single non-destructive `Connect <MAC>` (D6). */
    FILE *fp = fopen("/var/local/zbluetooth/bt_config.conf", "r");
    if (fp) {
        char line[256];
        char mac[32] = {0};
        while (fgets(line, sizeof(line), fp)) {
            /* Look for XX:XX:XX:XX:XX:XX pattern */
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
            (void)run_cmd_capture(conn_cmd, NULL, 0);
            sleep_ms(400);
            if (run_cmd_capture(bt_cmd, out, sizeof(out)) == 0 && out[0] != '\0' && out[0] != '\n') {
                return true;
            }
        }
    }

    /* Also check BTstate: if > 1 on some firmwares, treat as connected. */
    char *st_cmd[] = {"/usr/bin/lipc-get-prop", "com.lab126.btfd", "BTstate", NULL};
    if (run_cmd_capture(st_cmd, out, sizeof(out)) == 0 && atoi(out) >= 2) {
        return true;
    }

    if (err_msg && err_cap > 0) {
        snprintf(err_msg, err_cap, "Connect headphones in Settings \xE2\x80\xBA Bluetooth");
    }
    return false;
}

static int gst_write_s16le(AudioOut *ao, const int16_t *frames, int frame_count) {
    if (!ao || !ao->impl || !frames || frame_count < 0) return -EINVAL;
    GstAudioImpl *im = (GstAudioImpl *)ao->impl;
    if (frame_count == 0) return 0;

    const uint8_t *ptr = (const uint8_t *)frames;
    size_t remaining = (size_t)frame_count * DECODER_OUT_CHANNELS * sizeof(int16_t);
    while (remaining > 0) {
        ssize_t n = write(im->pcm_fd, ptr, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        ptr += n;
        remaining -= (size_t)n;
    }
    im->frames_written += frame_count;
    return frame_count;
}

static int send_ctrl_cmd(GstAudioImpl *im, const char *cmd, char *reply, size_t reply_cap) {
    if (im->ctrl_fd < 0) return -ENOTSUP;
    size_t len = strlen(cmd);
    if (write(im->ctrl_fd, cmd, len) != (ssize_t)len) {
        return -EIO;
    }
    if (reply && reply_cap > 0 && im->status_fd >= 0) {
        struct pollfd pfd = {.fd = im->status_fd, .events = POLLIN};
        if (poll(&pfd, 1, 150) > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(im->status_fd, reply, reply_cap - 1);
            if (n > 0) {
                reply[n] = '\0';
                return 0;
            }
        }
    }
    return 0;
}

static int gst_set_paused(AudioOut *ao, bool paused) {
    if (!ao || !ao->impl) return -EINVAL;
    GstAudioImpl *im = (GstAudioImpl *)ao->impl;
    if (im->paused == paused) return 0;
    im->paused = paused;
    char reply[64];
    (void)send_ctrl_cmd(im, paused ? "PAUSE\n" : "PLAY\n", reply, sizeof(reply));
    return 0;
}

static int gst_flush(AudioOut *ao) {
    if (!ao || !ao->impl) return -EINVAL;
    GstAudioImpl *im = (GstAudioImpl *)ao->impl;
    char reply[64];
    (void)send_ctrl_cmd(im, "FLUSH\n", reply, sizeof(reply));
    return 0;
}

static int64_t gst_delay_ms(AudioOut *ao) {
    if (!ao || !ao->impl) return 0;
    GstAudioImpl *im = (GstAudioImpl *)ao->impl;
    char reply[64] = {0};
    if (im->ctrl_fd >= 0 && send_ctrl_cmd(im, "POS\n", reply, sizeof(reply)) == 0) {
        if (strncmp(reply, "POS ", 4) == 0) {
            int64_t sink_ms = strtoll(reply + 4, NULL, 10);
            int64_t written_ms = (im->frames_written * 1000) / DECODER_OUT_RATE;
            if (sink_ms > 0 && written_ms >= sink_ms) {
                int64_t d = written_ms - sink_ms;
                return (d > 2000) ? 2000 : d;
            }
        }
    }
    /* Typical pipe + mixersink buffer latency fallback (D6). */
    return 250;
}

static void gst_close(AudioOut *ao) {
    if (!ao) return;
    GstAudioImpl *im = (GstAudioImpl *)ao->impl;
    if (im) {
        if (im->ctrl_fd >= 0) {
            (void)write(im->ctrl_fd, "STOP\n", 5);
            close(im->ctrl_fd);
        }
        if (im->pcm_fd >= 0) close(im->pcm_fd);
        if (im->status_fd >= 0) close(im->status_fd);
        if (im->pid > 0) {
            int status = 0;
            pid_t r = waitpid(im->pid, &status, WNOHANG);
            if (r == 0) {
                sleep_ms(50);
                if (waitpid(im->pid, &status, WNOHANG) == 0) {
                    kill(im->pid, SIGTERM);
                    waitpid(im->pid, &status, 0);
                }
            }
        }
        free(im);
    }
    free(ao);
}

int audio_out_open_gst(AudioOut **out, const char *helper_dir, char *err_msg, size_t err_cap) {
    if (!out) return -EINVAL;
    *out = NULL;
    if (err_msg && err_cap > 0) err_msg[0] = '\0';

    if (!check_or_reconnect_bt(err_msg, err_cap)) {
        return -ENOTCONN;
    }

    /* Pick armhf vs armel helper using KOReader's ld-linux-armhf.so.3 check (D6). */
    bool is_armhf = (access("/lib/ld-linux-armhf.so.3", F_OK) == 0);
    const char *dir = (helper_dir && helper_dir[0]) ? helper_dir : "/mnt/us/extensions/aloud/bin";

    char helper_path[1024];
    snprintf(helper_path, sizeof(helper_path), "%s/%s",
             dir, is_armhf ? "aloud-sink-armhf" : "aloud-sink-armel");
    bool have_helper = (access(helper_path, X_OK) == 0);
    if (!have_helper) {
        snprintf(helper_path, sizeof(helper_path), "%s/aloud-sink", dir);
        have_helper = (access(helper_path, X_OK) == 0);
    }

    int pcm_pipe[2] = {-1, -1};
    int ctrl_pipe[2] = {-1, -1};
    int status_pipe[2] = {-1, -1};

    if (pipe(pcm_pipe) != 0) {
        return -errno;
    }
    if (have_helper) {
        if (pipe(ctrl_pipe) != 0 || pipe(status_pipe) != 0) {
            close(pcm_pipe[0]); close(pcm_pipe[1]);
            if (ctrl_pipe[0] >= 0) { close(ctrl_pipe[0]); close(ctrl_pipe[1]); }
            return -errno;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        close(pcm_pipe[0]); close(pcm_pipe[1]);
        if (ctrl_pipe[0] >= 0) { close(ctrl_pipe[0]); close(ctrl_pipe[1]); }
        if (status_pipe[0] >= 0) { close(status_pipe[0]); close(status_pipe[1]); }
        return -err;
    }

    if (pid == 0) {
        close(pcm_pipe[1]);
        dup2(pcm_pipe[0], STDIN_FILENO);
        close(pcm_pipe[0]);

        if (have_helper) {
            close(ctrl_pipe[1]);
            close(status_pipe[0]);
            char cfd_str[16], sfd_str[16];
            snprintf(cfd_str, sizeof(cfd_str), "%d", ctrl_pipe[0]);
            snprintf(sfd_str, sizeof(sfd_str), "%d", status_pipe[1]);
            execl(helper_path, "aloud-sink", "--ctrl-fd", cfd_str, "--status-fd", sfd_str, (char *)NULL);
        }

        /* Fallback if helper could not start: firmware gst-launch CLI (D6). */
        if (access("/usr/lib/libgstreamer-1.0.so.0", F_OK) == 0) {
            execlp("gst-launch-1.0", "gst-launch-1.0", "-q",
                   "filesrc", "location=/dev/stdin", "!",
                   "audio/x-raw,format=S16LE,layout=interleaved,rate=44100,channels=2", "!",
                   "queue", "!", "mixersink", (char *)NULL);
        }
        execlp("gst-launch-0.10", "gst-launch-0.10", "-q",
               "filesrc", "location=/dev/stdin", "!",
               "audio/x-raw-int,endianness=1234,signed=true,width=16,depth=16,rate=44100,channels=2", "!",
               "queue", "!", "mixersink", (char *)NULL);
        execlp("gst-launch", "gst-launch", "-q",
               "filesrc", "location=/dev/stdin", "!",
               "audio/x-raw-int,endianness=1234,signed=true,width=16,depth=16,rate=44100,channels=2", "!",
               "queue", "!", "mixersink", (char *)NULL);
        _exit(127);
    }

    close(pcm_pipe[0]);
    if (have_helper) {
        close(ctrl_pipe[0]);
        close(status_pipe[1]);
    }

    AudioOut *ao = calloc(1, sizeof(*ao));
    GstAudioImpl *im = calloc(1, sizeof(*im));
    if (!ao || !im) {
        free(ao);
        free(im);
        close(pcm_pipe[1]);
        if (ctrl_pipe[1] >= 0) close(ctrl_pipe[1]);
        if (status_pipe[0] >= 0) close(status_pipe[0]);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -ENOMEM;
    }

    im->pid = pid;
    im->pcm_fd = pcm_pipe[1];
    im->ctrl_fd = have_helper ? ctrl_pipe[1] : -1;
    im->status_fd = have_helper ? status_pipe[0] : -1;

    /* If using the helper, wait briefly for its READY/OK banner so we detect
     * missing GStreamer or broken mixersink immediately. */
    if (have_helper && im->status_fd >= 0) {
        struct pollfd pfd = {.fd = im->status_fd, .events = POLLIN};
        if (poll(&pfd, 1, 800) > 0 && (pfd.revents & POLLIN)) {
            char banner[128] = {0};
            ssize_t n = read(im->status_fd, banner, sizeof(banner) - 1);
            if (n <= 0 || strncmp(banner, "ERR", 3) == 0) {
                if (err_msg && err_cap > 0) {
                    snprintf(err_msg, err_cap, "Audio sink unavailable (%s)",
                             banner[0] ? banner : "helper exited");
                }
                ao->impl = im;
                ao->close = gst_close;
                gst_close(ao);
                return -EIO;
            }
        }
    }

    ao->type = AUDIO_BACKEND_KINDLE_GST;
    ao->impl = im;
    ao->write_s16le = gst_write_s16le;
    ao->set_paused = gst_set_paused;
    ao->flush = gst_flush;
    ao->delay_ms = gst_delay_ms;
    ao->close = gst_close;
    *out = ao;
    return 0;
}
