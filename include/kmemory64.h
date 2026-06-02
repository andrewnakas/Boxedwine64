/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef __KMEMORY64_H__
#define __KMEMORY64_H__

#ifdef BOXEDWINE_GUEST_X64

#include <memory>
#include <unordered_map>
#include <map>

class KProcess;
class KThread;

// Minimal 64-bit guest memory for v1. Pages are allocated lazily on first
// touch via an unordered_map keyed by page number (vaddr >> 12). This is
// not the long-term design — a real two-level page table will replace it
// once the loader/decoder/syscall paths are exercising it. For Phase 1.5
// the priority is: enough surface to back ElfLoader64 segment mapping and
// 64-bit stack setup, with simple correctness over performance.
//
// Deliberately NOT supported in v1:
//   - file-backed mappings (mmap with fd)
//   - MAP_SHARED, copy-on-write
//   - futex pages, JIT page-write tracking
//   - native host memory mapping
// These will be added incrementally once the basics work end-to-end.

#define K64_PAGE_SIZE  4096
#define K64_PAGE_SHIFT 12
#define K64_PAGE_MASK  0xFFFULL

// Permission bits — kept numerically identical to PAGE_READ/PAGE_WRITE/
// PAGE_EXEC/PAGE_MAPPED in kmemory.h so that future merging is trivial.
#define K64_PAGE_READ    0x01
#define K64_PAGE_WRITE   0x02
#define K64_PAGE_EXEC    0x04
#define K64_PAGE_SHARED  0x08
#define K64_PAGE_MAPPED  0x20

struct K64Page {
    U8 data[K64_PAGE_SIZE];
    U32 flags = 0;
};

class KMemory64 {
public:
    explicit KMemory64(KProcess* process);
    ~KMemory64();

    // mmap subset: anonymous + fixed only in v1. Returns the mapped guest
    // address, or (U64)-errno on failure. addr MUST be page-aligned and
    // non-zero (no address-picking in v1).
    U64 mmapAnonymousFixed(U64 addr, U64 len, U32 prot);

    // Atomically pick a free address range AND map it, so two guest threads of
    // the same process (sharing this KMemory64) can never be handed overlapping
    // mmap(NULL,...) placements. The old split — allocMmapRange() scans for a
    // gap, returns, then the caller mmapAnonymousFixed()s it — is a TOCTOU race
    // under BOXEDWINE_MULTI_THREADED: both threads scan, both see the same gap
    // free (neither has mapped yet), both map there, and the second map ZEROES
    // the first → "malloc(): corrupted double linked list" in the guest heap
    // (wineserver during the boot storm). Holding mmapMutex across scan+map
    // closes it. `length` need not be page-aligned (rounded up). Returns the
    // page-aligned base. prot follows K_PROT_* (0x1/0x2/0x4).
    U64 mmapReserveAndMap(U64 length, U32 prot);

    // Change page permissions on already-mapped pages. addr must be page-
    // aligned. prot bits follow K_PROT_READ/WRITE/EXEC (0x1/0x2/0x4) — same
    // convention as mmapAnonymousFixed. Pages within [addr, addr+len) that
    // are NOT mapped are silently skipped (matches the Linux semantics
    // where partial mprotect on holes returns -ENOMEM, but our v1 callers
    // only invoke this against fully-mapped ranges — RELRO enforcement on
    // the GOT, and eventually JIT page-write tracking).
    //
    // Returns the requested addr on success, (U64)-errno on failure (only
    // failure mode today is addr not page-aligned). Does NOT touch page
    // data. Used by the loader to honor PT_GNU_RELRO after relocations.
    U64 mprotect(U64 addr, U64 len, U32 prot);

    // Unmap [addr, addr+len): drop the backing pages so the address range is
    // genuinely free for reuse. A no-op munmap (the old behaviour) breaks wine:
    // wine munmaps a view, removes it from its own views_tree, then maps a fresh
    // view at the SAME address — but if the pages are still mapped, wine's
    // create_view finds an overlapping NON-system view and aborts
    // (virtual.c:1578 "assert(view->protect & VPROT_SYSTEM)"). addr must be
    // page-aligned; partial pages at the tail are unmapped whole. Returns 0.
    U64 munmap(U64 addr, U64 len);

    // Permission query / page state.
    bool isPageMapped(U64 pageNum) const;
    U32  getPageFlags(U64 pageNum) const;

