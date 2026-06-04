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

#ifndef __XWIRECONNECTION_H__
#define __XWIRECONNECTION_H__

#include "boxedwine.h"
#include "kunixsocket.h"
#include "xwirepresent.h"

#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <utility>

class XWireConnection;

// The server-side peer of wine's X11 client socket. Its only job is to forward
// the "peer wrote" edge into the owning XWireConnection so the request stream is
// parsed synchronously on the writing (guest) thread.
class XWireServerSocket : public KUnixSocketObject {
public:
    XWireServerSocket(U32 domain, U32 type, U32 protocol)
        : KUnixSocketObject(domain, type, protocol) {}
    void onPeerWrote() override;
    bool isXWire() override { return true; }

    // Non-blocking drain of this server peer's own recvBuffer (the bytes wine
    // wrote). Returns 0 when empty. Unlike readNative it never blocks — onData()
    // runs on the guest's writing thread and must not park.
    U32 readNativeNonBlocking(U8* buffer, U32 len);

    std::weak_ptr<XWireConnection> owner;
};

// One connected X11 client (e.g. notepad's libX11). Owns the wire-protocol state
// machine: connection setup, resource tracking, request dispatch, and event
// generation. Replies and events are written back to the client socket via the
// server peer's writeNative (which lands in the client's recvBuffer + wakes any
// poll/epoll/read blocked on it).
class XWireConnection : public std::enable_shared_from_this<XWireConnection> {
public:
    XWireConnection(const std::shared_ptr<KUnixSocketObject>& client,
                    const std::shared_ptr<XWireServerSocket>& serverPeer);

    // Drain the server peer's recvBuffer and process as many complete requests
    // as are available. Called from XWireServerSocket::onPeerWrote.
    void onData();

