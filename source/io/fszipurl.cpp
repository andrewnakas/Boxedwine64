#include "boxedwine.h"
#include "fszipurl.h"

// See fszipurl.h — HTTP-Range-backed minizip IO for the M7 lazy rootfs.

bool fsZipUrlIsSpec(BString path) {
    return path.startsWith("bw64url:");
}

#if defined(BOXEDWINE_ZLIB) && defined(__EMSCRIPTEN__)

#include <emscripten.h>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

// zlib_filefunc64_def etc. come via unzip.h (already included by fszipurl.h
// inside extern "C"); ioapi.h alone doesn't parse without zlib.h's macros.

// Synchronous same-origin Range fetch on THIS worker thread. Returns bytes
// copied to dst, or a negative HTTP status / -1 on error. Sync XHR cannot use
// responseType=arraybuffer, so we use the classic x-user-defined charset trick
// and copy char codes into the heap. A 200 response (server ignored Range) is
// only usable when start==0; otherwise it is data from the wrong offset.
EM_JS(int, bw64SyncFetchRange, (const char* url, double start, double endIncl, double dstAddr, int want), {
    try {
        var u = UTF8ToString(url);
        var xhr = new XMLHttpRequest();
        xhr.open('GET', u, false);
        xhr.setRequestHeader('Range', 'bytes=' + start + '-' + endIncl);
        xhr.overrideMimeType('text/plain; charset=x-user-defined');
        xhr.send(null);
        if (xhr.status != 206 && !(xhr.status == 200 && start == 0)) return -xhr.status;
        var t = xhr.responseText;
        var n = t.length < want ? t.length : want;
        if (typeof growMemViews === 'function') growMemViews();
        var dst = Number(dstAddr);
        for (var i = 0; i < n; i++) HEAPU8[dst + i] = t.charCodeAt(i) & 0xff;
        return n;
    } catch (e) {
        return -1;
    }
});

namespace {

constexpr U64 BLOCK_SIZE = 512 * 1024;

struct UrlPart {
    BString url;
    U64 size;
    U64 start; // cumulative offset of this part in the virtual file
};

struct RangedFile {
    std::vector<UrlPart> parts;
    U64 total = 0;
    U64 pos = 0;
    bool error = false;
    U64 fetched = 0;      // stats: bytes downloaded so far
    U64 lastLogged = 0;
    std::mutex m;
    std::map<U64, std::unique_ptr<U8[]>> blocks;

    // Fetch [off, off+len) directly from the underlying parts into dst.
    bool fetchRaw(U64 off, U64 len, U8* dst) {
        while (len) {
            const UrlPart* p = nullptr;
            for (auto& part : parts) {
                if (off >= part.start && off < part.start + part.size) { p = &part; break; }
            }
            if (!p) return false;
            U64 inPart = off - p->start;
            U64 chunk = std::min(len, p->size - inPart);
            int got = bw64SyncFetchRange(p->url.c_str(), (double)inPart,
                                         (double)(inPart + chunk - 1),
                                         (double)(uintptr_t)dst, (int)chunk);
            if (got != (int)chunk) {
                klog_fmt("fszipurl: range fetch FAILED url=%s off=%llu len=%llu -> %d",
                         p->url.c_str(), (unsigned long long)inPart,
                         (unsigned long long)chunk, got);
                return false;
            }
            fetched += chunk;
            off += chunk;
            dst += chunk;
            len -= chunk;
        }
        if (fetched - lastLogged >= 16 * 1024 * 1024) {
            lastLogged = fetched;
            klog_fmt("fszipurl: %llu MB fetched lazily so far",
                     (unsigned long long)(fetched >> 20));
        }
        return true;
    }

    const U8* getBlock(U64 idx) {
        auto it = blocks.find(idx);
        if (it != blocks.end()) {
            return it->second.get();
        }
        U64 off = idx * BLOCK_SIZE;
        if (off >= total) {
            return nullptr;
        }
        U64 len = std::min(BLOCK_SIZE, total - off);
        auto buf = std::make_unique<U8[]>(BLOCK_SIZE);
        if (!fetchRaw(off, len, buf.get())) {
            return nullptr;
        }
        const U8* raw = buf.get();
        blocks[idx] = std::move(buf);
        return raw;
    }