    // Bulk copy host -> guest and guest -> host. Allocates target pages
    // if not yet present (zero-fills). Caller is responsible for permission
    // checks at the kernel layer if it cares.
    void memcpyToGuest(U64 dstGuest, const void* src, U64 len);
    void memcpyFromGuest(void* dst, U64 srcGuest, U64 len);
    void memsetGuest(U64 dstGuest, U8 value, U64 len);

    // Scalar accessors. Single-byte/word/dword/qword. No alignment check;
    // crossing a page boundary is handled by going through memcpy.
    U8  readb(U64 addr);
    U16 readw(U64 addr);
    U32 readd(U64 addr);
    U64 readq(U64 addr);
    void writeb(U64 addr, U8 value);
    void writew(U64 addr, U16 value);
    void writed(U64 addr, U32 value);
    void writeq(U64 addr, U64 value);

    // Stable host pointer for a guest address range. The page backing store
    // is a per-page unique_ptr<K64Page> whose data[] never moves for the life
    // of the mapping, so the returned pointer is stable and usable as a
    // cross-thread key (this is what the futex table keys on). The page is
    // allocated on demand if not yet present. Returns nullptr only if the
    // range would cross a page boundary (callers — futex words — are always
    // 4-byte aligned within a 4096-byte page, so this never happens in
    // practice; the check guards against a misaligned caller).
    U8* getRamPtr(U64 addr, U32 len);

    // Deep-copy every mapped page from `from` into this (empty) address space.
    // Used by fork (KProcess::clone64 non-thread): the child gets an independent
    // snapshot of the parent's memory. Not copy-on-write — a plain page copy —
    // which is correct (if not minimal) and fine for wine's fork-then-execve
    // pattern where the child replaces its image almost immediately.
    void cloneFrom(const KMemory64* from);

    // Diagnostics
    U64 mappedPageCount() const { return (U64)pages.size(); }
    // Pages that hold a committed backing buffer. Until lazy commit (Phase 2)
    // every mapped page is committed, so this equals mappedPageCount(); afterward
    // it tracks only pages whose K64Page::data is non-null (i.e. actually touched).
    U64 committedPageCount() const;

private:
    KProcess* process;
    std::unordered_map<U64, std::unique_ptr<K64Page>> pages;
    // Guards every mutation/lookup of `pages`. In the multi-threaded build all
    // guest threads share one KMemory64 and fault in pages concurrently; an
    // unordered_map rehash on emplace racing a find from another thread is UB
    // (it manifested as one thread's guest store landing on a stale K64Page*,
    // corrupting another thread's stack → wild RIP / hang in mt_probe). The
    // page payload (K64Page::data) never moves once allocated, so we only need
    // to serialize map structure access, not the subsequent memcpy. Compiles
    // away to nothing in the single-threaded build. Mutable so the const
    // lookups (getPage/isPageMapped/getPageFlags) can lock.
    mutable BOXEDWINE_MUTEX pagesMutex;

    // Serializes mmap address-space allocation (mmapReserveAndMap): the gap scan
    // and the reservation map must be one atomic step across sibling threads.
    // Separate from pagesMutex so a long scan doesn't block unrelated page
    // faults. Process-wide bump cursor (replaces the old per-CPU64 mmapNext so
    // every thread of the process advances the same pointer). 0 = uninitialised.
    BOXEDWINE_MUTEX mmapMutex;
    U64 mmapNext = 0;

    // Reserved address-space ranges drawn from the mmap region (>= K64_MMAP_BASE).
    // Keyed by start PAGE number, ordered, so a gap search is O(log n + ranges
    // scanned) instead of the old O(pages) per-page isPageMapped() scan that
    // degraded badly as the page map grew (the boot slowdown). munmap removes /
    // trims entries here so the address space is genuinely reusable. Guarded by
    // mmapMutex (same lock as the gap scan it feeds). `start`/`pages` are page
    // units. kind distinguishes wine's PROT_NONE reservations from our committed
    // anon/file maps (used by Phase 2/3 to free backing store safely).
    enum MMapKind : U8 { MMAP_ANON = 0, MMAP_FILE = 1, MMAP_RESERVED = 2 };
    struct MMapRange { U64 startPage; U64 pageCount; U32 prot; U8 kind; };
    std::map<U64, MMapRange> ranges;

    // Add/trim/remove range bookkeeping. Callers must hold mmapMutex.
    void rangeInsertLocked(U64 startPage, U64 pageCount, U32 prot, U8 kind);
    void rangeRemoveLocked(U64 startPage, U64 pageCount);

    K64Page* getOrAllocPage(U64 pageNum, U32 flagsIfNew);
    K64Page* getPage(U64 pageNum) const;
};

#endif // BOXEDWINE_GUEST_X64
#endif
