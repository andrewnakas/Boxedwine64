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
#include "xwireconnection.h"
#include "kunixsocket.h"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// X11 protocol constants (subset). Little-endian assumed for our replies; we
// only support a little-endian client (libX11 on x86-64 sends 'l' = 0x6c).
// ---------------------------------------------------------------------------
namespace {
    // request opcodes (X11 core)
    enum {
        X_CreateWindow          = 1,
        X_ChangeWindowAttributes= 2,
        X_GetWindowAttributes   = 3,
        X_DestroyWindow         = 4,
        X_MapWindow             = 8,
        X_UnmapWindow           = 10,
        X_ConfigureWindow       = 12,
        X_GetGeometry           = 14,
        X_QueryTree             = 15,
        X_InternAtom            = 16,
        X_GetAtomName           = 17,
        X_ChangeProperty        = 18,
        X_DeleteProperty        = 19,
        X_GetProperty           = 20,
        X_GetInputFocus         = 43,
        X_QueryExtension        = 98,
        X_GetExtension          = 99,
        X_CreatePixmap          = 53,
        X_FreePixmap            = 54,
        X_CreateGC              = 55,
        X_ChangeGC              = 56,
        X_FreeGC                = 60,
        X_ClearArea             = 61,
        X_CopyArea              = 62,
        X_PolyFillRectangle     = 70,
        X_PutImage              = 72,
        X_ImageText8            = 76,
        X_ImageText16           = 77,
        X_CreateColormap        = 78,
        X_QueryColors           = 91,
        X_QueryBestSize         = 97,
        X_NoOperation           = 127,
    };

    // error codes
    enum {
        BadRequest = 1, BadValue = 2, BadWindow = 3, BadPixmap = 4,
        BadAtom = 5, BadDrawable = 9, BadAccess = 10, BadAlloc = 11,
        BadColor = 12, BadGC = 13, BadIDChoice = 14, BadName = 15,
        BadLength = 16, BadImplementation = 17,
    };

    inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    inline uint32_t rd32(const uint8_t* p) {
        return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
    }
}

XWireConnection::XWireConnection(const std::shared_ptr<KUnixSocketObject>& client,
                                 const std::shared_ptr<XWireServerSocket>& serverPeer)
    : client(client), serverPeer(serverPeer) {
}

U32 XWireServerSocket::readNativeNonBlocking(U8* buffer, U32 len) {
    BOXEDWINE_CRITICAL_SECTION_WITH_CONDITION(this->lockCond);
    size_t avail = this->recvBuffer.size_used();
    if (avail == 0) return 0;
    if (len > avail) len = (U32)avail;
    if (!this->recvBuffer.get(buffer, len)) return 0;
    BOXEDWINE_CONDITION_SIGNAL_ALL(this->lockCond);
    return len;
}

void XWireServerSocket::onPeerWrote() {
    std::shared_ptr<XWireConnection> conn = owner.lock();
    if (conn) {
        conn->onData();
    }
}

// ---------------------------------------------------------------------------
// Outbound: accumulate into 'out', then flush in one writeNative so the client
// sees coherent reply records.
// ---------------------------------------------------------------------------
void XWireConnection::writeToClient(const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    out.insert(out.end(), p, p + len);
}

void XWireConnection::flushReplies() {
    if (out.empty()) return;
    std::shared_ptr<KUnixSocketObject> peer = serverPeer.lock();
    if (peer) {
        // writeNative on the server peer lands the bytes in the *client's*
        // recvBuffer and signals its read/poll condition — exactly the wakeup
        // path a real X server socket uses.
        peer->writeNative(out.data(), (U32)out.size());
    }
    out.clear();
}

// X11 error reply: 32 bytes.
void XWireConnection::sendError(uint8_t code, uint16_t seq, uint32_t badValue,
                                uint8_t majorOp, uint16_t minorOp) {
    uint8_t e[32] = {0};
    e[0] = 0;                 // error
    e[1] = code;
    e[2] = (uint8_t)(seq & 0xff);
    e[3] = (uint8_t)(seq >> 8);
    e[4] = (uint8_t)(badValue & 0xff);
    e[5] = (uint8_t)((badValue >> 8) & 0xff);
    e[6] = (uint8_t)((badValue >> 16) & 0xff);
    e[7] = (uint8_t)((badValue >> 24) & 0xff);
    e[8] = (uint8_t)(minorOp & 0xff);
    e[9] = (uint8_t)(minorOp >> 8);
    e[10] = majorOp;
    writeToClient(e, sizeof(e));
}

