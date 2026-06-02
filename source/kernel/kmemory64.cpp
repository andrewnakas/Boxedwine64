/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"
#include "kmemory64.h"

#ifdef BOXEDWINE_GUEST_X64

#include <string.h>
#include <cstdlib>

// K_EINVAL / K_ENOMEM live in the existing kernel headers. We return
// (U64)-errno from mmap-style calls following the 32-bit convention.
#ifndef K_EINVAL
#define K_EINVAL 22
#endif
#ifndef K_ENOMEM
#define K_ENOMEM 12
#endif

KMemory64::KMemory64(KProcess* process) : process(process) {}
KMemory64::~KMemory64() = default;

void KMemory64::cloneFrom(const KMemory64* from) {
    // Lock both sides: `from` may be a live address space (the forking parent),
    // and we're populating `this` (the fresh child). The parent could fault new
    // pages concurrently in MT mode; take its lock for a consistent snapshot.
    // recursive_mutex is fine even if from==this (it never is for fork). Use
    // explicit guards (the CRITICAL_SECTION macro hard-codes the name `lock`, so
    // it can't be used twice in one scope).
#ifdef BOXEDWINE_MULTI_THREADED
    std::lock_guard<std::recursive_mutex> lockFrom(from->pagesMutex);
    std::lock_guard<std::recursive_mutex> lockThis(pagesMutex);
#endif
    pages.clear();
    pages.reserve(from->pages.size());
    for (const auto& kv : from->pages) {
        auto copy = std::make_unique<K64Page>();
        copy->flags = kv.second->flags;
        // Only copy backing store for committed pages; an uncommitted page in
        // the parent (reserved but never touched) stays uncommitted in the child.
        if (kv.second->committed()) {
            ::memcpy(copy->commit(), kv.second->data, K64_PAGE_SIZE);
        }
        pages.emplace(kv.first, std::move(copy));
    }
}

K64Page* KMemory64::getPage(U64 pageNum) const {
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    auto it = pages.find(pageNum);
    return it == pages.end() ? nullptr : it->second.get();
}

K64Page* KMemory64::getOrAllocPage(U64 pageNum, U32 flagsIfNew) {
    // Lock spans find+emplace so a concurrent allocator can't rehash the map
    // under our iterator. The returned raw K64Page* stays valid after we drop
    // the lock because the unique_ptr payload never moves (see header note).
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    auto it = pages.find(pageNum);
    if (it != pages.end()) {
        return it->second.get();
    }
    auto page = std::make_unique<K64Page>();
    page->flags = flagsIfNew;
    K64Page* raw = page.get();
    pages.emplace(pageNum, std::move(page));
    return raw;
}

U64 KMemory64::committedPageCount() const {
    // Pages that actually hold a backing buffer (touched), as opposed to merely
    // reserved address space (data==nullptr). The gap between this and
    // mappedPageCount() is the memory lazy commit saves on wine's huge
    // PROT_NONE reservations.
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    U64 n = 0;
    for (const auto& kv : pages) {
        if (kv.second->committed()) n++;
    }
    return n;
}

bool KMemory64::isPageMapped(U64 pageNum) const {
    K64Page* p = getPage(pageNum);
    return p && (p->flags & K64_PAGE_MAPPED);
}

U8* KMemory64::getCommittedPagePtr(U64 pageNum) {
    K64Page* p = getPage(pageNum);
    return (p && p->committed()) ? p->data : nullptr;
}

U32 KMemory64::getPageFlags(U64 pageNum) const {
    K64Page* p = getPage(pageNum);
    return p ? p->flags : 0;
}

// Process-wide mmap base — must match MMAP64_BASE in syscall64.cpp. Anonymous
// and file-backed mmap(NULL,...) both draw from here so they can't collide.
#define K64_MMAP_BASE 0x700000000ULL
#define K64_MMAP_BASE_PAGE (K64_MMAP_BASE >> K64_PAGE_SHIFT)

