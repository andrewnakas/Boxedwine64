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
#include <cstdlib>
#include <string>
#include <algorithm>
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
        X_ConvertSelection      = 24,
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
        X_PolyPoint             = 63,
        X_PolyLine              = 64,
        X_PolySegment           = 65,
        X_PolyRectangle         = 66,
        X_FillPoly              = 69,
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
        X_FreeColormap          = 79,
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
    // Constructed inside the guest's connect() (acceptConnection), so the
    // current thread IS the connecting app's thread — record which process this
    // connection belongs to for app-switch teardown (dropAppByPid).
    KThread* thread = KThread::currentThread();
    if (thread && thread->process) {
        ownerPid = thread->process->id;
    }
    // Seed the X11 PREDEFINED atoms the clipboard path must recognize, and
    // start interned ids above the predefined range (1..68). Historically
    // interned ids started at 1, colliding with the predefined numbering —
    // harmless while the server never INTERPRETED atoms, but the selection
    // logic compares atoms by name (XA_STRING=31 arrives without ever being
    // interned), so the numbering must be authentic now.
    static const struct { uint32_t id; const char* name; } predef[] = {
        { 1, "PRIMARY" }, { 2, "SECONDARY" }, { 4, "ATOM" }, { 6, "CARDINAL" },
        { 31, "STRING" }, { 33, "WINDOW" },
    };
    for (auto& p : predef) {
        atoms[p.name] = p.id;
        atomNames[p.id] = p.name;
    }
    nextAtom = 0x100;
}

std::string XWireConnection::atomName(uint32_t atom) const {
    auto it = atomNames.find(atom);
    return it != atomNames.end() ? it->second : std::string();
}

