/*
 *  Copyright (C) 2012-2025  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

// A tiny self-contained 5x7 bitmap font, shared by the loading screen
// (xwirepresentSDL.cpp) and the X core-text path (xwireconnection.cpp's
// PolyText/ImageText handlers). Header-only with `inline` so both translation
// units get one copy of the table without an ODR/duplicate-symbol clash. ASCII
// only — digits, A-Z (lowercase folded to upper), and a few symbols; everything
// else renders blank. Ugly but legible, which is all wine's core-text controls
// need until a real FreeType rasterizer lands.

#ifndef __XWIREFONT_H__
#define __XWIREFONT_H__

#include <cstdint>

namespace xwirefont {

// Advance per character cell (glyph is 5 px wide + 1 px gap) and glyph height.
inline constexpr int CELL_W  = 6;
inline constexpr int GLYPH_H = 7;

struct Glyph { uint8_t col[5]; };   // 5 columns, bit row LSB = top pixel

// Write the 5 columns of glyph `c` into `out` (no shared static scratch, so it's
// safe to call from multiple guest threads concurrently). Lowercase folds to
// upper; unknown chars render blank.
inline void glyphInto(char c, uint8_t out[5]) {
    static const struct { char c; uint8_t col[5]; } font[] = {
        {' ',{0,0,0,0,0}}, {'.',{0,0x60,0x60,0,0}}, {',',{0,0x80,0x60,0,0}},
        {'-',{0x08,0x08,0x08,0x08,0x08}}, {':',{0,0x36,0x36,0,0}},
        {'/',{0x60,0x10,0x08,0x04,0x03}},
        {'%',{0x63,0x13,0x08,0x64,0x63}}, {'(',{0,0x3e,0x41,0,0}}, {')',{0,0x41,0x3e,0,0}},
        {'0',{0x3e,0x51,0x49,0x45,0x3e}}, {'1',{0,0x42,0x7f,0x40,0}},
        {'2',{0x42,0x61,0x51,0x49,0x46}}, {'3',{0x21,0x41,0x45,0x4b,0x31}},
        {'4',{0x18,0x14,0x12,0x7f,0x10}}, {'5',{0x27,0x45,0x45,0x45,0x39}},
        {'6',{0x3c,0x4a,0x49,0x49,0x30}}, {'7',{0x01,0x71,0x09,0x05,0x03}},
        {'8',{0x36,0x49,0x49,0x49,0x36}}, {'9',{0x06,0x49,0x49,0x29,0x1e}},
        {'A',{0x7e,0x11,0x11,0x11,0x7e}}, {'B',{0x7f,0x49,0x49,0x49,0x36}},
        {'C',{0x3e,0x41,0x41,0x41,0x22}}, {'D',{0x7f,0x41,0x41,0x22,0x1c}},
        {'E',{0x7f,0x49,0x49,0x49,0x41}}, {'F',{0x7f,0x09,0x09,0x09,0x01}},
        {'G',{0x3e,0x41,0x49,0x49,0x7a}}, {'H',{0x7f,0x08,0x08,0x08,0x7f}},
        {'I',{0,0x41,0x7f,0x41,0}}, {'J',{0x20,0x40,0x41,0x3f,0x01}},
        {'K',{0x7f,0x08,0x14,0x22,0x41}}, {'L',{0x7f,0x40,0x40,0x40,0x40}},
        {'M',{0x7f,0x02,0x0c,0x02,0x7f}}, {'N',{0x7f,0x04,0x08,0x10,0x7f}},
        {'O',{0x3e,0x41,0x41,0x41,0x3e}}, {'P',{0x7f,0x09,0x09,0x09,0x06}},
        {'Q',{0x3e,0x41,0x51,0x21,0x5e}}, {'R',{0x7f,0x09,0x19,0x29,0x46}},
        {'S',{0x46,0x49,0x49,0x49,0x31}}, {'T',{0x01,0x01,0x7f,0x01,0x01}},
        {'U',{0x3f,0x40,0x40,0x40,0x3f}}, {'V',{0x1f,0x20,0x40,0x20,0x1f}},
        {'W',{0x7f,0x20,0x18,0x20,0x7f}}, {'X',{0x63,0x14,0x08,0x14,0x63}},
        {'Y',{0x07,0x08,0x70,0x08,0x07}}, {'Z',{0x61,0x51,0x49,0x45,0x43}},
    };
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (auto& g : font) if (g.c == c) {
        for (int i = 0; i < 5; i++) out[i] = g.col[i];
        return;
    }
    for (int i = 0; i < 5; i++) out[i] = 0;     // blank
}

// Plot a NUL-terminated string into a 32-bit ARGB buffer at (x,y) (top-left of
// the first glyph), `color`, integer `scale`. Multi-byte UTF-8 collapses to '.'.
inline void drawText(uint32_t* fb, int W, int H, int x, int y, const char* s,
                     uint32_t color, int scale) {
    for (; *s; s++) {
        char c = *s;
        if ((uint8_t)c >= 0x80) { while (((uint8_t)s[1] & 0xc0) == 0x80) s++; c = '.'; }
        uint8_t col[5];
        glyphInto(c, col);
        for (int cx = 0; cx < 5; cx++)
            for (int row = 0; row < 7; row++)
                if (col[cx] & (1 << row))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int px = x + cx*scale + sx, py = y + row*scale + sy;
                            if (px >= 0 && px < W && py >= 0 && py < H)
                                fb[py*W + px] = color;
                        }
        x += CELL_W * scale;
    }
}

} // namespace xwirefont

#endif