U64 KMemory64::mmapAnonymousFixed(U64 addr, U64 len, U32 prot) {
    if (len == 0) return (U64)-K_EINVAL;
    if (addr & K64_PAGE_MASK) return (U64)-K_EINVAL;

    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;

    U32 flags = K64_PAGE_MAPPED;
    if (prot & 0x1) flags |= K64_PAGE_READ;
    if (prot & 0x2) flags |= K64_PAGE_WRITE | K64_PAGE_READ; // x86: write implies read
    if (prot & 0x4) flags |= K64_PAGE_EXEC;

    for (U64 i = 0; i < pageCount; i++) {
        K64Page* page = getOrAllocPage(pageStart + i, flags);
        page->flags = flags;
        // Lazy commit: a FRESH reservation gets NO backing buffer — it reads as
        // zero (memcpyFromGuest zero-fills an uncommitted page) and only commits
        // on first write. This is the leak fix: wine's huge PROT_NONE spans no
        // longer cost a 4 KB buffer per page. An ALREADY-COMMITTED page being
        // re-mapped (overlap) is zeroed in place to preserve MAP_ANONYMOUS
        // semantics and keep the file-mmap head/tail preservation logic valid.
        if (page->committed()) {
            ::memset(page->data, 0, K64_PAGE_SIZE);
        }
    }
    // Keep the reservation map authoritative for the mmap region. A MAP_FIXED
    // landing here (wine remapping inside an earlier reservation, or the
    // allocator's own commit) updates `ranges` so later gap searches see the
    // real occupancy. rangeInsertLocked ignores addresses below the base and
    // splits any range this one overwrites. mmapMutex is recursive, so this is
    // safe whether or not the caller (mmapReserveAndMap) already holds it.
    if (pageStart >= K64_MMAP_BASE_PAGE) {
        BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
        rangeInsertLocked(pageStart, pageCount, prot,
                          prot ? (U8)MMAP_ANON : (U8)MMAP_RESERVED);
    }
    return addr;
}

// Record a reservation in the ordered `ranges` map. Caller holds mmapMutex.
// Splits/clamps any pre-existing ranges that the new one overwrites so the map
// stays a non-overlapping cover of the live mmap-region reservations (wine
// MAP_FIXED-remaps inside an earlier reservation constantly). Only the mmap
// region (>= K64_MMAP_BASE) is tracked; the loader's fixed low maps don't need
// allocation bookkeeping because the gap search never looks below the base.
void KMemory64::rangeInsertLocked(U64 startPage, U64 pageCount, U32 prot, U8 kind) {
    if (pageCount == 0 || startPage < K64_MMAP_BASE_PAGE) return;
    rangeRemoveLocked(startPage, pageCount); // clear any overlap first
    ranges[startPage] = MMapRange{ startPage, pageCount, prot, kind };
}

// Remove/trim ranges overlapping [startPage, startPage+pageCount). Caller holds
// mmapMutex. A range straddling either edge is split into the surviving
// non-overlapping remnant(s). This is the address-space free that makes the
// region reusable; it does NOT touch page backing store (that is Phase 2).
void KMemory64::rangeRemoveLocked(U64 startPage, U64 pageCount) {
    if (pageCount == 0) return;
    U64 hole0 = startPage;
    U64 hole1 = startPage + pageCount; // exclusive
    // Start from the last range that could overlap: the one beginning at or
    // before hole0. std::map is ordered so we can walk forward from there.
    auto it = ranges.upper_bound(hole0);
    if (it != ranges.begin()) --it;
    while (it != ranges.end() && it->second.startPage < hole1) {
        U64 r0 = it->second.startPage;
        U64 r1 = r0 + it->second.pageCount; // exclusive
        if (r1 <= hole0) { ++it; continue; } // entirely before the hole
        // Overlaps. Drop it, then re-insert any surviving head/tail remnants.
        U32 prot = it->second.prot; U8 kind = it->second.kind;
        it = ranges.erase(it);
        if (r0 < hole0) ranges[r0] = MMapRange{ r0, hole0 - r0, prot, kind };
        if (r1 > hole1) ranges[hole1] = MMapRange{ hole1, r1 - hole1, prot, kind };
    }
}

U64 KMemory64::mmapReserveAndMap(U64 length, U32 prot) {
    U64 pageCount = (length + K64_PAGE_MASK) >> K64_PAGE_SHIFT;
    if (pageCount == 0) pageCount = 1;

    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    if (mmapNext == 0) mmapNext = K64_MMAP_BASE;
    U64 cursor = mmapNext >> K64_PAGE_SHIFT;
    if (cursor < K64_MMAP_BASE_PAGE) cursor = K64_MMAP_BASE_PAGE;

    // Gap search over the ordered reservation map: O(log n + ranges scanned),
    // not O(pages). Walk ranges from `cursor`; the first hole of `pageCount`
    // pages between consecutive reservations (or after the last one) wins.
    U64 candidate = cursor;
    auto it = ranges.lower_bound(candidate);
    // A range starting before `candidate` may still cover it — back up one and
    // push `candidate` past its end if so.
    if (it != ranges.begin()) {
        auto prev = std::prev(it);
        U64 prevEnd = prev->second.startPage + prev->second.pageCount;
        if (prevEnd > candidate) candidate = prevEnd;
    }
    for (; it != ranges.end(); ++it) {
        U64 gapEnd = it->second.startPage; // exclusive
        if (gapEnd >= candidate + pageCount) break; // fits before this range
        U64 rangeEnd = it->second.startPage + it->second.pageCount;
        if (rangeEnd > candidate) candidate = rangeEnd; // jump past it
    }
    // `candidate` now points at a hole large enough (either before `it` or past
    // the last range — address space above is effectively unbounded for us).
    U64 addr = candidate << K64_PAGE_SHIFT;
    // Map the pages NOW, under mmapMutex (recursive), so a concurrent sibling's
    // allocation sees them taken. mmapAnonymousFixed registers the reservation
    // in `ranges` itself, so the gap is claimed before we drop the lock.
    mmapAnonymousFixed(addr, pageCount << K64_PAGE_SHIFT, prot);
    mmapNext = (candidate + pageCount) << K64_PAGE_SHIFT;
    return addr;
}

