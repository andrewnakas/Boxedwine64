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
            screen->putBitsOnWnd(0, frame.data(), 32, (U32)fw * 4, 0, 0, fw, fh, nullptr, true);
            screen->present();
            if (getenv("BW64_XWIRE")) {
                uint32_t px = *(const uint32_t*)frame.data();
                klog_fmt("XWire SDL: presented %dx%d topleft=0x%08x", (int)fw, (int)fh, px);
            }
        }

        pumpEvents();
    }

private:

    void pumpEvents() {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
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
    }

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
};

XWirePresentSinkSDL g_sink;

} // namespace

// Install the SDL sink so the X11 wire server presents to a host window. Called
// from the 64-bit GUI launch path when video is enabled. Idempotent.
void installXWireSink() {
    if (KSystem::videoOption == VIDEO_NO_WINDOW) return;
    g_xwirePresentSink = &g_sink;
}

// Drive present + input from the main loop (main thread). Safe to call when no
// sink is installed.
void tickXWirePresent() {
    if (g_xwirePresentSink) {
        g_xwirePresentSink->tickMainThread();
    }
}

#endif // BOXEDWINE_GUEST_X64
