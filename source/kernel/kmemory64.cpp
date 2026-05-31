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
        ::memcpy(copy->data, kv.second->data, K64_PAGE_SIZE);
        copy->flags = kv.second->flags;
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

bool KMemory64::isPageMapped(U64 pageNum) const {
    K64Page* p = getPage(pageNum);
    return p && (p->flags & K64_PAGE_MAPPED);
}

U32 KMemory64::getPageFlags(U64 pageNum) const {
    K64Page* p = getPage(pageNum);
    return p ? p->flags : 0;
}

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
        // If the page already existed (e.g. overlap), update its flags and
        // zero its contents per MAP_ANONYMOUS semantics.
        page->flags = flags;
        ::memset(page->data, 0, K64_PAGE_SIZE);
    }
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
        U32 preserved = it->second->flags & K64_PAGE_SHARED;
        it->second->flags = newFlags | preserved;
    }
    return addr;
}

void KMemory64::memcpyToGuest(U64 dstGuest, const void* src, U64 len) {
    const U8* s = (const U8*)src;
    while (len) {
        U64 pageNum = dstGuest >> K64_PAGE_SHIFT;
        U64 offsetInPage = dstGuest & K64_PAGE_MASK;
        U64 chunk = K64_PAGE_SIZE - offsetInPage;
        if (chunk > len) chunk = len;
        K64Page* page = getOrAllocPage(pageNum, K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
        ::memcpy(page->data + offsetInPage, s, (size_t)chunk);
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
        if (page) {
            ::memcpy(d, page->data + offsetInPage, (size_t)chunk);
        } else {
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
        ::memset(page->data + offsetInPage, value, (size_t)chunk);
        dstGuest += chunk;
        len -= chunk;
    }
}

U8 KMemory64::readb(U64 addr) {
    K64Page* p = getPage(addr >> K64_PAGE_SHIFT);
    return p ? p->data[addr & K64_PAGE_MASK] : 0;
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
    p->data[addr & K64_PAGE_MASK] = value;
}

U8* KMemory64::getRamPtr(U64 addr, U32 len) {
    U64 offsetInPage = addr & K64_PAGE_MASK;
    if (offsetInPage + len > K64_PAGE_SIZE) {
        // Would span two pages — host pointer wouldn't be contiguous.
        return nullptr;
    }
    K64Page* page = getOrAllocPage(addr >> K64_PAGE_SHIFT,
                                   K64_PAGE_MAPPED | K64_PAGE_READ | K64_PAGE_WRITE);
    return page->data + offsetInPage;
}

void KMemory64::writew(U64 addr, U16 value) { memcpyToGuest(addr, &value, 2); }
void KMemory64::writed(U64 addr, U32 value) { memcpyToGuest(addr, &value, 4); }
void KMemory64::writeq(U64 addr, U64 value) { memcpyToGuest(addr, &value, 8); }

#endif // BOXEDWINE_GUEST_X64
