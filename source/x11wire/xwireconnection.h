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

    // ---- resource model (minimal) ----
    struct XWindow {
        uint32_t parent = 0;
        int16_t x = 0, y = 0;
        uint16_t width = 0, height = 0;
        uint32_t eventMask = 0;
        bool mapped = false;
        bool isRoot = false;
    };
    std::unordered_map<uint32_t, XWindow> windows;
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
    std::weak_ptr<XWireServerSocket> serverPeer;

    uint32_t internAtom(const std::string& name, bool onlyIfExists);
    void processOneRequest(const uint8_t* req, uint32_t len);
    void doHandshake();
    void ensureWindow();
};

#endif
