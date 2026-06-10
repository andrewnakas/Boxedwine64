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
#include <atomic>

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
    // Resource-id base of the X connection that CREATED this window. Connection
    // bases are allocated monotonically (allocClientIdBase), so a window's owner
    // base uniquely identifies which app/connection-generation it belongs to.
    // The app-switch adopt logic uses this to adopt ONLY a window owned by a
    // connection opened AFTER the switch was armed (i.e. the newly spawned app) —
    // a mapSerial gate alone is not enough, because the still-running previous app
    // keeps mapping/repainting windows that also get fresh serials and could
    // otherwise re-win the base. 0 = unknown/server-owned (e.g. the root).
    uint32_t ownerClientBase = 0;
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

// An off-screen drawable (X pixmap). winex11 renders some GDI ops (notably
// StretchDIBits, which DOOM's doomgeneric backend uses every frame) by PutImage-
// ing the bits into a pixmap and then CopyArea-ing the pixmap onto the window.
// We model the pixmap as a plain ARGB8888 framebuffer so that PutImage-into-
// pixmap + CopyArea-pixmap->window reproduces the on-window result. Without this
// DOOM's frames land in a pixmap we used to throw away (the canvas stayed on the
// boot splash). Guarded by XWireServer::regMutex like windows.
struct XWirePixmap {
    uint16_t w = 0, h = 0;
    uint8_t depth = 24;
    std::vector<uint8_t> fb;            // w*h*4 ARGB8888 (0x00RRGGBB)
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
    // Persistent-session app switch: when set, the next real (non-override-
    // redirect) window OWNED BY A CONNECTION OPENED AFTER THE ARM (ownerClientBase
    // >= adoptArmClientBase) is forcibly adopted as the base presentWindow,
    // regardless of size. The owner-base gate is what ensures we adopt the NEWLY
    // SPAWNED app's window — not the still-running previous app's window
    // repainting. (A mapSerial-only gate was not enough: the old app keeps
    // mapping/repainting its own windows, which get fresh serials too, and so it
    // could re-win the base — observed as "launching Snake showed Minesweeper".)
    // This switches the canvas to a newly launched app WITHOUT killing the
    // previous one (whose mid-session teardown crashes the runtime). Cleared once
    // a window is adopted. Guarded by regMutex.
    bool adoptNextWindowAsBase = false;
    uint64_t adoptArmSerial = 0;
    // Resource-id base watermark captured when the switch was armed (== the
    // server's nextClientBase at arm time). A window is adopted as the new base
    // ONLY if its ownerClientBase >= this watermark, i.e. it was created by a
    // connection opened AFTER the arm (the freshly spawned app). This is the real
    // discriminator that stops the still-running previous app — whose connections
    // were all allocated BEFORE the arm — from re-winning the base via a late
    // repaint or popup map. adoptArmSerial is kept only as a tiebreaker/log aid.
    uint32_t adoptArmClientBase = 0;

    // GL foreground drawable. A GL app (e.g. glcube) renders via the gl64 bridge
    // and presents readback frames tagged with its GLX *drawable* id, which is a
    // DIFFERENT X resource id than the X11 *window* the same app maps (winex11
    // opens a separate X connection per thread; the drawable resolves on one, the
    // MapWindow arrives on another). So the present-sink's "window == presentWindow"
    // filter would drop every GL frame (the "stale Notepad / no cube" bug). The
    // gl64 bridge publishes its current drawable here on glXMakeCurrent; the sink
    // accepts a frame whose window matches EITHER presentWindow (GDI apps) OR this
    // (the foreground GL app). Cleared when a GDI window is adopted as the base or
    // on app-switch arm, so a still-running background GL app can't paint over the
    // GDI app the user switched to. 0 = no GL app is foreground. Guarded by regMutex
    // for writes from connection threads; the gl64 bridge sets it directly (single
    // writer at MakeCurrent time) and the sink reads it racily (benign: worst case
    // one stale frame).
    uint32_t glPresentDrawable = 0;

    // GC id -> graphics context (foreground/background/font for core-text). Font
    // id -> name (all map to the single builtin 5x7 font for now). Both guarded
    // by regMutex like the window registry.
    std::unordered_map<uint32_t, XWireGC> gcs;
    std::unordered_map<uint32_t, std::string> fonts;
    // Off-screen drawables (pixmap id -> backing fb). See XWirePixmap. Created by
    // X_CreatePixmap, filled by X_PutImage when the target is a pixmap, and read
    // by X_CopyArea pixmap->window. Guarded by regMutex.
    std::unordered_map<uint32_t, XWirePixmap> pixmaps;

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

    // Deferred-present path used by the per-request draw handlers (PutImage /
    // CopyArea / text / map-unmap). composeAndPresent() copies the full
    // host-sized framebuffer, and GDI apps repaint in many small bands — calling
    // it inline per request composited the same frame dozens of times. Instead
    // the handlers mark the scene dirty here, and tickXWirePresent() (every
    // frame, on the present thread) flushes at most ONE composite per tick.
    void schedulePresent() { presentPending.store(true, std::memory_order_release); }
    void flushPendingPresent() {
        if (presentPending.exchange(false, std::memory_order_acq_rel)) {
            composeAndPresent();
        }
    }
    std::atomic<bool> presentPending{false};

    // Persistent-session app switch: mark every window owned by `ownerBase`
    // (a connection's resource-id base) as un-mapped, so the previous app's
    // windows vanish from BOTH the compositor (composeAndPresent skips
    // !mapped windows) and the input hit-test (deliverInputEvents skips
    // !mapped). The previous app stays RUNNING (no kill — that crashes the
    // runtime) but becomes invisible and inert: the canvas shows only the new
    // app and keyboard/mouse go to it. Call with `ownerBase` = the OUTGOING
    // base window's ownerClientBase, captured before presentWindow is moved to
    // the new app. No-op for ownerBase==0. Locks regMutex internally; safe to
    // call from a connection thread holding NO lock. Returns true if it changed
    // anything (caller then arms a present-sink clear to wipe stale pixels).
    bool unmapAppWindows(uint32_t ownerBase);

    // Persistent-session app switch: drop every non-root window and clear the
    // base (presentWindow=0) so the NEXT app's first-mapped window is adopted as
    // the base instead of compositing the new app onto the closed app's larger
    // window. Called from the session bridge after killing the previous app.
    // Locks regMutex internally.
    void resetForAppSwitch();

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