U64 KMemory64::mprotect(U64 addr, U64 len, U32 prot) {
    if (addr & K64_PAGE_MASK) return (U64)-K_EINVAL;
    if (len == 0) return addr;

    U32 newFlags = K64_PAGE_MAPPED;
    if (prot & 0x1) newFlags |= K64_PAGE_READ;
    if (prot & 0x2) newFlags |= K64_PAGE_WRITE | K64_PAGE_READ; // write implies read
    if (prot & 0x4) newFlags |= K64_PAGE_EXEC;

    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(pagesMutex);
    for (U64 i = 0; i < pageCount; i++) {
        // Preserve SHARED bit if it was set; everything else is replaced
        // wholesale. Holes (no page) are skipped — see header comment.
        auto it = pages.find(pageStart + i);
        if (it == pages.end()) continue;
        U32 preserved = it->second->flags & (K64_PAGE_SHARED | K64_PAGE_PINNED);
        it->second->flags = newFlags | preserved;
    }
    return addr;
}

U64 KMemory64::munmap(U64 addr, U64 len) {
    if (addr & K64_PAGE_MASK) return (U64)-K_EINVAL;
    if (len == 0) return 0;
    U64 pageStart = addr >> K64_PAGE_SHIFT;
    U64 pageCount = (len + K64_PAGE_SIZE - 1) >> K64_PAGE_SHIFT;

    // Free the ADDRESS SPACE only — drop/trim the reservation record so the
    // range is reusable and the gap search stays fast.
    //
    // We deliberately do NOT decommit (free) the page buffers here, even though
    // that would reclaim more memory. Decommitting was measured to REGRESS boot:
    // wine/wineserver munmap a region and then lazily re-read it; with the
    // buffer freed those reads return ZERO instead of the stale-but-present
    // bytes, which tripped `wineserver: release_object: Assertion obj->refcount`
    // deterministically (refcount read back as 0). This is the same lazy-touch
    // hazard that sank the earlier pages.erase() real-munmap (commit 3ae9ca3a,
    // reverted). The big leak win comes from LAZY COMMIT on the mmap side (fresh
    // reservations carry no buffer at all — wine's 1.9 GB PROT_NONE spans now
    // cost ~60 MB), not from freeing on munmap, so address-space-only munmap
    // keeps essentially all the benefit with none of the breakage.
    BOXEDWINE_CRITICAL_SECTION_WITH_MUTEX(mmapMutex);
    rangeRemoveLocked(pageStart, pageCount);
    return 0;
}

// BW64_WATCH=0xADDR[,len] — log any guest write that overlaps [ADDR, ADDR+len).
// Used to find who fills (or fails to fill) a runtime callback slot. The host
// callstack isn't captured, but the value + the fact that a write happened at
// all distinguishes "never written" from "written with garbage". Parsed once.
static U64 g_watchAddr = 0, g_watchLen = 0;
static bool g_watchInit = false;
static void initWatch() {
    g_watchInit = true;
    const char* e = std::getenv("BW64_WATCH");
    if (!e) return;
    g_watchAddr = std::strtoull(e, nullptr, 0);
    const char* comma = std::strchr(e, ',');
    g_watchLen = comma ? std::strtoull(comma + 1, nullptr, 0) : 8;
    if (g_watchLen == 0) g_watchLen = 8;
}

