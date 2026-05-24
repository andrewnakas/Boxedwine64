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
#define X64_SYS_stat              4
#define X64_SYS_fstat             5
#define X64_SYS_lstat             6
#define X64_SYS_poll              7
#define X64_SYS_lseek             8
#define X64_SYS_mmap              9
#define X64_SYS_mprotect          10
#define X64_SYS_munmap            11
#define X64_SYS_brk               12
#define X64_SYS_rt_sigaction      13
#define X64_SYS_rt_sigprocmask    14
#define X64_SYS_ioctl             16
#define X64_SYS_pread64           17
#define X64_SYS_writev            20
#define X64_SYS_access            21
#define X64_SYS_readlink          89
#define X64_SYS_getpid            39
#define X64_SYS_exit              60
#define X64_SYS_uname             63
#define X64_SYS_getuid            102
#define X64_SYS_getgid            104
#define X64_SYS_geteuid           107
#define X64_SYS_getegid           108
#define X64_SYS_arch_prctl        158
#define X64_SYS_gettid            186
#define X64_SYS_futex             202
#define X64_SYS_set_tid_address   218
#define X64_SYS_clock_gettime     228
#define X64_SYS_exit_group        231
#define X64_SYS_openat            257
#define X64_SYS_newfstatat        262
#define X64_SYS_set_robust_list   273
#define X64_SYS_prlimit64         302
#define X64_SYS_getrandom         318

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
    if (count == 0) return 0;
    if (count > (1ULL << 20)) count = 1ULL << 20;
    std::vector<U8> buffer((size_t)count + 1);
    cpu->memory->memcpyFromGuest(buffer.data(), buf, count);
    buffer[count] = 0;
    if (fd == 1 || fd == 2) {
        // Tee stdout/stderr to host console so ld-linux + glibc diagnostics
        // surface immediately. Also forward to the kobject so anything
        // tailing the host FS still sees it.
        klog_fmt("[guest fd=%llu] %s", (unsigned long long)fd, (const char*)buffer.data());
    }
    if (!cpu->thread || !cpu->thread->process) {
        return count;
    }
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) {
        if (fd == 1 || fd == 2) return count; // already klog'd
        return (U64)-9; // -EBADF
    }
    if (!fdesc->canWrite()) {
        return (U64)-K_EINVAL;
    }
    U32 wrote = fdesc->kobject->writeNative(buffer.data(), (U32)count);
    return (S32)wrote < 0 ? (U64)(S64)(S32)wrote : (U64)wrote;
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
    U64 oldBrk = p->brkEnd64;
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
    p->brkEnd64 = newBrk;
    return newBrk;
}

// Forward decl for the file-backed path; defined below.
static U64 sys_mmap64_file(CPU64* cpu, U64 addr, U64 length, U64 prot,
                           U64 flags, U64 fd, U64 offset);