    // Push an input/structure event onto the client (called from the SDL input
    // pump). Honors the per-window event mask.
    void sendExpose(uint32_t window, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

    // Tell the client its window holds the keyboard focus (FocusIn, code 9).
    // Without it wine's window never activates its input queue, so KeyPress
    // events we deliver are ignored even though they arrive. Sent when a window
    // maps; GetInputFocus is also answered with the present window (not root).
    void sendFocusIn(uint32_t window);

    // MapNotify (code 19) — winex11 selects StructureNotifyMask and blocks its
    // message pump until it sees the window become viewable. Sent on X_MapWindow.
    void sendMapNotify(uint32_t window);

    // Emit an EnterNotify/LeaveNotify crossing event (code 7/8). winex11 selects
    // for EnterWindowMask and uses the crossing to associate the pointer with
    // the window; without it clicks are mishandled (they defocus the edit and
    // menus never track). Sent the first time the pointer is over the presented
    // window. Mirrors the 32-bit XWindow::crossingNotify.
    void sendCrossing(uint32_t window, bool enter, int16_t x, int16_t y);

    // Drain any queued host input to the client and flush, even with no incoming
    // request. Called by XWireServer::pumpInput from the main-thread present tick
    // so an idle app still receives keystrokes/clicks. flushReplies' writeNative
    // path is the thread-safe client-wakeup (same as onPeerWrote).
    void pumpInputAndFlush() { deliverInputEvents(); flushReplies(); }

private:
    // ---- wire helpers ----
    void writeToClient(const void* data, uint32_t len);
    void flushReplies();
    void sendError(uint8_t code, uint16_t seq, uint32_t badValue, uint8_t majorOp, uint16_t minorOp);

    bool handshakeDone = false;
    bool bigEndian = false;             // client byte order
    uint16_t sequence = 0;              // last-processed request sequence

    // Pending outbound bytes, flushed to the socket in one shot per onData().
    std::vector<uint8_t> out;

    // Inbound assembly buffer (a request may straddle recvBuffer chunks).
    std::vector<uint8_t> in;

    // Per-request zero-padded dispatch scratch (see onData). Reused across
    // requests to avoid per-request allocation. REQ_SCRATCH_FLOOR is >= the
    // largest fixed request-header offset any handler reads (X11 core requests
    // are at most 32 header bytes; 64 leaves margin), so an undersized request
    // never reads past the assembled bytes into uninitialized memory.
    static constexpr uint32_t REQ_SCRATCH_FLOOR = 64;
    // Extra zero bytes appended after the request so a handler whose over-read is
    // driven by an in-request sub-length byte (max 255 entries * 2 bytes wide)
    // cannot run past the allocation even on a malformed request.
    static constexpr uint32_t REQ_SCRATCH_GUARD = 1024;
    std::vector<uint8_t> reqScratch;

    // ---- resource model ----
    // The window/drawable registry is SERVER-GLOBAL (XWireServer::windows),
    // because X resource ids are shared across all client connections and
    // winex11 creates vs. draws a window on different connections. Atoms stay
    // local — they're only round-tripped within a connection here.

    // Decode an X PutImage payload into the target window's backing framebuffer
    // (in the server registry) and, if it's the presented window, push the full
    // image to the host sink.
    void blitPutImage(uint32_t drawable, uint8_t format, uint8_t depth,
                      int16_t dstX, int16_t dstY, uint16_t w, uint16_t h,
                      const uint8_t* data, uint32_t dataBytes);

    // Render X core text (PolyText/ImageText) into a window's text-overlay buffer
    // using the GC's foreground (and background, for ImageText). y is the X text
    // BASELINE. blitTextItems draws a whole PolyText item list with one lock +
    // one present. Both no-op for an unknown/pixmap drawable.
    void blitText(uint32_t drawable, uint32_t gc, int16_t x, int16_t y,
                  const std::string& chars, bool imageText);
    void blitTextItems(uint32_t drawable, uint32_t gc, int16_t y,
                       const std::vector<std::pair<int16_t, std::string>>& items);

    // Host input -> X11 wire events (Phase 2c input path).
    void deliverInputEvents();
    void sendInputEvent(uint8_t code, uint32_t window, const XWireInputEvent& ev,
                        int16_t winX, int16_t winY);
    std::unordered_map<std::string, uint32_t> atoms;   // name -> atom id
    std::unordered_map<uint32_t, std::string> atomNames;
    uint32_t nextAtom = 1;

    uint32_t rootWindow = 0;
    uint32_t rootVisual = 0;
    uint32_t rootColormap = 0;
    uint32_t clientIdBase = 0;
    uint32_t clientIdMask = 0;

    // Pointer-button mask (Button1Mask=1<<8 ...) tracked across input events so
    // motion during a drag and the press/release sequence report the correct
    // logical button state, like the 32-bit XServer path.
    uint32_t buttonState = 0;

    // The window the pointer has "entered" (we sent EnterNotify for). 0 = none.
    // We send a crossing the first time input lands so winex11 associates the
    // pointer with the window before any click.
    uint32_t enteredWindow = 0;

    // The window we last sent FocusIn for (via X_SetInputFocus). Tracked so a
    // click that re-focuses the same window doesn't thrash FocusIn every time.
    uint32_t lastFocus = 0;

    // The macOS window has been created/shown for this connection.
    bool windowShown = false;
    uint16_t screenWidth = 1024;
    uint16_t screenHeight = 768;

    std::weak_ptr<KUnixSocketObject> client;
    // STRONG ref: nothing else keeps the server-side peer alive. The guest's
    // client socket only holds it through a weak connection ptr, and
    // serverPeer->owner is weak. If this were weak too, the XWireServerSocket
    // would be destroyed the instant acceptConnection() returned, leaving the
    // guest's X socket with an expired peer (not writable, no parser) — winex11
    // then polls, sees no peer, and shutdown()+close()s the display. The
    // XWireServer::connections vector keeps the XWireConnection (and thus this
    // peer) alive for the lifetime of the connection.
    std::shared_ptr<XWireServerSocket> serverPeer;

    uint32_t internAtom(const std::string& name, bool onlyIfExists);
    void processOneRequest(const uint8_t* req, uint32_t len);
    void doHandshake();
    void ensureWindow();
};

#endif
