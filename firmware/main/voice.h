/* voice.h - offline voice commands for the muma board (esp-sr).
 * "Computer" (WakeNet9) opens a listening window (VOICE_WINDOW_MS); every recognised
 * command re-opens it, so "next ... next ... stop" needs one wake word. */
#ifndef VOICE_H
#define VOICE_H
#include <stdbool.h>
#include <stdint.h>

enum {
    VC_NONE = 0,
    VC_FEED, VC_LIGHT_ON, VC_LIGHT_OFF, VC_TAP, VC_SLEEP,
    VC_BUS, VC_FISH,
    VC_NEXT, VC_PREV, VC_STOP, VC_SUMMARY,
    VC_CANCEL,
    VC_WAKE,                           /* the wake word was heard (screen badge) */
    VC_COUNT
};

bool voice_init(void);                 /* models from the "model" partition; false = no voice */
int  voice_take(void);                 /* the next recognised command (VC_*), once; VC_NONE if none */
bool voice_busy(void);                 /* speech in progress or the window open: heavy work should wait */
bool voice_listening(void);            /* the window is open (after "computer") */
const char *voice_last_label(int *age_ms);   /* what was last heard, for the screen */
/* the badge: drawn onto any RGB565 frame (stride in pixels), scale = font dot size */
void voice_overlay(uint16_t *fb, int stride, int w, int h, int scale);
#endif
