#define _POSIX_C_SOURCE 200809L
#include "icons.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

typedef struct {
    stbtt_vertex verts[256];
    int count;
} PathBuilder;

static void pb_move(PathBuilder *pb, int x, int y) {
    if (pb->count >= 256) return;
    stbtt_vertex *v = &pb->verts[pb->count++];
    memset(v, 0, sizeof(*v));
    v->type = STBTT_vmove;
    v->x = (stbtt_vertex_type)x;
    v->y = (stbtt_vertex_type)y;
}

static void pb_line(PathBuilder *pb, int x, int y) {
    if (pb->count >= 256) return;
    stbtt_vertex *v = &pb->verts[pb->count++];
    memset(v, 0, sizeof(*v));
    v->type = STBTT_vline;
    v->x = (stbtt_vertex_type)x;
    v->y = (stbtt_vertex_type)y;
}

static void pb_rect(PathBuilder *pb, int x0, int y0, int x1, int y1) {
    pb_move(pb, x0, y0);
    pb_line(pb, x1, y0);
    pb_line(pb, x1, y1);
    pb_line(pb, x0, y1);
    pb_line(pb, x0, y0);
}

static void pb_thick_segment(PathBuilder *pb, float x0, float y0, float x1, float y1, float half_w) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) return;
    float nx = (-dy / len) * half_w;
    float ny = (dx / len) * half_w;

    int ax = (int)lroundf(x0 + nx), ay = (int)lroundf(y0 + ny);
    int bx = (int)lroundf(x1 + nx), by = (int)lroundf(y1 + ny);
    int cx = (int)lroundf(x1 - nx), cy = (int)lroundf(y1 - ny);
    int dxp = (int)lroundf(x0 - nx), dyp = (int)lroundf(y0 - ny);

    pb_move(pb, ax, ay);
    pb_line(pb, bx, by);
    pb_line(pb, cx, cy);
    pb_line(pb, dxp, dyp);
    pb_line(pb, ax, ay);
}

static void pb_arc_band(PathBuilder *pb, float cx, float cy, float r_inner, float r_outer,
                        float deg_start, float deg_end, int steps) {
    if (steps < 4) steps = 4;
    if (steps > 48) steps = 48;
    const float kPi = 3.1415926535f;

    float a0 = deg_start * kPi / 180.0f;
    pb_move(pb, (int)lroundf(cx + cosf(a0) * r_outer), (int)lroundf(cy + sinf(a0) * r_outer));
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / (float)steps;
        float a = (deg_start + (deg_end - deg_start) * t) * kPi / 180.0f;
        pb_line(pb, (int)lroundf(cx + cosf(a) * r_outer), (int)lroundf(cy + sinf(a) * r_outer));
    }
    for (int i = steps; i >= 0; i--) {
        float t = (float)i / (float)steps;
        float a = (deg_start + (deg_end - deg_start) * t) * kPi / 180.0f;
        pb_line(pb, (int)lroundf(cx + cosf(a) * r_inner), (int)lroundf(cy + sinf(a) * r_inner));
    }
    pb_line(pb, (int)lroundf(cx + cosf(a0) * r_outer), (int)lroundf(cy + sinf(a0) * r_outer));
}

void icon_draw(Canvas *c, IconId icon, int x, int y, int size_px, uint8_t tone) {
    if (!c || size_px <= 2) return;

    PathBuilder pb;
    memset(&pb, 0, sizeof(pb));

    /* All icon coordinates are defined in a 1000 x 1000 box. */
    switch (icon) {
        case ICON_PLAY:
            /* Optically shifted slightly right of geometric center inside a circle. */
            pb_move(&pb, 300, 200);
            pb_line(&pb, 780, 500);
            pb_line(&pb, 300, 800);
            pb_line(&pb, 300, 200);
            break;

        case ICON_PAUSE:
            pb_rect(&pb, 260, 210, 430, 790);
            pb_rect(&pb, 570, 210, 740, 790);
            break;

        case ICON_PREV_CHAPTER:
            pb_rect(&pb, 180, 220, 290, 780);
            pb_move(&pb, 800, 220);
            pb_line(&pb, 340, 500);
            pb_line(&pb, 800, 780);
            pb_line(&pb, 800, 220);
            break;

        case ICON_NEXT_CHAPTER:
            pb_move(&pb, 200, 220);
            pb_line(&pb, 660, 500);
            pb_line(&pb, 200, 780);
            pb_line(&pb, 200, 220);
            pb_rect(&pb, 710, 220, 820, 780);
            break;

        case ICON_SKIP_BACK:
            pb_arc_band(&pb, 500.0f, 540.0f, 310.0f, 395.0f, -65.0f, 225.0f, 28);
            /* Counter-clockwise arrowhead at top-left (~-90 deg). */
            pb_move(&pb, 520, 70);
            pb_line(&pb, 290, 195);
            pb_line(&pb, 520, 320);
            pb_line(&pb, 520, 70);
            break;

        case ICON_SKIP_FWD:
            pb_arc_band(&pb, 500.0f, 540.0f, 310.0f, 395.0f, -45.0f, 245.0f, 28);
            /* Clockwise arrowhead at top-right (~-90 deg). */
            pb_move(&pb, 480, 70);
            pb_line(&pb, 710, 195);
            pb_line(&pb, 480, 320);
            pb_line(&pb, 480, 70);
            break;

        case ICON_CHEVRON_LEFT:
            pb_thick_segment(&pb, 650.0f, 180.0f, 330.0f, 500.0f, 52.0f);
            pb_thick_segment(&pb, 330.0f, 500.0f, 650.0f, 820.0f, 52.0f);
            break;

        case ICON_CHEVRON_RIGHT:
            pb_thick_segment(&pb, 350.0f, 180.0f, 670.0f, 500.0f, 52.0f);
            pb_thick_segment(&pb, 670.0f, 500.0f, 350.0f, 820.0f, 52.0f);
            break;

        case ICON_CLOSE:
            pb_thick_segment(&pb, 220.0f, 220.0f, 780.0f, 780.0f, 48.0f);
            pb_thick_segment(&pb, 780.0f, 220.0f, 220.0f, 780.0f, 48.0f);
            break;
    }

    if (pb.count == 0) return;

    uint8_t *alpha = calloc((size_t)size_px * (size_t)size_px, 1);
    if (!alpha) return;

    stbtt__bitmap bmp;
    bmp.w = size_px;
    bmp.h = size_px;
    bmp.stride = size_px;
    bmp.pixels = alpha;

    float scale = (float)size_px / 1000.0f;
    stbtt_Rasterize(&bmp, 0.35f, pb.verts, pb.count, scale, scale, 0.0f, 0.0f, 0, 0, 0, NULL);
    canvas_blend_alpha(c, x, y, alpha, size_px, size_px, size_px, tone);
    free(alpha);
}
