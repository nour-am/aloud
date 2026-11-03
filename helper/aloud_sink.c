/* aloud-sink: tiny dynamically linked GStreamer mixersink bridge for Kindle.
 *
 * Why this exists (ARCHITECTURE.md D2 & D6, RESEARCH.md §6.2-6.3):
 * Bluetooth Kindles have no ALSA card; the only working path to A2DP is Lab126's
 * proprietary GStreamer `mixersink` plugin (GStreamer 0.10 on FW < 5.19, 1.0 on
 * FW >= 5.19). Because the main `aloud` binary is statically linked with musl for
 * cross-firmware immunity, it cannot dlopen glibc-linked GStreamer plugins. This
 * ~200-line helper is built twice with Zig (`arm-linux-gnueabi.2.12` for soft-float
 * FW < 5.16.3 and `arm-linux-gnueabihf.2.20` for hard-float FW >= 5.16.3), dlopens
 * whichever libgstreamer is on the device, and feeds S16LE 44.1 kHz stereo PCM from
 * stdin into `mixersink`. */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef enum {
    GST_STATE_VOID_PENDING = 0,
    GST_STATE_NULL         = 1,
    GST_STATE_READY        = 2,
    GST_STATE_PAUSED       = 3,
    GST_STATE_PLAYING      = 4
} GstState;

typedef enum {
    GST_FORMAT_DEFAULT = 1,
    GST_FORMAT_BYTES   = 2,
    GST_FORMAT_TIME    = 3
} GstFormat;

typedef void (*gst_init_fn)(int *argc, char ***argv);
typedef void *(*gst_parse_launch_fn)(const char *pipeline_description, void **error);
typedef int (*gst_element_set_state_fn)(void *element, GstState state);
typedef int (*gst_element_get_state_fn)(void *element, GstState *state, GstState *pending, uint64_t timeout);
typedef int (*gst_element_query_position_10_fn)(void *element, GstFormat format, int64_t *cur);
typedef int (*gst_element_query_position_010_fn)(void *element, GstFormat *format, int64_t *cur);
typedef void (*gst_object_unref_fn)(void *object);

static void write_status(int status_fd, const char *msg) {
    if (status_fd < 0 || !msg) return;
    size_t len = strlen(msg);
    (void)write(status_fd, msg, len);
}

/* Quarter-wave 16-bit sine lookup table (64 entries) for dependency-free 440 Hz
 * diagnostic tone generation (`aloud-sink --tone 10`). */
static const int16_t kSineQuarter[65] = {
        0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
     6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767
};

static int16_t sine_sample_q8(uint32_t phase_u8) {
    uint8_t idx = (uint8_t)(phase_u8 & 0xFFU);
    uint8_t quad = (idx >> 6) & 3U;
    uint8_t off = idx & 63U;
    int16_t v = (quad & 1U) ? kSineQuarter[64 - off] : kSineQuarter[off];
    /* Attenuate to ~35% full scale so headphone testing is comfortable. */
    int32_t scaled = ((int32_t)v * 90) >> 8;
    return (quad & 2U) ? (int16_t)(-scaled) : (int16_t)scaled;
}

static int feed_self_tone_pipe(int seconds) {
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
        const int rate = 44100;
        const int freq = 440;
        int total_frames = rate * seconds;
        int16_t chunk[1024 * 2];
        uint32_t phase_fp = 0;
        uint32_t step_fp = (uint32_t)(((uint64_t)freq << 24) / (uint64_t)rate);

        int sent = 0;
        while (sent < total_frames) {
            int n = total_frames - sent;
            if (n > 1024) n = 1024;
            for (int i = 0; i < n; i++) {
                int16_t s = sine_sample_q8(phase_fp >> 16);
                chunk[i * 2] = s;
                chunk[i * 2 + 1] = s;
                phase_fp += step_fp;
            }
            size_t bytes = (size_t)n * 4U;
            const uint8_t *p = (const uint8_t *)chunk;
            while (bytes > 0) {
                ssize_t w = write(pfd[1], p, bytes);
                if (w <= 0) _exit(0);
                p += w;
                bytes -= (size_t)w;
            }
            sent += n;
        }
        close(pfd[1]);
        _exit(0);
    }
    close(pfd[1]);
    dup2(pfd[0], STDIN_FILENO);
    close(pfd[0]);
    return 0;
}

