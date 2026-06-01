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

// SDL implementation of the X11 wire server's presentation sink (Phase 2c).
//
// The wire server (on guest threads) calls submitFrame()/onWindowMapped() to
// hand off the latest window pixels; this class double-buffers them under a
// mutex. The platform main thread calls tickMainThread() from doMainLoop():
// it (lazily) creates the host SDL window, uploads the latest frame to a
// streaming texture, presents it, and pumps SDL key/mouse events into an input
// queue the wire server drains via nextInputEvent().
//
// Only built for the 64-bit guest GUI path. Activated by installXWireSink()
// when video is enabled (not -novideo).

#include "boxedwine.h"

#ifdef BOXEDWINE_GUEST_X64

#include "xwirepresent.h"
#include "ksystem.h"
#include "knativesystem.h"
#include "knativescreen.h"

#include <SDL.h>
#include <mutex>
#include <deque>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Loading screen: a self-contained progress bar + status text rendered to the
// host window during the wine boot storm (which takes tens of seconds under the
// interpreter), shown until the guest paints its first real frame. No font
// dependency — a tiny built-in 5x7 bitmap covers the ASCII we emit. KSystem
// publishes the stage label + percent (KSystem::noteBootStage from execve).
// ---------------------------------------------------------------------------

// 5x7 bitmap font, columns LSB=top. Index by ASCII-0x20. Only glyphs we use are
// filled; the rest render blank (a space), which is fine for status text.
struct Glyph { uint8_t col[5]; };
const Glyph& glyph(char c) {
    static Glyph blank = {{0,0,0,0,0}};
    // Compact set: digits, upper, lower (mapped to upper), and a few symbols.
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
    static Glyph found;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (auto& g : font) if (g.c == c) {
        for (int i = 0; i < 5; i++) found.col[i] = g.col[i];
        return found;
    }
    return blank;
}

void drawText(uint32_t* fb, int W, int H, int x, int y, const char* s,
              uint32_t color, int scale) {
    for (; *s; s++) {
        // crude UTF-8 skip: treat the ellipsis '…' (3 bytes) as one glyph slot
        char c = *s;
        if ((uint8_t)c >= 0x80) { while (((uint8_t)s[1] & 0xc0) == 0x80) s++; c = '.'; }
        const Glyph& g = glyph(c);
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (g.col[col] & (1 << row))
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int px = x + col*scale + sx, py = y + row*scale + sy;
                            if (px >= 0 && px < W && py >= 0 && py < H)
                                fb[py*W + px] = color;
                        }
        x += 6 * scale;
    }
}

void fillRect(uint32_t* fb, int W, int H, int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++)
        if (i >= 0 && i < W && j >= 0 && j < H) fb[j*W + i] = color;
}

// Render a loading frame to a freshly-allocated ARGB buffer (caller presents it).
void renderLoadingFrame(std::vector<uint8_t>& out, int W, int H,
                        const char* label, int pct) {
    out.assign((size_t)W * H * 4, 0);
    uint32_t* fb = reinterpret_cast<uint32_t*>(out.data());
    const uint32_t BG = 0xff1e1e28, FG = 0xffe0e0ec, DIM = 0xff8a8aa0;
    const uint32_t BAR_BG = 0xff343448, BAR_FG = 0xff5a9bf0;
    for (int i = 0; i < W*H; i++) fb[i] = BG;

    drawText(fb, W, H, W/2 - 13*9, H/2 - 70, "BOXEDWINE64", FG, 3);
    drawText(fb, W, H, W/2 - 15*6, H/2 - 40, "Starting Wine, please wait", DIM, 1);

    // progress bar
    int bw = W - 120, bh = 22, bx = 60, by = H/2;
    fillRect(fb, W, H, bx-2, by-2, bw+4, bh+4, DIM);
    fillRect(fb, W, H, bx, by, bw, bh, BAR_BG);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    fillRect(fb, W, H, bx, by, bw * pct / 100, bh, BAR_FG);

    char line[160];
    snprintf(line, sizeof(line), "%d%%  %s", pct, label ? label : "");
    drawText(fb, W, H, bx, by + bh + 14, line, FG, 2);
}

