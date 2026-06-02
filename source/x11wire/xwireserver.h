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

// In-process X11 wire-protocol server for 64-bit wine (Milestone F, Phase 2).
//
// 64-bit wine links real libX11, which speaks the X11 wire protocol over an
// AF_UNIX socket (/tmp/.X11-unix/X<display>). There is no real X server in the
// guest rootfs, so when wine connects to that path, KUnixSocketObject::connect
// hands the client socket here instead of returning ECONNREFUSED. We pair the
// client with a server-side peer socket whose onPeerWrote() override parses the
// request stream synchronously (on wine's own thread) and writes replies/events
// back through the normal socket buffer — so all the existing epoll/poll/read
// plumbing wakes wine exactly as a real X server's socket would.
//
// Presentation is via the existing SDL/Metal KNativeScreen backend (PutImage ->
// putBitsOnWnd -> present). Software rendering only; no MIT-SHM, no GLX.

#ifndef __XWIRESERVER_H__
#define __XWIRESERVER_H__

#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdint>
#include <mutex>

class KUnixSocketObject;
class XWireConnection;

// A server-global X drawable (window or window-backed pixmap). X resource ids
// are global across all client connections — winex11 opens a separate X
// connection per thread, so the connection that CREATES a window is often not
// the one that PUTIMAGES into it. The window model + backing framebuffer
// therefore live here, in the server, not in any single XWireConnection.
//
// NOTE: named XWireWindow (not XWindow) deliberately — the 32-bit X server has a
// global `class XWindow` in source/x11/xwindow.h, and a second global type of the
// same name is an ODR violation: the linker merges their destructor symbols, so
// destroying one can run the other's destructor over the wrong memory (a real
// stack/heap-corruption crash, caught by ASan). Likewise XWireGC below avoids the
// global `typedef XID GContext` in source/x11/x11.h.
struct XWireWindow {
    uint32_t parent = 0;
    int16_t x = 0, y = 0;
    uint16_t width = 0, height = 0;
    uint32_t eventMask = 0;
    bool mapped = false;
    bool isRoot = false;
    // Override-redirect (menus/tooltips/combo dropdowns set this so the WM leaves
    // them alone). Used by the compositor to recognize overlay popups.
    bool overrideRedirect = false;
    // Monotonic map order so the compositor stacks later-mapped windows on top
    // (a freshly-opened menu draws over everything below it).
    uint64_t mapSerial = 0;
    // X cursor associated with this window via CWCursor (0 = none/inherit). The
    // host shows this shape while the pointer is over the window.
    uint32_t cursor = 0;
    // ARGB8888 backing store for software-blitted (PutImage) content. Lazily
    // sized; persists between partial PutImages so a complete client area is
    // always available to present.
    std::vector<uint8_t> fb;
    uint16_t fbW = 0, fbH = 0;
    // ARGB8888 text overlay, same dimensions as fb. X core-text (PolyText/
    // ImageText) is drawn here, NOT into fb, so a later PutImage band that
    // rewrites fb can't erase the glyphs; composeAndPresent blits it on top of
    // fb (pixel != 0 = opaque). Cleared per-region by a PutImage that repaints
    // that region. Empty until the window first draws text.
    std::vector<uint8_t> textFb;
};

// An X graphics context. We model only the bits the core-text path needs:
// foreground/background pixel colors and the (informational) font id. Stored as
// opaque ARGB (0xff000000 | pixel) so blits don't have to special-case alpha.
struct XWireGC {
    uint32_t foreground = 0xff000000;   // opaque black
    uint32_t background = 0xffffffff;   // opaque white
    uint32_t font = 0;
};

class XWireServer {
public:
    static XWireServer& instance();

    // True if the AF_UNIX connect target is an X server display socket, i.e.
    // "/tmp/.X11-unix/X<n>" (libX11's standard unix transport path).
    static bool isXDisplayPath(const char* path);

    // Pair the connecting client socket with a freshly created server-side peer
    // and start a wire connection. Returns false only on hard failure.
    bool acceptConnection(const std::shared_ptr<KUnixSocketObject>& client);

    // ---- server-global resource registry (thread-safe) ----
    // The registry mutex guards windows + presentWindow. Connections run on
    // different guest threads, so all access goes through these helpers (or
    // holds regMutex directly for compound ops).
    std::mutex regMutex;
    std::unordered_map<uint32_t, XWireWindow> windows;
    uint32_t presentWindow = 0;     // base window composited under any overlays

    // GC id -> graphics context (foreground/background/font for core-text). Font
    // id -> name (all map to the single builtin 5x7 font for now). Both guarded
    // by regMutex like the window registry.
    std::unordered_map<uint32_t, XWireGC> gcs;
    std::unordered_map<uint32_t, std::string> fonts;

    // Monotonic counter handed to XWireWindow.mapSerial on each MapWindow, so the
    // compositor can stack overlays in map order (newest on top).
    uint64_t mapSerialCounter = 0;

    // cursorId -> X core cursor shape (the XC_* glyph number, e.g. 152=xterm).
    // Populated from X_CreateGlyphCursor; read when a window's CWCursor points at
    // the id so the host shows the matching shape. Guarded by regMutex.
    std::unordered_map<uint32_t, uint32_t> cursorShapes;

    // Composite the base window + any mapped overlay windows (menus/popups) into
    // one host-sized image and hand it to the present sink. Call with regMutex
    // NOT held (it locks internally). Cheap no-op when nothing is dirty.
    void composeAndPresent();

    // Selection (clipboard) ownership: selection-atom -> owner window. wine's
    // clipboard manager does SetSelectionOwner then polls GetSelectionOwner to
    // confirm it owns CLIPBOARD/PRIMARY; if we never record the owner the poll
    // never sees itself win and spins forever (the boot wedge). Guarded by
    // regMutex like the other registries.
    std::unordered_map<uint32_t, uint32_t> selectionOwners;

    // Allocate the next distinct client resource-id base (so ids never collide
    // across connections).
    uint32_t allocClientIdBase();

    // Flush any queued host input (from the SDL pump) out to the guest, even
    // when the app is idle and isn't sending requests. Iterates connections and
    // calls deliverInputEvents()+flushReplies(); the flush lands bytes in the
    // client's recvBuffer and signals its poll/read condition (the same
    // thread-safe wakeup path onPeerWrote uses), so it's safe to call from the
    // main thread's present tick. No-op when nothing is queued.
    void pumpInput();

private:
    XWireServer() {}
    std::mutex connMutex;   // guards `connections` (accept on guest threads)
    std::vector<std::shared_ptr<XWireConnection>> connections;
    uint32_t nextClientBase = 0x00400000;
};

#endif
