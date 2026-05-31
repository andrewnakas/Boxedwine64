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

    // Diagnostics
    U64 mappedPageCount() const { return (U64)pages.size(); }

private:
    KProcess* process;
    std::unordered_map<U64, std::unique_ptr<K64Page>> pages;

    K64Page* getOrAllocPage(U64 pageNum, U32 flagsIfNew);
    K64Page* getPage(U64 pageNum) const;
};

#endif // BOXEDWINE_GUEST_X64
#endif
