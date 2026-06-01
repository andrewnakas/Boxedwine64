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

class XWireConnection;

// The server-side peer of wine's X11 client socket. Its only job is to forward
// the "peer wrote" edge into the owning XWireConnection so the request stream is
// parsed synchronously on the writing (guest) thread.
class XWireServerSocket : public KUnixSocketObject {
public:
    XWireServerSocket(U32 domain, U32 type, U32 protocol)
        : KUnixSocketObject(domain, type, protocol) {}
    void onPeerWrote() override;

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

    // Host input -> X11 wire events (Phase 2c input path).
    void deliverInputEvents();
    void sendInputEvent(uint8_t code, uint32_t window, const XWireInputEvent& ev);
    std::unordered_map<std::string, uint32_t> atoms;   // name -> atom id
    std::unordered_map<uint32_t, std::string> atomNames;
    uint32_t nextAtom = 1;

    uint32_t rootWindow = 0;
    uint32_t rootVisual = 0;
    uint32_t rootColormap = 0;
    uint32_t clientIdBase = 0;
    uint32_t clientIdMask = 0;

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