std::string XWireConnection::selectionKey(uint32_t atom) const {
    std::string name = atomName(atom);
    if (!name.empty()) return name;
    // Unknown atom — key it uniquely to this connection so two connections'
    // unrelated unknown atoms can't alias each other.
    return "#" + std::to_string(clientIdBase) + ":" + std::to_string(atom);
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
            // Stamp the creating connection's resource-id base so the app-switch
            // adopt logic can tell the freshly spawned app's windows apart from a
            // still-running previous app's (whose base was allocated earlier).
            w.ownerClientBase = clientIdBase;
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
            // When this map adopts a NEW base (app switch), the outgoing base's
            // owner-client-base is captured here so we can hide that app's windows
            // (unmapAppWindows) + wipe the canvas AFTER dropping the registry lock.
            uint32_t outgoingOwnerBase = 0;
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
                    // Persistent-session app switch: adopt the NEW app's window as
                    // the canvas base at MAP time. (Doing this only in the PutImage
                    // path misses GL apps like glcube, which render via OpenGL/
                    // glReadPixels and never PutImage their window — so the cube
                    // stayed composited beside Notepad.) The just-assigned serial
                    // is > adoptArmSerial, so this is the new app, not the still-
                    // running previous app. One-shot.
                    // Adopt as the new canvas base ONLY if this window belongs to
                    // a connection opened AFTER the switch was armed (the freshly
                    // spawned app). The still-running previous app keeps mapping
                    // and repainting its own windows — they get fresh mapSerials
                    // too, so a serial gate alone let the OLD app re-win the base
                    // (observed: launching Snake showed Minesweeper). Its
                    // connections were all allocated before the arm, so their
                    // ownerClientBase < adoptArmClientBase and they're skipped.
                    if (srv.adoptNextWindowAsBase && !it->second.overrideRedirect &&
                        it->second.ownerClientBase >= srv.adoptArmClientBase &&
                        it->second.mapSerial > srv.adoptArmSerial) {
                        // Capture the app we're switching AWAY from (the old base's
                        // owner) before moving the base, so we can hide its windows
                        // below. Skip if the old base belongs to the same app (a
                        // multi-window app re-adopting its own window).
                        auto ob = srv.windows.find(srv.presentWindow);
                        if (ob != srv.windows.end() &&
                            ob->second.ownerClientBase != it->second.ownerClientBase) {
                            outgoingOwnerBase = ob->second.ownerClientBase;
                        }
                        srv.presentWindow = wid;
                        srv.adoptNextWindowAsBase = false;
                        klog_fmt("XWire: adopted window 0x%x (owner base 0x%x >= arm 0x%x, serial %llu) as new base (app switch, on map)",
                                 wid, it->second.ownerClientBase, srv.adoptArmClientBase,
                                 (unsigned long long)it->second.mapSerial);
                    }
                }
            }
            // App switch: hide the outgoing app's windows (compositor + hit-test
            // skip !mapped) and wipe the canvas so its last frame doesn't linger
            // until the new app paints. Done OUTSIDE the lock (unmapAppWindows
            // takes regMutex itself). The new window gets sendFocusIn below, so
            // keyboard/mouse follow to the app the user switched to.
            if (outgoingOwnerBase) {
                if (XWireServer::instance().unmapAppWindows(outgoingOwnerBase) &&
                    g_xwirePresentSink) {
                    g_xwirePresentSink->requestClear();
                }
            }
            if (found) {
                ensureWindow();
                // MapNotify FIRST: winex11 selects StructureNotifyMask and blocks
                // its message pump waiting for it to confirm the window is
                // viewable. Without it a GL app parks in GetMessage forever after
                // mapping and never reaches its first frame.
                sendMapNotify(wid);
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
            if (wasOverlay) srv.schedulePresent();
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
        case X_CreatePixmap: {
            // CreatePixmap(depth, pid, drawable, width, height):
            //   req[1]=depth, @4 pid (new pixmap id), @8 drawable (parent),
            //   @12 width, @14 height. Model it as an ARGB8888 fb so a later
            //   PutImage-into-pixmap + CopyArea-pixmap->window works (DOOM's
            //   StretchDIBits path). Zero-size pixmaps are tracked but unallocated.
            uint8_t depth = req[1];
            uint32_t pid = rd32(req + 4);
            uint16_t pw = rd16(req + 12);
            uint16_t ph = rd16(req + 14);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            XWirePixmap pm;
            pm.w = pw; pm.h = ph; pm.depth = depth;
            if (pw && ph) pm.fb.assign((size_t)pw * ph * 4, 0);
            srv.pixmaps[pid] = std::move(pm);
            // DIAG (ungated, one-shot): confirm DOOM creates pixmaps.
            static int s_cpDiag = 0;
            if (s_cpDiag < 4) { s_cpDiag++;
                klog_fmt("XWire DIAG: CreatePixmap id=0x%x %dx%d depth=%d", (int)pid, (int)pw, (int)ph, (int)depth); }
            break;
        }
        case X_FreePixmap: {
            uint32_t pid = rd32(req + 4);
            XWireServer& srv = XWireServer::instance();
            std::lock_guard<std::mutex> lk(srv.regMutex);
            srv.pixmaps.erase(pid);
            break;
        }
        case X_CopyArea: {
            // CopyArea(src-drawable, dst-drawable, gc, src-x, src-y, dst-x,
            //          dst-y, width, height):
            //   @4 src, @8 dst, @12 gc, @16 src-x, @18 src-y, @20 dst-x,
            //   @22 dst-y, @24 width, @26 height. The case we must handle is
            //   pixmap -> window (winex11's StretchDIBits/blit-via-pixmap path):
            //   copy the rect out of the pixmap fb into the destination window's
            //   fb, then present. Other combinations (win->win, win->pixmap) are
            //   not produced by the GDI apps we host, so they stay no-ops.
            uint32_t srcId = rd32(req + 4);
            uint32_t dstId = rd32(req + 8);
            int16_t srcX = (int16_t)rd16(req + 16);
            int16_t srcY = (int16_t)rd16(req + 18);
            int16_t dstX = (int16_t)rd16(req + 20);
            int16_t dstY = (int16_t)rd16(req + 22);
            uint16_t cw = rd16(req + 24);
            uint16_t ch = rd16(req + 26);
            copyAreaPixmapToWindow(srcId, dstId, srcX, srcY, dstX, dstY, cw, ch);
            break;
        }
        case X_ClearArea:
        case X_DeleteProperty:
        case X_CreateColormap:
        case X_FreeColormap:
        case X_PolyFillRectangle: {
            // PolyFillRectangle(drawable, gc, [x,y,w,h]...): filled rects in the
            // GC foreground. taskmgr/many GDI apps draw control backgrounds and
            // bar-graph bars this way. (Previously this opcode fell THROUGH into
            // X_ChangeProperty and was silently dropped — a latent no-op.)
            uint32_t drawable = rd32(req + 4);
            uint32_t gcId     = rd32(req + 8);
            std::vector<std::array<int16_t,4>> rects;   // x,y,w,h
            for (uint32_t pos = 12; pos + 8 <= len; pos += 8) {
                rects.push_back({ (int16_t)rd16(req+pos), (int16_t)rd16(req+pos+2),
                                  (int16_t)rd16(req+pos+4), (int16_t)rd16(req+pos+6) });
            }
            drawFillRectangles(drawable, gcId, rects);
            break;
        }
        case X_PolyRectangle: {
            // PolyRectangle(drawable, gc, [x,y,w,h]...): rectangle OUTLINES in
            // the GC foreground (4 line segments each). Window/control borders.
            uint32_t drawable = rd32(req + 4);
            uint32_t gcId     = rd32(req + 8);
            std::vector<std::array<int16_t,4>> rects;
            for (uint32_t pos = 12; pos + 8 <= len; pos += 8) {
                rects.push_back({ (int16_t)rd16(req+pos), (int16_t)rd16(req+pos+2),
                                  (int16_t)rd16(req+pos+4), (int16_t)rd16(req+pos+6) });
            }
            drawRectangleOutlines(drawable, gcId, rects);
            break;
        }
        case X_PolySegment: {
            // PolySegment(drawable, gc, [x1,y1,x2,y2]...): independent line
            // segments in the GC foreground. taskmgr's graph grid + traces.
            uint32_t drawable = rd32(req + 4);
            uint32_t gcId     = rd32(req + 8);
            std::vector<std::array<int16_t,4>> segs;    // x1,y1,x2,y2
            for (uint32_t pos = 12; pos + 8 <= len; pos += 8) {
                segs.push_back({ (int16_t)rd16(req+pos), (int16_t)rd16(req+pos+2),
                                 (int16_t)rd16(req+pos+4), (int16_t)rd16(req+pos+6) });
            }
            drawSegments(drawable, gcId, segs);
            break;
        }
        case X_PolyLine:
        case X_PolyPoint: {
            // PolyLine(drawable, gc, mode, [x,y]...): a connected polyline (each
            // point to the next) in the GC foreground. PolyPoint plots the points
            // themselves. coordinate-mode @1 (0=Origin absolute, 1=Previous
            // relative). We treat points as a vertex list and, for PolyLine,
            // connect consecutive vertices.
            uint32_t drawable = rd32(req + 4);
            uint32_t gcId     = rd32(req + 8);
            uint8_t  coordMode = req[1];
            std::vector<std::pair<int16_t,int16_t>> pts;
            int16_t cx = 0, cy = 0;
            for (uint32_t pos = 12; pos + 4 <= len; pos += 4) {
                int16_t px = (int16_t)rd16(req+pos), py = (int16_t)rd16(req+pos+2);
                if (coordMode == 1 && !pts.empty()) { px = (int16_t)(cx + px); py = (int16_t)(cy + py); }
                cx = px; cy = py;
                pts.push_back({px, py});
            }
            drawPolyline(drawable, gcId, pts, /*connect=*/opcode == X_PolyLine);
            break;
        }
        case X_FillPoly: {
            // FillPoly(drawable, gc, shape, coord-mode, [x,y]...): a filled
            // polygon in the GC foreground. shape @ data, coord-mode @ data+1.
            // We scanline-fill the vertex polygon (even-odd).
            uint32_t drawable = rd32(req + 4);
            uint32_t gcId     = rd32(req + 8);
            uint8_t  coordMode = (len > 13) ? req[13] : 0;
            std::vector<std::pair<int16_t,int16_t>> pts;
            int16_t cx = 0, cy = 0;
            for (uint32_t pos = 16; pos + 4 <= len; pos += 4) {
                int16_t px = (int16_t)rd16(req+pos), py = (int16_t)rd16(req+pos+2);
                if (coordMode == 1 && !pts.empty()) { px = (int16_t)(cx + px); py = (int16_t)(cy + py); }
                cx = px; cy = py;
                pts.push_back({px, py});
            }
            fillPolygon(drawable, gcId, pts);
            break;
        }
        case X_ChangeProperty: {
            // ChangeProperty(mode, window, property, type, format, data-len,
            // data). Stored keyed by (window, property NAME) so a different
            // connection can read it back (GetProperty) — that's how selection
            // data transfers. Special case: a write to the BW64_CLIP property
            // is wine's clipboard thread answering our synthesized
            // SelectionRequest (see X_SetSelectionOwner) — harvest the text
            // into the host clipboard buffer.
            if (len < 24) break;
            uint8_t mode = req[1];                  // 0=Replace 1=Prepend 2=Append
            uint32_t window   = rd32(req + 4);
            uint32_t property = rd32(req + 8);
            uint32_t type     = rd32(req + 12);
            uint8_t format    = req[16];
            uint32_t units    = rd32(req + 20);
            uint32_t bytes = units * (format == 32 ? 4 : format == 16 ? 2 : 1);
            if ((uint64_t)24 + bytes > len) bytes = (len > 24) ? (uint32_t)(len - 24) : 0;
            std::string propName = selectionKey(property);
            std::string typeName = atomName(type);
            XWireServer& srv = XWireServer::instance();
            bool harvested = false;
            {
                std::lock_guard<std::mutex> lk(srv.regMutex);
                XWireServer::XWireProp& p = srv.windowProps[{window, propName}];
                if (mode == 2 && p.format == format) {          // Append
                    p.data.insert(p.data.end(), req + 24, req + 24 + bytes);
                } else if (mode == 1 && p.format == format) {   // Prepend
                    p.data.insert(p.data.begin(), req + 24, req + 24 + bytes);
                } else {                                        // Replace
                    p.type = typeName;
                    p.format = format;
                    p.data.assign(req + 24, req + 24 + bytes);
                }
                if (propName == "BW64_CLIP" && format == 8) {
                    srv.clipboardText.assign((const char*)p.data.data(), p.data.size());
                    harvested = true;
                }
            }
            if (harvested) {
                klog_fmt("XWire: harvested guest clipboard (%d bytes)", (int)bytes);
            }
            break;
        }
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
            // Return the recorded owner for this selection. wine's clipboard
            // manager SetSelectionOwner's CLIPBOARD/PRIMARY then polls here to
            // confirm it won ownership; without an owner registry we'd always
            // answer None and wine would re-poll forever (the boot wedge).
            // When NO guest owns it but the HOST has clipboard text, report the
            // root window as a stand-in owner — wine then takes the import path
            // (ConvertSelection) on paste instead of deciding the clipboard is
            // empty.
            uint32_t selection = rd32(req + 4);
            std::string sel = selectionKey(selection);
            uint32_t owner = 0;
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                auto it = srv.selectionOwners.find(sel);
                if (it != srv.selectionOwners.end()) {
                    owner = it->second;
                } else if ((sel == "CLIPBOARD" || sel == "PRIMARY") &&
                           !srv.clipboardText.empty()) {
                    owner = rootWindow;        // host-owned stand-in
                    klog_fmt("XWire: GetSelectionOwner(%s) -> host sentinel", sel.c_str());
                }
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
            // SetSelectionOwner(owner, selection, time): record owner (by
            // selection NAME — ids are per-connection) so the subsequent
            // GetSelectionOwner poll sees it. No reply.
            //
            // CLIPBOARD harvest: a guest app just copied. There is no other X
            // client to ask for the data, and the browser can't pull it on
            // demand (clipboard writes need a user gesture with fresh data) —
            // so harvest EAGERLY: synthesize the SelectionRequest a paste-ing
            // client would have sent. wine's clipboard thread (this very
            // connection) answers with ChangeProperty(requestor, BW64_CLIP,
            // UTF8_STRING, <text>) + SendEvent(SelectionNotify); the
            // ChangeProperty handler below spots BW64_CLIP and copies the text
            // into XWireServer::clipboardText for the JS side to read.
            uint32_t owner     = rd32(req + 4);
            uint32_t selection = rd32(req + 8);
            std::string sel = selectionKey(selection);
            bool harvest = false;
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                if (owner) srv.selectionOwners[sel] = owner;
                else       srv.selectionOwners.erase(sel);
                harvest = owner && sel == "CLIPBOARD";
            }
            klog_fmt("XWire: SetSelectionOwner %s owner=0x%x", sel.c_str(), owner);
            if (harvest) {
                uint8_t e[32] = {0};
                e[0] = 30;                                  // SelectionRequest
                e[2] = (uint8_t)(sequence & 0xff);
                e[3] = (uint8_t)(sequence >> 8);
                uint32_t t = (uint32_t)KSystem::getMilliesSinceStart();
                memcpy(e + 4, &t, 4);                       // time
                memcpy(e + 8, &owner, 4);                   // owner
                // Requestor: a never-allocated id at the top of THIS client's
                // own resource range (clients allocate from the bottom), so the
                // owner's ChangeProperty lands on a window id we can recognize
                // without colliding with a real window.
                uint32_t requestor = clientIdBase | clientIdMask;
                memcpy(e + 12, &requestor, 4);
                memcpy(e + 16, &selection, 4);              // selection (their id)
                uint32_t target = internAtom("UTF8_STRING", false);
                memcpy(e + 20, &target, 4);
                uint32_t property = internAtom("BW64_CLIP", false);
                memcpy(e + 24, &property, 4);
                writeToClient(e, sizeof(e));
                klog_fmt("XWire: clipboard harvest armed (owner=0x%x sel=%s)", owner, sel.c_str());
            }
            break;
        }
        case X_ConvertSelection: {
            // A guest app is PASTING: ConvertSelection(requestor, selection,
            // target, property, time). Serve the HOST clipboard text directly
            // (the host is the only "other side" — guest-to-guest clipboard
            // never reaches X, wine handles it internally via wineserver):
            // store the data as a property on the requestor window and send
            // SelectionNotify. TARGETS gets the format list; text-ish targets
            // get UTF-8 bytes; anything else gets property=None ("can't
            // convert").
            uint32_t requestor = rd32(req + 4);
            uint32_t selection = rd32(req + 8);
            uint32_t target    = rd32(req + 12);
            uint32_t property  = rd32(req + 16);
            uint32_t time      = rd32(req + 20);
            std::string tgtName = atomName(target);
            if (!property) property = target;   // ICCCM: obsolete clients
            std::string propName = atomName(property);
            std::string text;
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                text = srv.clipboardText;
            }
            bool ok = false;
            if (tgtName == "TARGETS") {
                // Reply with the supported conversion targets (type ATOM,
                // format 32) — ids interned in THIS connection's numbering.
                uint32_t list[4] = {
                    internAtom("TARGETS", false),
                    internAtom("UTF8_STRING", false),
                    internAtom("STRING", false),
                    internAtom("TEXT", false),
                };
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                XWireServer::XWireProp& p = srv.windowProps[{requestor, propName}];
                p.type = "ATOM";
                p.format = 32;
                p.data.assign((uint8_t*)list, (uint8_t*)list + sizeof(list));
                ok = true;
            } else if (tgtName == "UTF8_STRING" || tgtName == "STRING" ||
                       tgtName == "TEXT" || tgtName == "COMPOUND_TEXT") {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                XWireServer::XWireProp& p = srv.windowProps[{requestor, propName}];
                p.type = (tgtName == "STRING") ? "STRING" : "UTF8_STRING";
                p.format = 8;
                p.data.assign(text.begin(), text.end());
                ok = true;
            }
            uint8_t e[32] = {0};
            e[0] = 31;                                      // SelectionNotify
            e[2] = (uint8_t)(sequence & 0xff);
            e[3] = (uint8_t)(sequence >> 8);
            memcpy(e + 4, &time, 4);
            memcpy(e + 8, &requestor, 4);
            memcpy(e + 12, &selection, 4);
            memcpy(e + 16, &target, 4);
            uint32_t prop = ok ? property : 0;              // None = no conversion
            memcpy(e + 20, &prop, 4);
            writeToClient(e, sizeof(e));
            klog_fmt("XWire: ConvertSelection sel=%s target=%s prop=%s -> %s (%d bytes staged)",
                     selectionKey(selection).c_str(), tgtName.c_str(), propName.c_str(),
                     ok ? "served" : "none", (int)text.size());
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
            // GetProperty(delete, window, property, type, long-offset,
            // long-length). Serves the windowProps store (selection data
            // transfer); unknown properties still report "does not exist".
            uint8_t del       = req[1];
            uint32_t window   = rd32(req + 4);
            uint32_t property = rd32(req + 8);
            uint32_t offset   = rd32(req + 16) * 4;     // long-offset in 32-bit units
            uint32_t maxBytes = rd32(req + 20) * 4;     // long-length in 32-bit units
            std::string propName = selectionKey(property);

            XWireServer::XWireProp prop;
            bool found = false;
            uint32_t bytesAfter = 0;
            std::vector<uint8_t> slice;
            {
                XWireServer& srv = XWireServer::instance();
                std::lock_guard<std::mutex> lk(srv.regMutex);
                auto it = srv.windowProps.find({window, propName});
                if (it != srv.windowProps.end()) {
                    found = true;
                    prop = it->second;
                    if (offset > prop.data.size()) offset = (uint32_t)prop.data.size();
                    uint32_t avail = (uint32_t)prop.data.size() - offset;
                    uint32_t take = avail < maxBytes ? avail : maxBytes;
                    slice.assign(prop.data.begin() + offset,
                                 prop.data.begin() + offset + take);
                    bytesAfter = avail - take;
                    if (del && bytesAfter == 0) srv.windowProps.erase(it);
                }
            }
            if (!found) {
                uint8_t r[32] = {0};
                r[0] = 1;
                r[2] = (uint8_t)(sequence & 0xff);
                r[3] = (uint8_t)(sequence >> 8);
                writeToClient(r, sizeof(r));
                break;
            }
            klog_fmt("XWire: GetProperty win=0x%x %s -> %d bytes (type=%s, after=%d)",
                     window, propName.c_str(), (int)slice.size(), prop.type.c_str(),
                     (int)bytesAfter);
            // The reply's type atom must be in THIS connection's numbering.
            uint32_t typeAtom = prop.type.empty() ? 0 : internAtom(prop.type, false);
            uint32_t unitBytes = prop.format == 32 ? 4 : prop.format == 16 ? 2 : 1;
            uint32_t valueUnits = unitBytes ? (uint32_t)slice.size() / unitBytes : 0;
            uint32_t padded = ((uint32_t)slice.size() + 3) & ~3u;
            std::vector<uint8_t> r(32 + padded, 0);
            r[0] = 1;
            r[1] = prop.format;
            r[2] = (uint8_t)(sequence & 0xff);
            r[3] = (uint8_t)(sequence >> 8);
            uint32_t replyLen = padded / 4;
            memcpy(r.data() + 4, &replyLen, 4);
            memcpy(r.data() + 8, &typeAtom, 4);
            memcpy(r.data() + 12, &bytesAfter, 4);
            memcpy(r.data() + 16, &valueUnits, 4);
            if (!slice.empty()) memcpy(r.data() + 32, slice.data(), slice.size());
            writeToClient(r.data(), (uint32_t)r.size());
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
            if (getenv("BW64_XWIRE"))
                klog_fmt("XWire: GLX minor=%d seq=%d reqlen=%d", (int)minor, (int)sequence,
                         (int)(((uint32_t)req[2] | ((uint32_t)req[3] << 8))));
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

        // KEYBOARD events follow the FOCUS window (the presented app), not the
        // pointer. POINTER events (button/motion) hit-test to the topmost mapped
        // window under the cursor. Mixing the two was the Tetris "no input" bug:
        // a keyboard-only game got its keys hit-tested to whatever window was
        // under the (possibly elsewhere) cursor — often nothing or the wrong
        // window — so KeyPress never reached the game. X semantics: keys go to the
        // focus window; only pointer events use the cursor position. (The pointer
        // hit-test still routes a modal dialog's clicks to the dialog, the case it
        // was added for; keyboard already worked there because the dialog grabs
        // focus, which is exactly what we now honor for keys.)
        const bool isKey = (ev.type == XWireInputEvent::EvKeyDown ||
                            ev.type == XWireInputEvent::EvKeyUp);
        uint32_t target = presentWindow;
        int16_t tgtX = baseX, tgtY = baseY;   // target window root origin
        uint32_t mask = 0;
        {
            std::lock_guard<std::mutex> lk(srv.regMutex);
            if (!isKey) {
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
        // Not a window — it may be an off-screen PIXMAP. winex11 renders some GDI
        // ops (StretchDIBits, used by DOOM every frame) by PutImage-ing into a
        // pixmap and then CopyArea-ing the pixmap onto the window. Store the bits
        // into the pixmap fb here so the later CopyArea (copyAreaPixmapToWindow)
        // can blit them to the window. Only ZPixmap (format 2) at depth 24/32 is
        // the BGRX byte order we present 1:1.
        auto pit = srv.pixmaps.find(drawable);
        // DIAG (ungated, one-shot): is DOOM PutImaging into a pixmap at all?
        static int s_pmDiag = 0;
        if (s_pmDiag < 4) {
            s_pmDiag++;
            klog_fmt("XWire DIAG: PutImage to non-window drawable=0x%x %dx%d fmt=%d depth=%d isPixmap=%d",
                     (int)drawable, (int)w, (int)h, (int)format, (int)depth,
                     (int)(pit != srv.pixmaps.end()));
        }
        if (pit != srv.pixmaps.end() && format == 2 && (depth == 24 || depth == 32)) {
            XWirePixmap& pm = pit->second;
            // Grow the pixmap fb if this blit reaches past its current extent
            // (winex11 may create a 0x0 pixmap then size it via the first blit).
            uint16_t needW = (uint16_t)((dstX > 0 ? dstX : 0) + w);
            uint16_t needH = (uint16_t)((dstY > 0 ? dstY : 0) + h);
            if (needW > pm.w || needH > pm.h) {
                uint16_t nw = needW > pm.w ? needW : pm.w;
                uint16_t nh = needH > pm.h ? needH : pm.h;
                std::vector<uint8_t> grown((size_t)nw * nh * 4, 0);
                for (uint16_t y = 0; y < pm.h; y++)
                    if (!pm.fb.empty())
                        memcpy(grown.data() + (size_t)y * nw * 4,
                               pm.fb.data() + (size_t)y * pm.w * 4,
                               (size_t)pm.w * 4);
                pm.fb.swap(grown); pm.w = nw; pm.h = nh;
            }
            // Copy each source row (ZPixmap is already BGRX == our fb order) into
            // the pixmap fb at (dstX,dstY), clipping to the pixmap bounds.
            const uint32_t srcPitch = (uint32_t)w * 4;
            for (uint16_t row = 0; row < h; row++) {
                int py = dstY + row;
                if (py < 0 || py >= pm.h) continue;
                int px = dstX; uint16_t copyW = w; uint32_t srcOff = 0;
                if (px < 0) { srcOff = (uint32_t)(-px) * 4; copyW = (uint16_t)(copyW + px); px = 0; }
                if (px >= pm.w || copyW == 0) continue;
                if (px + copyW > pm.w) copyW = (uint16_t)(pm.w - px);
                if ((size_t)row * srcPitch + srcOff + (size_t)copyW * 4 > dataBytes) break;
                memcpy(pm.fb.data() + ((size_t)py * pm.w + px) * 4,
                       data + (size_t)row * srcPitch + srcOff, (size_t)copyW * 4);
            }
        }
        return;
    }
    XWireWindow& win = it->second;
    if (win.isRoot) return;     // never present the root/desktop background

    // DIAG (ungated, one-shot): is DOOM PutImaging directly to its WINDOW?
    static int s_winPiDiag = 0;
    if (s_winPiDiag < 4) { s_winPiDiag++;
        klog_fmt("XWire DIAG: PutImage to WINDOW=0x%x %dx%d at(%d,%d)", (int)drawable, (int)w, (int)h, (int)dstX, (int)dstY); }

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
    // Capture the app being switched away from (old base owner) when this blit
    // adopts a new base, so its windows can be hidden + the canvas wiped after
    // the lock drops. 0 = no switch happened in this call.
    uint32_t outgoingOwnerBase = 0;
    if (srv.adoptNextWindowAsBase && !win.overrideRedirect &&
        win.ownerClientBase >= srv.adoptArmClientBase &&
        win.mapSerial > srv.adoptArmSerial) {
        // Persistent-session app switch: the NEW app's first real window (created
        // by a connection opened after the switch was armed, so ownerClientBase is
        // at/above the arm watermark — NOT the still-running previous app, whose
        // connections were all allocated earlier) forcibly becomes the base,
        // regardless of size, so the canvas switches to the newly launched app
        // without killing the previous one. One-shot.
        auto ob = srv.windows.find(srv.presentWindow);
        if (ob != srv.windows.end() &&
            ob->second.ownerClientBase != win.ownerClientBase) {
            outgoingOwnerBase = ob->second.ownerClientBase;
        }
        srv.presentWindow = drawable;
        srv.adoptNextWindowAsBase = false;
        // This is the PutImage path — only GDI apps reach it (GL apps render via
        // the gl64 bridge and never PutImage their window). So a GDI app is now the
        // base: drop any foreground-GL claim so a still-running background glcube
        // can't paint over it.
        srv.glPresentDrawable = 0;
        klog_fmt("XWire: adopted window 0x%x (serial %llu) as new base (app switch)",
                 drawable, (unsigned long long)win.mapSerial);
    } else if (srv.presentWindow == 0) {
        srv.presentWindow = drawable;
        srv.glPresentDrawable = 0;  // GDI app taking the base (PutImage path)
    } else if (drawable != srv.presentWindow && !win.overrideRedirect && win.mapped) {
        // Largest-window re-election. The `mapped` gate matters: a window hidden
        // host-side on app switch (unmapAppWindows) belongs to the OUTGOING app —
        // if it keeps repainting (notepad's caret blink) and is bigger than the
        // new app's window, it would steal back the base, putting the old app
        // back on screen AND taking the keyboard with it (keys follow
        // presentWindow). Observed live as: switch notepad->DOOM, DOOM renders
        // but notepad stays behind it and eats all input.
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
    // App switch (PutImage path): hide the outgoing app's windows + wipe the
    // canvas so its last frame doesn't linger behind the new app. unmapAppWindows
    // re-locks regMutex, so this must run after the unlock above.
    if (outgoingOwnerBase) {
        if (srv.unmapAppWindows(outgoingOwnerBase) && g_xwirePresentSink) {
            g_xwirePresentSink->requestClear();
        }
    }
    // Recompose base + overlays whenever any window draws, so a menu/popup that
    // PutImages into its own (non-base) window still appears on the host.
    srv.schedulePresent();
}

void XWireConnection::copyAreaPixmapToWindow(uint32_t srcId, uint32_t dstId,
                                             int16_t srcX, int16_t srcY,
                                             int16_t dstX, int16_t dstY,
                                             uint16_t w, uint16_t h) {
    XWireServer& srv = XWireServer::instance();
    {
        std::unique_lock<std::mutex> lk(srv.regMutex);
        // DIAG (ungated, one-shot): is DOOM's CopyArea firing, and is src a pixmap
        // we modeled / dst a window we know?
        static int s_caDiag = 0;
        if (s_caDiag < 6) {
            s_caDiag++;
            klog_fmt("XWire DIAG: CopyArea src=0x%x(pm=%d) dst=0x%x(win=%d) %dx%d",
                     (int)srcId, (int)(srv.pixmaps.count(srcId)),
                     (int)dstId, (int)(srv.windows.count(dstId)), (int)w, (int)h);
        }
        auto pit = srv.pixmaps.find(srcId);
        if (pit == srv.pixmaps.end()) return;          // src not a pixmap we model
        auto wit = srv.windows.find(dstId);
        if (wit == srv.windows.end() || wit->second.isRoot) return; // dst not a window
        XWirePixmap& pm = pit->second;
        XWireWindow& win = wit->second;
        if (pm.fb.empty() || !pm.w || !pm.h) return;

        // Ensure the destination window fb is at least big enough for the blit.
        uint16_t needW = (uint16_t)((dstX > 0 ? dstX : 0) + w);
        uint16_t needH = (uint16_t)((dstY > 0 ? dstY : 0) + h);
        uint16_t winW = win.fbW > needW ? win.fbW : needW;
        uint16_t winH = win.fbH > needH ? win.fbH : needH;
        if (winW > win.width)  win.width = winW;
        if (winH > win.height) win.height = winH;
        if (win.fbW != winW || win.fbH != winH || win.fb.empty()) {
            std::vector<uint8_t> grown((size_t)winW * winH * 4, 0);
            for (uint16_t y = 0; y < win.fbH; y++)
                if (!win.fb.empty())
                    memcpy(grown.data() + (size_t)y * winW * 4,
                           win.fb.data() + (size_t)y * win.fbW * 4,
                           (size_t)win.fbW * 4);
            win.fb.swap(grown);
            win.fbW = winW; win.fbH = winH;
        }

        // Blit the rect from pixmap (srcX,srcY) -> window (dstX,dstY), clipping to
        // both. Both fbs are ARGB8888 (BGRX byte order) so it's a straight memcpy.
        for (uint16_t row = 0; row < h; row++) {
            int sy = srcY + row, dy = dstY + row;
            if (sy < 0 || sy >= pm.h || dy < 0 || dy >= win.fbH) continue;
            int sx = srcX, dx = dstX; uint16_t copyW = w;
            if (sx < 0) { copyW = (uint16_t)(copyW + sx); dx -= sx; sx = 0; }
            if (dx < 0) { copyW = (uint16_t)(copyW + dx); sx -= dx; dx = 0; }
            if (copyW == 0 || sx >= pm.w || dx >= win.fbW) continue;
            if (sx + copyW > pm.w)     copyW = (uint16_t)(pm.w - sx);
            if (dx + copyW > win.fbW)  copyW = (uint16_t)(win.fbW - dx);
            if (copyW == 0) continue;
            memcpy(win.fb.data() + ((size_t)dy * win.fbW + dx) * 4,
                   pm.fb.data()  + ((size_t)sy * pm.w   + sx) * 4,
                   (size_t)copyW * 4);
        }

        // First content into this window: if no base is presented yet (or this is
        // the app being switched to), adopt it so the present sink shows it. The
        // PutImage path normally does this, but DOOM only ever CopyAreas, so its
        // adoption must happen here. Reuse the same gate as the PutImage path.
        if (srv.adoptNextWindowAsBase && !win.overrideRedirect &&
            win.ownerClientBase >= srv.adoptArmClientBase &&
            win.mapSerial > srv.adoptArmSerial) {
            uint32_t outgoingOwnerBase = 0;
            auto ob = srv.windows.find(srv.presentWindow);
            if (ob != srv.windows.end() &&
                ob->second.ownerClientBase != win.ownerClientBase)
                outgoingOwnerBase = ob->second.ownerClientBase;
            srv.presentWindow = dstId;
            srv.adoptNextWindowAsBase = false;
            srv.glPresentDrawable = 0;
            klog_fmt("XWire: adopted window 0x%x as new base (app switch, CopyArea)", dstId);
            lk.unlock();
            if (outgoingOwnerBase && srv.unmapAppWindows(outgoingOwnerBase) &&
                g_xwirePresentSink) {
                g_xwirePresentSink->requestClear();
            }
            srv.schedulePresent();
            return;
        } else if (srv.presentWindow == 0) {
            srv.presentWindow = dstId;
            srv.glPresentDrawable = 0;
        }
    }
    srv.schedulePresent();
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
    srv.schedulePresent();
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
    srv.schedulePresent();   // one present for the whole item list
}

// ---------------------------------------------------------------------------
// X core drawing primitives (M14). All draw opaque GC-foreground pixels into a
// window's ARGB framebuffer. Helpers below operate on a live XWireWindow under
// the caller's regMutex; the public methods lock, resolve the GC, and present.
// ---------------------------------------------------------------------------
namespace {
    // Plot one pixel into win.fb at (x,y), clipped. fb is ARGB8888 row-major.
    inline void plotPixel(XWireWindow& win, int x, int y, uint32_t argb) {
        if (x < 0 || y < 0 || x >= win.fbW || y >= win.fbH) return;
        if (win.fb.size() < (size_t)win.fbW * win.fbH * 4) return;
        ((uint32_t*)win.fb.data())[(size_t)y * win.fbW + x] = argb;
    }
    // Bresenham line from (x0,y0) to (x1,y1).
    void drawLine(XWireWindow& win, int x0, int y0, int x1, int y1, uint32_t argb) {
        int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
        int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
            plotPixel(win, x0, y0, argb);
            if (x0 == x1 && y0 == y1) break;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    // True if this drawable is a paintable (non-root) window; resolves fg.
    XWireWindow* paintTarget(XWireServer& srv, uint32_t drawable) {
        auto it = srv.windows.find(drawable);
        if (it == srv.windows.end() || it->second.isRoot) return nullptr;
        // The fb must be sized (a PutImage/CopyArea created it). If the app
        // draws primitives before any blit, fb is empty and there is nothing to
        // paint onto yet — skip (rare; the first Expose drives a blit first).
        if (it->second.fb.empty() || !it->second.fbW || !it->second.fbH) return nullptr;
        return &it->second;
    }
    uint32_t gcForeground(XWireServer& srv, uint32_t gcId) {
        auto git = srv.gcs.find(gcId);
        return (git != srv.gcs.end()) ? git->second.foreground : 0xff000000;
    }
}

void XWireConnection::drawFillRectangles(uint32_t drawable, uint32_t gcId,
                                         const std::vector<std::array<int16_t,4>>& rects) {
    if (rects.empty()) return;
    XWireServer& srv = XWireServer::instance();
    { std::lock_guard<std::mutex> lk(srv.regMutex);
      XWireWindow* w = paintTarget(srv, drawable); if (!w) return;
      uint32_t fg = gcForeground(srv, gcId);
      for (auto& r : rects)
        for (int yy = r[1]; yy < r[1] + r[3]; yy++)
            for (int xx = r[0]; xx < r[0] + r[2]; xx++)
                plotPixel(*w, xx, yy, fg);
    }
    srv.schedulePresent();
}

void XWireConnection::drawRectangleOutlines(uint32_t drawable, uint32_t gcId,
                                            const std::vector<std::array<int16_t,4>>& rects) {
    if (rects.empty()) return;
    XWireServer& srv = XWireServer::instance();
    { std::lock_guard<std::mutex> lk(srv.regMutex);
      XWireWindow* w = paintTarget(srv, drawable); if (!w) return;
      uint32_t fg = gcForeground(srv, gcId);
      for (auto& r : rects) {
        int x0 = r[0], y0 = r[1], x1 = r[0] + r[2], y1 = r[1] + r[3];
        drawLine(*w, x0, y0, x1, y0, fg);   // top
        drawLine(*w, x0, y1, x1, y1, fg);   // bottom
        drawLine(*w, x0, y0, x0, y1, fg);   // left
        drawLine(*w, x1, y0, x1, y1, fg);   // right
      }
    }
    srv.schedulePresent();
}

void XWireConnection::drawSegments(uint32_t drawable, uint32_t gcId,
                                   const std::vector<std::array<int16_t,4>>& segs) {
    if (segs.empty()) return;
    XWireServer& srv = XWireServer::instance();
    { std::lock_guard<std::mutex> lk(srv.regMutex);
      XWireWindow* w = paintTarget(srv, drawable); if (!w) return;
      uint32_t fg = gcForeground(srv, gcId);
      for (auto& s : segs) drawLine(*w, s[0], s[1], s[2], s[3], fg);
    }
    srv.schedulePresent();
}

void XWireConnection::drawPolyline(uint32_t drawable, uint32_t gcId,
                                   const std::vector<std::pair<int16_t,int16_t>>& pts, bool connect) {
    if (pts.empty()) return;
    XWireServer& srv = XWireServer::instance();
    { std::lock_guard<std::mutex> lk(srv.regMutex);
      XWireWindow* w = paintTarget(srv, drawable); if (!w) return;
      uint32_t fg = gcForeground(srv, gcId);
      if (connect && pts.size() >= 2) {
        for (size_t i = 1; i < pts.size(); i++)
            drawLine(*w, pts[i-1].first, pts[i-1].second, pts[i].first, pts[i].second, fg);
      } else {
        for (auto& p : pts) plotPixel(*w, p.first, p.second, fg);
      }
    }
    srv.schedulePresent();
}

void XWireConnection::fillPolygon(uint32_t drawable, uint32_t gcId,
                                  const std::vector<std::pair<int16_t,int16_t>>& pts) {
    if (pts.size() < 3) return;
    XWireServer& srv = XWireServer::instance();
    { std::lock_guard<std::mutex> lk(srv.regMutex);
      XWireWindow* w = paintTarget(srv, drawable); if (!w) return;
      uint32_t fg = gcForeground(srv, gcId);
      // Even-odd scanline fill. Find the y-extent, then for each scanline collect
      // edge crossings, sort, and fill between pairs.
      int minY = pts[0].second, maxY = pts[0].second;
      for (auto& p : pts) { minY = std::min<int>(minY, p.second); maxY = std::max<int>(maxY, p.second); }
      if (minY < 0) minY = 0;
      if (maxY >= w->fbH) maxY = w->fbH - 1;
      std::vector<int> xs;
      for (int y = minY; y <= maxY; y++) {
        xs.clear();
        for (size_t i = 0, n = pts.size(); i < n; i++) {
            int x0 = pts[i].first, y0 = pts[i].second;
            int x1 = pts[(i+1)%n].first, y1 = pts[(i+1)%n].second;
            if (y0 == y1) continue;
            if ((y >= y0 && y < y1) || (y >= y1 && y < y0)) {
                int xc = x0 + (int)((long)(y - y0) * (x1 - x0) / (y1 - y0));
                xs.push_back(xc);
            }
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2)
            for (int x = xs[i]; x <= xs[i+1]; x++) plotPixel(*w, x, y, fg);
      }
    }
    srv.schedulePresent();
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

void XWireConnection::sendSelectionClear(uint32_t ownerWindow, const std::string& selectionName) {
    uint8_t e[32] = {0};
    e[0] = 29;                                  // SelectionClear
    e[2] = (uint8_t)(sequence & 0xff);
    e[3] = (uint8_t)(sequence >> 8);
    uint32_t t = (uint32_t)KSystem::getMilliesSinceStart();
    memcpy(e + 4, &t, 4);
    memcpy(e + 8, &ownerWindow, 4);
    uint32_t sel = internAtom(selectionName, false);
    memcpy(e + 12, &sel, 4);
    writeToClient(e, sizeof(e));
    flushReplies();
    klog_fmt("XWire: SelectionClear -> owner 0x%x (%s)", ownerWindow, selectionName.c_str());
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

void XWireConnection::sendMapNotify(uint32_t window) {
    // MapNotify (event code 19). winex11 selects StructureNotifyMask on its
    // windows and BLOCKS in its message pump waiting for the MapNotify that
    // confirms the window became viewable (X11DRV's wait_for_withdrawn_state /
    // the map-state machine). Without it, a GL app's GetMessage/PeekMessage never
    // settles after mapping — it parks forever before its first frame (the cube
    // never draws). event-window(4) == window(8) for a top-level (we send the
    // non-redirected form). override-redirect(12)=0.
    uint8_t e[32] = {0};
    e[0] = 19;                                 // MapNotify
    e[2] = (uint8_t)(sequence & 0xff);
    e[3] = (uint8_t)(sequence >> 8);
    memcpy(e + 4, &window, 4);                 // event window
    memcpy(e + 8, &window, 4);                 // window
    e[12] = 0;                                 // override-redirect
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
