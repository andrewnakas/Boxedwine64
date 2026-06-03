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
#include "xwirefont.h"
#include "kunixsocket.h"
#include "knativesystem.h"
#include "knativescreen.h"

#include <cstring>
#include <string>
#include <vector>
#include <utility>

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
        X_SetSelectionOwner     = 22,
        X_GetSelectionOwner     = 23,
        X_SendEvent             = 25,
        X_GrabPointer           = 26,
        X_UngrabPointer         = 27,
        X_ChangeActivePointerGrab = 30,
        X_GrabKeyboard          = 31,
        X_UngrabKeyboard        = 32,
        X_AllowEvents           = 35,
        X_GrabServer            = 36,
        X_UngrabServer          = 37,
        X_QueryPointer          = 38,
        X_GetMotionEvents       = 39,
        X_TranslateCoords       = 40,
        X_WarpPointer           = 41,
        X_SetInputFocus         = 42,
        X_GetInputFocus         = 43,
        X_GetPointerMapping     = 117,
        X_SetClipRectangles     = 59,
        X_AllocColor            = 84,
        X_CreateCursor          = 93,
        X_CreateGlyphCursor     = 94,
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
        X_OpenFont              = 45,
        X_CloseFont             = 46,
        X_QueryFont             = 47,
        X_QueryTextExtents      = 48,
        X_ListFonts             = 49,
        X_ListFontsWithInfo     = 50,
        X_GetFontPath           = 52,
        X_PolyText8             = 74,
        X_PolyText16            = 75,
        X_ImageText8            = 76,
        X_ImageText16           = 77,
        X_CreateColormap        = 78,
        X_QueryColors           = 91,
        X_QueryBestSize         = 97,
        X_GetKeyboardMapping    = 101,
        X_GetModifierMapping    = 119,
        X_NoOperation           = 127,
    };

    // GLX extension. We advertise it present (see X_QueryExtension) so wine's
    // winex11.drv loads our direct-rendering libGL.so.1; GL itself never rides
    // this wire (the guest libGL traps straight to the host). The major opcode
    // is the request-stream opcode libX11 assigns to GLX after QueryExtension.
    enum {
        GLX_MAJOR_OPCODE = 149,
        GLX_FIRST_EVENT  = 95,   // first GLX event code (BufferSwapComplete et al.)
        // GLX minor opcodes (the few libX11 might still send synchronously).
        X_GLXQueryVersion        = 7,
        X_GLXQueryServerString   = 19,
        X_GLXClientInfo          = 20,
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

    // Window value-mask bits shared by CreateWindow + ChangeWindowAttributes.
    enum {
        CWBackPixmap=0, CWBackPixel=1, CWBorderPixmap=2, CWBorderPixel=3,
        CWBitGravity=4, CWWinGravity=5, CWBackingStore=6, CWBackingPlanes=7,
        CWBackingPixel=8, CWOverrideRedirect=9, CWSaveUnder=10, CWEventMask=11,
        CWDontPropagate=12, CWColormap=13, CWCursor=14,
    };

    // Walk a CW value list (in mask-bit order) and pull out the few fields we
    // model: event mask, override-redirect (marks menus/popups), and cursor.
    void applyWindowValues(XWireWindow& w, uint32_t mask, const uint8_t* vals) {
        uint32_t slot = 0;
        for (int bit = 0; bit <= CWCursor; bit++) {
            if (!(mask & (1u << bit))) continue;
            uint32_t v = rd32(vals + slot * 4);
            switch (bit) {
                case CWOverrideRedirect: w.overrideRedirect = (v != 0); break;
                case CWEventMask:        w.eventMask = v; break;
                case CWCursor:           w.cursor = v; break;
                default: break;
            }
            slot++;
        }
    }

    // GC value-mask bits (value list is in this order, 4 bytes per present
    // value). We only model the ones core-text needs.
    enum {
        GCFunction=0, GCPlaneMask=1, GCForeground=2, GCBackground=3,
        GCLineWidth=4, GCLineStyle=5, GCCapStyle=6, GCJoinStyle=7,
        GCFillStyle=8, GCFillRule=9, GCTile=10, GCStipple=11,
        GCTileStipXOrigin=12, GCTileStipYOrigin=13, GCFont=14,
        GCSubwindowMode=15, GCGraphicsExposures=16, GCClipXOrigin=17,
        GCClipYOrigin=18, GCClipMask=19, GCDashOffset=20, GCDashList=21,
        GCArcMode=22,
    };

    // Apply a GC value list to a XWireGC. The pixel value for our TrueColor
    // 24/32 visual is 0x00RRGGBB; store it as opaque ARGB.
    void applyGCValues(XWireGC& gc, uint32_t mask, const uint8_t* vals) {
        uint32_t slot = 0;
        for (int bit = 0; bit <= GCArcMode; bit++) {
            if (!(mask & (1u << bit))) continue;
            uint32_t v = rd32(vals + slot * 4);
            switch (bit) {
                case GCForeground: gc.foreground = 0xff000000u | (v & 0x00ffffffu); break;
                case GCBackground: gc.background = 0xff000000u | (v & 0x00ffffffu); break;
                case GCFont:       gc.font = v; break;
                default: break;
            }
            slot++;
        }
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
        XWireWindow& rw = srv.windows[rootWindow];
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
            // wid at req[4]; parent at req[8]; x,y,w,h at req[12..]; the CW value
            // mask is at req[28], value list at req[32].
            uint32_t wid = rd32(req + 4);
            XWireWindow w;
            w.parent = rd32(req + 8);
            w.x = (int16_t)rd16(req + 12);
            w.y = (int16_t)rd16(req + 14);
            w.width = rd16(req + 16);
            w.height = rd16(req + 18);
            applyWindowValues(w, rd32(req + 28), req + 32);
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
            int applyShape = -1;
            {
                std::lock_guard<std::mutex> lk(srv.regMutex);
                auto it = srv.windows.find(wid);
                if (it != srv.windows.end()) {
                    applyWindowValues(it->second, mask, req + 12);
                    // If this CWCursor set a cursor for a window the pointer is in
                    // (base or a mapped overlay), resolve it to its glyph shape and
                    // show that as the host cursor — so the on-screen pointer is
                    // wine's own cursor (I-beam in text, arrow elsewhere).
                    if ((mask & (1u << 14)) && it->second.cursor) {
                        bool relevant = (wid == srv.presentWindow) ||
                                        (it->second.mapped && !it->second.isRoot);
                        if (relevant) {
                            auto cit = srv.cursorShapes.find(it->second.cursor);
                            applyShape = (cit != srv.cursorShapes.end())
                                         ? (int)cit->second : 68 /*XC_left_ptr*/;
                        }
                    }
                }
            }
            if (applyShape >= 0) {
                KNativeScreenPtr screen = KNativeSystem::getScreen();
                if (screen) screen->setCursorByX11Shape(applyShape);
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
                    it->second.mapSerial = ++srv.mapSerialCounter;  // stack order
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
                // Grant keyboard focus to the mapped window so wine activates
                // its input queue — otherwise delivered KeyPress events are
                // ignored and typing feels dead.
                sendFocusIn(wid);
            }
            break;
        }
        case X_UnmapWindow: {
            uint32_t wid = rd32(req + 4);
            XWireServer& srv = XWireServer::instance();
            bool wasOverlay = false;
            {
                std::lock_guard<std::mutex> lk(srv.regMutex);
                auto it = srv.windows.find(wid);
                if (it != srv.windows.end()) {
                    it->second.mapped = false;
                    wasOverlay = (wid != srv.presentWindow && !it->second.isRoot);
                }
            }
            // A menu/popup just closed — recompose so it disappears from the host.
            if (wasOverlay) srv.composeAndPresent();
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
        case X_ConfigureWindow: {
            // Track popup/menu geometry on root so the compositor places them
            // correctly. Value mask @8, value list @12; bits in order:
            // 0=x,1=y,2=width,3=height,4=border,5=sibling,6=stack-mode.
            uint32_t wid = rd32(req + 4);
            uint16_t mask = rd16(req + 8);
            const uint8_t* vals = req + 12;
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            auto it = srv.windows.find(wid);
            if (it != srv.windows.end()) {
                uint32_t slot = 0;
                for (int bit = 0; bit < 7; bit++) {
                    if (mask & (1u << bit)) {
                        int32_t v = (int32_t)rd32(vals + slot * 4);
                        switch (bit) {
                            case 0: it->second.x = (int16_t)v; break;
                            case 1: it->second.y = (int16_t)v; break;
                            case 2: it->second.width = (uint16_t)v; break;
                            case 3: it->second.height = (uint16_t)v; break;
                            default: break;   // border/sibling/stack — ignored
                        }
                        slot++;
                    }
                }
                if (getenv("BW64_XWIRE")) {
                    klog_fmt("XWire: ConfigureWindow wid=0x%x -> x=%d y=%d %dx%d",
                             (int)wid, (int)it->second.x, (int)it->second.y,
                             (int)it->second.width, (int)it->second.height);
                }
            }
            break;
        }
        case X_CreateGC: {
            // cid@4, drawable@8, value-mask@12, value list@16.
            uint32_t cid  = rd32(req + 4);
            uint32_t mask = rd32(req + 12);
            XWireGC gc;
            applyGCValues(gc, mask, req + 16);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            srv.gcs[cid] = gc;
            break;
        }
        case X_ChangeGC: {
            // gc@4, value-mask@8, value list@12. Default-creates the GC if unseen.
            uint32_t cid  = rd32(req + 4);
            uint32_t mask = rd32(req + 8);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            applyGCValues(srv.gcs[cid], mask, req + 12);
            break;
        }
        case X_FreeGC: {
            uint32_t cid = rd32(req + 4);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            srv.gcs.erase(cid);
            break;
        }
        case X_OpenFont: {
            // fid@4, length-of-name@8 (u16), name@12. All map to the one builtin
            // font; we just record the id so CloseFont can drop it. Reply-less.
            uint32_t fid = rd32(req + 4);
            uint16_t nlen = rd16(req + 8);
            std::string name;
            if (len >= 12u + nlen) name.assign((const char*)(req + 12), nlen);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            srv.fonts[fid] = name;
            break;
        }
        case X_CloseFont: {
            uint32_t fid = rd32(req + 4);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            srv.fonts.erase(fid);
            break;
        }
        case X_ClearArea:
        case X_FreePixmap:
        case X_DeleteProperty:
        case X_CreatePixmap:
        case X_CreateColormap:
        case X_PolyFillRectangle:
        case X_CopyArea:
        case X_ChangeProperty:
        case X_SetClipRectangles:
        case X_CreateGlyphCursor: {
            // This block is a catch-all for reply-less drawing/property ops we
            // don't model; only CreateGlyphCursor needs to inspect its payload.
            // Guard the field reads by opcode + length so a shorter request
            // (e.g. a 16-byte CopyArea) never reads past the assembled request.
            if (opcode == X_CreateGlyphCursor && len >= 18) {
                // CreateGlyphCursor(cid, source-font, mask-font, source-char,
                // mask-char, fg/bg rgb). The source-char IS the XC_* cursor shape
                // glyph (cursor font), e.g. 152=xterm/I-beam, 68=left_ptr. Record
                // cid -> shape so a window's CWCursor can drive the host cursor.
                uint32_t cid = rd32(req + 4);
                uint16_t sourceChar = rd16(req + 16);
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                srv.cursorShapes[cid] = sourceChar;
            }
            break;
        }
        case X_CreateCursor:
        case X_FreeCursor:
        case X_RecolorCursor:
        case X_NoOperation:
            // No reply expected (or drawing handled later in Phase 2b/2c).
            break;

        case X_GetSelectionOwner: {
            // Return the recorded owner for this selection atom. wine's clipboard
            // manager SetSelectionOwner's CLIPBOARD/PRIMARY then polls here to
            // confirm it won ownership; without an owner registry we'd always
            // answer None and wine would re-poll forever (the boot wedge).
            uint32_t selection = rd32(req + 4);
            uint32_t owner = 0;
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                auto it = srv.selectionOwners.find(selection);
                if (it != srv.selectionOwners.end()) owner = it->second;
            }
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            memcpy(r + 8, &owner, 4);          // owner window (or None=0)
            writeToClient(r, sizeof(r));
            break;
        }
        case X_SetSelectionOwner: {
            // SetSelectionOwner(owner, selection, time): record owner so the
            // subsequent GetSelectionOwner poll sees it and stops spinning. No
            // reply (this is a reply-less request).
            uint32_t owner     = rd32(req + 4);
            uint32_t selection = rd32(req + 8);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            if (owner) srv.selectionOwners[selection] = owner;
            else       srv.selectionOwners.erase(selection);
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
            XWireWindow w;
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
            r[1] = 1;                          // revert-to: PointerRoot
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            // Report the presented window as focused (not root) so wine treats
            // the app window as active and processes keyboard input. Falls back
            // to root when nothing is presented yet.
            uint32_t focus = rootWindow;
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                if (srv.presentWindow) focus = srv.presentWindow;
            }
            memcpy(r + 8, &focus, 4);          // focus window
            writeToClient(r, sizeof(r));
            break;
        }
        // --- Pointer/focus/grab requests issued by wine's click handler ---
        // (X11DRV_ButtonPress -> grab/focus/query). Before these were handled
        // they fell into default: and the reply-expecting ones (GrabPointer,
        // QueryPointer, TranslateCoords, ...) sent NO reply, so libX11 blocked
        // forever inside the click handler on wine's GUI thread — freezing the
        // message pump, which stopped keystroke processing AND the caret timer.
        case X_GrabPointer:
        case X_GrabKeyboard: {
            // Always grant the grab. Reply: r[1]=status (GrabSuccess=0).
            uint8_t r[32] = {0};
            r[0] = 1;
            r[1] = 0;                          // GrabSuccess
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            writeToClient(r, sizeof(r));
            break;
        }
        case X_QueryPointer: {
            // Report the pointer over the presented window at its last position
            // with the current button/modifier mask. Without a reply wine blocks.
            uint32_t child = 0;
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                child = srv.presentWindow;
            }
            int px = 0, py = 0; uint32_t mod = 0;
            if (g_xwirePresentSink) g_xwirePresentSink->lastPointer(px, py, mod);
            uint8_t r[32] = {0};
            r[0] = 1;
            r[1] = 1;                          // same-screen = True
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            memcpy(r + 8, &rootWindow, 4);      // root
            memcpy(r + 12, &child, 4);          // child (window under pointer)
            int16_t x = (int16_t)px, y = (int16_t)py;
            memcpy(r + 16, &x, 2);              // root-x
            memcpy(r + 18, &y, 2);              // root-y
            memcpy(r + 20, &x, 2);              // win-x
            memcpy(r + 22, &y, 2);              // win-y
            uint16_t state = (uint16_t)(mod | buttonState);
            memcpy(r + 24, &state, 2);          // button+modifier mask
            writeToClient(r, sizeof(r));
            break;
        }
        case X_TranslateCoords: {
            // Single-window model: window and root share a coordinate space, so
            // pass the source coords through unchanged. dst-window = req src-win.
            int16_t sx = (int16_t)rd16(req + 12);
            int16_t sy = (int16_t)rd16(req + 14);
            uint32_t child = 0;
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                child = srv.presentWindow;
            }
            uint8_t r[32] = {0};
            r[0] = 1;
            r[1] = 1;                          // same-screen = True
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            memcpy(r + 8, &child, 4);           // child
            memcpy(r + 12, &sx, 2);             // dst-x
            memcpy(r + 14, &sy, 2);             // dst-y
            writeToClient(r, sizeof(r));
            break;
        }
        case X_GetMotionEvents: {
            // Report no buffered motion history (nevents=0).
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            // reply-length @4 = 0; nevents @8 = 0
            writeToClient(r, sizeof(r));
            break;
        }
        case X_GetPointerMapping: {
            // Identity 3-button map {1,2,3}. Variable-length reply: r[1]=length,
            // reply-length = number of 4-byte units holding the map (1 unit holds
            // 3 bytes + 1 pad). Mirror X_GetModifierMapping's body layout.
            uint8_t r[36] = {0};
            r[0] = 1;
            r[1] = 3;                          // map length (buttons)
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint32_t replyLen = 1;             // 1 four-byte unit follows the 32-byte header
            memcpy(r + 4, &replyLen, 4);
            r[32] = 1; r[33] = 2; r[34] = 3;   // identity map
            writeToClient(r, sizeof(r));
            break;
        }
        case X_SetInputFocus: {
            // Re-activate wine's input queue for the focused top-level when the
            // focus window actually changes (a click can move focus between the
            // frame and a child). Only send FocusIn on change to avoid thrash.
            uint32_t focusWin = rd32(req + 4);
            if (focusWin && focusWin != lastFocus) {
                bool known = false;
                {
                    XWireServer& srv = XWireServer::instance();
                    std::lock_guard<std::mutex> lk(srv.regMutex);
                    known = srv.windows.count(focusWin) != 0;
                }
                if (known) {
                    sendFocusIn(focusWin);
                    lastFocus = focusWin;
                }
            }
            break;                             // no reply
        }
        // Reply-less grab/ungrab/pointer requests: accept and no-op so libX11's
        // stream stays in sync. (X_WarpPointer cursor warp is a no-op for now.)
        case X_SendEvent:
        case X_UngrabPointer:
        case X_ChangeActivePointerGrab:
        case X_UngrabKeyboard:
        case X_AllowEvents:
        case X_GrabServer:
        case X_UngrabServer:
        case X_WarpPointer:
            break;
        case X_QueryExtension: {
            // Most extensions are reported absent (MIT-SHM, XKB, ...) so libX11
            // uses the plain core path (non-SHM PutImage). EXCEPTION: GLX. wine's
            // winex11.drv calls glXQueryExtension(), which under libX11 first does
            // XQueryExtension("GLX") over the wire; if we say "absent" wine prints
            // "GLX extension is missing, disabling OpenGL" and never loads our
            // libGL. So we advertise GLX present with a major opcode. Actual GL
            // rendering is DIRECT (our guest libGL.so.1 traps straight to the host
            // — see source/opengl/gl64bridge), so no GLX rendering rides this wire;
            // we only need the existence handshake to pass.
            uint16_t nameLen = rd16(req + 4);
            std::string extName((const char*)(req + 8), nameLen);
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            if (extName == "GLX") {
                r[8] = 1;                      // present = true
                r[9] = GLX_MAJOR_OPCODE;       // major-opcode
                r[10] = GLX_FIRST_EVENT;       // first-event
                r[11] = 0;                     // first-error
            } else {
                r[8] = 0;                      // present = false
                r[9] = 0;
                r[10] = 0;
                r[11] = 0;
            }
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
        case GLX_MAJOR_OPCODE: {
            // GLX requests over the wire. Our guest libGL renders directly (host
            // trap), so winex11 issues almost no GLX wire traffic — but libX11's
            // GLX glue may still send a couple of reply-expecting requests during
            // init. Answer them minimally so wine never blocks waiting on a reply.
            uint8_t minor = req[1];
            switch (minor) {
                case X_GLXQueryVersion: {
                    // reply: major(4)=1, minor(4)=4 in the reply body
                    uint8_t r[32] = {0};
                    r[0] = 1;
                    r[2] = (uint8_t)(sequence & 0xff);
                    r[3] = (uint8_t)(sequence >> 8);
                    uint32_t maj = 1, min = 4;
                    memcpy(r + 8, &maj, 4);
                    memcpy(r + 12, &min, 4);
                    writeToClient(r, sizeof(r));
                    break;
                }
                case X_GLXClientInfo:
                    // no reply expected (client->server info)
                    break;
                default: {
                    // Any other reply-expecting GLX request: send an empty 32-byte
                    // reply so a synchronous client doesn't hang. (Requests with no
                    // reply will just see this as the next reply for a later seq;
                    // winex11's direct path doesn't depend on these.)
                    if (getenv("BW64_XWIRE")) {
                        klog_fmt("XWire: GLX minor=%d (stub-reply) seq=%d", (int)minor, (int)sequence);
                    }
                    break;
                }
            }
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

        // ---- Core fonts: reply with our single builtin 5x7 monospace font's
        // metrics so libX11 builds a usable XFontStruct and never blocks. These
        // are REPLY-expecting; falling into default: would hang the client.
        case X_QueryFont: {
            // 60-byte fixed reply, 0 properties, 0 per-char infos. all-chars-exist
            // + populated min/max bounds => libX11 needs no per-char data.
            uint8_t r[60] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint32_t replyLen = 7;              // 7 + 2*nProps(0) + 3*nChars(0)
            memcpy(r + 4, &replyLen, 4);
            auto putCharInfo = [](uint8_t* p, int16_t cw, int16_t asc, int16_t desc) {
                int16_t lsb = 0, rsb = cw;
                memcpy(p + 0, &lsb, 2); memcpy(p + 2, &rsb, 2);
                memcpy(p + 4, &cw, 2);  memcpy(p + 6, &asc, 2);
                memcpy(p + 8, &desc, 2);        // attributes @10 = 0
            };
            putCharInfo(r + 8,  xwirefont::CELL_W, 6, 1);   // min-bounds
            putCharInfo(r + 24, xwirefont::CELL_W, 6, 1);   // max-bounds
            uint16_t minCh = 32, maxCh = 126, defCh = 32, nProps = 0;
            memcpy(r + 40, &minCh, 2);
            memcpy(r + 42, &maxCh, 2);
            memcpy(r + 44, &defCh, 2);
            memcpy(r + 46, &nProps, 2);
            r[48] = 0;                          // draw-direction LeftToRight
            r[51] = 1;                          // all-chars-exist
            int16_t fa = 6, fd = 1;
            memcpy(r + 52, &fa, 2);
            memcpy(r + 54, &fd, 2);
            // nCharInfos @56 = 0
            writeToClient(r, sizeof(r));
            break;
        }
        case X_QueryTextExtents: {
            // req[1] bit0 = odd-length flag; CHAR2B string from @8 to padded end.
            uint32_t bodyBytes = (len > 8) ? (len - 8) : 0;
            uint32_t nChars = bodyBytes / 2;
            if ((req[1] & 1) && nChars) nChars--;   // odd-length padded one CHAR2B
            uint8_t r[32] = {0};
            r[0] = 1;
            r[1] = 0;                          // draw-direction LeftToRight
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            int16_t fa = 6, fd = 1;
            memcpy(r + 8,  &fa, 2);            // font-ascent
            memcpy(r + 10, &fd, 2);            // font-descent
            memcpy(r + 12, &fa, 2);            // overall-ascent
            memcpy(r + 14, &fd, 2);            // overall-descent
            int32_t width = (int32_t)(nChars * xwirefont::CELL_W);
            memcpy(r + 16, &width, 4);         // overall-width
            int32_t left = 0, right = width;
            memcpy(r + 20, &left, 4);
            memcpy(r + 24, &right, 4);
            writeToClient(r, sizeof(r));
            break;
        }
        case X_ListFonts: {
            // Return one name ("fixed"), length-prefixed + padded to 4.
            static const char kName[] = "fixed";
            uint8_t nameLen = (uint8_t)(sizeof(kName) - 1);
            uint32_t bodyLen = 1u + nameLen;
            uint32_t pad = (4 - (bodyLen & 3)) & 3;
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint32_t replyLen = (bodyLen + pad) / 4;
            memcpy(r + 4, &replyLen, 4);
            uint16_t nNames = 1;
            memcpy(r + 8, &nNames, 2);
            writeToClient(r, sizeof(r));
            writeToClient(&nameLen, 1);
            writeToClient(kName, nameLen);
            for (uint32_t i = 0; i < pad; i++) { uint8_t z = 0; writeToClient(&z, 1); }
            break;
        }
        case X_ListFontsWithInfo: {
            // Series-of-replies request; emit only the terminating reply
            // (length-of-name byte = 0 => end of list) so libX11 sees an empty
            // list and returns instead of blocking.
            uint8_t r[60] = {0};
            r[0] = 1;
            r[1] = 0;                          // length-of-name 0 = last reply
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint32_t replyLen = 7;
            memcpy(r + 4, &replyLen, 4);
            writeToClient(r, sizeof(r));
            break;
        }
        case X_GetFontPath: {
            // Empty font path (reply-length 0, number-of-STRs 0).
            uint8_t r[32] = {0};
            r[0] = 1;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            writeToClient(r, sizeof(r));
            break;
        }

        // ---- Core text drawing (reply-less). Draw into the window's text
        // overlay so a later PutImage band can't erase the glyphs.
        case X_ImageText8:
        case X_ImageText16: {
            bool wide = (opcode == X_ImageText16);
            uint8_t n = req[1];
            uint32_t need = 16u + (wide ? 2u * n : n);
            if (len < need) break;             // truncated; bail safely
            uint32_t drawable = rd32(req + 4);
            uint32_t gc       = rd32(req + 8);
            int16_t  x = (int16_t)rd16(req + 12);
            int16_t  y = (int16_t)rd16(req + 14);
            std::string s; s.reserve(n);
            for (uint32_t i = 0; i < n; i++) {
                if (!wide) s.push_back((char)req[16 + i]);
                else {
                    uint8_t hi = req[16 + 2*i], lo = req[16 + 2*i + 1];
                    s.push_back(hi ? '.' : (char)lo);
                }
            }
            blitText(drawable, gc, x, y, s, /*imageText=*/true);
            break;
        }
        case X_PolyText8:
        case X_PolyText16: {
            bool wide = (opcode == X_PolyText16);
            uint32_t drawable = rd32(req + 4);
            uint32_t gc       = rd32(req + 8);
            int16_t  penX = (int16_t)rd16(req + 12);
            int16_t  penY = (int16_t)rd16(req + 14);
            // Walk the text-item list at @16. Each item: [len][delta][len bytes],
            // or a font-shift item (len==255 + 4 font-id bytes), or pad (len==0).
            std::vector<std::pair<int16_t, std::string>> items;  // (drawX, text)
            uint32_t p = 16;
            while (p + 1 < len) {
                uint8_t lenByte = req[p];
                if (lenByte == 0) break;                  // pad / end of items
                if (lenByte == 255) { p += 1 + 4; continue; }  // font shift: skip
                if (p + 2 > len) break;
                int8_t delta = (int8_t)req[p + 1];
                uint32_t strBytes = wide ? (uint32_t)lenByte * 2 : lenByte;
                if (p + 2 + strBytes > len) break;        // truncated; bail
                const uint8_t* sp = req + p + 2;
                std::string s; s.reserve(lenByte);
                for (uint32_t i = 0; i < lenByte; i++) {
                    if (!wide) s.push_back((char)sp[i]);
                    else { uint8_t hi = sp[2*i], lo = sp[2*i+1]; s.push_back(hi ? '.' : (char)lo); }
                }
                penX = (int16_t)(penX + delta);
                items.emplace_back(penX, s);
                penX = (int16_t)(penX + (int)lenByte * xwirefont::CELL_W);
                p += 2 + strBytes;
            }
            blitTextItems(drawable, gc, penY, items);
            break;
        }
        default:
            // Unknown request. If it expects a reply we'd hang libX11; but most
            // unknown ones here are reply-less. Log so the discovery loop sees
            // exactly which opcode notepad needs next. BW64_XWIREDUMP adds a hex
            // dump of the request header + data1/data2 fields so an unknown
            // fixed-length request can be decoded (e.g. distinguishing a real
            // PolyText8 from something else riding opcode 65).
            if (getenv("BW64_XWIREDUMP")) {
                uint32_t n = (len < 32) ? len : 32;
                char hex[3*32 + 1]; int o = 0;
                for (uint32_t i = 0; i < n && o + 3 < (int)sizeof(hex); i++)
                    o += snprintf(hex + o, sizeof(hex) - o, "%02x ", req[i]);
                klog_fmt("XWire: unhandled opcode=%d len=%d seq=%d data1=0x%x data2=0x%x bytes[%s]",
                         (int)opcode, (int)len, (int)sequence,
                         (unsigned)rd32(req + 4), (unsigned)rd32(req + 8), hex);
            } else {
                klog_fmt("XWire: unhandled request opcode=%d len=%d (seq=%d)",
                         (int)opcode, (int)len, (int)sequence);
            }
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
        // Dispatch from a zero-padded copy, not directly out of `in`. Several
        // request handlers read fixed fields (e.g. rd32(req+4), req[16+i]) whose
        // offsets can exceed this particular request's declared reqLen — a short
        // request would otherwise read past the assembled bytes into the vector's
        // reserved-but-uninitialized tail (ASan "container-overflow") and act on
        // stale/garbage data, which cascaded into wineserver heap corruption
        // (release_object / unaligned-tcache) and GUI-client aborts. Padding to a
        // safe header floor makes every such over-read return a deterministic 0.
        reqScratch.assign(in.begin(), in.begin() + reqLen);
        // Pad with a zero guard region so any handler over-read — a fixed field
        // past a short request, or a sub-length-driven variable read on a
        // malformed request — lands on deterministic zeros inside this buffer
        // rather than past the allocation. Floor keeps tiny requests covered.
        uint32_t scratchLen = (reqLen > REQ_SCRATCH_FLOOR ? reqLen : REQ_SCRATCH_FLOOR) + REQ_SCRATCH_GUARD;
        reqScratch.resize(scratchLen, 0);
        processOneRequest(reqScratch.data(), reqLen);
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
    uint32_t pw;
    int16_t baseX = 0, baseY = 0;
    {
        std::lock_guard<std::mutex> lk(srv.regMutex);
        pw = srv.presentWindow;
        if (!pw) return;
        // Owner check: the presented (base) window's id base must match ours, so
        // only one connection drains the shared host input queue.
        if ((pw & ~clientIdMask) != clientIdBase) return;
        auto it = srv.windows.find(pw);
        if (it == srv.windows.end()) return;
        // Base window root origin. Host coords are relative to the base content
        // (presented at host 0,0), so root coords = host + base origin, and an
        // overlay at root (ox,oy) occupies host [ox-baseX, oy-baseY).
        baseX = it->second.x; baseY = it->second.y;
    }
    uint32_t presentWindow = pw;

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

        // Hit-test: route the event to the TOPMOST mapped window under the
        // cursor, not the global presentWindow. The old code always delivered to
        // presentWindow (chosen by largest area = the main window), so when a
        // modal dialog (Save As) opened as a separate, smaller top-level window,
        // clicks kept going to the main window behind it — the dialog's buttons
        // never saw the press (keyboard still worked because the dialog grabs
        // input focus, which is what carries keystrokes). Now an overlay window
        // whose host rect contains the pointer wins, and the event is translated
        // into that window's coordinates.
        uint32_t target = presentWindow;
        int16_t tgtX = baseX, tgtY = baseY;   // target window root origin
        uint32_t mask = 0;
        {
            std::lock_guard<std::mutex> lk(srv.regMutex);
            uint64_t bestSerial = 0;
            for (auto& kv : srv.windows) {
                XWireWindow& w = kv.second;
                if (kv.first == presentWindow || w.isRoot) continue;
                if (!w.mapped || !w.fbW || !w.fbH) continue;
                int hx0 = (int)w.x - baseX, hy0 = (int)w.y - baseY;   // host rect
                if (ev.x >= hx0 && ev.x < hx0 + (int)w.fbW &&
                    ev.y >= hy0 && ev.y < hy0 + (int)w.fbH &&
                    w.mapSerial >= bestSerial) {
                    bestSerial = w.mapSerial;
                    target = kv.first;
                    tgtX = w.x; tgtY = w.y;
                }
            }
            auto it = srv.windows.find(target);
            if (it != srv.windows.end()) mask = it->second.eventMask;
        }
        // Only this connection's own windows can be delivered to from here (each
        // connection writes events to its own client socket). If the topmost hit
        // belongs to another connection, that connection's own pump will service
        // it; skip here to avoid writing a foreign window id down our socket.
        if ((target & ~clientIdMask) != clientIdBase) continue;

        // event-x/y are relative to the TARGET window: host coord minus the
        // target's host origin (target root origin minus base origin).
        XWireInputEvent local = ev;
        local.x = ev.x - ((int)tgtX - (int)baseX);
        local.y = ev.y - ((int)tgtY - (int)baseY);

        if (ev.type == XWireInputEvent::EvMotion && !(mask & wantMask)) continue;
        // Announce EnterNotify when the pointer crosses into a new target window;
        // winex11 tracks the pointer window via crossing events (a click without
        // it defocuses the control / menus don't track).
        if (enteredWindow != target) {
            sendCrossing(target, true, (int16_t)local.x, (int16_t)local.y);
            enteredWindow = target;
        }
        if (getenv("BW64_XWIRE")) {
            klog_fmt("XWire input: deliver code=%d detail=%d host(%d,%d)->win(%d,%d) to win=0x%x (mask=0x%x)",
                     (int)code, (int)ev.detail, (int)ev.x, (int)ev.y,
                     (int)local.x, (int)local.y, (unsigned)target, (unsigned)mask);
        }
        sendInputEvent(code, target, local, tgtX, tgtY);
    }
}