static U64 sys_mmap64(CPU64* cpu, U64 addr, U64 length, U64 prot, U64 flags, U64 fd, U64 offset) {
    if (!(flags & K_MAP_ANONYMOUS)) {
        return sys_mmap64_file(cpu, addr, length, prot, flags, fd, offset);
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

// struct utsname is 6 × 65-byte fixed strings on x86-64 Linux (390 bytes).
static U64 sys_uname64(CPU64* cpu, U64 bufAddr) {
    if (!bufAddr) return (U64)-K_EFAULT;
    char buf[6 * 65];
    std::memset(buf, 0, sizeof(buf));
    auto setField = [&](int idx, const char* s) {
        std::strncpy(buf + idx * 65, s, 64);
    };
    setField(0, "Linux");                  // sysname
    setField(1, "boxedwine64");            // nodename
    setField(2, "6.1.0-boxedwine");        // release — pretend modern kernel
    setField(3, "#1 SMP boxedwine64");     // version
    setField(4, "x86_64");                 // machine
    setField(5, "(none)");                 // domainname
    cpu->memory->memcpyToGuest(bufAddr, buf, sizeof(buf));
    return 0;
}

// getrandom — fill buffer with pseudo-random bytes. Good enough for ld.so's
// stack canary; not cryptographically strong, but glibc only needs entropy.
static U64 sys_getrandom64(CPU64* cpu, U64 bufAddr, U64 buflen, U64 /*flags*/) {
    if (!bufAddr || buflen == 0) return 0;
    if (buflen > 256) buflen = 256;
    U8 tmp[256];
    static U64 seed = 0x9E3779B97F4A7C15ULL;
    for (U64 i = 0; i < buflen; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        tmp[i] = (U8)(seed >> 33);
    }
    cpu->memory->memcpyToGuest(bufAddr, tmp, buflen);
    return buflen;
}

// prlimit64(pid, resource, new_limit*, old_limit*) — for v1 we always pretend
// "no limit" by writing RLIM64_INFINITY to old_limit if requested.
static U64 sys_prlimit64_64(CPU64* cpu, U64 /*pid*/, U64 /*res*/, U64 newLim, U64 oldLim) {
    if (oldLim) {
        // struct rlimit64 { __u64 rlim_cur; __u64 rlim_max; }
        cpu->memory->writeq(oldLim, ~0ULL);
        cpu->memory->writeq(oldLim + 8, ~0ULL);
    }
    (void)newLim;
    return 0;
}

// clock_gettime(clk, struct timespec*). Returns wall-clock from the host so
// glibc gets monotonically advancing values.
static U64 sys_clock_gettime64(CPU64* cpu, U64 /*clk*/, U64 tsAddr) {
    if (!tsAddr) return (U64)-K_EFAULT;
    U64 us = KSystem::getSystemTimeAsMicroSeconds();
    U64 sec = us / 1000000ULL;
    U64 nsec = (us % 1000000ULL) * 1000ULL;
    cpu->memory->writeq(tsAddr, sec);
    cpu->memory->writeq(tsAddr + 8, nsec);
    return 0;
}

// read/write/open/close — wired to the existing 32-bit KProcess FD table via
// a bounce buffer. The kobject->readNative path is host-pointer, so the
// 64-bit guest address never has to flow through the 32-bit memory layer.
// Reads from fd 0 still return 0 (EOF) so apps that probe stdin don't hang.
static U64 sys_read64(CPU64* cpu, U64 fd, U64 buf, U64 count) {
    if (fd == 0) return 0;
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) return (U64)-9; // -EBADF
    if (!fdesc->canRead()) return (U64)-K_EINVAL;
    if (count == 0) return 0;
    // Cap to a reasonable per-call size — ld-linux reads in 4KB-64KB chunks.
    if (count > (1ULL << 20)) count = 1ULL << 20;
    std::vector<U8> tmp((size_t)count);
    U32 got = fdesc->kobject->readNative(tmp.data(), (U32)count);
    if ((S32)got < 0) {
        return (U64)(S64)(S32)got; // sign-extend kernel errno
    }
    if (got > 0) {
        cpu->memory->memcpyToGuest(buf, tmp.data(), got);
    }
    return (U64)got;
}

// openat(dirfd, path, flags, mode). For dirfd we honour AT_FDCWD (-100) and
// any "absolute" path. Relative paths against a real dirfd aren't supported
// yet — ld-linux always passes AT_FDCWD or absolute, so this covers the
// startup path.
#ifndef K_AT_FDCWD
#define K_AT_FDCWD (-100)
#endif
static U64 sys_openat64(CPU64* cpu, U64 dirfd, U64 pathAddr, U64 flags, U64 /*mode*/) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!pathAddr) return (U64)-K_EFAULT;
    char path[1024] = {0};
    cpu->memory->memcpyFromGuest(path, pathAddr, sizeof(path) - 1);
    bool isAbs = (path[0] == '/');
    if (!isAbs && (S32)dirfd != K_AT_FDCWD) {
        klog_fmt("sys_openat64: relative path '%s' with dirfd=%d not yet supported",
                 path, (int)(S32)dirfd);
        return (U64)-2;
    }
    KProcess* process = cpu->thread->process.get();
    KFileDescriptorPtr result;
    U32 rc = process->openFile(process->currentDirectory, BString::copy(path),
                               (U32)flags, result);
    if ((S32)rc < 0) {
        klog_fmt("sys_openat64: open('%s') -> %d", path, (int)(S32)rc);
        return (U64)(S64)(S32)rc;
    }
    return (U64)result->handle;
}

static U64 sys_close64(CPU64* cpu, U64 fd) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    return (U64)(S64)(S32)cpu->thread->process->close((FD)fd);
}

