#ifndef XIAOJI_GL_DISPLAY_H
#define XIAOJI_GL_DISPLAY_H

/* The one display setting (docs/graphics.md, G2): the size the host reports (rcGetFBParam FB_WIDTH and FB_HEIGHT),
 * its density (FB_XDPI and FB_YDPI, and ro.sf.lcd_density in props.c), and the frame the web page shows.
 * 540x960 at 240 dpi is 360x640 dp, a small phone. It is modest so that each frame copy is about 2 MB. */
#define XJ_FB_WIDTH 540
#define XJ_FB_HEIGHT 960
#define XJ_FB_DPI 240
#define XJ_FB_DPI_STR "240"

#endif