// Emit a 32-byte X11 input event record (KeyPress/Release, Button*, Motion).
// (winX,winY) = the event window's root origin: event-x/y are window-relative
// (== host content coords), root-x/y add the origin back.
void XWireConnection::sendInputEvent(uint8_t code, uint32_t window, const XWireInputEvent& ev,
                                     int16_t winX, int16_t winY) {
    uint8_t e[32] = {0};
    e[0] = code;
    e[1] = (uint8_t)ev.detail;                 // keycode / button
    e[2] = (uint8_t)(sequence & 0xff);
    e[3] = (uint8_t)(sequence >> 8);
    // time @4 — a REAL monotonic timestamp (ms). winex11 uses event time for
    // click/double-click timing and menu tracking; a constant 0 (CurrentTime)
    // breaks menu open/close and caret placement. Mirrors the 32-bit XServer
    // which uses KSystem::getMilliesSinceStart() (source/x11/xserver.cpp:700).
    uint32_t t = (uint32_t)KSystem::getMilliesSinceStart();
    memcpy(e + 4, &t, 4);
    memcpy(e + 8,  &rootWindow, 4);             // root
    memcpy(e + 12, &window, 4);                 // event window
    // child @16 = None(0)
    int16_t ex = (int16_t)ev.x, ey = (int16_t)ev.y;   // window-relative (host content)
    int16_t rx = (int16_t)(ev.x + winX);              // absolute root coords
    int16_t ry = (int16_t)(ev.y + winY);
    memcpy(e + 20, &rx, 2);                     // root-x
    memcpy(e + 22, &ry, 2);                     // root-y
    memcpy(e + 24, &ex, 2);                     // event-x
    memcpy(e + 26, &ey, 2);                     // event-y
    // state = modifier mask + pointer-button mask. ev.state already carries the
    // modifier bits (Shift/Ctrl/...). For button events X reports the button
    // state JUST PRIOR to the event: a press has the bit clear, a release has it
    // set — matching the 32-bit buttonNotify (xwindow.cpp). Track our own button
    // mask across events so motion during a drag carries the held button too.
    uint16_t state = (uint16_t)ev.state;
    if (code == 4 /*ButtonPress*/) {
        // bit reflects state before press (still up); then remember it's down.
        state |= (uint16_t)(buttonState & ~(1u << (7 + ev.detail)));
        buttonState |= (1u << (7 + ev.detail));      // Button1Mask = 1<<8
    } else if (code == 5 /*ButtonRelease*/) {
        // bit reflects state before release (still down).
        buttonState |= (1u << (7 + ev.detail));
        state |= (uint16_t)buttonState;
        buttonState &= ~(1u << (7 + ev.detail));
    } else {
        state |= (uint16_t)buttonState;              // motion/key carry held buttons
    }
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
    std::unique_lock<std::mutex> lk(srv.regMutex);

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
    XWireWindow& win = it->second;
    if (win.isRoot) return;     // never present the root/desktop background

    // Track the window's full extent from the blit reach. winex11 tiles the
    // client area in horizontal bands, so the max (dstX+w, dstY+h) across blits
    // is the true client size even when CreateWindow gave us 0x0.
    uint16_t reachW = (uint16_t)((dstX > 0 ? dstX : 0) + w);
    uint16_t reachH = (uint16_t)((dstY > 0 ? dstY : 0) + h);
    if (reachW > win.width)  win.width = reachW;
    if (reachH > win.height) win.height = reachH;

    // Adopt the BASE (background) window — the one we composite overlays onto.
    // The first window an app draws into becomes the base; thereafter the LARGEST
    // non-override-redirect window wins (the main client area, not a tiny menu/
    // tooltip popup that also PutImages). Overlays go on top via composeAndPresent.
    if (srv.presentWindow == 0) {
        srv.presentWindow = drawable;
    } else if (drawable != srv.presentWindow && !win.overrideRedirect) {
        auto pit = srv.windows.find(srv.presentWindow);
        uint32_t curArea = (pit != srv.windows.end())
                           ? (uint32_t)pit->second.width * pit->second.height : 0;
        uint32_t newArea = (uint32_t)win.width * win.height;
        if (newArea > curArea) srv.presentWindow = drawable;
    }

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
        // Keep the text overlay aligned with fb so composeAndPresent can blit it
        // 1:1. Grow-and-preserve identically (only if it was already allocated).
        if (!win.textFb.empty()) {
            std::vector<uint8_t> tgrown((size_t)winW * winH * bpp, 0);
            uint16_t copyH = win.fbH < winH ? win.fbH : winH;
            uint16_t copyW = win.fbW < winW ? win.fbW : winW;
            for (uint16_t y = 0; y < copyH; y++) {
                memcpy(tgrown.data() + (size_t)y * winW * bpp,
                       win.textFb.data() + (size_t)y * win.fbW * bpp,
                       (size_t)copyW * bpp);
            }
            win.textFb.swap(tgrown);
        }
        win.fbW = winW;
        win.fbH = winH;
    }

    // The text overlay, when present, is always sized to exactly fbW*fbH*bpp (see
    // the grow block above and drawTextOverlay). Guard the clear below by its real
    // size so a stale/empty overlay can never be written out of bounds.
    const size_t textBytes = win.textFb.size();

    // Copy each source row into the framebuffer at (dstX, dstY), clipping to the
    // window bounds. A repaint of a region supersedes any stale core-text there,
    // so clear the overlapping rows of the text overlay too.
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
        size_t off = ((size_t)dy * winW + dx) * bpp;
        if (textBytes && off + (size_t)copyW * bpp <= textBytes)
            memset(win.textFb.data() + off, 0, (size_t)copyW * bpp);
    }

    // Done mutating the registry — drop the lock before composing (it re-locks).
    lk.unlock();
    // Recompose base + overlays whenever any window draws, so a menu/popup that
    // PutImages into its own (non-base) window still appears on the host.
    srv.composeAndPresent();
}