// SDL scancode -> X11 keycode (evdev: X keycode == Linux evdev code + 8). Wine's
// winex11 builds its keymap from GetKeyboardMapping, which we answer minimally,
// so it maps keycodes through the standard evdev layout. This covers the common
// typing + editing set; unmapped keys fall through as 0 (ignored).
uint32_t sdlScancodeToX11(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_ESCAPE: return 9;
        case SDL_SCANCODE_1: return 10;  case SDL_SCANCODE_2: return 11;
        case SDL_SCANCODE_3: return 12;  case SDL_SCANCODE_4: return 13;
        case SDL_SCANCODE_5: return 14;  case SDL_SCANCODE_6: return 15;
        case SDL_SCANCODE_7: return 16;  case SDL_SCANCODE_8: return 17;
        case SDL_SCANCODE_9: return 18;  case SDL_SCANCODE_0: return 19;
        case SDL_SCANCODE_MINUS: return 20; case SDL_SCANCODE_EQUALS: return 21;
        case SDL_SCANCODE_BACKSPACE: return 22; case SDL_SCANCODE_TAB: return 23;
        case SDL_SCANCODE_Q: return 24; case SDL_SCANCODE_W: return 25;
        case SDL_SCANCODE_E: return 26; case SDL_SCANCODE_R: return 27;
        case SDL_SCANCODE_T: return 28; case SDL_SCANCODE_Y: return 29;
        case SDL_SCANCODE_U: return 30; case SDL_SCANCODE_I: return 31;
        case SDL_SCANCODE_O: return 32; case SDL_SCANCODE_P: return 33;
        case SDL_SCANCODE_LEFTBRACKET: return 34; case SDL_SCANCODE_RIGHTBRACKET: return 35;
        case SDL_SCANCODE_RETURN: return 36; case SDL_SCANCODE_LCTRL: return 37;
        case SDL_SCANCODE_A: return 38; case SDL_SCANCODE_S: return 39;
        case SDL_SCANCODE_D: return 40; case SDL_SCANCODE_F: return 41;
        case SDL_SCANCODE_G: return 42; case SDL_SCANCODE_H: return 43;
        case SDL_SCANCODE_J: return 44; case SDL_SCANCODE_K: return 45;
        case SDL_SCANCODE_L: return 46; case SDL_SCANCODE_SEMICOLON: return 47;
        case SDL_SCANCODE_APOSTROPHE: return 48; case SDL_SCANCODE_GRAVE: return 49;
        case SDL_SCANCODE_LSHIFT: return 50; case SDL_SCANCODE_BACKSLASH: return 51;
        case SDL_SCANCODE_Z: return 52; case SDL_SCANCODE_X: return 53;
        case SDL_SCANCODE_C: return 54; case SDL_SCANCODE_V: return 55;
        case SDL_SCANCODE_B: return 56; case SDL_SCANCODE_N: return 57;
        case SDL_SCANCODE_M: return 58; case SDL_SCANCODE_COMMA: return 59;
        case SDL_SCANCODE_PERIOD: return 60; case SDL_SCANCODE_SLASH: return 61;
        case SDL_SCANCODE_RSHIFT: return 62; case SDL_SCANCODE_LALT: return 64;
        case SDL_SCANCODE_SPACE: return 65; case SDL_SCANCODE_CAPSLOCK: return 66;
        case SDL_SCANCODE_F1: return 67;  case SDL_SCANCODE_F2: return 68;
        case SDL_SCANCODE_F3: return 69;  case SDL_SCANCODE_F4: return 70;
        case SDL_SCANCODE_F5: return 71;  case SDL_SCANCODE_F6: return 72;
        case SDL_SCANCODE_F7: return 73;  case SDL_SCANCODE_F8: return 74;
        case SDL_SCANCODE_F9: return 75;  case SDL_SCANCODE_F10: return 76;
        case SDL_SCANCODE_HOME: return 110; case SDL_SCANCODE_UP: return 111;
        case SDL_SCANCODE_LEFT: return 113; case SDL_SCANCODE_RIGHT: return 114;
        case SDL_SCANCODE_END: return 115;  case SDL_SCANCODE_DOWN: return 116;
        case SDL_SCANCODE_DELETE: return 119;
        default: return 0;
    }
}

// X11 modifier mask bits, accumulated from SDL key state.
uint32_t sdlModState() {
    SDL_Keymod m = SDL_GetModState();
    uint32_t s = 0;
    if (m & KMOD_SHIFT) s |= 0x01;   // ShiftMask
    if (m & KMOD_CAPS)  s |= 0x02;   // LockMask
    if (m & KMOD_CTRL)  s |= 0x04;   // ControlMask
    if (m & KMOD_ALT)   s |= 0x08;   // Mod1Mask
    return s;
}