void KMemory64::memcpyToGuest(U64 dstGuest, const void* src, U64 len) {
    const U8* s = (const U8*)src;
    if (!g_watchInit) initWatch();
    if (g_watchAddr && dstGuest < g_watchAddr + g_watchLen && dstGuest + len > g_watchAddr) {
        U64 v = 0;
        U64 within = (g_watchAddr >= dstGuest) ? (g_watchAddr - dstGuest) : 0;
        if (within + 8 <= len) std::memcpy(&v, (const U8*)src + within, 8);
        klog_fmt("BW64_WATCH: write to 0x%llx (watch 0x%llx) len=%llu first8=0x%llx",
                 (unsigned long long)dstGuest, (unsigned long long)g_watchAddr,
                 (unsigned long long)len, (unsigned long long)v);
    }
    while (len) {
        U64 pageNum = dstGuest >> K64_PAGE_SHIFT;
        U64 offsetInPage = dstGuest & K64_PAGE_MASK;
        U64 chunk = K64_PAGE_SIZE - offsetInPage;
        if (chunk > len) chunk = len;
        K64Page* page = getOrAllocPage(pageNum, K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
        ::memcpy(page->commit() + offsetInPage, s, (size_t)chunk); // commit-on-write
        dstGuest += chunk;
        s += chunk;
        len -= chunk;
    }
}

void KMemory64::memcpyFromGuest(void* dst, U64 srcGuest, U64 len) {
    U8* d = (U8*)dst;
    while (len) {
        U64 pageNum = srcGuest >> K64_PAGE_SHIFT;
        U64 offsetInPage = srcGuest & K64_PAGE_MASK;
        U64 chunk = K64_PAGE_SIZE - offsetInPage;
        if (chunk > len) chunk = len;
        K64Page* page = getPage(pageNum);
        if (page && page->committed()) {
            ::memcpy(d, page->data + offsetInPage, (size_t)chunk);
        } else {
            // Absent slot OR reserved-but-uncommitted page → reads as zero.
            ::memset(d, 0, (size_t)chunk);
        }
        srcGuest += chunk;
        d += chunk;
        len -= chunk;
    }
}

void KMemory64::memsetGuest(U64 dstGuest, U8 value, U64 len) {
    while (len) {
        U64 pageNum = dstGuest >> K64_PAGE_SHIFT;
        U64 offsetInPage = dstGuest & K64_PAGE_MASK;
        U64 chunk = K64_PAGE_SIZE - offsetInPage;
        if (chunk > len) chunk = len;
        K64Page* page = getOrAllocPage(pageNum, K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
        ::memset(page->commit() + offsetInPage, value, (size_t)chunk); // commit-on-write
        dstGuest += chunk;
        len -= chunk;
    }
}

U8 KMemory64::readb(U64 addr) {
    K64Page* p = getPage(addr >> K64_PAGE_SHIFT);
    return (p && p->committed()) ? p->data[addr & K64_PAGE_MASK] : 0;
}

U16 KMemory64::readw(U64 addr) {
    U16 v; memcpyFromGuest(&v, addr, 2); return v;
}

U32 KMemory64::readd(U64 addr) {
    U32 v; memcpyFromGuest(&v, addr, 4); return v;
}

U64 KMemory64::readq(U64 addr) {
    U64 v; memcpyFromGuest(&v, addr, 8); return v;
}

void KMemory64::writeb(U64 addr, U8 value) {
    K64Page* p = getOrAllocPage(addr >> K64_PAGE_SHIFT, K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
    p->commit()[addr & K64_PAGE_MASK] = value; // commit-on-write
}

U8* KMemory64::getRamPtr(U64 addr, U32 len) {
    U64 offsetInPage = addr & K64_PAGE_MASK;
    if (offsetInPage + len > K64_PAGE_SIZE) {
        // Would span two pages — host pointer wouldn't be contiguous.
        return nullptr;
    }
    K64Page* page = getOrAllocPage(addr >> K64_PAGE_SHIFT,
                                   K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
    // Commit + PIN. Callers (the 64-bit futex table, atomic RMW in common_lock)
    // keep this raw host pointer across a blocking wait. If a concurrent munmap
    // decommitted the buffer, a re-commit would move it and the futex wake would
    // target a stale address. The pin flag tells munmap/mprotect to free the
    // address space but LEAVE this page's buffer in place. Once allocated, the
    // buffer is never reallocated, so the returned pointer stays valid.
    page->flags |= K64_PAGE_PINNED;
    return page->commit() + offsetInPage;
}

void KMemory64::writew(U64 addr, U16 value) { memcpyToGuest(addr, &value, 2); }
void KMemory64::writed(U64 addr, U32 value) { memcpyToGuest(addr, &value, 4); }
void KMemory64::writeq(U64 addr, U64 value) { memcpyToGuest(addr, &value, 8); }

#endif // BOXEDWINE_GUEST_X64
