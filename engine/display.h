#ifndef XIAOJI_DISPLAY_H
#define XIAOJI_DISPLAY_H

/* The display's size in pixels and its density: the one setting the framebuffer parameters (rcGetFBParam in gl_null.c and
 * gl_webgl.c, FB_WIDTH, FB_HEIGHT, FB_XDPI, FB_YDPI), ro.sf.lcd_density (props.c), the touchscreen's ABS_MT_POSITION ranges (evdev.c),
 * and the web page's frame size (web/worker.js and web/input.js mirror the size). A backend that changes the size changes it here. 540x960 at 240 dpi is 360x640 dp, chosen for the frame
 * copy's speed (about 2 MB per frame). */
#define XJ_DISPLAY_W 540
#define XJ_DISPLAY_H 960
#define XJ_DISPLAY_DPI 240
#define XJ_DISPLAY_DPI_STR "240"

#endif
