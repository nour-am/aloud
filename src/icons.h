#ifndef ALOUD_ICONS_H
#define ALOUD_ICONS_H

#include <stdint.h>
#include "canvas.h"

typedef enum {
    ICON_PLAY = 0,
    ICON_PAUSE,
    ICON_PREV_CHAPTER,
    ICON_NEXT_CHAPTER,
    ICON_SKIP_BACK,
    ICON_SKIP_FWD,
    ICON_CHEVRON_LEFT,
    ICON_CHEVRON_RIGHT,
    ICON_CLOSE
} IconId;

/* Rasterize a vector icon of size_px x size_px at (x, y) using stbtt_Rasterize (D5). */
void icon_draw(Canvas *c, IconId icon, int x, int y, int size_px, uint8_t tone);

#endif /* ALOUD_ICONS_H */
