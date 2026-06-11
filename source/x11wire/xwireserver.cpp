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
#include "xwirepresent.h"
#include "kunixsocket.h"
#include "ksocket.h"

#include <algorithm>
#include <cstring>
#include <vector>

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

uint32_t XWireServer::allocClientIdBase() {
    std::lock_guard<std::mutex> lk(regMutex);
    uint32_t base = nextClientBase;
    nextClientBase += 0x00200000;   // step past the 21-bit client id mask
    return base;
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
    size_t nconn;
    {
        std::lock_guard<std::mutex> lk(connMutex);
        connections.push_back(conn);
        nconn = connections.size();
    }

    klog_fmt("XWireServer: accepted X11 client connection (now %d open)",
             (int)nconn);
    return true;
}

// Composite the base (main) window with any mapped overlay windows (menus,
// dropdowns, tooltips, dialogs) — each its own top-level X window that wine
// draws once — into a single host-sized image, then submit it to the present
// sink. Without this only `presentWindow` is shown and menus render to an
// invisible backing store (they ARE open in wine, just never blitted).
void XWireServer::composeAndPresent() {
    if (!g_xwirePresentSink) return;

    // Snapshot what we need under the lock, then blit outside it.
    uint32_t baseId = 0;
    uint16_t hostW = 0, hostH = 0;
    std::vector<uint8_t> canvas;          // host-sized ARGB, == base window fb
    std::vector<uint8_t> baseText;        // base window text overlay (may be empty)
    struct Overlay { int x, y; uint16_t w, h; std::vector<uint8_t> fb;
                     std::vector<uint8_t> textFb; uint64_t serial; };
    std::vector<Overlay> overlays;
    {
        std::lock_guard<std::mutex> lk(regMutex);
        baseId = presentWindow;
        if (!baseId) return;
        auto bit = windows.find(baseId);
        if (bit == windows.end() || bit->second.fb.empty()) return;
        XWireWindow& base = bit->second;
        hostW = base.fbW; hostH = base.fbH;
        canvas = base.fb;                 // copy the background
        baseText = base.textFb;           // copy the text overlay
        // The base client is at this root origin (e.g. (4,23)); we present its fb
        // at host (0,0), so an overlay at root (ox,oy) lands at host (ox-baseX,
        // oy-baseY).
        int baseX = base.x, baseY = base.y;
        uint64_t baseSerial = base.mapSerial;

        // Gather mapped overlay windows (anything mapped + drawn that isn't the
        // base or the root), oldest-mapped first so newer menus land on top.
        // EXCLUDE windows mapped BEFORE the base (older serial): those belong to a
        // previously-launched app that's still running in the persistent session.
        // Compositing them would draw the old app (e.g. Notepad) on top of/beside
        // the new base (e.g. glcube). The current app's own popups/menus map
        // after its main window, so serial >= baseSerial keeps them.
        for (auto& kv : windows) {
            XWireWindow& w = kv.second;
            if (kv.first == baseId || w.isRoot) continue;
            if (!w.mapped || w.fb.empty() || !w.fbW || !w.fbH) continue;
            if (w.mapSerial < baseSerial) continue; // window from an older app
            overlays.push_back({ (int)w.x - baseX, (int)w.y - baseY,
                                 w.fbW, w.fbH, w.fb, w.textFb, w.mapSerial });
        }
    }
    if (canvas.empty() || !hostW || !hostH) return;

    const uint32_t bpp = 4;
    // Blit the base window's text overlay onto the canvas (opaque where != 0).
    if (baseText.size() == canvas.size()) {
        const uint32_t* t = reinterpret_cast<const uint32_t*>(baseText.data());
        uint32_t* c = reinterpret_cast<uint32_t*>(canvas.data());
        size_t n = (size_t)hostW * hostH;
        for (size_t i = 0; i < n; i++) if (t[i]) c[i] = t[i];
    }
    if (!overlays.empty()) {
        std::sort(overlays.begin(), overlays.end(),
                  [](const Overlay& a, const Overlay& b) { return a.serial < b.serial; });
        for (const Overlay& ov : overlays) {
            bool haveText = (ov.textFb.size() == (size_t)ov.w * ov.h * bpp);
            for (uint16_t row = 0; row < ov.h; row++) {
                int dy = ov.y + row;
                if (dy < 0 || dy >= hostH) continue;
                int dx = ov.x;
                uint16_t copyW = ov.w;
                uint32_t srcOff = (uint32_t)row * ov.w;     // in pixels
                const uint8_t* src = ov.fb.data() + (size_t)srcOff * bpp;
                if (dx < 0) { srcOff += (uint32_t)(-dx); src += (size_t)(-dx) * bpp; copyW = (uint16_t)(copyW + dx); dx = 0; }
                if (dx >= hostW || copyW == 0) continue;
                if (dx + copyW > hostW) copyW = (uint16_t)(hostW - dx);
                memcpy(canvas.data() + ((size_t)dy * hostW + dx) * bpp, src, (size_t)copyW * bpp);
                // Overlay this window's own text on top of its just-blitted fb.
                if (haveText) {
                    const uint32_t* t = reinterpret_cast<const uint32_t*>(ov.textFb.data()) + srcOff;
                    uint32_t* c = reinterpret_cast<uint32_t*>(canvas.data()) + (size_t)dy * hostW + dx;
                    for (uint16_t i = 0; i < copyW; i++) if (t[i]) c[i] = t[i];
                }
            }
        }
    }

    g_xwirePresentSink->onWindowMapped(baseId, hostW, hostH);
    g_xwirePresentSink->submitFrame(baseId, hostW, hostH, canvas.data(), (uint32_t)hostW * bpp);
}

