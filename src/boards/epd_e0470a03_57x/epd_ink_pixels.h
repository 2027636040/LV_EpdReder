/* SPDX-License-Identifier: Apache-2.0 */
#ifndef __EPD_INK_PIXELS_H__
#define __EPD_INK_PIXELS_H__

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define EPD_INK_NATIVE_WIDTH 1216U
#define EPD_INK_NATIVE_HEIGHT 684U
#define EPD_INK_PIXELS (EPD_INK_NATIVE_WIDTH * EPD_INK_NATIVE_HEIGHT)

typedef struct
{
    unsigned int x0, y0, x1, y1;
} epd_ink_area_t;

typedef struct
{
    epd_ink_area_t area, previous;
    bool pending, previous_valid, all_white;
} epd_ink_pixels_t;

static inline uint32_t epd_ink_unpack4(uint16_t packed)
{
    return (packed & 15U) | ((packed & 0xf0U) << 4) |
           ((packed & 0xf00U) << 8) | ((packed & 0xf000U) << 12);
}

static inline bool epd_ink_pixels_begin(epd_ink_pixels_t *state, uint8_t *mixed)
{
    bool white = true;
    for (unsigned int i = 0; i < EPD_INK_PIXELS; ++i)
    {
        unsigned int gray = mixed[i] & 15U;
        if (gray != 0 && gray != 15) return false;
        if (gray != 15) white = false;
    }
    for (unsigned int i = 0; i < EPD_INK_PIXELS; ++i)
    {
        unsigned int gray = mixed[i] & 15U;
        mixed[i] = gray | (gray << 4);
    }
    memset(state, 0, sizeof(*state));
    state->all_white = white;
    return true;
}

static inline bool epd_ink_pixels_region(epd_ink_pixels_t *state, unsigned int x,
                                       unsigned int y, unsigned int w, unsigned int h)
{
    state->pending = false;
    if (!w || !h || x >= EPD_INK_NATIVE_HEIGHT || y >= EPD_INK_NATIVE_WIDTH ||
        w > EPD_INK_NATIVE_HEIGHT - x || h > EPD_INK_NATIVE_WIDTH - y) return false;
    state->area.x0 = y & ~3U;
    state->area.x1 = (y + h - 1) | 3U;
    state->area.y0 = EPD_INK_NATIVE_HEIGHT - x - w;
    state->area.y1 = EPD_INK_NATIVE_HEIGHT - 1 - x;
    state->pending = true;
    return true;
}

/* Validate the entire region before changing history. A rejected draw consumes
 * its region, but leaves both pixels and the previous transition area intact. */
static inline bool epd_ink_pixels_prepare(epd_ink_pixels_t *state, uint8_t *mixed,
                                        const uint8_t *source, uint32_t *changed)
{
    *changed = 0;
    if (!state->pending) return false;
    state->pending = false;
    const epd_ink_area_t *area = &state->area;
    if (state->all_white && !area->x0 && !area->y0 &&
        area->x1 == EPD_INK_NATIVE_WIDTH - 1 && area->y1 == EPD_INK_NATIVE_HEIGHT - 1)
    {
        unsigned int i;
        for (i = 0; i < EPD_INK_PIXELS / 2; ++i) if (source[i]) break;
        if (i == EPD_INK_PIXELS / 2)
        {
            memset(mixed, 0xf0, EPD_INK_PIXELS);
            *changed = EPD_INK_PIXELS;
            state->previous = *area;
            state->previous_valid = true;
            state->all_white = false;
            return true;
        }
    }
    for (unsigned int y = area->y0; y <= area->y1; ++y)
    {
        for (unsigned int x = area->x0; x <= area->x1; x += 4)
        {
            unsigned int pos = y * EPD_INK_NATIVE_WIDTH + x;
            uint16_t packed;
            uint32_t old;
            memcpy(&packed, source + pos / 2, sizeof(packed));
            memcpy(&old, mixed + pos, sizeof(old));
            uint32_t next = epd_ink_unpack4(packed);
            old &= 0x0f0f0f0fU;
            if (next != (next & 0x01010101U) * 15U || ((~old) & next)) return false;
        }
    }
    if (state->previous_valid)
    {
        const epd_ink_area_t *previous = &state->previous;
        for (unsigned int y = previous->y0; y <= previous->y1; ++y)
        {
            for (unsigned int x = previous->x0; x <= previous->x1; x += 4)
            {
                uint8_t *pixel = mixed + y * EPD_INK_NATIVE_WIDTH + x;
                uint32_t value;
                memcpy(&value, pixel, sizeof(value));
                value &= 0x0f0f0f0fU;
                value |= value << 4;
                memcpy(pixel, &value, sizeof(value));
            }
        }
    }
    for (unsigned int y = area->y0; y <= area->y1; ++y)
    {
        for (unsigned int x = area->x0; x <= area->x1; x += 4)
        {
            unsigned int pos = y * EPD_INK_NATIVE_WIDTH + x;
            uint16_t packed;
            uint32_t old;
            memcpy(&packed, source + pos / 2, sizeof(packed));
            memcpy(&old, mixed + pos, sizeof(old));
            uint32_t next = epd_ink_unpack4(packed);
            old &= 0x0f0f0f0fU;
            uint32_t changes = (old & ~next) & 0x01010101U;
            uint32_t value = (old << 4) | next;
            memcpy(mixed + pos, &value, sizeof(value));
            *changed += (changes * 0x01010101U) >> 24;
        }
    }
    state->previous = *area;
    state->previous_valid = true;
    if (*changed) state->all_white = false;
    return true;
}

#endif