uint32_t XWireConnection::internAtom(const std::string& name, bool onlyIfExists) {
    auto it = atoms.find(name);
    if (it != atoms.end()) return it->second;
    if (onlyIfExists) return 0; // None
    uint32_t id = nextAtom++;
    atoms[name] = id;
    atomNames[id] = name;
    return id;
}

// ---------------------------------------------------------------------------
// Connection setup. Client sends a 12-byte (+padded auth) request:
//   byte 0: byte order ('l' = LSB-first, 'B' = MSB-first)
//   byte 1: unused
//   u16   : protocol-major-version
//   u16   : protocol-minor-version
//   u16   : auth-proto-name length (n)
//   u16   : auth-proto-data length (d)
//   u16   : unused
//   then n bytes name + pad, d bytes data + pad
// We reply with a Setup-success record advertising one TrueColor screen.
// ---------------------------------------------------------------------------
void XWireConnection::doHandshake() {
    // Build a minimal Setup reply: header (8) + body. The body contains the
    // vendor string, the pixmap-format list, and one SCREEN with one DEPTH with
    // one VISUALTYPE. We pre-size all variable parts so length fields are exact.

    const char vendor[] = "Boxedwine";
    const uint16_t vendorLen = (uint16_t)(sizeof(vendor) - 1);
    const uint16_t vendorPad = (uint16_t)((4 - (vendorLen & 3)) & 3);

    // Resource-id base/mask handed to the client.
    clientIdBase = 0x00400000;
    clientIdMask = 0x001fffff;
    rootWindow   = 0x00000260;   // arbitrary, in the server-owned id space
    rootVisual   = 0x00000021;
    rootColormap = 0x00000020;

    // -- assemble body --
    std::vector<uint8_t> body;
    auto put8  = [&](uint8_t v){ body.push_back(v); };
    auto put16 = [&](uint16_t v){ body.push_back((uint8_t)(v&0xff)); body.push_back((uint8_t)(v>>8)); };
    auto put32 = [&](uint32_t v){ for (int i=0;i<4;i++) body.push_back((uint8_t)((v>>(8*i))&0xff)); };
    auto pad   = [&](uint32_t n){ for (uint32_t i=0;i<n;i++) body.push_back(0); };

    put32(0x00b00001);          // release-number (arbitrary)
    put32(clientIdBase);        // resource-id-base
    put32(clientIdMask);        // resource-id-mask
    put32(0);                   // motion-buffer-size
    put16(vendorLen);           // length of vendor
    put16(0xffff);              // maximum-request-length (in 4-byte units)
    put8(1);                    // number of SCREENs
    put8(1);                    // number of pixmap FORMATs
    put8(0);                    // image-byte-order: 0 = LSBFirst
    put8(0);                    // bitmap-format-bit-order: 0 = LeastSignificant
    put8(32);                   // bitmap-format-scanline-unit
    put8(32);                   // bitmap-format-scanline-pad
    put8(8);                    // min-keycode
    put8(255);                  // max-keycode
    pad(4);                     // unused
    body.insert(body.end(), (const uint8_t*)vendor, (const uint8_t*)vendor + vendorLen);
    pad(vendorPad);

    // pixmap FORMAT (8 bytes) — one entry for depth 24/bpp 32.
    put8(24);                   // depth
    put8(32);                   // bits-per-pixel
    put8(32);                   // scanline-pad
    pad(5);

    // SCREEN
    put32(rootWindow);          // root window
    put32(rootColormap);        // default colormap
    put32(0x00ffffff);          // white-pixel
    put32(0x00000000);          // black-pixel
    put32(0);                   // current-input-masks
    put16(screenWidth);         // width in pixels
    put16(screenHeight);        // height in pixels
    put16((uint16_t)(screenWidth * 254 / 720));  // width in mm (~96dpi)
    put16((uint16_t)(screenHeight * 254 / 720)); // height in mm
    put16(1);                   // min-installed-maps
    put16(1);                   // max-installed-maps
    put32(rootVisual);          // root-visual
    put8(0);                    // backing-stores: 0 = Never
    put8(0);                    // save-unders: false
    put8(24);                   // root-depth
    put8(1);                    // number of allowed DEPTHs

    // DEPTH (8 byte header) + one VISUALTYPE (24 bytes)
    put8(24);                   // depth
    pad(1);
    put16(1);                   // number of VISUALTYPES
    pad(4);
    // VISUALTYPE
    put32(rootVisual);          // visual-id
    put8(4);                    // class: 4 = TrueColor
    put8(8);                    // bits-per-rgb-value
    put16(256);                 // colormap-entries
    put32(0x00ff0000);          // red-mask
    put32(0x0000ff00);          // green-mask
    put32(0x000000ff);          // blue-mask
    pad(4);

    // -- header (8 bytes) --
    // success(1), unused(1), proto-major(2), proto-minor(2),
    // length-of-additional-data-in-4-byte-units(2)
    if (body.size() & 3) pad((uint32_t)((4 - (body.size() & 3)) & 3));
    uint16_t addLen = (uint16_t)(body.size() / 4);

    uint8_t hdr[8];
    hdr[0] = 1;                 // success
    hdr[1] = 0;
    hdr[2] = 11; hdr[3] = 0;    // protocol-major = 11
    hdr[4] = 0;  hdr[5] = 0;    // protocol-minor = 0
    hdr[6] = (uint8_t)(addLen & 0xff);
    hdr[7] = (uint8_t)(addLen >> 8);

    writeToClient(hdr, sizeof(hdr));
    writeToClient(body.data(), (uint32_t)body.size());

    // Register the root window in our model so geometry queries answer.
    XWindow& rw = windows[rootWindow];
    rw.isRoot = true; rw.mapped = true;
    rw.width = screenWidth; rw.height = screenHeight;

    handshakeDone = true;
    klog_fmt("XWire: handshake complete (vendor=%s root=0x%x visual=0x%x), %d body bytes",
             vendor, rootWindow, rootVisual, (int)body.size());
}