// lseek — wired straight to KObject::seek.
static U64 sys_lseek64(CPU64* cpu, U64 fd, U64 offset, U64 whence) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) return (U64)-9;
    S64 off = (S64)offset;
    S64 pos;
    if (whence == 0) { // SEEK_SET
        pos = fdesc->kobject->seek(off);
    } else if (whence == 1) { // SEEK_CUR
        pos = fdesc->kobject->getPos() + off;
        pos = fdesc->kobject->seek(pos);
    } else if (whence == 2) { // SEEK_END
        pos = fdesc->kobject->length() + off;
        pos = fdesc->kobject->seek(pos);
    } else {
        return (U64)-K_EINVAL;
    }
    return (U64)pos;
}

// writeStatBuf64 — write the x86-64 Linux struct stat (144 bytes) into
// guest memory. Field offsets match the canonical glibc/kernel layout for
// __NR_fstat / __NR_newfstatat result buffers on x86-64.
static void writeStatBuf64(KMemory64* mem, U64 addr, U64 size, U32 mode,
                            U64 ino, U32 uid, U32 gid, U64 mtime) {
    U8 buf[144];
    std::memset(buf, 0, sizeof(buf));
    auto put64 = [&](U32 off, U64 v) { std::memcpy(buf + off, &v, 8); };
    auto put32 = [&](U32 off, U32 v) { std::memcpy(buf + off, &v, 4); };
    put64(0,  1);             // st_dev (fake)
    put64(8,  ino);           // st_ino
    put64(16, 1);             // st_nlink
    put32(24, mode);          // st_mode
    put32(28, uid);           // st_uid
    put32(32, gid);           // st_gid
    put32(36, 0);             // __pad0
    put64(40, 0);             // st_rdev
    put64(48, size);          // st_size
    put64(56, 4096);          // st_blksize
    put64(64, (size + 511) / 512); // st_blocks (512-byte units)
    put64(72, mtime);         // st_atime
    put64(80, 0);
    put64(88, mtime);         // st_mtime
    put64(96, 0);
    put64(104, mtime);        // st_ctime
    put64(112, 0);
    mem->memcpyToGuest(addr, buf, sizeof(buf));
}

static U64 sys_fstat64(CPU64* cpu, U64 fd, U64 statbuf) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!statbuf) return (U64)-K_EFAULT;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) return (U64)-9; // -EBADF
    // Reach for the underlying KFile to ask the FsNode for metadata.
    std::shared_ptr<KFile> kfile = std::dynamic_pointer_cast<KFile>(fdesc->kobject);
    U64 size = 0;
    U32 mode = 0100644; // S_IFREG | 0644
    U64 ino = 0;
    U64 mtime = 0;
    if (kfile && kfile->openFile) {
        size = (U64)kfile->openFile->length();
        if (kfile->openFile->node) {
            mode  = kfile->openFile->node->getMode();
            ino   = kfile->openFile->node->id;
            mtime = (U64)kfile->openFile->node->lastModified();
        }
    } else {
        // Non-file kobject (socket/pipe): claim S_IFCHR so callers don't
        // assume seekable.
        mode = 0020666;
    }
    writeStatBuf64(cpu->memory, statbuf, size, mode, ino, 1000, 1000, mtime);
    return 0;
}

// File-backed mmap. Reads the requested file region into freshly mmap'd
// pages. Not lazy/COW — eager copy is simple and bounded for ld-linux's
// typical 200 KiB lib mapping.
static U64 sys_mmap64_file(CPU64* cpu, U64 addr, U64 length, U64 prot,
                           U64 flags, U64 fd, U64 offset) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    KFileDescriptorPtr fdesc = cpu->thread->process->getFileDescriptor((FD)fd);
    if (!fdesc) return (U64)-9; // -EBADF
    std::shared_ptr<KFile> kfile = std::dynamic_pointer_cast<KFile>(fdesc->kobject);
    if (!kfile || !kfile->openFile) {
        return (U64)-K_ENOSYS;
    }
    if (addr == 0) {
        static U64 anonBump = 0x700000000ULL;
        addr = anonBump;
        anonBump += (length + 0xFFF) & ~0xFFFULL;
    }
    U64 aligned = addr & ~0xFFFULL;
    U64 mapLen = (length + (addr - aligned) + 0xFFF) & ~0xFFFULL;
    U64 mapped = cpu->memory->mmapAnonymousFixed(aligned, mapLen, (U32)prot);
    if ((S64)mapped < 0) {
        return mapped;
    }
    // Read [offset, offset+length) and copy into [addr, addr+length).
    std::vector<U8> buf((size_t)length);
    S64 saved = kfile->openFile->getFilePointer();
    kfile->openFile->seek((S64)offset);
    U32 got = kfile->openFile->readNative(buf.data(), (U32)length);
    kfile->openFile->seek(saved);
    if (got > 0) {
        cpu->memory->memcpyToGuest(addr, buf.data(), got);
    }
    (void)flags;
    return addr;
}