void XWireServer::resetForAppSwitch() {
    uint32_t outgoingBase = 0;
    {
    std::lock_guard<std::mutex> lk(regMutex);
    // The connection base that owns the CURRENT base window — resolved to its
    // guest pid below (outside regMutex) so the kill path can terminate the
    // process that really owns the window, not just the relay pid JS tracked.
    auto ow = windows.find(presentWindow);
    if (ow != windows.end()) outgoingBase = ow->second.ownerClientBase;
    // Arm "adopt the next app window as the base". Windows are NOT wiped here —
    // the launcher kills the outgoing app right after queueing the new spawn,
    // and that kill path (dropAppByPid) erases the old app's windows; until it
    // runs, the owner-base watermark below keeps the old app from re-winning
    // the base.
    adoptNextWindowAsBase = true;
    // Only adopt a window mapped AFTER now, so the new app's fresh window wins
    // over the still-running previous app's existing window (older serial).
    adoptArmSerial = mapSerialCounter;
    // The real discriminator: capture the current client-base watermark. Every
    // connection the newly spawned app opens gets a base >= this; every still-
    // running previous app's connection has a base < this. So a window is the new
    // app's iff ownerClientBase >= adoptArmClientBase — independent of how much
    // the old app keeps remapping/repainting. (nextClientBase is guarded by the
    // same regMutex we hold here.)
    adoptArmClientBase = nextClientBase;
    // Drop any foreground-GL claim: the app being switched away from may be a
    // still-running GL app (glcube) that keeps swapping buffers. Clearing this
    // means its frames are filtered out until/unless the NEW app is itself GL and
    // re-claims via glXMakeCurrent. Prevents a background cube painting over the
    // GDI app the user just switched to.
    glPresentDrawable = 0;
    klog_fmt("XWire: app switch armed (adopt next window mapped after serial %llu)",
             (unsigned long long)adoptArmSerial);
    }
    // Resolve the outgoing window's owning connection to its guest pid (the
    // app the user is switching away from). connMutex and regMutex are never
    // held together anywhere, so taking connMutex after dropping regMutex
    // keeps that invariant.
    uint32_t pid = 0;
    if (outgoingBase) {
        std::lock_guard<std::mutex> lk(connMutex);
        for (auto& c : connections) {
            if (c->idBase() == outgoingBase) { pid = c->ownerPid; break; }
        }
    }
    outgoingAppPid.store(pid, std::memory_order_release);
    if (pid) {
        klog_fmt("XWire: outgoing app on switch = pid %d (window owner base 0x%x)",
                 (int)pid, outgoingBase);
    }
}

// Consumed by the session bridge's kill path: the guest pid that owns the
// window being switched away from (captured at arm time), one-shot.
uint32_t bw64TakeOutgoingAppPid() {
    return XWireServer::instance().outgoingAppPid.exchange(0);
}

