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

#include "boxedwine.h"
#include "xwireserver.h"
#include "xwireconnection.h"
#include "kunixsocket.h"
#include "ksocket.h"

#include <cstring>

XWireServer& XWireServer::instance() {
    static XWireServer s;
    return s;
}

bool XWireServer::isXDisplayPath(const char* path) {
    if (!path) return false;
    // libX11 unix transport: "/tmp/.X11-unix/X0", "/tmp/.X11-unix/X1", ...
    static const char prefix[] = "/tmp/.X11-unix/X";
    size_t pl = sizeof(prefix) - 1;
    if (strncmp(path, prefix, pl) != 0) return false;
    // require at least one trailing display digit
    return path[pl] >= '0' && path[pl] <= '9';
}

bool XWireServer::acceptConnection(const std::shared_ptr<KUnixSocketObject>& client) {
    if (!client) return false;

    // Create the server-side peer with the same domain/type/protocol as the
    // client so reads/writes behave identically to a real socketpair.
    auto serverPeer = std::make_shared<XWireServerSocket>(K_AF_UNIX, K_SOCK_STREAM, 0);

    // Pair the two directions (mirrors KUnixSocketObject::accept's wiring).
    client->connection = serverPeer;
    serverPeer->connection = client;
    serverPeer->connected = true;

    auto conn = std::make_shared<XWireConnection>(client, serverPeer);
    serverPeer->owner = conn;
    connections.push_back(conn);

    klog_fmt("XWireServer: accepted X11 client connection (now %d open)",
             (int)connections.size());
    return true;
}