int main(int argc, char **argv) {
    int ctrl_fd = -1;
    int status_fd = -1;
    int tone_seconds = 0;
    const char *fifo_path = NULL;

    /* The parent closes the status pipe as soon as it sends STOP; a late reply must
     * not kill us (SIGPIPE) before the pipeline is torn down cleanly. */
    signal(SIGPIPE, SIG_IGN);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ctrl-fd") == 0 && i + 1 < argc) {
            ctrl_fd = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--status-fd") == 0 && i + 1 < argc) {
            status_fd = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fifo") == 0 && i + 1 < argc) {
            fifo_path = argv[++i];
        } else if (strcmp(argv[i], "--tone") == 0) {
            tone_seconds = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : 10;
            if (tone_seconds <= 0) tone_seconds = 10;
        }
    }

    if (tone_seconds > 0) {
        if (feed_self_tone_pipe(tone_seconds) != 0) {
            fprintf(stderr, "aloud-sink: failed to start tone generator pipe\n");
            return 1;
        }
    }

    bool is_gst10 = true;
    void *lib = dlopen("libgstreamer-1.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        lib = dlopen("/usr/lib/libgstreamer-1.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    }
    if (!lib) {
        is_gst10 = false;
        lib = dlopen("libgstreamer-0.10.so.0", RTLD_NOW | RTLD_GLOBAL);
        if (!lib) {
            lib = dlopen("/usr/lib/libgstreamer-0.10.so.0", RTLD_NOW | RTLD_GLOBAL);
        }
    }
    if (!lib) {
        write_status(status_fd, "ERR no-libgstreamer\n");
        fprintf(stderr, "aloud-sink: neither libgstreamer-1.0 nor 0.10 found\n");
        return 2;
    }

    gst_init_fn p_gst_init = (gst_init_fn)dlsym(lib, "gst_init");
    gst_parse_launch_fn p_parse = (gst_parse_launch_fn)dlsym(lib, "gst_parse_launch");
    gst_element_set_state_fn p_set_state = (gst_element_set_state_fn)dlsym(lib, "gst_element_set_state");
    gst_element_get_state_fn p_get_state = (gst_element_get_state_fn)dlsym(lib, "gst_element_get_state");
    gst_object_unref_fn p_unref = (gst_object_unref_fn)dlsym(lib, "gst_object_unref");
    void *p_query_pos = dlsym(lib, "gst_element_query_position");

    if (!p_gst_init || !p_parse || !p_set_state || !p_unref) {
        write_status(status_fd, "ERR missing-gst-symbols\n");
        return 3;
    }

    int gst_argc = 0;
    char **gst_argv = NULL;
    p_gst_init(&gst_argc, &gst_argv);

    const char *caps = is_gst10
        ? "audio/x-raw, format=S16LE, layout=interleaved, rate=44100, channels=2"
        : "audio/x-raw-int, endianness=1234, signed=true, width=16, depth=16, rate=44100, channels=2";

    char desc[512];
    void *pipeline = NULL;
    void *gerr = NULL;

    const char *q_opts = "queue max-size-buffers=2 max-size-bytes=16384 max-size-time=100000000";

    if (fifo_path) {
        snprintf(desc, sizeof(desc), "filesrc location=\"%s\" ! %s ! %s ! mixersink", fifo_path, caps, q_opts);
        pipeline = p_parse(desc, &gerr);
    } else {
        /* Use filesrc location=/dev/stdin first (part of coreelements, present on all
         * Kindle firmwares including stripped FW 5.19.x; RESEARCH.md §6.3). */
        snprintf(desc, sizeof(desc), "filesrc location=/dev/stdin ! %s ! %s ! mixersink", caps, q_opts);
        pipeline = p_parse(desc, &gerr);
        if (!pipeline || gerr) {
            gerr = NULL;
            snprintf(desc, sizeof(desc), "fdsrc fd=0 ! %s ! %s ! mixersink", caps, q_opts);
            pipeline = p_parse(desc, &gerr);
        }
    }

    if (!pipeline) {
        write_status(status_fd, "ERR parse-pipeline-failed\n");
        fprintf(stderr, "aloud-sink: failed to create pipeline: %s\n", desc);
        _exit(4);
    }

    if (p_set_state(pipeline, GST_STATE_PLAYING) <= 0) {
        write_status(status_fd, "ERR set-playing-failed\n");
        _exit(5);
    }

    write_status(status_fd, "OK\n");

    if (ctrl_fd < 0) {
        /* Standalone / tone mode: wait for duration or until killed. */
        int wait_s = (tone_seconds > 0) ? (tone_seconds + 1) : 3600;
        for (int i = 0; i < wait_s * 10; i++) {
            usleep(100000);
        }
        _exit(0);
    }

    char cmd_buf[128];
    size_t cmd_len = 0;
    bool running = true;
    while (running) {
        struct pollfd pfd = {.fd = ctrl_fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 500);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            break;
        }
        if (pfd.revents & POLLIN) {
            ssize_t n = read(ctrl_fd, cmd_buf + cmd_len, sizeof(cmd_buf) - 1 - cmd_len);
            if (n <= 0) break;
            cmd_len += (size_t)n;
            cmd_buf[cmd_len] = '\0';

            char *nl;
            while ((nl = strchr(cmd_buf, '\n')) != NULL) {
                *nl = '\0';
                if (strcmp(cmd_buf, "PAUSE") == 0 || strcmp(cmd_buf, "PLAY") == 0) {
                    /* mixersink hands audio to Lab126's mixer daemon, which buffers
                     * seconds of it; only pausing the pipeline stops that playout at
                     * once. PLAYING<->PAUSED never needs filesrc's stream lock (only
                     * READY/NULL do), and it is exactly how KinAMP pauses on device
                     * (RESEARCH.md §6.3). The call returns ASYNC without blocking. */
                    (void)p_set_state(pipeline, strcmp(cmd_buf, "PAUSE") == 0
                                                    ? GST_STATE_PAUSED : GST_STATE_PLAYING);
                    write_status(status_fd, "OK\n");
                } else if (strcmp(cmd_buf, "FLUSH") == 0) {
                    /* Buffered mixer audio cannot be dropped in place; the parent flushes
                     * by stopping this helper and starting a fresh pipeline. */
                    write_status(status_fd, "OK\n");
                } else if (strcmp(cmd_buf, "POS") == 0) {
                    int64_t pos_ns = 0;
                    int ok = 0;
                    if (p_query_pos) {
                        if (is_gst10) {
                            ok = ((gst_element_query_position_10_fn)p_query_pos)(
                                pipeline, GST_FORMAT_TIME, &pos_ns);
                        } else {
                            GstFormat fmt = GST_FORMAT_TIME;
                            ok = ((gst_element_query_position_010_fn)p_query_pos)(
                                pipeline, &fmt, &pos_ns);
                        }
                    }
                    char rep[64];
                    int64_t pos_ms = (ok && pos_ns > 0) ? (pos_ns / 1000000LL) : 0;
                    snprintf(rep, sizeof(rep), "POS %" PRId64 "\n", pos_ms);
                    write_status(status_fd, rep);
                } else if (strcmp(cmd_buf, "STOP") == 0) {
                    running = false;
                    break;
                }
                size_t rem = cmd_len - (size_t)(nl - cmd_buf + 1);
                memmove(cmd_buf, nl + 1, rem + 1);
                cmd_len = rem;
            }
        }
    }

    (void)p_get_state;
    (void)p_unref;
    /* Silence first, then tear down so mixersink closes its mixer stream cleanly
     * (otherwise the daemon may keep playing what it buffered). The parent closes the
     * PCM writer before STOP, so filesrc sees EOF and NULL cannot block on read();
     * the alarm is a backstop in case mixersink IPC teardown hangs. */
    alarm(1);
    (void)p_set_state(pipeline, GST_STATE_PAUSED);
    (void)p_set_state(pipeline, GST_STATE_NULL);
    _exit(0);
}