    U32 read(void* out, U32 size) {
        std::lock_guard<std::mutex> lk(m);
        U8* dst = (U8*)out;
        U32 done = 0;
        while (size) {
            if (pos >= total) {
                break; // EOF
            }
            U64 idx = pos / BLOCK_SIZE;
            U64 inBlock = pos % BLOCK_SIZE;
            U64 blockLen = std::min(BLOCK_SIZE, total - idx * BLOCK_SIZE);
            const U8* b = getBlock(idx);
            if (!b) {
                error = true;
                break;
            }
            U32 chunk = (U32)std::min<U64>(size, blockLen - inBlock);
            memcpy(dst, b + inBlock, chunk);
            pos += chunk;
            dst += chunk;
            size -= chunk;
            done += chunk;
        }
        return done;
    }
};

voidpf urlOpen64(voidpf opaque, const void* filename, int mode) {
    (void)filename; (void)mode;
    return opaque; // the RangedFile* itself is the stream
}
uLong urlRead(voidpf opaque, voidpf stream, void* buf, uLong size) {
    (void)opaque;
    return ((RangedFile*)stream)->read(buf, (U32)size);
}
uLong urlWrite(voidpf opaque, voidpf stream, const void* buf, uLong size) {
    (void)opaque; (void)stream; (void)buf; (void)size;
    return 0; // read-only
}
ZPOS64_T urlTell64(voidpf opaque, voidpf stream) {
    (void)opaque;
    return ((RangedFile*)stream)->pos;
}
long urlSeek64(voidpf opaque, voidpf stream, ZPOS64_T offset, int origin) {
    (void)opaque;
    RangedFile* f = (RangedFile*)stream;
    std::lock_guard<std::mutex> lk(f->m);
    U64 base = (origin == ZLIB_FILEFUNC_SEEK_CUR) ? f->pos
             : (origin == ZLIB_FILEFUNC_SEEK_END) ? f->total : 0;
    f->pos = base + offset;
    return 0;
}
int urlClose(voidpf opaque, voidpf stream) {
    (void)opaque;
    delete (RangedFile*)stream;
    return 0;
}
int urlError(voidpf opaque, voidpf stream) {
    (void)opaque;
    return ((RangedFile*)stream)->error ? 1 : 0;
}

} // namespace

unzFile fsZipUrlOpen(BString spec) {
    // bw64url:<totalBytes>;<url1>|<size1>;<url2>|<size2>;...
    BString body = spec.substr(8);
    std::vector<BString> fields;
    body.split(';', fields);
    if (fields.size() < 2) {
        klog_fmt("fszipurl: bad spec (need total;url|size...): %s", spec.c_str());
        return nullptr;
    }
    auto f = std::make_unique<RangedFile>();
    f->total = (U64)atoll(fields[0].c_str());
    U64 cum = 0;
    for (size_t i = 1; i < fields.size(); i++) {
        std::vector<BString> us;
        fields[i].split('|', us);
        if (us.size() != 2) {
            klog_fmt("fszipurl: bad url|size field: %s", fields[i].c_str());
            return nullptr;
        }
        UrlPart p;
        p.url = us[0];
        p.size = (U64)atoll(us[1].c_str());
        p.start = cum;
        cum += p.size;
        f->parts.push_back(p);
    }
    if (cum != f->total) {
        klog_fmt("fszipurl: part sizes (%llu) != total (%llu)",
                 (unsigned long long)cum, (unsigned long long)f->total);
        return nullptr;
    }
    zlib_filefunc64_def fns = {};
    fns.zopen64_file = urlOpen64;
    fns.zread_file = urlRead;
    fns.zwrite_file = urlWrite;
    fns.ztell64_file = urlTell64;
    fns.zseek64_file = urlSeek64;
    fns.zclose_file = urlClose;
    fns.zerror_file = urlError;
    fns.opaque = f.get();
    unzFile z = unzOpen2_64(spec.c_str(), &fns);
    if (z) {
        klog_fmt("fszipurl: mounted %llu MB zip lazily (%d part(s), 512KB blocks)",
                 (unsigned long long)(f->total >> 20), (int)f->parts.size());
        f.release(); // owned by the unzFile stream now (freed in urlClose)
    }
    return z;
}

#elif defined(BOXEDWINE_ZLIB)

unzFile fsZipUrlOpen(BString spec) {
    klog_fmt("fszipurl: bw64url specs need the emscripten build: %s", spec.c_str());
    return nullptr;
}

#endif
