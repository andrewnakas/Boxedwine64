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
#include "xwireserver.h"
#include "xwireconnection.h"
#include "ksystem.h"
#include "knativesystem.h"
#include "knativescreen.h"
#include "xwirefont.h"

#include <SDL.h>
#include <mutex>
#include <deque>
#include <atomic>
#include <vector>
#include <functional>
#include <condition_variable>

// Defined in the multi-threaded main loop; true only on the platform main thread.
bool isMainthread();

namespace {

// ---- main-thread work queue ------------------------------------------------
// macOS forbids NSWindow/SDL_CreateWindow off the main thread, but the gl64
// bridge runs on a guest thread. xwireRunOnMainThread() enqueues a closure that
// the main loop drains (drainMainThreadWork below, from tickXWirePresent) and
// blocks the caller until it has run.
struct MainThreadJob {
    const std::function<void()>* fn;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
};
std::mutex g_mtwMutex;
std::deque<MainThreadJob*> g_mtwQueue;

void drainMainThreadWork() {
    for (;;) {
        MainThreadJob* job = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mtwMutex);
            if (g_mtwQueue.empty()) break;
            job = g_mtwQueue.front();
            g_mtwQueue.pop_front();
        }
        (*job->fn)();
        {
            std::lock_guard<std::mutex> lk(job->m);
            job->done = true;
        }
        job->cv.notify_one();
    }
}

// ---------------------------------------------------------------------------
// Loading screen: a self-contained progress bar + status text rendered to the
// host window during the wine boot storm (which takes tens of seconds under the
// interpreter), shown until the guest paints its first real frame. No font
// dependency — a tiny built-in 5x7 bitmap covers the ASCII we emit. KSystem
// publishes the stage label + percent (KSystem::noteBootStage from execve).
// ---------------------------------------------------------------------------

// The 5x7 bitmap font now lives in the shared header xwirefont.h (used here for
// the loading screen and by the X core-text path). drawText is brought into this
// anonymous namespace so the existing call sites below stay unchanged.
using xwirefont::drawText;

void fillRect(uint32_t* fb, int W, int H, int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++)
        if (i >= 0 && i < W && j >= 0 && j < H) fb[j*W + i] = color;
}

// Render a loading frame to a freshly-allocated ARGB buffer (caller presents it).
// Shows the title, the current stage, a progress bar, and a live scrolling log
// of recent boot activity (so the user sees what's actually loading).
void renderLoadingFrame(std::vector<uint8_t>& out, int W, int H,
                        const char* label, int pct,
                        const std::vector<BString>& logLines) {
    out.assign((size_t)W * H * 4, 0);
    uint32_t* fb = reinterpret_cast<uint32_t*>(out.data());
    const uint32_t BG = 0xff1e1e28, FG = 0xffe0e0ec, DIM = 0xff8a8aa0;
    const uint32_t LOGC = 0xff6f6f88;
    const uint32_t BAR_BG = 0xff343448, BAR_FG = 0xff5a9bf0;
    for (int i = 0; i < W*H; i++) fb[i] = BG;

    drawText(fb, W, H, 60, 40, "BOXEDWINE64", FG, 3);
    drawText(fb, W, H, 60, 72, "Starting Wine, please wait", DIM, 1);

    // progress bar
    int bw = W - 120, bh = 22, bx = 60, by = 110;
    fillRect(fb, W, H, bx-2, by-2, bw+4, bh+4, DIM);
    fillRect(fb, W, H, bx, by, bw, bh, BAR_BG);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    fillRect(fb, W, H, bx, by, bw * pct / 100, bh, BAR_FG);

    char line[160];
    snprintf(line, sizeof(line), "%d%%  %s", pct, label ? label : "");
    drawText(fb, W, H, bx, by + bh + 12, line, FG, 2);

    // live activity log
    int ly = by + bh + 48;
    drawText(fb, W, H, bx, ly, "ACTIVITY", DIM, 1);
    ly += 14;
    for (size_t i = 0; i < logLines.size(); i++) {
        BString s = logLines[i];
        if (s.length() > 64) s = s.substr(0, 64);
        drawText(fb, W, H, bx, ly + (int)i * 12, s.c_str(), LOGC, 1);
    }
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
        if (inputQ.empty()) inputPending.store(false, std::memory_order_relaxed);
        return true;
    }

    bool hasInput() const override {
        return inputPending.load(std::memory_order_relaxed);
    }

    void lastPointer(int& x, int& y, uint32_t& state) const override {
        std::lock_guard<std::mutex> lk(mtx);
        x = lastX; y = lastY;
        state = sdlModState();
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
            const int LW = 720, LH = 520;
            if (!loadingShown) {
                screen->setScreenSize(LW, LH);
                screen->showWindow(true);
                loadingShown = true;
            }
            BString label = KSystem::bootProgressLabel;
            int pct = KSystem::bootProgressPercent;
            std::vector<BString> log = KSystem::getBootLogTail(26);
            std::vector<uint8_t> lf;
            renderLoadingFrame(lf, LW, LH, label.isEmpty() ? "Booting…" : label.c_str(),
                               pct, log);
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
        if (inputQ.size() < 1024) {
            inputQ.push_back(ie);
            inputPending.store(true, std::memory_order_relaxed);
        }
    }

    mutable std::mutex mtx;
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
    std::atomic<bool> inputPending{false}; // lock-free "queue non-empty" flag
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

// Run `fn` on the platform main thread, blocking until it completes. Inline if
// already on the main thread; otherwise enqueue and wait for drainMainThreadWork
// (called from tickXWirePresent on the main thread). See xwirepresent.h.
void xwireRunOnMainThread(const std::function<void()>& fn) {
    if (isMainthread()) { fn(); return; }
    MainThreadJob job;
    job.fn = &fn;
    {
        std::lock_guard<std::mutex> lk(g_mtwMutex);
        g_mtwQueue.push_back(&job);
    }
    std::unique_lock<std::mutex> lk(job.m);
    job.cv.wait(lk, [&]{ return job.done; });
}

// Drive present + input from the main loop (main thread). Safe to call when no
// sink is installed.
void tickXWirePresent() {
    // Run any work the gl64 bridge (or others) deferred to the main thread —
    // e.g. SDL_CreateWindow for the hidden GL target. Done unconditionally (even
    // with no sink) so a headless-but-GL path still drains.
    drainMainThreadWork();
    if (g_xwirePresentSink) {
        g_xwirePresentSink->tickMainThread();
        // Flush queued host input to the guest only when there IS input — a
        // lock-free check, so during the boot storm (no input) we take zero
        // mutexes and never contend with the guest threads holding the conn/reg
        // locks (that contention livelocked the boot). When input is queued the
        // flush lands bytes in the client recvBuffer + signals its condition.
        if (g_xwirePresentSink->hasInput()) {
            XWireServer::instance().pumpInput();
        }
    }
}

#endif // BOXEDWINE_GUEST_X64