// ---------------------------------------------------------------------------
// X core text. Glyphs go into the window's text-overlay buffer (NOT fb), so a
// later PutImage band can't erase them; composeAndPresent blits the overlay on
// top of fb. Caller must hold srv.regMutex. `win` is a live registry entry; the
// overlay is grown to match fb and the string is drawn at baseline `y`.
// ---------------------------------------------------------------------------
namespace {
    // Draw one string into win.textFb at (x, baselineY). fg/bg are opaque ARGB.
    // imageText fills the text box with bg first; PolyText (imageText=false)
    // draws foreground glyphs only (transparent elsewhere = overlay pixel 0).
    void drawTextOverlay(XWireWindow& win, int16_t x, int16_t baselineY,
                         const std::string& s, uint32_t fg, uint32_t bg,
                         bool imageText) {
        if (s.empty() || !win.fbW || !win.fbH) return;
        const uint32_t bpp = 4;
        // Size the overlay to EXACTLY match fb's allocation, so the (W*H) index
        // space below and the 1:1 blit in composeAndPresent can never run past
        // either buffer even if win.width/height got ahead of the fb extent.
        size_t want = (size_t)win.fbW * win.fbH * bpp;
        if (win.fb.size() < want) return;        // fb not yet grown to fbW*fbH
        if (win.textFb.size() != want)
            win.textFb.assign(want, 0);
        uint32_t* tfb = reinterpret_cast<uint32_t*>(win.textFb.data());
        int W = win.fbW, H = win.fbH;
        int top = baselineY - 6;                 // ascent: glyph top from baseline
        int textW = (int)s.size() * xwirefont::CELL_W;

        if (imageText) {
            for (int row = 0; row < xwirefont::GLYPH_H; row++) {
                int py = top + row;
                if (py < 0 || py >= H) continue;
                for (int col = 0; col < textW; col++) {
                    int px = x + col;
                    if (px >= 0 && px < W) tfb[py*W + px] = bg;
                }
            }
        }
        int penX = x;
        for (char c : s) {
            uint8_t gcol[5];
            xwirefont::glyphInto(c, gcol);
            for (int col = 0; col < 5; col++)
                for (int row = 0; row < 7; row++)
                    if (gcol[col] & (1 << row)) {
                        int px = penX + col, py = top + row;
                        if (px >= 0 && px < W && py >= 0 && py < H)
                            tfb[py*W + px] = fg;
                    }
            penX += xwirefont::CELL_W;
        }
    }
}

