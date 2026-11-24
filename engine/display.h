#ifndef XIAOJI_DISPLAY_H
#define XIAOJI_DISPLAY_H

/* The display's size in pixels: the one setting the framebuffer parameters (rcGetFBParam in gl_null.c, FB_WIDTH and
 * FB_HEIGHT) and the touchscreen's ABS_MT_POSITION ranges (evdev.c) both read. A backend that changes the size changes
 * it here. */
#define XJ_DISPLAY_W 720
#define XJ_DISPLAY_H 1280

#endif
