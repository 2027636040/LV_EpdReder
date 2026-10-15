/* SPDX-License-Identifier: Apache-2.0 */
#ifndef __EPD_INK_H__
#define __EPD_INK_H__

#ifdef __cplusplus
extern "C" {
#endif

enum { EPD_INK_FAST_FRAMES = 5, EPD_INK_DEFAULT_FRAMES = 7 };

/* EPD DevKit only. One rendering thread owns configure/region/draw/disable.
 * Wait for all LVGL/LCD work before starting. Each draw uses the synchronous
 * RT-Thread draw_rect operation and a full native 1216x684 GRAY4 buffer, with
 * the left pixel in the low nibble. Coordinates below are portrait 684x1216.
 * The displayed background must be binary. Only white-to-black is accepted.
 * Configure(0) ends the powered session and requests a normal full refresh. */
int epd_ink_configure(unsigned int frames);
int epd_ink_region(unsigned int x, unsigned int y, unsigned int w, unsigned int h);

#ifdef __cplusplus
}
#endif
#endif