void XWireConnection::blitText(uint32_t drawable, uint32_t gcId, int16_t x, int16_t y,
                               const std::string& chars, bool imageText) {
    if (chars.empty()) return;
    XWireServer& srv = XWireServer::instance();
    std::unique_lock<std::mutex> lk(srv.regMutex);
    uint32_t fg = 0xff000000, bg = 0xffffffff;
    auto git = srv.gcs.find(gcId);
    if (git != srv.gcs.end()) { fg = git->second.foreground; bg = git->second.background; }
    auto it = srv.windows.find(drawable);
    if (it == srv.windows.end() || it->second.isRoot) return;  // pixmap/root -> skip
    drawTextOverlay(it->second, x, y, chars, fg, bg, imageText);
    lk.unlock();
    srv.composeAndPresent();
}

void XWireConnection::blitTextItems(uint32_t drawable, uint32_t gcId, int16_t y,
                                    const std::vector<std::pair<int16_t, std::string>>& items) {
    if (items.empty()) return;
    XWireServer& srv = XWireServer::instance();
    std::unique_lock<std::mutex> lk(srv.regMutex);
    uint32_t fg = 0xff000000, bg = 0xffffffff;
    auto git = srv.gcs.find(gcId);
    if (git != srv.gcs.end()) { fg = git->second.foreground; bg = git->second.background; }
    auto it = srv.windows.find(drawable);
    if (it == srv.windows.end() || it->second.isRoot) return;  // pixmap/root -> skip
    for (const auto& item : items)
        drawTextOverlay(it->second, item.first, y, item.second, fg, bg, /*imageText=*/false);
    lk.unlock();
    srv.composeAndPresent();   // one present for the whole item list
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

void XWireConnection::sendFocusIn(uint32_t window) {
    // FocusIn (event code 9). winex11 listens for this to mark the window
    // active; without it wine never routes keystrokes to the focused control,
    // so typing appears dead even though KeyPress events are delivered.
    uint8_t e[32] = {0};
    e[0] = 9;                                  // FocusIn
    e[1] = 3;                                  // detail = NotifyNonlinear (a real
                                               // WM sends this when focus moves to
                                               // a top-level window from elsewhere
                                               // in the hierarchy; winex11's
                                               // X11DRV_FocusIn activates on it)
    e[2] = (uint8_t)(sequence & 0xff);
    e[3] = (uint8_t)(sequence >> 8);
    memcpy(e + 4, &window, 4);                 // event window
    e[8] = 0;                                  // mode = NotifyNormal
    writeToClient(e, sizeof(e));
}

void XWireConnection::sendCrossing(uint32_t window, bool enter, int16_t x, int16_t y) {
    // EnterNotify(7) / LeaveNotify(8). winex11 selects EnterWindowMask and needs
    // this to track which window the pointer is in; without it clicks defocus
    // and menus don't open. Field layout mirrors the 32-bit crossingNotify.
    uint8_t e[32] = {0};
    e[0] = enter ? 7 : 8;
    e[1] = 0;                                  // detail = NotifyAncestor
    e[2] = (uint8_t)(sequence & 0xff);
    e[3] = (uint8_t)(sequence >> 8);
    uint32_t t = (uint32_t)KSystem::getMilliesSinceStart();
    memcpy(e + 4, &t, 4);                       // time
    memcpy(e + 8,  &rootWindow, 4);             // root
    memcpy(e + 12, &window, 4);                 // event window
    // child @16 = None(0)
    memcpy(e + 20, &x, 2);                      // root-x
    memcpy(e + 22, &y, 2);                      // root-y
    memcpy(e + 24, &x, 2);                      // event-x
    memcpy(e + 26, &y, 2);                      // event-y
    // state @28 = 0; mode @30 = NotifyNormal(0)
    e[31] = 1;                                  // same-screen=True (bit0), focus=0
    writeToClient(e, sizeof(e));
}