// ---------------------------------------------------------------------------
// Request dispatch. Each core request is >= 4 bytes; req[0]=opcode, req[1]=data,
// req[2..3]=length in 4-byte units (the full request size including header).
// ---------------------------------------------------------------------------
void XWireConnection::processOneRequest(const uint8_t* req, uint32_t len) {
    uint8_t opcode = req[0];
    sequence++;

    switch (opcode) {
        case X_CreateWindow: {
            // wid at req[4]; parent at req[8]; x,y,w,h at req[12..]
            uint32_t wid = rd32(req + 4);
            XWindow w;
            w.parent = rd32(req + 8);
            w.x = (int16_t)rd16(req + 12);
            w.y = (int16_t)rd16(req + 14);
            w.width = rd16(req + 16);
            w.height = rd16(req + 18);
            windows[wid] = w;
            break;
        }
        case X_ChangeWindowAttributes: {
            uint32_t wid = rd32(req + 4);
            uint32_t mask = rd32(req + 8);
            auto it = windows.find(wid);
            if (it != windows.end()) {
                // CWEventMask is bit 11 (0x800). The value list follows the mask
                // in mask-bit order; event-mask is the value if that bit is set
                // and it's the only/last bit we track. Find its slot.
                const uint8_t* vals = req + 12;
                uint32_t slot = 0;
                for (int bit = 0; bit < 15; bit++) {
                    if (mask & (1u << bit)) {
                        if (bit == 11) { it->second.eventMask = rd32(vals + slot*4); }
                        slot++;
                    }
                }
            }
            break;
        }
        case X_MapWindow: {
            uint32_t wid = rd32(req + 4);
            auto it = windows.find(wid);
            if (it != windows.end()) {
                it->second.mapped = true;
                ensureWindow();
                // A mapped top-level window expects an Expose to draw itself.
                if (it->second.width && it->second.height) {
                    sendExpose(wid, 0, 0, it->second.width, it->second.height);
                }
            }
            break;
        }
        case X_UnmapWindow: {
            uint32_t wid = rd32(req + 4);
            auto it = windows.find(wid);
            if (it != windows.end()) it->second.mapped = false;
            break;
        }
        case X_DestroyWindow: {
            windows.erase(rd32(req + 4));
            break;
        }
        case X_ConfigureWindow:
        case X_ClearArea:
        case X_FreeGC:
        case X_FreePixmap:
        case X_DeleteProperty:
        case X_ChangeGC:
        case X_CreateGC:
        case X_CreatePixmap:
        case X_CreateColormap:
        case X_PolyFillRectangle:
        case X_CopyArea:
        case X_PutImage:
        case X_ImageText8:
        case X_ImageText16:
        case X_ChangeProperty:
        case X_NoOperation:
            // No reply expected (or drawing handled later in Phase 2b/2c).
            break;

        case X_GetGeometry: {
            uint32_t drawable = rd32(req + 4);
            XWindow w;
            auto it = windows.find(drawable);
            if (it != windows.end()) w = it->second;
            else { w.width = screenWidth; w.height = screenHeight; }
            uint8_t r[32] = {0};
            r[0] = 1;                          // reply
            r[1] = 24;                         // depth
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            // reply-length = 0
            uint32_t root = rootWindow;
            memcpy(r + 8, &root, 4);            // root
            int16_t x = w.x, y = w.y;
            memcpy(r + 12, &x, 2);
            memcpy(r + 14, &y, 2);
            memcpy(r + 16, &w.width, 2);
            memcpy(r + 18, &w.height, 2);
            // border-width(2)@20 = 0
            writeToClient(r, sizeof(r));
            break;
        }
        case X_QueryTree: {
            // reply: root + parent(None) + 0 children
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            memcpy(r + 8, &rootWindow, 4);     // root
            // parent @12 = 0 (None), nchildren @16 = 0
            writeToClient(r, sizeof(r));
            break;
        }
        case X_InternAtom: {
            uint8_t onlyIfExists = req[1];
            uint16_t nameLen = rd16(req + 4);
            std::string name((const char*)(req + 8), nameLen);
            uint32_t atom = internAtom(name, onlyIfExists != 0);
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            memcpy(r + 8, &atom, 4);
            writeToClient(r, sizeof(r));
            break;
        }
        case X_GetAtomName: {
            uint32_t atom = rd32(req + 4);
            std::string name;
            auto it = atomNames.find(atom);
            if (it != atomNames.end()) name = it->second;
            uint16_t nameLen = (uint16_t)name.size();
            uint16_t padLen = (uint16_t)((4 - (nameLen & 3)) & 3);
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint32_t replyLen = (uint32_t)((nameLen + padLen) / 4);
            memcpy(r + 4, &replyLen, 4);
            memcpy(r + 8, &nameLen, 2);
            writeToClient(r, sizeof(r));
            if (nameLen) writeToClient(name.data(), nameLen);
            for (uint16_t i = 0; i < padLen; i++) { uint8_t z = 0; writeToClient(&z, 1); }
            break;
        }
        case X_GetProperty: {
            // Always report "property does not exist": type=None, len=0.
            uint8_t r[32] = {0};
            r[0] = 1;
            r[1] = 0;                          // format
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            // reply-length(4)@4 = 0, type@8 = None(0), bytes-after@12 = 0,
            // length-of-value@16 = 0
            writeToClient(r, sizeof(r));
            break;
        }
        case X_GetWindowAttributes: {
            // 3-reply-unit reply (44 bytes total = 32 + 12). Report a simple
            // mapped, viewable TrueColor input/output window.
            uint8_t r[44] = {0};
            r[0] = 1;
            r[1] = 2;                          // backing-store: NotUseful=0; use WhenMapped? keep 0
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint32_t replyLen = 3;
            memcpy(r + 4, &replyLen, 4);
            memcpy(r + 8, &rootVisual, 4);     // visual
            uint16_t cls = 1;                  // InputOutput
            memcpy(r + 12, &cls, 2);
            r[16] = 1;                         // map-is-installed
            r[17] = 2;                         // map-state: Viewable
            writeToClient(r, sizeof(r));
            break;
        }
        case X_GetInputFocus: {
            uint8_t r[32] = {0};
            r[0] = 1;
            r[1] = 0;                          // revert-to: None
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint32_t focus = rootWindow;
            memcpy(r + 8, &focus, 4);          // focus window
            writeToClient(r, sizeof(r));
            break;
        }
        case X_QueryExtension: {
            // Report every extension as not present (incl. MIT-SHM, GLX, XKB),
            // so libX11 uses the plain core path (non-SHM PutImage).
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            r[8] = 0;                          // present = false
            r[9] = 0;                          // major-opcode
            r[10] = 0;                         // first-event
            r[11] = 0;                         // first-error
            writeToClient(r, sizeof(r));
            break;
        }
        case X_QueryBestSize: {
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint16_t w = rd16(req + 8), h = rd16(req + 10);
            memcpy(r + 8, &w, 2);
            memcpy(r + 10, &h, 2);
            writeToClient(r, sizeof(r));
            break;
        }
        case X_QueryColors: {
            // reply with 0 colors
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            writeToClient(r, sizeof(r));
            break;
        }
        default:
            // Unknown request. If it expects a reply we'd hang libX11; but most
            // unknown ones here are reply-less. Log so the discovery loop sees
            // exactly which opcode notepad needs next.
            klog_fmt("XWire: unhandled request opcode=%d len=%d (seq=%d)",
                     (int)opcode, (int)len, (int)sequence);
            break;
    }
}