// writev — iterate the iovec array, calling write per segment. Each iovec
// entry on x86-64 is { u64 base; u64 len }.
static U64 sys_writev64(CPU64* cpu, U64 fd, U64 iov, U64 iovcnt) {
    U64 total = 0;
    for (U64 i = 0; i < iovcnt; i++) {
        U64 base = cpu->memory->readq(iov + i * 16 + 0);
        U64 len  = cpu->memory->readq(iov + i * 16 + 8);
        if (len == 0) continue;
        S64 wrote = (S64)sys_write64(cpu, fd, base, len);
        if (wrote < 0) {
            return total > 0 ? total : (U64)wrote;
        }
        total += (U64)wrote;
        if ((U64)wrote < len) break; // short write
    }
    return total;
}

// readlink — return -ENOENT so callers fall back; /proc/self/exe etc.
static U64 sys_readlink64(CPU64* /*cpu*/, U64 /*pathAddr*/, U64 /*buf*/, U64 /*sz*/) {
    return (U64)-2; // -ENOENT
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
        case X64_SYS_writev:
            ret = sys_writev64(cpu, a1, a2, a3);
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
        case X64_SYS_ioctl:
            // ld-linux makes these calls before main; safe to no-op.
            ret = 0;
            break;
        case X64_SYS_read:
            ret = sys_read64(cpu, a1, a2, a3);
            break;
        case X64_SYS_open:
        case X64_SYS_openat:
            // open(path, flags, mode) — same arg layout once we shift one.
            if (nr == X64_SYS_open) ret = sys_openat64(cpu, ~0ULL, a1, a2, a3);
            else                    ret = sys_openat64(cpu, a1,    a2, a3, a4);
            break;
        case X64_SYS_close:
            ret = sys_close64(cpu, a1);
            break;
        case X64_SYS_fstat:
            ret = sys_fstat64(cpu, a1, a2);
            break;
        case X64_SYS_stat:
        case X64_SYS_lstat:
        case X64_SYS_newfstatat:
            ret = (U64)-2; // path-based stat still ENOENT until we wire openFile + stat
            break;
        case X64_SYS_lseek:
            ret = sys_lseek64(cpu, a1, a2, a3);
            break;
        case X64_SYS_pread64:
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_readlink:
            ret = sys_readlink64(cpu, a1, a2, a3);
            break;
        case X64_SYS_access:
            ret = (U64)-2; // -ENOENT
            break;
        case X64_SYS_uname:
            ret = sys_uname64(cpu, a1);
            break;
        case X64_SYS_getrandom:
            ret = sys_getrandom64(cpu, a1, a2, a3);
            break;
        case X64_SYS_prlimit64:
            ret = sys_prlimit64_64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_clock_gettime:
            ret = sys_clock_gettime64(cpu, a1, a2);
            break;
        case X64_SYS_getuid:
        case X64_SYS_geteuid:
        case X64_SYS_getgid:
        case X64_SYS_getegid:
            ret = 1000; // pretend uid/gid 1000
            break;
        case X64_SYS_getpid:
        case X64_SYS_gettid:
            ret = cpu->thread ? cpu->thread->id : 1;
            break;
        case X64_SYS_futex:
            // ld-linux uses futex for lazy init; FUTEX_WAKE on uncontended
            // mutex returns 0. We'll return success and skip blocking.
            ret = 0;
            break;
        case X64_SYS_poll:
            ret = 0; // timeout — nothing ready
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