class XWirePresentSinkSDL : public XWirePresentSink {
public:
    void submitFrame(uint32_t window, uint16_t width, uint16_t height,
                     const uint8_t* pixels, uint32_t pitch) override {
        std::lock_guard<std::mutex> lk(mtx);
        if (pendingW != width || pendingH != height) {
            pending.assign((size_t)width * height * 4, 0);
            pendingW = width;
            pendingH = height;
        }
        // Compact to a tight width*4 pitch for SDL_UpdateTexture.
        uint32_t tight = (uint32_t)width * 4;
        for (uint16_t y = 0; y < height; y++) {
            memcpy(pending.data() + (size_t)y * tight, pixels + (size_t)y * pitch, tight);
        }
        haveFrame = true;
        frameDirty = true;
    }

    void onWindowMapped(uint32_t window, uint16_t width, uint16_t height) override {
        std::lock_guard<std::mutex> lk(mtx);
        wantW = width;
        wantH = height;
        wantWindow = true;
    }

    bool nextInputEvent(XWireInputEvent& out) override {
        std::lock_guard<std::mutex> lk(mtx);
        if (inputQ.empty()) return false;
        out = inputQ.front();
        inputQ.pop_front();
        return true;
    }

    void tickMainThread() override {
        // Present + input pump (main thread only). We render through BoxedWine's
        // OWN screen (KNativeSystem::getScreen()), not a private SDL window: the
        // existing screen already owns the composited Metal renderer + window and
        // is driven by doMainLoop. A second independent SDL window/renderer on
        // macOS never gets a composited drawable (stays invisible), so we reuse
        // the working one and just feed it our X11 frames via putBitsOnWnd.
        KNativeScreenPtr screen = KNativeSystem::getScreen();
        if (!screen) return;

        // (Re)size + show the host window when the guest maps its window.
        bool doResize = false; uint16_t rw = 0, rh = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (wantWindow && (wantW != shownW || wantH != shownH)) {
                doResize = true; rw = wantW; rh = wantH;
                shownW = wantW; shownH = wantH;
            }
        }
        if (doResize && rw && rh) {
            screen->setScreenSize(rw, rh);
            screen->showWindow(true);
            klog_fmt("XWire SDL: host screen sized to %dx%d (BoxedWine screen)", (int)rw, (int)rh);
        }

        // Loading screen: until the guest paints its first frame, show a labeled
        // progress bar driven by KSystem::bootProgress* (set as the wine boot
        // chain execve()s each PE stage). The boot storm is tens of seconds under
        // the interpreter, so without this the host window is a blank gray void.
        if (!haveFrame && KSystem::bootProgressPercent >= 0) {
            const int LW = 640, LH = 360;
            if (!loadingShown) {
                screen->setScreenSize(LW, LH);
                screen->showWindow(true);
                loadingShown = true;
            }
            BString label = KSystem::bootProgressLabel;
            int pct = KSystem::bootProgressPercent;
            // Creep the bar within a stage so it never looks frozen during the
            // long mid-boot stretches (capped just under the next stage's start).
            if (pct == lastShownPct && pct < 99) creep = (creep + 1) % 30;
            else { creep = 0; lastShownPct = pct; }
            int shownPct = pct + creep / 10; // +0..2 jitter
            std::vector<uint8_t> lf;
            renderLoadingFrame(lf, LW, LH, label.isEmpty() ? "Booting…" : label.c_str(), shownPct);
            screen->putBitsOnWnd(0, lf.data(), 32, (U32)LW * 4, 0, 0, LW, LH, nullptr, true);
            screen->present();
        }

        // Blit the latest frame and present.
        std::vector<uint8_t> frame;
        uint16_t fw = 0, fh = 0;
        {
            std::lock_guard<std::mutex> lk(mtx);
            if (haveFrame && frameDirty) {
                frame = pending; fw = pendingW; fh = pendingH;
                frameDirty = false;
            }
        }
        if (!frame.empty() && fw && fh) {
            // First real guest frame — retire the loading screen.
            KSystem::bootProgressPercent = -1;
            screen->putBitsOnWnd(0, frame.data(), 32, (U32)fw * 4, 0, 0, fw, fh, nullptr, true);
            screen->present();
            if (getenv("BW64_XWIRE")) {
                uint32_t px = *(const uint32_t*)frame.data();
                klog_fmt("XWire SDL: presented %dx%d topleft=0x%08x", (int)fw, (int)fh, px);
            }
        }

