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

// Synchronous same-origin Range fetch from a guest pthread, built on
// fetch() ONLY. Chrome ~151/152 rejects the XHR paths in this COEP worker
// context (both a hand-rolled sync XHR and emscripten_fetch — whose internal
// transport is XHR — start returning network errors after which lazy reads
// fail; bisected on Chrome-for-Testing 152, passes on 149/150). Mechanism:
// the calling pthread queues a task to the browser main runtime thread with
// emscripten_proxy_sync_with_ctx and blocks; on the main thread an EM_JS kicks
// an async fetch(Range) whose .then copies the bytes into the (SAB-backed)
// destination, stores the byte count, and finishes the proxy ctx, waking the
// caller. fetch() is exactly what the launcher's eager path uses — the one
// transport every Chrome keeps working.
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

namespace {
struct FetchJob {
    const char* url;
    U64 start;
    U64 endIncl;
    U8* dst;
    int want;
    int result; // bytes copied or negative status/-1
};
}

extern "C" {
// Called from JS on the main thread when the fetch settles.
EMSCRIPTEN_KEEPALIVE void bw64FetchDone(double ctxAddr, double jobAddr, int result) {
    FetchJob* job = (FetchJob*)(uintptr_t)jobAddr;
    job->result = result;
    emscripten_proxy_finish((em_proxying_ctx*)(uintptr_t)ctxAddr);
}
}

EM_JS(void, bw64StartFetch, (double ctxAddr, double jobAddr, const char* url, double start, double endIncl, double dstAddr, int want), {
    var u = UTF8ToString(url);
    var range = 'bytes=' + start + '-' + endIncl;
    fetch(u, { headers: { 'Range': range }, cache: 'no-store' }).then(function (r) {
        if (r.status != 206 && !(r.status == 200 && start == 0)) {
            throw { bwStatus: r.status };
        }
        return r.arrayBuffer();
    }).then(function (ab) {
        var src = new Uint8Array(ab);
        var n = src.length < want ? src.length : want;
        if (typeof growMemViews === 'function') growMemViews();
        HEAPU8.set(src.subarray(0, n), Number(dstAddr));
        _bw64FetchDone(ctxAddr, jobAddr, n);
    }).catch(function (e) {
        var code = (e && e.bwStatus) ? -e.bwStatus : -1;
        _bw64FetchDone(ctxAddr, jobAddr, code);
    });
});

static void bw64FetchOnMain(em_proxying_ctx* ctx, void* arg) {
    FetchJob* job = (FetchJob*)arg;
    bw64StartFetch((double)(uintptr_t)ctx, (double)(uintptr_t)job, job->url,
                   (double)job->start, (double)job->endIncl,
                   (double)(uintptr_t)job->dst, job->want);
    // Do NOT finish here — bw64FetchDone finishes when the fetch settles.
}

static int bw64SyncFetchRange(const char* url, double start, double endIncl, U8* dst, int want) {
    FetchJob job;
    job.url = url;
    job.start = (U64)start;
    job.endIncl = (U64)endIncl;
    job.dst = dst;
    job.want = want;
    job.result = -1;
    if (!emscripten_proxy_sync_with_ctx(emscripten_proxy_get_system_queue(),
                                        emscripten_main_runtime_thread_id(),
                                        bw64FetchOnMain, &job)) {
        return -1;
    }
    return job.result;
}

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
                                         dst, (int)chunk);
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

    U32 readAt(U64& pos, void* out, U32 size) {
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

// One RangedFile (block cache) is shared per spec across every unzOpen of it
// (FsZip::init + the static helpers each open the zip; without sharing, each
// re-fetched the central directory). Streams carry their own cursor.
struct RangedStream {
    std::shared_ptr<RangedFile> file;
    U64 pos = 0;
};
std::mutex g_registryMutex;
std::map<std::string, std::shared_ptr<RangedFile>> g_registry;

voidpf urlOpen64(voidpf opaque, const void* filename, int mode) {
    (void)filename; (void)mode;
    RangedStream* st = new RangedStream();
    st->file = *(std::shared_ptr<RangedFile>*)opaque;
    return st;
}
uLong urlRead(voidpf opaque, voidpf stream, void* buf, uLong size) {
    (void)opaque;
    RangedStream* st = (RangedStream*)stream;
    return st->file->readAt(st->pos, buf, (U32)size);
}
uLong urlWrite(voidpf opaque, voidpf stream, const void* buf, uLong size) {
    (void)opaque; (void)stream; (void)buf; (void)size;
    return 0; // read-only
}
ZPOS64_T urlTell64(voidpf opaque, voidpf stream) {
    (void)opaque;
    return ((RangedStream*)stream)->pos;
}
long urlSeek64(voidpf opaque, voidpf stream, ZPOS64_T offset, int origin) {
    (void)opaque;
    RangedStream* st = (RangedStream*)stream;
    U64 base = (origin == ZLIB_FILEFUNC_SEEK_CUR) ? st->pos
             : (origin == ZLIB_FILEFUNC_SEEK_END) ? st->file->total : 0;
    st->pos = base + offset;
    return 0;
}
int urlClose(voidpf opaque, voidpf stream) {
    (void)opaque;
    delete (RangedStream*)stream; // the shared RangedFile lives in the registry
    return 0;
}
int urlError(voidpf opaque, voidpf stream) {
    (void)opaque;
    return ((RangedStream*)stream)->file->error ? 1 : 0;
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
    std::lock_guard<std::mutex> reg(g_registryMutex);
    auto& slot = g_registry[std::string(spec.c_str())];
    if (slot) {
        zlib_filefunc64_def fns = {};
        fns.zopen64_file = urlOpen64;
        fns.zread_file = urlRead;
        fns.zwrite_file = urlWrite;
        fns.ztell64_file = urlTell64;
        fns.zseek64_file = urlSeek64;
        fns.zclose_file = urlClose;
        fns.zerror_file = urlError;
        fns.opaque = &slot;
        return unzOpen2_64(spec.c_str(), &fns);
    }
    auto f = std::make_shared<RangedFile>();
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
    slot = f; // registered BEFORE open so opaque points at stable storage
    zlib_filefunc64_def fns = {};
    fns.zopen64_file = urlOpen64;
    fns.zread_file = urlRead;
    fns.zwrite_file = urlWrite;
    fns.ztell64_file = urlTell64;
    fns.zseek64_file = urlSeek64;
    fns.zclose_file = urlClose;
    fns.zerror_file = urlError;
    fns.opaque = &slot;
    unzFile z = unzOpen2_64(spec.c_str(), &fns);
    if (z) {
        klog_fmt("fszipurl: mounted %llu MB zip lazily (%d part(s), 512KB blocks)",
                 (unsigned long long)(f->total >> 20), (int)f->parts.size());
    } else {
        g_registry.erase(std::string(spec.c_str()));
    }
    return z;
}

#elif defined(BOXEDWINE_ZLIB)

unzFile fsZipUrlOpen(BString spec) {
    klog_fmt("fszipurl: bw64url specs need the emscripten build: %s", spec.c_str());
    return nullptr;
}

#endif
