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
struct XWindow {
    uint32_t parent = 0;
    int16_t x = 0, y = 0;
    uint16_t width = 0, height = 0;
    uint32_t eventMask = 0;
    bool mapped = false;
    bool isRoot = false;
    // ARGB8888 backing store for software-blitted (PutImage) content. Lazily
    // sized; persists between partial PutImages so a complete client area is
    // always available to present.
    std::vector<uint8_t> fb;
    uint16_t fbW = 0, fbH = 0;
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
    std::unordered_map<uint32_t, XWindow> windows;
    uint32_t presentWindow = 0;     // drawable currently shown on the host

    // Allocate the next distinct client resource-id base (so ids never collide
    // across connections).
    uint32_t allocClientIdBase();

private:
    XWireServer() {}
    std::vector<std::shared_ptr<XWireConnection>> connections;
    uint32_t nextClientBase = 0x00400000;
};

#endif