        // NOTE: we do NOT SDL_PollEvent here. BoxedWine's own input layer
        // (platform/sdl/knativeinputSDL.cpp) owns the single SDL event pump on
        // this same main thread; a second poll here would race it and steal
        // events at random. Instead knativeinputSDL forwards each key/mouse
        // event to us via xwireForwardSdlEvent() -> forwardSdlEvent() below.
    }

    // Translate one SDL key/mouse event to an XWireInputEvent and queue it.
    // Called from the platform input pump (knativeinputSDL::handlSdlEvent) so
    // there is exactly ONE SDL_PollEvent in the process. Safe from the main
    // thread; push() is mutex-guarded.
    void forwardSdlEvent(const SDL_Event& e) {
        XWireInputEvent ie;
        switch (e.type) {
            case SDL_KEYDOWN:
            case SDL_KEYUP: {
                uint32_t kc = sdlScancodeToX11(e.key.keysym.scancode);
                if (!kc) break;
                ie.type = (e.type == SDL_KEYDOWN) ? XWireInputEvent::EvKeyDown
                                                  : XWireInputEvent::EvKeyUp;
                ie.detail = kc;
                ie.state = sdlModState();
                ie.x = lastX; ie.y = lastY;
                push(ie);
                break;
            }
            case SDL_MOUSEMOTION:
                lastX = e.motion.x; lastY = e.motion.y;
                ie.type = XWireInputEvent::EvMotion;
                ie.x = lastX; ie.y = lastY;
                ie.state = sdlModState();
                push(ie);
                break;
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                lastX = e.button.x; lastY = e.button.y;
                ie.type = (e.type == SDL_MOUSEBUTTONDOWN) ? XWireInputEvent::EvButtonDown
                                                          : XWireInputEvent::EvButtonUp;
                // X buttons: 1=left,2=middle,3=right
                ie.detail = (e.button.button == SDL_BUTTON_LEFT) ? 1 :
                            (e.button.button == SDL_BUTTON_MIDDLE) ? 2 :
                            (e.button.button == SDL_BUTTON_RIGHT) ? 3 : e.button.button;
                ie.x = lastX; ie.y = lastY;
                ie.state = sdlModState();
                push(ie);
                break;
            }
            default: break;
        }
    }

private:

    void push(const XWireInputEvent& ie) {
        std::lock_guard<std::mutex> lk(mtx);
        // Cap the queue so a stalled guest can't grow it unbounded.
        if (inputQ.size() < 1024) inputQ.push_back(ie);
    }

    std::mutex mtx;
    std::vector<uint8_t> pending;
    uint16_t pendingW = 0, pendingH = 0;
    bool haveFrame = false, frameDirty = false;
    bool wantWindow = false;
    uint16_t wantW = 0, wantH = 0;
    std::deque<XWireInputEvent> inputQ;
    int lastX = 0, lastY = 0;
    uint16_t shownW = 0, shownH = 0;     // last size pushed to the host screen
    bool loadingShown = false;           // loading-screen window sized/shown once
    int lastShownPct = -1, creep = 0;    // loading-bar anti-freeze jitter
};

XWirePresentSinkSDL g_sink;

} // namespace

// Install the SDL sink so the X11 wire server presents to a host window. Called
// from the 64-bit GUI launch path when video is enabled. Idempotent.
void installXWireSink() {
    if (KSystem::videoOption == VIDEO_NO_WINDOW) return;
    g_xwirePresentSink = &g_sink;
}

// Forward one SDL input event from BoxedWine's single platform event pump
// (knativeinputSDL) into the XWire input queue. No-op when no sink is active
// (headless / -novideo / 32-bit). Keeps a single SDL_PollEvent in the process.
void xwireForwardSdlEvent(const SDL_Event& e) {
    if (g_xwirePresentSink) g_sink.forwardSdlEvent(e);
}

// Drive present + input from the main loop (main thread). Safe to call when no
// sink is installed.
void tickXWirePresent() {
    if (g_xwirePresentSink) {
        g_xwirePresentSink->tickMainThread();
    }
}

#endif // BOXEDWINE_GUEST_X64