void XWireConnection::onData() {
    std::shared_ptr<XWireServerSocket> peer = serverPeer.lock();
    if (!peer) return;

    // Drain everything currently buffered into our assembly buffer.
    uint8_t chunk[4096];
    U32 got;
    while ((got = peer->readNativeNonBlocking(chunk, sizeof(chunk))) > 0) {
        in.insert(in.end(), chunk, chunk + got);
    }

    if (!handshakeDone) {
        // Need the 12-byte fixed part to know auth lengths.
        if (in.size() < 12) return;
        uint8_t order = in[0];
        bigEndian = (order == 'B');
        uint16_t nameLen = rd16(in.data() + 6);
        uint16_t dataLen = rd16(in.data() + 8);
        uint32_t namePad = (4 - (nameLen & 3)) & 3;
        uint32_t dataPad = (4 - (dataLen & 3)) & 3;
        uint32_t setupLen = 12 + nameLen + namePad + dataLen + dataPad;
        if (in.size() < setupLen) return;     // wait for the rest
        in.erase(in.begin(), in.begin() + setupLen);
        doHandshake();
        flushReplies();
        // fall through to process any pipelined requests
    }

    // Process complete requests. Core request length is in req[2..3] (4-byte
    // units); the minimum is 1 unit (4 bytes).
    while (in.size() >= 4) {
        uint32_t units = rd16(in.data() + 2);
        if (units == 0) units = 1;            // defensive
        uint32_t reqLen = units * 4;
        if (in.size() < reqLen) break;        // incomplete; wait for more
        processOneRequest(in.data(), reqLen);
        in.erase(in.begin(), in.begin() + reqLen);
    }
    flushReplies();
}

void XWireConnection::ensureWindow() {
    if (windowShown) return;
    windowShown = true;
    // SDL window creation + present wiring lands in Phase 2c. For now mark it so
    // the handshake path is exercised headlessly without a window.
    klog_fmt("XWire: first window mapped (SDL present wiring is Phase 2c)");
}

// ---------------------------------------------------------------------------
// Events: 32-byte records pushed to the client. Sequence is the last-processed
// request's sequence number.
// ---------------------------------------------------------------------------
void XWireConnection::sendExpose(uint32_t window, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint8_t e[32] = {0};
    e[0] = 12;                                 // Expose
    e[2] = (uint8_t)(sequence & 0xff);
    e[3] = (uint8_t)(sequence >> 8);
    memcpy(e + 4, &window, 4);
    memcpy(e + 8, &x, 2);
    memcpy(e + 10, &y, 2);
    memcpy(e + 12, &w, 2);
    memcpy(e + 14, &h, 2);
    // count @16 = 0 (last expose)
    writeToClient(e, sizeof(e));
}
