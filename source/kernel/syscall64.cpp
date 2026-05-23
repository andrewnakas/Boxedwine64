/*
 *  Copyright (C) 2012-2026  The BoxedWine Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "boxedwine.h"

#ifdef BOXEDWINE_GUEST_X64

#include "syscall64.h"
#include "cpu64.h"
#include "kmemory64.h"

// x86-64 Linux syscall numbers used here. The canonical table lives in
// arch/x86/entry/syscalls/syscall_64.tbl in the Linux source; the values
// below are stable Linux ABI and never change.
#define X64_SYS_read              0
#define X64_SYS_write             1
#define X64_SYS_open              2
#define X64_SYS_close             3
#define X64_SYS_mmap              9
#define X64_SYS_mprotect          10
#define X64_SYS_munmap            11
#define X64_SYS_brk               12
#define X64_SYS_rt_sigaction      13
#define X64_SYS_rt_sigprocmask    14
#define X64_SYS_writev            20
#define X64_SYS_access            21
#define X64_SYS_exit              60
#define X64_SYS_arch_prctl        158
#define X64_SYS_set_tid_address   218
#define X64_SYS_exit_group        231
#define X64_SYS_set_robust_list   273

// arch_prctl subfunctions
#define X64_ARCH_SET_GS  0x1001
#define X64_ARCH_SET_FS  0x1002
#define X64_ARCH_GET_FS  0x1003
#define X64_ARCH_GET_GS  0x1004

// Linux returns errno as the negated value in RAX. We use the same K_*
// constants as the 32-bit path so error semantics stay aligned.
#ifndef K_ENOSYS
#define K_ENOSYS 38
#endif
#ifndef K_EINVAL
#define K_EINVAL 22
#endif
#ifndef K_EFAULT
#define K_EFAULT 14
#endif

// MAP_* bits used by mmap. Kept local to avoid pulling kernel.h here.
#ifndef K_MAP_ANONYMOUS
#define K_MAP_ANONYMOUS 0x20
#endif
#ifndef K_MAP_FIXED
#define K_MAP_FIXED 0x10
#endif

static U64 sys_write64(CPU64* cpu, U64 fd, U64 buf, U64 count) {
    // For the early-boot phase we only need to honour writes to fd 1/2
    // (stdout/stderr) so ld-linux's diagnostic messages reach the host
    // console. Real fd routing comes later via KThread::process.
    if (fd != 1 && fd != 2) {
        klog_fmt("sys_write64: fd=%llu not yet routed", (unsigned long long)fd);
        return (U64)-K_ENOSYS;
    }
    // Read bytes out of guest memory in chunks and printf them. count
    // capped at 64K per call so a hostile binary can't hang us.
    if (count > 65536) count = 65536;
    std::vector<U8> buffer((size_t)count + 1);
    cpu->memory->memcpyFromGuest(buffer.data(), buf, count);
    buffer[count] = 0;
    klog_fmt("[guest fd=%llu] %s", (unsigned long long)fd, (const char*)buffer.data());
    return count;
}

static U64 sys_arch_prctl64(CPU64* cpu, U64 code, U64 addr) {
    switch (code) {
        case X64_ARCH_SET_FS:
            cpu->fsbase = addr;
            return 0;
        case X64_ARCH_SET_GS:
            cpu->gsbase = addr;
            return 0;
        case X64_ARCH_GET_FS:
            cpu->memory->writeq(addr, cpu->fsbase);
            return 0;
        case X64_ARCH_GET_GS:
            cpu->memory->writeq(addr, cpu->gsbase);
            return 0;
        default:
            return (U64)-K_EINVAL;
    }
}

// brk(0) → returns current break. brk(new) → tries to set break, returns
// new break on success or old break on failure (Linux contract). Stored
// per-process on KProcess::brkEnd in the 32-bit path; we put it on the
// thread's process the same way.
static U64 sys_brk64(CPU64* cpu, U64 newBrk) {
    if (!cpu->thread || !cpu->thread->process) {
        return 0;
    }
    KProcess* p = cpu->thread->process.get();
    U64 oldBrk = p->brkEnd;
    if (newBrk == 0 || newBrk < oldBrk) {
        return oldBrk;
    }
    // Map the gap as anonymous RW. mmapAnonymousFixed expects page-aligned
    // addresses; round oldBrk up and newBrk up to page boundaries.
    U64 alignedOld = (oldBrk + 0xFFF) & ~0xFFFULL;
    U64 alignedNew = (newBrk + 0xFFF) & ~0xFFFULL;
    if (alignedNew > alignedOld) {
        U64 ret = cpu->memory->mmapAnonymousFixed(alignedOld, alignedNew - alignedOld, 0x3); // PROT_READ|WRITE
        if ((S64)ret < 0) {
            return oldBrk;
        }
    }
    p->brkEnd = (U32)newBrk; // NOTE: brkEnd is U32 on KProcess in v1 — will widen alongside other 64-bit kprocess fields
    return newBrk;
}

static U64 sys_mmap64(CPU64* cpu, U64 addr, U64 length, U64 prot, U64 flags, U64 fd, U64 offset) {
    if (!(flags & K_MAP_ANONYMOUS)) {
        klog_fmt("sys_mmap64: file-backed mmap fd=%lld not yet implemented", (long long)(S64)fd);
        return (U64)-K_ENOSYS;
    }
    if (addr == 0) {
        // Anonymous mmap without MAP_FIXED: pick a high address. v1 uses
        // a bump allocator on KProcess; for the skeleton we use a fixed
        // base that won't collide with PIE-loaded segments.
        static U64 anonBump = 0x700000000ULL;
        addr = anonBump;
        anonBump += (length + 0xFFF) & ~0xFFFULL;
    }
    U64 ret = cpu->memory->mmapAnonymousFixed(addr & ~0xFFFULL, length, (U32)prot);
    (void)offset;
    return ret;
}

static U64 sys_exit64(CPU64* cpu, U64 status) {
    klog_fmt("CPU64: exit syscall, status=%lld", (long long)status);
    cpu->yield = true;
    return 0;
}

void ksyscall64(CPU64* cpu) {
    if (!cpu) return;
    U64 nr   = cpu->reg[X64_RAX].u64;
    U64 a1   = cpu->reg[X64_RDI].u64;
    U64 a2   = cpu->reg[X64_RSI].u64;
    U64 a3   = cpu->reg[X64_RDX].u64;
    U64 a4   = cpu->reg[X64_R10].u64;
    U64 a5   = cpu->reg[X64_R8].u64;
    U64 a6   = cpu->reg[X64_R9].u64;
    U64 ret  = (U64)-K_ENOSYS;

    switch (nr) {
        case X64_SYS_write:
            ret = sys_write64(cpu, a1, a2, a3);
            break;
        case X64_SYS_arch_prctl:
            ret = sys_arch_prctl64(cpu, a1, a2);
            break;
        case X64_SYS_brk:
            ret = sys_brk64(cpu, a1);
            break;
        case X64_SYS_mmap:
            ret = sys_mmap64(cpu, a1, a2, a3, a4, a5, a6);
            break;
        case X64_SYS_mprotect:
            // v1: no-op success. KMemory64 doesn't track per-page prot
            // changes after mmap yet; for ld-linux's typical "make
            // .text RX, .data RW" calls this is benign because we
            // currently treat every page as RWX.
            ret = 0;
            break;
        case X64_SYS_munmap:
            // v1: no-op success. Until KMemory64 supports unmap we'd
            // rather leak than synthesise a failure that confuses ld.
            ret = 0;
            break;
        case X64_SYS_set_tid_address:
            // Returns the tid; we don't track per-thread clear_child_tid
            // yet, so just give back something plausible.
            ret = cpu->thread ? cpu->thread->id : 1;
            break;
        case X64_SYS_set_robust_list:
        case X64_SYS_rt_sigprocmask:
        case X64_SYS_rt_sigaction:
            // ld-linux makes these calls before main; safe to no-op.
            ret = 0;
            break;
        case X64_SYS_exit:
        case X64_SYS_exit_group:
            ret = sys_exit64(cpu, a1);
            break;
        default:
            klog_fmt("ksyscall64: unimplemented syscall #%llu (RDI=0x%llx RSI=0x%llx RDX=0x%llx)",
                     (unsigned long long)nr,
                     (unsigned long long)a1,
                     (unsigned long long)a2,
                     (unsigned long long)a3);
            ret = (U64)-K_ENOSYS;
            break;
    }

    cpu->reg[X64_RAX].setU64(ret);
}

#endif // BOXEDWINE_GUEST_X64
