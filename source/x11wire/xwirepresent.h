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

// Presentation bridge for the X11 wire server. The wire server runs on the
// guest's own threads and must never touch SDL directly (SDL video + the event
// pump live on the platform main thread). So the wire server talks to an
// abstract sink: it pushes the latest window pixels (from PutImage) and polls
// for translated input events; the platform layer (platform/sdl) implements the
// sink, owns the host window, and drives present()/pumpInput() from doMainLoop()
// on the main thread.

#ifndef __XWIREPRESENT_H__
#define __XWIREPRESENT_H__

#include <cstdint>
#include <functional>

// A single input event the host produced (key/button/motion), already
// translated to X11 semantics. The wire connection turns these into wire
// events for the focused window.
struct XWireInputEvent {
    // Names are prefixed to avoid colliding with X11/X.h macros (KeyPress,
    // ButtonPress, ... are #defined to ints there).
    enum Type { EvKeyDown, EvKeyUp, EvButtonDown, EvButtonUp, EvMotion } type;
    int x = 0, y = 0;          // pointer position in window coords
    uint32_t detail = 0;       // keycode (key events) or button number (button events)
    uint32_t state = 0;        // modifier/button mask
};

// Implemented by the platform layer. All methods other than the explicitly
// main-thread ones (present/pumpInput) may be called from a guest thread, so
// implementations must be internally synchronized.
class XWirePresentSink {
public:
    virtual ~XWirePresentSink() {}

    // Guest-thread: hand off the latest full-window image. Pixels are 32-bit
    // little-endian (B,G,R,X byte order == X11 ZPixmap on a TrueColor
    // 0x00RRGGBB visual). 'pitch' is the source row stride in bytes. The sink
    // copies what it needs; 'pixels' is only valid for the duration of the call.
    virtual void submitFrame(uint32_t window, uint16_t width, uint16_t height,
                             const uint8_t* pixels, uint32_t pitch) = 0;

    // Guest-thread: a top-level window was mapped at this size; the sink should
    // ensure a host window of (at least) these dimensions exists.
    virtual void onWindowMapped(uint32_t window, uint16_t width, uint16_t height) = 0;

    // Guest-thread: drain queued host input events. Returns false when empty.
    // The wire connection calls this from its onData() pump so events ride the
    // socket the next time wine reads it.
    virtual bool nextInputEvent(XWireInputEvent& out) = 0;

    // Cheap lock-free check: is any host input queued? Lets the main-thread
    // present tick skip pumpInput() entirely (no mutex traffic) when idle, so it
    // never contends with the guest threads during the boot storm.
    virtual bool hasInput() const = 0;

    // Guest-thread: the last known pointer position (window coords) and current
    // modifier/button mask, as tracked by the SDL pump. The wire connection uses
    // this to answer X_QueryPointer so wine's click handler doesn't block.
    virtual void lastPointer(int& x, int& y, uint32_t& state) const = 0;

    // Main-thread (doMainLoop): present the most recent submitted frame and pump
    // the host event queue into the input queue. No-op when nothing changed.
    virtual void tickMainThread() = 0;
};

// The single active sink (nullptr when running headless / -novideo). Set by the
// platform layer during window init; read by the wire connection. Plain pointer
// with process lifetime — never freed mid-run.
extern XWirePresentSink* g_xwirePresentSink;

// Install the SDL-backed sink when video is enabled (no-op under -novideo).
// Called once during the 64-bit GUI launch path. Defined in xwirepresentSDL.cpp.
void installXWireSink();

// Drive present + host input from the main loop (main thread). No-op when no
// sink is installed. Defined in xwirepresentSDL.cpp.
void tickXWirePresent();

// Forward one SDL key/mouse event into the XWire input queue. Called from
// BoxedWine's single platform SDL event pump (platform/sdl/knativeinputSDL)
// so input isn't split across two competing SDL_PollEvent loops. No-op when no
// sink is active. Declared with a forward-declared SDL_Event so this header
// stays SDL-agnostic. Defined in xwirepresentSDL.cpp.
union SDL_Event;
void xwireForwardSdlEvent(const SDL_Event& e);

// Run `fn` on the platform MAIN thread and block until it completes. macOS
// requires SDL_CreateWindow / NSWindow on the main thread, but the gl64 bridge
// runs on a guest thread — so it hands window creation here. The work is drained
// by tickXWirePresent() (called from doMainLoop on the main thread). If called
// FROM the main thread it runs inline. Safe before any sink is installed (still
// pumped by the main loop). Defined in xwirepresentSDL.cpp.
void xwireRunOnMainThread(const std::function<void()>& fn);

#endif
