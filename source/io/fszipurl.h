#ifndef __FSZIPURL_H__
#define __FSZIPURL_H__

// M7 lazy rootfs: an HTTP-Range-backed "file" for minizip, so a rootfs zip can
// be MOUNTED from a URL without downloading it first. The launcher passes the
// zip as a spec string instead of a MEMFS path:
//
//   bw64url:<totalBytes>;<url1>|<size1>;<url2>|<size2>;...
//
// (multiple url|size pairs = the split .partNNN files served on Pages, treated
// as one concatenated virtual file). Reads fetch missing 512 KB blocks with a
// SYNCHRONOUS same-origin XHR — legal because guest threads run on pthread web
// workers, never the browser main thread — and cache them for the session.
//
// Only compiled to a working implementation under __EMSCRIPTEN__; elsewhere
// fsZipUrlIsSpec always returns false and FsZip uses the normal file path.

#include "boxedwine.h"

bool fsZipUrlIsSpec(BString path);

#ifdef BOXEDWINE_ZLIB
// Same prelude as fszip.cpp: an earlier (system) zlib header may have set the
// OF() macro guard differently; force the repo minizip's own definitions.
#undef OF
#define STRICTUNZIP
extern "C" {
#include "../../lib/zlib/contrib/minizip/unzip.h"
}
// Open a bw64url: spec with minizip. Returns nullptr on parse/open failure.
unzFile fsZipUrlOpen(BString spec);
#endif

#endif
