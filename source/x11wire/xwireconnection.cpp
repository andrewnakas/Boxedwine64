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
#include "xwireserver.h"
#include "xwirepresent.h"
#include "kunixsocket.h"

#include <cstring>
#include <string>

// The active presentation sink (platform/sdl sets it during window init). null
// when headless / -novideo, in which case all PutImage/Map presentation is a
// no-op and the wire server runs exactly as before.
XWirePresentSink* g_xwirePresentSink = nullptr;

// US-layout keysyms for the X keycodes our SDL->X mapping emits (evdev code + 8,
// see sdlScancodeToX11). GetKeyboardMapping must return REAL keysyms or winex11
// builds an all-NoSymbol keymap and typed keys produce no characters. Two
// keysyms per keycode: { unshifted, shifted }. Indexed by keycode (8..); 0 =
// NoSymbol. Latin-1 letter keysyms == their ASCII codes; X special keys use the
// 0xFFxx range. Covers the standard typing + editing set.
struct KeyPair { uint32_t lo, hi; };
static KeyPair keysymForKeycode(int kc) {
    switch (kc) {
        case 9:  return {0xFF1B, 0xFF1B};                 // Escape
        case 10: return {'1','!'};  case 11: return {'2','@'};
        case 12: return {'3','#'};  case 13: return {'4','$'};
        case 14: return {'5','%'};  case 15: return {'6','^'};
        case 16: return {'7','&'};  case 17: return {'8','*'};
        case 18: return {'9','('};  case 19: return {'0',')'};
        case 20: return {'-','_'};  case 21: return {'=','+'};
        case 22: return {0xFF08, 0xFF08};                 // BackSpace
        case 23: return {0xFF09, 0xFF09};                 // Tab
        case 24: return {'q','Q'};  case 25: return {'w','W'};
        case 26: return {'e','E'};  case 27: return {'r','R'};
        case 28: return {'t','T'};  case 29: return {'y','Y'};
        case 30: return {'u','U'};  case 31: return {'i','I'};
        case 32: return {'o','O'};  case 33: return {'p','P'};
        case 34: return {'[','{'};  case 35: return {']','}'};
        case 36: return {0xFF0D, 0xFF0D};                 // Return
        case 37: return {0xFFE3, 0xFFE3};                 // Control_L
        case 38: return {'a','A'};  case 39: return {'s','S'};
        case 40: return {'d','D'};  case 41: return {'f','F'};
        case 42: return {'g','G'};  case 43: return {'h','H'};
        case 44: return {'j','J'};  case 45: return {'k','K'};
        case 46: return {'l','L'};  case 47: return {';',':'};
        case 48: return {'\'','"'}; case 49: return {'`','~'};
        case 50: return {0xFFE1, 0xFFE1};                 // Shift_L
        case 51: return {'\\','|'};
        case 52: return {'z','Z'};  case 53: return {'x','X'};
        case 54: return {'c','C'};  case 55: return {'v','V'};
        case 56: return {'b','B'};  case 57: return {'n','N'};
        case 58: return {'m','M'};  case 59: return {',','<'};
        case 60: return {'.','>'};  case 61: return {'/','?'};
        case 62: return {0xFFE2, 0xFFE2};                 // Shift_R
        case 64: return {0xFFE9, 0xFFE9};                 // Alt_L
        case 65: return {' ',' '};                        // space
        case 105:return {0xFFE4, 0xFFE4};                 // Control_R
        case 110:return {0xFF50, 0xFF50};                 // Home
        case 111:return {0xFF52, 0xFF52};                 // Up
        case 112:return {0xFF55, 0xFF55};                 // Prior (PgUp)
        case 113:return {0xFF51, 0xFF51};                 // Left
        case 114:return {0xFF53, 0xFF53};                 // Right
        case 115:return {0xFF57, 0xFF57};                 // End
        case 116:return {0xFF54, 0xFF54};                 // Down
        case 117:return {0xFF56, 0xFF56};                 // Next (PgDn)
        case 118:return {0xFF63, 0xFF63};                 // Insert
        case 119:return {0xFFFF, 0xFFFF};                 // Delete
        default: return {0, 0};
    }
}

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
        X_GetSelectionOwner     = 23,
        X_GetInputFocus         = 43,
        X_SetClipRectangles     = 59,
        X_AllocColor            = 84,
        X_CreateCursor          = 93,
        X_FreeCursor            = 95,
        X_RecolorCursor         = 96,
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
        X_GetKeyboardMapping    = 101,
        X_GetModifierMapping    = 119,
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
    const std::shared_ptr<XWireServerSocket>& peer = serverPeer;
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

    // Resource-id base/mask handed to the client. Every client gets a DISTINCT
    // base (from the server) so resource ids are globally unique across
    // connections. Combined with the server-global window registry, this lets
    // the connection that PUTIMAGES a window find it even though a different
    // connection CREATED it (winex11 uses one X connection per thread).
    clientIdMask = 0x001fffff;
    clientIdBase = XWireServer::instance().allocClientIdBase();
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

    // Register the root window in the server-global model so geometry queries
    // answer. rootWindow is the same constant on every connection.
    {
        XWireServer& srv = XWireServer::instance();
        std::lock_guard<std::mutex> lk(srv.regMutex);
        XWindow& rw = srv.windows[rootWindow];
        rw.isRoot = true; rw.mapped = true;
        rw.width = screenWidth; rw.height = screenHeight;
    }

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

    if (getenv("BW64_XWIRE")) {
        klog_fmt("XWire: req opcode=%d detail=%d len=%d seq=%d",
                 (int)opcode, (int)req[1], (int)len, (int)sequence);
    }

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
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                srv.windows[wid] = w;
            }
            if (getenv("BW64_XWIRE")) {
                klog_fmt("XWire: CreateWindow wid=0x%x parent=0x%x %dx%d (base=0x%x)",
                         (int)wid, (int)w.parent, (int)w.width, (int)w.height, (int)clientIdBase);
            }
            break;
        }
        case X_ChangeWindowAttributes: {
            uint32_t wid = rd32(req + 4);
            uint32_t mask = rd32(req + 8);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            auto it = srv.windows.find(wid);
            if (it != srv.windows.end()) {
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
            uint16_t ew = 0, eh = 0;
            bool found = false;
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                auto it = srv.windows.find(wid);
                if (it != srv.windows.end()) {
                    it->second.mapped = true;
                    ew = it->second.width  ? it->second.width  : screenWidth;
                    eh = it->second.height ? it->second.height : screenHeight;
                    found = true;
                }
            }
            if (found) {
                ensureWindow();
                // The host window + presentWindow are chosen lazily on the first
                // PutImage (CreateWindow/Map geometry is unreliable — winex11
                // sizes via ConfigureWindow). Here we just send the Expose that
                // prompts the app to paint, which triggers that PutImage; a
                // generous fallback extent makes the app repaint its whole area.
                sendExpose(wid, 0, 0, ew, eh);
            }
            break;
        }
        case X_UnmapWindow: {
            uint32_t wid = rd32(req + 4);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            auto it = srv.windows.find(wid);
            if (it != srv.windows.end()) it->second.mapped = false;
            break;
        }
        case X_DestroyWindow: {
            uint32_t wid = rd32(req + 4);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            srv.windows.erase(wid);
            break;
        }
        case X_PutImage: {
            // PutImage(format, drawable, gc, width, height, dst-x, dst-y,
            //          left-pad, depth, <image data>). For a software-rendered
            // GDI app, winex11 ZPixmap-blits the whole client area into the
            // window's backing pixmap/window. We composite into a per-window
            // ARGB framebuffer and, when it targets the presented window, hand
            // the latest full image to the host sink.
            //  req[1]=format (2=ZPixmap), @4 drawable, @8 gc, @12 width,
            //  @14 height, @16 dst-x, @18 dst-y, @20 left-pad, @21 depth.
            uint8_t format = req[1];
            uint32_t drawable = rd32(req + 4);
            uint16_t w = rd16(req + 12);
            uint16_t h = rd16(req + 14);
            int16_t dstX = (int16_t)rd16(req + 16);
            int16_t dstY = (int16_t)rd16(req + 18);
            uint8_t depth = req[21];
            const uint8_t* img = req + 24;
            uint32_t imgBytes = (len > 24) ? (len - 24) : 0;
            blitPutImage(drawable, format, depth, dstX, dstY, w, h, img, imgBytes);
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
        case X_ImageText8:
        case X_ImageText16:
        case X_ChangeProperty:
        case X_SetClipRectangles:
        case X_CreateCursor:
        case X_FreeCursor:
        case X_RecolorCursor:
        case X_NoOperation:
            // No reply expected (or drawing handled later in Phase 2b/2c).
            break;

        case X_GetSelectionOwner: {
            // Report "no owner" (None). winex11 queries clipboard selection
            // owners during init; a None reply is correct for a fresh display.
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            // owner @8 = 0 (None)
            writeToClient(r, sizeof(r));
            break;
        }
        case X_AllocColor: {
            // AllocColor(cmap, red, green, blue) -> echo the requested RGB back
            // and synthesize a pixel from the high bytes (TrueColor 0xRRGGBB).
            // Our visual is 24bpp TrueColor so the pixel is just the packed RGB.
            uint16_t red   = rd16(req + 8);
            uint16_t green = rd16(req + 10);
            uint16_t blue  = rd16(req + 12);
            uint32_t pixel = ((uint32_t)(red   >> 8) << 16) |
                             ((uint32_t)(green >> 8) << 8)  |
                              (uint32_t)(blue  >> 8);
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            memcpy(r + 8, &red, 2);
            memcpy(r + 10, &green, 2);
            memcpy(r + 12, &blue, 2);
            memcpy(r + 16, &pixel, 4);
            writeToClient(r, sizeof(r));
            break;
        }

        case X_GetGeometry: {
            uint32_t drawable = rd32(req + 4);
            XWindow w;
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                auto it = srv.windows.find(drawable);
                if (it != srv.windows.end()) { w.x = it->second.x; w.y = it->second.y; w.width = it->second.width; w.height = it->second.height; }
                else { w.width = screenWidth; w.height = screenHeight; }
            }
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
        case X_GetKeyboardMapping: {
            // GetKeyboardMapping(first-keycode, count). Reply carries
            // keysyms-per-keycode (n) and count*n keysym values. winex11 calls
            // this during init to build its keymap; a real layout isn't needed
            // to bring up a window, so report n=1 and NoSymbol (0) for every
            // requested keycode — enough to satisfy the round-trip.
            uint8_t first = req[4];   // first keycode requested
            uint8_t count = req[5];   // number of keycodes
            const uint8_t n = 2;      // keysyms per keycode: {unshifted, shifted}
            uint32_t syms = (uint32_t)count * n;
            // Fixed 32-byte reply header + syms*4 bytes of keysym data.
            std::vector<uint8_t> r(32 + (size_t)syms * 4, 0);
            r[0] = 1;                 // reply
            r[1] = n;                 // keysyms-per-keycode
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint32_t replyLen = syms; // length in 4-byte units = syms*4/4
            memcpy(r.data() + 4, &replyLen, 4);
            // Fill REAL US-layout keysyms so winex11's keymap maps keys to chars.
            for (uint32_t i = 0; i < count; i++) {
                KeyPair kp = keysymForKeycode((int)first + (int)i);
                uint32_t off = 32 + i * n * 4;
                memcpy(r.data() + off,     &kp.lo, 4);
                memcpy(r.data() + off + 4, &kp.hi, 4);
            }
            writeToClient(r.data(), (uint32_t)r.size());
            break;
        }
        case X_GetModifierMapping: {
            // GetModifierMapping reply: keycodes-per-modifier (n) in r[1], then
            // 8*n keycodes for the 8 modifiers in order: Shift, Lock, Control,
            // Mod1(Alt), Mod2..Mod5. Bind real keycodes so winex11 knows which
            // keys are Shift/Ctrl/Alt — without this, Shift+key gives no capital.
            const uint8_t n = 2;      // keycodes per modifier
            uint32_t bytes = (uint32_t)8 * n;
            std::vector<uint8_t> r(32 + bytes, 0);
            r[0] = 1;                 // reply
            r[1] = n;                 // keycodes-per-modifier
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint32_t replyLen = (uint32_t)2 * n; // 4-byte units
            memcpy(r.data() + 4, &replyLen, 4);
            uint8_t* k = r.data() + 32;
            k[0*n + 0] = 50; k[0*n + 1] = 62;   // Shift: Shift_L, Shift_R
            // Lock (index 1): none
            k[2*n + 0] = 37; k[2*n + 1] = 105;  // Control: Control_L, Control_R
            k[3*n + 0] = 64;                     // Mod1 (Alt): Alt_L
            writeToClient(r.data(), (uint32_t)r.size());
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
    const std::shared_ptr<XWireServerSocket>& peer = serverPeer;
    if (!peer) return;

    // Drain everything currently buffered into our assembly buffer.
    uint8_t chunk[4096];
    U32 got;
    U32 drained = 0;
    while ((got = peer->readNativeNonBlocking(chunk, sizeof(chunk))) > 0) {
        in.insert(in.end(), chunk, chunk + got);
        drained += got;
    }
    if (getenv("BW64_XWIRE")) {
        klog_fmt("XWire: onData drained=%u inBuf=%zu handshakeDone=%d",
                 drained, in.size(), (int)handshakeDone);
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
    // Ride any pending host input out on this same wake (wine just talked to us,
    // so it'll read the socket again right away — events delivered now arrive
    // promptly without needing a separate writer thread).
    deliverInputEvents();
    flushReplies();
}

// Drain host input from the sink and emit X11 input events to the focused
// (presented) window, honoring its event mask. Called on the guest thread from
// onData(). Only the connection that OWNS the presented window (its id falls in
// this connection's resource-id range) delivers input, so events aren't
// duplicated across the several connections sharing the global registry.
void XWireConnection::deliverInputEvents() {
    if (!g_xwirePresentSink) return;
    XWireServer& srv = XWireServer::instance();
    uint32_t pw, mask;
    {
        std::lock_guard<std::mutex> lk(srv.regMutex);
        pw = srv.presentWindow;
        if (!pw) return;
        // Owner check: the presented window's id base must match ours.
        if ((pw & ~clientIdMask) != clientIdBase) return;
        auto it = srv.windows.find(pw);
        if (it == srv.windows.end()) return;
        mask = it->second.eventMask;
    }
    uint32_t presentWindow = pw;     // local copy for sendInputEvent

    XWireInputEvent ev;
    while (g_xwirePresentSink->nextInputEvent(ev)) {
        uint8_t code;
        uint32_t wantMask;
        switch (ev.type) {
            case XWireInputEvent::EvKeyDown:    code = 2;  wantMask = 0x00000001; break; // KeyPressMask
            case XWireInputEvent::EvKeyUp:      code = 3;  wantMask = 0x00000002; break; // KeyReleaseMask
            case XWireInputEvent::EvButtonDown: code = 4;  wantMask = 0x00000004; break; // ButtonPressMask
            case XWireInputEvent::EvButtonUp:   code = 5;  wantMask = 0x00000008; break; // ButtonReleaseMask
            case XWireInputEvent::EvMotion:     code = 6;  wantMask = 0x00000040; break; // PointerMotionMask
            default: continue;
        }
        // Deliver even if the mask bit isn't set for button/key — wine's windows
        // usually select these, and dropping them silently would feel dead. But
        // respect an explicit motion opt-out to avoid event floods.
        if (ev.type == XWireInputEvent::EvMotion && !(mask & wantMask)) continue;
        if (getenv("BW64_XWIRE")) {
            klog_fmt("XWire input: deliver code=%d detail=%d to win=0x%x (mask=0x%x)",
                     (int)code, (int)ev.detail, (unsigned)presentWindow, (unsigned)mask);
        }
        sendInputEvent(code, presentWindow, ev);
    }
}

// Emit a 32-byte X11 input event record (KeyPress/Release, Button*, Motion).
void XWireConnection::sendInputEvent(uint8_t code, uint32_t window, const XWireInputEvent& ev) {
    uint8_t e[32] = {0};
    e[0] = code;
    e[1] = (uint8_t)ev.detail;                 // keycode / button
    e[2] = (uint8_t)(sequence & 0xff);
    e[3] = (uint8_t)(sequence >> 8);
    // time @4 (4) — leave 0 (CurrentTime-ish; wine tolerates monotonic-ish 0)
    memcpy(e + 8,  &rootWindow, 4);             // root
    memcpy(e + 12, &window, 4);                 // event window
    // child @16 = None(0)
    int16_t rx = (int16_t)ev.x, ry = (int16_t)ev.y;
    memcpy(e + 20, &rx, 2);                     // root-x
    memcpy(e + 22, &ry, 2);                     // root-y
    memcpy(e + 24, &rx, 2);                     // event-x
    memcpy(e + 26, &ry, 2);                     // event-y
    uint16_t state = (uint16_t)ev.state;
    memcpy(e + 28, &state, 2);                  // state
    e[30] = 1;                                  // same-screen = True
    writeToClient(e, sizeof(e));
}

void XWireConnection::ensureWindow() {
    if (windowShown) return;
    windowShown = true;
    klog_fmt("XWire: first window mapped (present sink=%s)",
             g_xwirePresentSink ? "SDL" : "headless");
}

// Decode an X11 PutImage (ZPixmap, TrueColor 24/32bpp) into the target window's
// ARGB backing store and, when it's the window we present, hand the full image
// to the host sink. winex11 sends 32-bit-per-pixel ZPixmap on our visual, so a
// source row is w*4 bytes in 0x00RRGGBB (== ARGB8888 with X in the high byte)
// little-endian order — already what SDL_PIXELFORMAT_ARGB8888 expects. Non-32bpp
// or non-ZPixmap forms are uncommon for GDI blits; we ignore them (the window
// keeps its prior contents) rather than mis-decode.
void XWireConnection::blitPutImage(uint32_t drawable, uint8_t format, uint8_t depth,
                                   int16_t dstX, int16_t dstY, uint16_t w, uint16_t h,
                                   const uint8_t* data, uint32_t dataBytes) {
    if (!w || !h) return;
    if (format != 2 /*ZPixmap*/) return;        // only ZPixmap supported
    if (depth != 24 && depth != 32) return;     // only TrueColor 24/32

    const uint32_t bpp = 4;
    const uint32_t srcPitch = (uint32_t)w * bpp;
    if ((uint64_t)srcPitch * h > dataBytes) {
        // Truncated payload (shouldn't happen for a single-request blit); bail
        // rather than read past the buffer.
        return;
    }

    XWireServer& srv = XWireServer::instance();
    std::lock_guard<std::mutex> lk(srv.regMutex);

    if (getenv("BW64_XWIRE")) {
        klog_fmt("XWire: PutImage drawable=0x%x %dx%d at (%d,%d) presentWindow=0x%x isWin=%d",
                 (int)drawable, (int)w, (int)h, (int)dstX, (int)dstY,
                 (int)srv.presentWindow, (int)(srv.windows.count(drawable) ? 1 : 0));
    }
    auto it = srv.windows.find(drawable);
    if (it == srv.windows.end()) {
        // A pixmap (off-screen drawable) we don't model as a window; ignore for
        // now — winex11 normally CopyAreas it to the window, which is the path we
        // present. (CopyArea compositing is a later refinement.)
        return;
    }
    XWindow& win = it->second;
    if (win.isRoot) return;     // never present the root/desktop background

    // Adopt the first window an app actually draws into as the presented window
    // (server-global, so it survives the create-vs-draw connection split). The
    // PutImage target + tiling is the reliable "this is the real client area"
    // signal; CreateWindow/Map geometry is unreliable (winex11 resizes via
    // ConfigureWindow which we don't fully model).
    if (srv.presentWindow == 0 || drawable == srv.presentWindow) {
        srv.presentWindow = drawable;
    }

    // Track the window's full extent from the blit reach. winex11 tiles the
    // client area in horizontal bands, so the max (dstX+w, dstY+h) across blits
    // is the true client size even when CreateWindow gave us 0x0.
    uint16_t reachW = (uint16_t)((dstX > 0 ? dstX : 0) + w);
    uint16_t reachH = (uint16_t)((dstY > 0 ? dstY : 0) + h);
    if (reachW > win.width)  win.width = reachW;
    if (reachH > win.height) win.height = reachH;

    uint16_t winW = win.width ? win.width : w;
    uint16_t winH = win.height ? win.height : h;
    if (win.fbW != winW || win.fbH != winH) {
        // Preserve existing content when the framebuffer grows (bands arrive one
        // at a time): re-layout the old rows into the resized buffer.
        std::vector<uint8_t> grown((size_t)winW * winH * bpp, 0);
        if (!win.fb.empty()) {
            uint16_t copyH = win.fbH < winH ? win.fbH : winH;
            uint16_t copyW = win.fbW < winW ? win.fbW : winW;
            for (uint16_t y = 0; y < copyH; y++) {
                memcpy(grown.data() + (size_t)y * winW * bpp,
                       win.fb.data() + (size_t)y * win.fbW * bpp,
                       (size_t)copyW * bpp);
            }
        }
        win.fb.swap(grown);
        win.fbW = winW;
        win.fbH = winH;
    }

    bool isPresented = (drawable == srv.presentWindow);
    if (g_xwirePresentSink && isPresented) {
        g_xwirePresentSink->onWindowMapped(drawable, winW, winH);
    }

    // Copy each source row into the framebuffer at (dstX, dstY), clipping to the
    // window bounds.
    for (uint16_t row = 0; row < h; row++) {
        int dy = dstY + row;
        if (dy < 0 || dy >= winH) continue;
        int dx = dstX;
        uint16_t copyW = w;
        const uint8_t* src = data + (size_t)row * srcPitch;
        if (dx < 0) { src += (size_t)(-dx) * bpp; copyW = (uint16_t)(copyW + dx); dx = 0; }
        if (dx >= winW || copyW == 0) continue;
        if (dx + copyW > winW) copyW = (uint16_t)(winW - dx);
        uint8_t* dstRow = win.fb.data() + ((size_t)dy * winW + dx) * bpp;
        memcpy(dstRow, src, (size_t)copyW * bpp);
    }

    if (g_xwirePresentSink && isPresented) {
        g_xwirePresentSink->submitFrame(drawable, winW, winH, win.fb.data(),
                                        (uint32_t)winW * bpp);
    }
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