bool XWireServer::unmapAppWindows(uint32_t ownerBase) {
    if (!ownerBase) return false;
    std::lock_guard<std::mutex> lk(regMutex);
    bool changed = false;
    int n = 0;
    for (auto& kv : windows) {
        XWireWindow& w = kv.second;
        if (w.isRoot) continue;
        if (w.ownerClientBase == ownerBase && w.mapped) {
            // Mark the outgoing app's window un-mapped so the compositor
            // (composeAndPresent skips !mapped) and the input hit-test
            // (deliverInputEvents skips !mapped) both ignore it. The window
            // record + its framebuffer are kept intact; the app keeps running
            // and could be re-presented later. We do NOT send the client an
            // UnmapNotify — that would make wine think the WM withdrew its
            // window and could perturb its message loop; this is a host-side
            // presentation hide only.
            w.mapped = false;
            changed = true;
            n++;
        }
    }
    if (changed) {
        klog_fmt("XWire: unmapped %d window(s) of outgoing app (owner base 0x%x) on switch",
                 n, ownerBase);
    }
    return changed;
}

bool XWireServer::dropAppByPid(uint32_t pid) {
    if (!pid) return false;
    // Collect the killed app's connections (one per winex11 thread) and remove
    // them from the pump list so deliverInputEvents/flushReplies stop touching
    // their dead sockets. Each connection's resource-id base identifies the X
    // resources it created.
    std::vector<uint32_t> bases;
    size_t droppedConns = 0;
    {
        std::lock_guard<std::mutex> lk(connMutex);
        for (auto it = connections.begin(); it != connections.end();) {
            if ((*it)->ownerPid == pid) {
                if ((*it)->idBase()) bases.push_back((*it)->idBase());
                it = connections.erase(it);
                droppedConns++;
            } else {
                ++it;
            }
        }
    }
    if (!droppedConns) return false;

    // A resource id's top bits are its creating connection's base (the 21-bit
    // client id mask below the base step). True for window/pixmap/gc/font ids
    // and for ownerClientBase values themselves (bases are mask-aligned).
    auto owned = [&bases](uint32_t rid) {
        uint32_t base = rid & ~0x001fffffu;
        for (uint32_t b : bases) {
            if (base == b) return true;
        }
        return false;
    };

    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(regMutex);
        for (auto it = windows.begin(); it != windows.end();) {
            if (!it->second.isRoot &&
                (owned(it->first) || owned(it->second.ownerClientBase))) {
                it = windows.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        for (auto it = pixmaps.begin(); it != pixmaps.end();) {
            if (owned(it->first)) { it = pixmaps.erase(it); } else { ++it; }
        }
        for (auto it = gcs.begin(); it != gcs.end();) {
            if (owned(it->first)) { it = gcs.erase(it); } else { ++it; }
        }
        for (auto it = fonts.begin(); it != fonts.end();) {
            if (owned(it->first)) { it = fonts.erase(it); } else { ++it; }
        }
        for (auto it = cursorShapes.begin(); it != cursorShapes.end();) {
            if (owned(it->first)) { it = cursorShapes.erase(it); } else { ++it; }
        }
        for (auto it = selectionOwners.begin(); it != selectionOwners.end();) {
            if (owned(it->second)) { it = selectionOwners.erase(it); } else { ++it; }
        }
        if (presentWindow && owned(presentWindow)) {
            presentWindow = 0;
            changed = true;
        }
        if (glPresentDrawable && owned(glPresentDrawable)) {
            glPresentDrawable = 0;
        }
    }
    klog_fmt("XWire: dropped killed app pid=%d (%d connection(s), %d resource base(s))",
             (int)pid, (int)droppedConns, (int)bases.size());
    return changed;
}

// Free-function shim so the persistent-session bridge (wine64session.cpp) can
// trigger an app-switch reset without including the XWire header.
void bw64ResetPresentForSwitch() {
    XWireServer::instance().resetForAppSwitch();
}

// Shim for the session bridge's kill path: erase the killed app's host-side X
// presence and, if it owned the canvas, wipe to black so its last frame doesn't
// linger while the next app boots.
void bw64DropAppPresenceByPid(uint32_t pid) {
    XWireServer& srv = XWireServer::instance();
    if (srv.dropAppByPid(pid)) {
        if (g_xwirePresentSink) {
            g_xwirePresentSink->requestClear();
        }
        srv.schedulePresent();
    }
}

// Flush queued host input to every connection from the main thread's present
// tick, so an idle app (not sending requests) still gets keystrokes/clicks.
void XWireServer::pumpInput() {
    std::vector<std::shared_ptr<XWireConnection>> snapshot;
    {
        std::lock_guard<std::mutex> lk(connMutex);
        snapshot = connections;
    }
    for (auto& c : snapshot) c->pumpInputAndFlush();
}
