#ifndef ALOUD_PLATFORM_H
#define ALOUD_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "canvas.h"

typedef enum {
    ALOUD_WFM_NONE = 0,
    ALOUD_WFM_DU,         /* Fast monochrome update (play icon toggle, time tick, scrubber) */
    ALOUD_WFM_GL16,       /* Non-flashing 16-gray partial update (lists, sheets) */
    ALOUD_WFM_GC16_FLASH  /* Flashing 16-gray full fidelity update (covers, every 6th full repaint) */
} AloudWaveform;

typedef enum {
    ALOUD_EV_NONE = 0,
    ALOUD_EV_TAP,
    ALOUD_EV_SWIPE_LEFT,
    ALOUD_EV_SWIPE_RIGHT,
    ALOUD_EV_KEY_NEXT,
    ALOUD_EV_KEY_PREV,
    ALOUD_EV_QUIT
} AloudEventType;

typedef struct {
    AloudEventType type;
    int x;
    int y;
    int start_x;
    int start_y;
} AloudInputEvent;

/* Portable evdev constants for touch/key state machine (matches <linux/input.h>
 * so unit tests on macOS and real evdev on Kindle run the exact same code). */
#define ALOUD_EV_SYN 0x00
#define ALOUD_EV_KEY 0x01
#define ALOUD_EV_ABS 0x03

#define ALOUD_SYN_REPORT    0
#define ALOUD_SYN_MT_REPORT 2

#define ALOUD_BTN_TOUCH    0x14a
#define ALOUD_KEY_HOME     102
#define ALOUD_KEY_PAGEUP   104
#define ALOUD_KEY_PAGEDOWN 109
#define ALOUD_KEY_FRAME_TAP 65

#define ALOUD_ABS_X              0x00
#define ALOUD_ABS_Y              0x01
#define ALOUD_ABS_MT_SLOT        0x2f
#define ALOUD_ABS_MT_POSITION_X  0x35
#define ALOUD_ABS_MT_POSITION_Y  0x36
#define ALOUD_ABS_MT_TRACKING_ID 0x39

typedef struct {
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int view_w;
    int view_h;
    int dpi;
    uint8_t canonical_rota; /* 0 = UR, 1 = CW, 2 = UD, 3 = CCW */
    bool touch_swap_axes;
    bool touch_mirror_x;
    bool touch_mirror_y;
    bool is_oasis; /* Oasis reverses 104/109 vs Voyage (RESEARCH.md §5) */

    /* Active contact tracking (supports Protocol A, Protocol B, and KT4 BTN_TOUCH quirk). */
    int cur_slot;
    int raw_x;
    int raw_y;
    bool have_x;
    bool have_y;
    bool contact_down;
    bool pending_down;
    bool pending_lift;
    bool mt_proto_a_seen_pos;
    int start_screen_x;
    int start_screen_y;
    int last_screen_x;
    int last_screen_y;
} TouchDecoder;

void touch_decoder_init(TouchDecoder *td, int min_x, int max_x, int min_y, int max_y,
                        int view_w, int view_h, int dpi, uint8_t canonical_rota,
                        bool swap_axes, bool mirror_x, bool mirror_y, bool is_oasis);

void touch_map_coords(const TouchDecoder *td, int raw_x, int raw_y,
                      int *out_x, int *out_y);

/* Feed a raw Linux input_event (type, code, value). Returns true and populates
 * *out_ev when a complete gesture (TAP, SWIPE_LEFT, SWIPE_RIGHT, KEY_*) finishes. */
bool touch_decoder_feed(TouchDecoder *td, uint16_t type, uint16_t code, int32_t value,
                        AloudInputEvent *out_ev);

typedef struct Platform Platform;

/* Initialize the active platform (Kindle FBInk+evdev on Kindle build, or simulator on host). */
int platform_init(Platform **out, int sim_w, int sim_h, int sim_dpi);
void platform_free(Platform *plat);

int platform_width(const Platform *plat);
int platform_height(const Platform *plat);
int platform_dpi(const Platform *plat);
bool platform_has_volume_control(const Platform *plat);

/* Blit the rectangular region r from canvas to the display and trigger an e-ink
 * refresh with the given waveform. */
int platform_refresh(Platform *plat, const Canvas *c, AloudRect r, AloudWaveform wfm);

/* Populate pollable input file descriptors. Returns number of fds written. */
int platform_get_input_fds(Platform *plat, int *fds_out, int max_fds);

/* Read pending input events from any ready input fd. Returns true if an event was produced. */
bool platform_poll_event(Platform *plat, AloudInputEvent *out_ev);

/* Background playback (ARCHITECTURE.md D13): close every input device so the stock
 * reader gets all touches and none of them wake Aloud. */
void platform_release_input(Platform *plat);

/* Coming back to the foreground: re-read the framebuffer state (the reader may have
 * rotated it meanwhile) and grab input again. Returns 1 if the view size changed, 0 if
 * not, negative on error. */
int platform_take_display(Platform *plat);

/* Write the current canvas to a grayscale PNG file (used by simulator and golden tests). */
int platform_write_png(const Canvas *c, const char *png_path);

#endif /* ALOUD_PLATFORM_H */
