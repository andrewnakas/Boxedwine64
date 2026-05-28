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
#define X64_SYS_madvise           28
#define X64_SYS_mremap            25
#define X64_SYS_sigaltstack       131
#define X64_SYS_rt_sigreturn      15
#define X64_SYS_dup               32
#define X64_SYS_dup2              33
#define X64_SYS_getcwd            79
#define X64_SYS_chdir             80
#define X64_SYS_fcntl             72
#define X64_SYS_pipe              22
#define X64_SYS_pipe2             293
#define X64_SYS_getdents64        217
#define X64_SYS_tgkill            234
#define X64_SYS_prlimit64         302
#define X64_SYS_getrandom         318
#define X64_SYS_sched_yield       24
#define X64_SYS_sched_setaffinity 203
#define X64_SYS_sched_getaffinity 204
#define X64_SYS_statfs            137
#define X64_SYS_fstatfs           138
#define X64_SYS_kill              62
#define X64_SYS_gettimeofday      96
#define X64_SYS_getrusage         98
#define X64_SYS_sysinfo           99
#define X64_SYS_getppid           110
#define X64_SYS_getpgrp           111
#define X64_SYS_getpgid           121
#define X64_SYS_getsid            124
#define X64_SYS_clock_getres      229
#define X64_SYS_clock_nanosleep   230
#define X64_SYS_nanosleep         35
#define X64_SYS_rseq              334
#define X64_SYS_clone3            435
#define X64_SYS_eventfd2          290
#define X64_SYS_epoll_create1     291
#define X64_SYS_epoll_ctl         233
#define X64_SYS_epoll_wait        232
#define X64_SYS_pwrite64          18
#define X64_SYS_readv             19
#define X64_SYS_select            23
#define X64_SYS_chmod             90
#define X64_SYS_fchmod            91
#define X64_SYS_clone             56
#define X64_SYS_wait4             61
#define X64_SYS_pause             34
#define X64_SYS_getitimer         36
#define X64_SYS_setitimer         38
#define X64_SYS_rt_sigpending     127
#define X64_SYS_rt_sigtimedwait   128
#define X64_SYS_rt_sigqueueinfo   129
#define X64_SYS_rt_sigsuspend     130

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
#ifndef K_EAGAIN
#define K_EAGAIN 11
#endif
#ifndef K_ENOMEM
#define K_ENOMEM 12
#endif
#ifndef K_ECHILD
#define K_ECHILD 10
#endif
#ifndef K_EINTR
#define K_EINTR 4
#endif
#ifndef K_EBADF
#define K_EBADF 9
#endif
#ifndef K_EMFILE
#define K_EMFILE 24
#endif
#ifndef K_ESRCH
#define K_ESRCH 3
#endif

// FUTEX_* op codes from <linux/futex.h>. We handle WAIT/WAKE + their
// BITSET variants (glibc 2.35+ uses WAKE_BITSET for pthread_cond_signal)
// and recognize REQUEUE/CMP_REQUEUE/WAKE_OP so we don't reject them with
// EINVAL — they degrade to "no waiters woken" which is correct for our
// single-threaded world. The CLOCK_REALTIME bit (0x100) is ignored
// because we don't block anyway.
#define X64_FUTEX_WAIT             0
#define X64_FUTEX_WAKE             1
#define X64_FUTEX_REQUEUE          3
#define X64_FUTEX_CMP_REQUEUE      4
#define X64_FUTEX_WAKE_OP          5
#define X64_FUTEX_WAIT_BITSET      9
#define X64_FUTEX_WAKE_BITSET      10
#define X64_FUTEX_PRIVATE_FLAG     128
#define X64_FUTEX_CLOCK_REALTIME   256
#define X64_FUTEX_WAIT_PRIVATE     (X64_FUTEX_WAIT | X64_FUTEX_PRIVATE_FLAG)
#define X64_FUTEX_WAKE_PRIVATE     (X64_FUTEX_WAKE | X64_FUTEX_PRIVATE_FLAG)

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

// Forward decl — sys_newfstatat64 may delegate to sys_fstat64 for AT_EMPTY_PATH.
static U64 sys_fstat64(CPU64* cpu, U64 fd, U64 statbuf);

// Path-based stat shared by stat/lstat/newfstatat. followSymlink controls
// the lstat vs stat distinction (Fs::getNodeFromLocalPath's third arg).
static U64 sys_stat_path64(CPU64* cpu, U64 pathAddr, U64 statbuf, bool followSymlink) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!pathAddr || !statbuf) return (U64)-K_EFAULT;
    char path[1024] = {0};
    cpu->memory->memcpyFromGuest(path, pathAddr, sizeof(path) - 1);
    BString bpath = BString::copy(path);
    std::shared_ptr<FsNode> node = Fs::getNodeFromLocalPath(
        cpu->thread->process->currentDirectory, bpath, followSymlink);
    if (!node) return (U64)-2; // -ENOENT
    U64 size  = node->length();
    U32 mode  = node->getMode();
    U64 ino   = node->id;
    U64 mtime = node->lastModified() / 1000; // ms → seconds
    writeStatBuf64(cpu->memory, statbuf, size, mode, ino, 1000, 1000, mtime);
    return 0;
}

// newfstatat(dirfd, path, statbuf, flags). AT_EMPTY_PATH (0x1000) means
// "stat the dirfd itself"; AT_SYMLINK_NOFOLLOW (0x100) makes it lstat.
#ifndef K_AT_SYMLINK_NOFOLLOW
#define K_AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef K_AT_EMPTY_PATH
#define K_AT_EMPTY_PATH 0x1000
#endif
static U64 sys_newfstatat64(CPU64* cpu, U64 dirfd, U64 pathAddr, U64 statbuf, U64 flags) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!statbuf) return (U64)-K_EFAULT;
    // AT_EMPTY_PATH with NULL/"" path means stat the fd.
    if ((flags & K_AT_EMPTY_PATH) && (!pathAddr || cpu->memory->readb(pathAddr) == 0)) {
        return sys_fstat64(cpu, dirfd, statbuf);
    }
    // We only honour AT_FDCWD or absolute paths for now (matches openat).
    char path[1024] = {0};
    cpu->memory->memcpyFromGuest(path, pathAddr, sizeof(path) - 1);
    bool isAbs = (path[0] == '/');
    if (!isAbs && (S32)dirfd != K_AT_FDCWD) {
        return (U64)-2;
    }
    return sys_stat_path64(cpu, pathAddr, statbuf, !(flags & K_AT_SYMLINK_NOFOLLOW));
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

// readlink(path, buf, bufsize) — resolve a symlink. /proc/self/exe is the
// big-ticket caller for glibc startup; everything else falls back to the
// FsNode link field if present.
static U64 sys_readlink64(CPU64* cpu, U64 pathAddr, U64 buf, U64 sz) {
    if (!cpu->thread || !cpu->thread->process) return (U64)-K_ENOSYS;
    if (!pathAddr || !buf || sz == 0) return (U64)-K_EFAULT;
    char path[1024] = {0};
    cpu->memory->memcpyFromGuest(path, pathAddr, sizeof(path) - 1);
    BString resolved;
    if (std::strcmp(path, "/proc/self/exe") == 0 || std::strcmp(path, "/proc/thread-self/exe") == 0) {
        resolved = cpu->thread->process->exe;
    } else {
        std::shared_ptr<FsNode> n = Fs::getNodeFromLocalPath(
            cpu->thread->process->currentDirectory, BString::copy(path), false);
        if (!n || !n->isLink()) return (U64)-22; // -EINVAL on non-symlink
        resolved = n->link;
    }
    U64 toCopy = (U64)resolved.length();
    if (toCopy > sz) toCopy = sz;
    cpu->memory->memcpyToGuest(buf, resolved.c_str(), toCopy);
    return toCopy;
}

// Futex with real per-address waiter bookkeeping.
//
// We still cannot truly block (no KThread64 yet), so WAIT returns -EAGAIN
// even when the word matches. But we DO count would-block events per
// address in cpu->futexWaiters, and WAKE drains up to `val` of them and
// returns the count actually drained — matching the kernel's "number
// woken" semantics. The difference from the old stub:
//
//   - A WAKE that follows a WAIT-on-match now returns 1, not 0, which
//     lets glibc's condvar fast path see that its broadcast wasn't a
//     no-op (the old behaviour caused some __pthread_cond_signal paths
//     to loop forever calling FUTEX_WAKE expecting >0).
//   - Waiter counts are addressable per `uaddr`, so WAKE on one futex
//     does not spuriously claim to wake waiters on another.
//
// Storage is freed automatically when count drops to 0 (erase from map).
// Map lives on CPU64 so it's process-scoped — exactly what FUTEX_PRIVATE
// expects. When multi-thread support lands, the map moves to KProcess64
// with no shape change.
//
//   WAIT / WAIT_BITSET (+ _PRIVATE): read 32-bit word at uaddr; if
//       != val return -EAGAIN immediately. If == val, bump waiter count
//       and ALSO return -EAGAIN (would-block placeholder). The bumped
//       count is consumed by the next WAKE on the same address.
//   WAKE / WAKE_BITSET (+ _PRIVATE): drain min(val, count) from the
//       waiter slot for uaddr and return that drain count.
//   REQUEUE / CMP_REQUEUE / WAKE_OP: 0 (no kernel-side migration yet).
//   Anything else: -ENOSYS so glibc takes the user-space fallback.
//
// uaddr==0 → -EFAULT for all ops (matches kernel).
static U64 sys_futex64(CPU64* cpu, U64 uaddr, U32 op, U32 val) {
    if (uaddr == 0) return (U64)-K_EFAULT;
    U32 baseOp = op & ~(X64_FUTEX_PRIVATE_FLAG | X64_FUTEX_CLOCK_REALTIME);
    switch (baseOp) {
        case X64_FUTEX_WAKE:
        case X64_FUTEX_WAKE_BITSET: {
            auto it = cpu->futexWaiters.find(uaddr);
            if (it == cpu->futexWaiters.end()) return 0;
            U32 toDrain = it->second < val ? it->second : val;
            it->second -= toDrain;
            if (it->second == 0) cpu->futexWaiters.erase(it);
            return toDrain;
        }
        case X64_FUTEX_WAIT:
        case X64_FUTEX_WAIT_BITSET: {
            if (!cpu->memory) return (U64)-K_EFAULT;
            U32 cur = cpu->memory->readd(uaddr);
            if (cur != val) return (U64)-K_EAGAIN;
            // Word matches — in a real kernel we'd park here. Record the
            // would-block so a subsequent WAKE returns 1 instead of 0,
            // then return EAGAIN so the (single-threaded) caller makes
            // forward progress via retry.
            cpu->futexWaiters[uaddr] += 1;
            return (U64)-K_EAGAIN;
        }
        case X64_FUTEX_REQUEUE:
        case X64_FUTEX_CMP_REQUEUE:
        case X64_FUTEX_WAKE_OP:
            return 0;
        default:
            return (U64)-K_ENOSYS;
    }
}

// rt_sigtimedwait(set, info, timeout, sigsetsize) — block till a signal in
// `set` arrives or `timeout` elapses. With no delivery implemented, the
// only correct non-hanging answer is -EAGAIN (timeout). Validate sigsetsize
// for the standard 8-byte mask first.
static U64 sys_rt_sigtimedwait64(CPU64* /*cpu*/, U64 setPtr, U64 /*infoPtr*/,
                                  U64 /*timeoutPtr*/, U64 sigsetsize) {
    if (sigsetsize != 8) return (U64)-K_EINVAL;
    if (setPtr == 0) return (U64)-K_EFAULT;
    // No signals pending; timeout result.
    return (U64)-K_EAGAIN;
}

// rt_sigsuspend(mask, sigsetsize) — replace mask, sleep till signal, restore.
// With no delivery, returning -EINTR forces glibc to retry the surrounding
// loop (correct for poll/select-on-signal idioms).
static U64 sys_rt_sigsuspend64(CPU64* /*cpu*/, U64 maskPtr, U64 sigsetsize) {
    if (sigsetsize != 8) return (U64)-K_EINVAL;
    if (maskPtr == 0) return (U64)-K_EFAULT;
    return (U64)-K_EINTR;
}

// rt_sigpending(set, sigsetsize) — write currently-pending signal mask.
// Nothing is ever pending in our world; write zeros and succeed.
static U64 sys_rt_sigpending64(CPU64* cpu, U64 setPtr, U64 sigsetsize) {
    if (sigsetsize != 8) return (U64)-K_EINVAL;
    if (setPtr == 0) return (U64)-K_EFAULT;
    if (!cpu->memory) return (U64)-K_EFAULT;
    cpu->memory->writeq(setPtr, 0);
    return 0;
}

// rt_sigaction(2) — storage-only round-trip. We do not yet deliver signals
// to guest handlers (that needs the signal-frame builder of Milestone B),
// but glibc's startup *registers* SIGFPE/SIGSEGV/SIGPIPE handlers very
// early and later queries them; previously this was a bald no-op so the
// second sigaction(SIG, NULL, &old) call always reported SIG_DFL, which
// confuses libpthread's "did the user install a handler?" check.
//
// x86-64 `struct kernel_sigaction` layout (32 bytes):
//   off  0: sa_handler  (8)
//   off  8: sa_flags    (8)
//   off 16: sa_restorer (8)
//   off 24: sa_mask     (8, = full sigset_t on x86-64)
//
// Signal numbers 1..64 are valid; SIGKILL(9) and SIGSTOP(19) can be queried
// but cannot have their handlers changed — we accept the read and silently
// drop the write, matching kernel behaviour.
static U64 sys_rt_sigaction64(CPU64* cpu, U64 sig, U64 actPtr, U64 oldActPtr,
                              U64 sigsetsize) {
    if (sig < 1 || sig > 64) return (U64)-K_EINVAL;
    if (sigsetsize != 8) return (U64)-K_EINVAL; // x86-64 sigset_t is 8 bytes
    if (!cpu->memory) return (U64)-K_EFAULT;

    CPU64::SigAction& slot = cpu->sigActions[sig];

    if (oldActPtr) {
        cpu->memory->writeq(oldActPtr + 0,  slot.installed ? slot.handler  : 0);
        cpu->memory->writeq(oldActPtr + 8,  slot.installed ? slot.flags    : 0);
        cpu->memory->writeq(oldActPtr + 16, slot.installed ? slot.restorer : 0);
        cpu->memory->writeq(oldActPtr + 24, slot.installed ? slot.mask     : 0);
    }

    if (actPtr) {
        if (sig == 9 || sig == 19) {
            // SIGKILL / SIGSTOP: read accepted, write ignored — matches Linux.
            return 0;
        }
        slot.handler  = cpu->memory->readq(actPtr + 0);
        slot.flags    = cpu->memory->readq(actPtr + 8);
        slot.restorer = cpu->memory->readq(actPtr + 16);
        slot.mask     = cpu->memory->readq(actPtr + 24);
        slot.installed = true;
    }
    return 0;
}

// rt_sigprocmask(2) — storage-only round-trip, paired with rt_sigaction
// above. how=SIG_BLOCK(0)/SIG_UNBLOCK(1)/SIG_SETMASK(2). v1 enforces nothing
// at delivery time (no delivery yet) but lets pthread_sigmask round-trip
// without losing state, which libpthread queries during thread init.
#define X64_SIG_BLOCK   0
#define X64_SIG_UNBLOCK 1
#define X64_SIG_SETMASK 2

static U64 sys_rt_sigprocmask64(CPU64* cpu, U64 how, U64 setPtr, U64 oldSetPtr,
                                U64 sigsetsize) {
    if (sigsetsize != 8) return (U64)-K_EINVAL;
    if (!cpu->memory) return (U64)-K_EFAULT;

    if (oldSetPtr) {
        cpu->memory->writeq(oldSetPtr, cpu->sigMask);
    }
    if (setPtr) {
        U64 incoming = cpu->memory->readq(setPtr);
        // SIGKILL(9) and SIGSTOP(19) can never be blocked — strip them
        // from any incoming mask to match kernel behaviour.
        U64 kernelStrip = (1ULL << (9 - 1)) | (1ULL << (19 - 1));
        incoming &= ~kernelStrip;
        switch (how) {
            case X64_SIG_BLOCK:   cpu->sigMask |=  incoming; break;
            case X64_SIG_UNBLOCK: cpu->sigMask &= ~incoming; break;
            case X64_SIG_SETMASK: cpu->sigMask  =  incoming; break;
            default: return (U64)-K_EINVAL;
        }
    }
    return 0;
}

// sigaltstack(2) — storage-only round-trip, completes the sig{action,
// procmask, altstack} trio. Layout of stack_t on x86-64 (24 bytes):
//   off  0: ss_sp    (8)
//   off  8: ss_flags (4)
//   off 12: pad      (4)
//   off 16: ss_size  (8)
//
// Kernel rules we honour here:
//   - If oldss != NULL, write the current state first.
//   - If ss != NULL with SS_DISABLE(2): clear the registration.
//   - If ss != NULL otherwise: ss_flags must be 0 (or SS_AUTODISARM=0x80000000)
//     and ss_size must be >= MINSIGSTKSZ (~2048). Reject otherwise.
//   - Cannot change altstack while currently executing on it (SS_ONSTACK
//     flag set). We don't track that yet, so the check is skipped.
#define X64_SS_ONSTACK     1
#define X64_SS_DISABLE     2
#define X64_SS_AUTODISARM  0x80000000u
#define X64_MINSIGSTKSZ    2048

static U64 sys_sigaltstack64(CPU64* cpu, U64 ssPtr, U64 oldSsPtr) {
    if (!cpu->memory) return (U64)-K_EFAULT;

    if (oldSsPtr) {
        cpu->memory->writeq(oldSsPtr + 0,  cpu->sigAltStack.ssSp);
        cpu->memory->writed(oldSsPtr + 8,  cpu->sigAltStack.ssFlags);
        cpu->memory->writed(oldSsPtr + 12, 0);
        cpu->memory->writeq(oldSsPtr + 16, cpu->sigAltStack.ssSize);
    }

    if (ssPtr) {
        U64 sp    = cpu->memory->readq(ssPtr + 0);
        U32 flags = cpu->memory->readd(ssPtr + 8);
        U64 size  = cpu->memory->readq(ssPtr + 16);

        if (flags & X64_SS_DISABLE) {
            cpu->sigAltStack.ssSp    = 0;
            cpu->sigAltStack.ssFlags = X64_SS_DISABLE;
            cpu->sigAltStack.ssSize  = 0;
        } else {
            U32 allowed = X64_SS_AUTODISARM;
            if (flags & ~allowed) return (U64)-K_EINVAL;
            if (size < X64_MINSIGSTKSZ) return (U64)-K_ENOMEM;
            cpu->sigAltStack.ssSp    = sp;
            cpu->sigAltStack.ssFlags = flags;
            cpu->sigAltStack.ssSize  = size;
        }
    }
    return 0;
}

// sched_getaffinity(2): glibc + libgomp probe this very early to decide thread
// pool sizes. We expose exactly one CPU. The Linux signature is unusual:
//   ssize_t sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask)
// On success returns the number of bytes the kernel actually wrote (clamped
// to cpusetsize, but must be a multiple of sizeof(long)=8). cpusetsize must
// be a multiple of sizeof(long) and >= 8.
//
// We write 8 bytes (one U64 with bit 0 set) and return 8. Userspace then
// counts the bits to get nproc, which is exactly what we want it to see.
static U64 sys_sched_getaffinity64(CPU64* cpu, U64 pid, U64 cpusetsize, U64 maskPtr) {
    (void)pid; // ignore — we model one process
    if (cpusetsize == 0 || (cpusetsize & 7)) return (U64)-K_EINVAL;
    if (!maskPtr) return (U64)-K_EFAULT;
    if (!cpu->memory) return (U64)-K_EFAULT;

    // Write 1 in the low qword (CPU 0 is in our affinity set), zero the rest
    // up to cpusetsize. Userspace inspects only the bytes the kernel wrote
    // (which is our return value), so 8 bytes is enough.
    cpu->memory->writeq(maskPtr, 1);
    for (U64 off = 8; off < cpusetsize && off < 1024; off += 8) {
        cpu->memory->writeq(maskPtr + off, 0);
    }
    return 8;
}

// sched_setaffinity(2): silently accept any mask — we don't actually move
// the thread anywhere, but reject obviously bogus calls.
static U64 sys_sched_setaffinity64(CPU64* cpu, U64 pid, U64 cpusetsize, U64 maskPtr) {
    (void)cpu; (void)pid; (void)maskPtr;
    if (cpusetsize == 0 || (cpusetsize & 7)) return (U64)-K_EINVAL;
    return 0;
}

// statfs(2) / fstatfs(2) — fixed-shape stub. Layout of x86-64 struct statfs
// (120 bytes, all fields long-sized):
//   off  0: f_type        off 56: f_fsid (8)
//   off  8: f_bsize       off 64: f_namelen
//   off 16: f_blocks      off 72: f_frsize
//   off 24: f_bfree       off 80: f_flags
//   off 32: f_bavail      off 88..119: f_spare[4]
//   off 40: f_files
//   off 48: f_ffree
//
// glibc's dynamic loader probes statfs("/proc"), the system's ld.so.cache
// directory, etc. The load-bearing fields are f_type (to know "is this
// tmpfs/proc/etc?"), f_bsize (cache alignment), and f_namelen (path
// canonicalisation). We claim tmpfs (0x01021994) with 4096-byte blocks
// and 255-byte filenames, zero free/total — glibc tolerates the lie.
#define X64_TMPFS_MAGIC 0x01021994

static U64 sys_statfs64_common(CPU64* cpu, U64 bufPtr) {
    if (!bufPtr) return (U64)-K_EFAULT;
    if (!cpu->memory) return (U64)-K_EFAULT;
    cpu->memory->writeq(bufPtr +  0, X64_TMPFS_MAGIC); // f_type
    cpu->memory->writeq(bufPtr +  8, 4096);            // f_bsize
    cpu->memory->writeq(bufPtr + 16, 0);               // f_blocks
    cpu->memory->writeq(bufPtr + 24, 0);               // f_bfree
    cpu->memory->writeq(bufPtr + 32, 0);               // f_bavail
    cpu->memory->writeq(bufPtr + 40, 0);               // f_files
    cpu->memory->writeq(bufPtr + 48, 0);               // f_ffree
    cpu->memory->writeq(bufPtr + 56, 0);               // f_fsid
    cpu->memory->writeq(bufPtr + 64, 255);             // f_namelen
    cpu->memory->writeq(bufPtr + 72, 4096);            // f_frsize
    cpu->memory->writeq(bufPtr + 80, 0);               // f_flags
    cpu->memory->writeq(bufPtr + 88, 0);
    cpu->memory->writeq(bufPtr + 96, 0);
    cpu->memory->writeq(bufPtr + 104, 0);
    cpu->memory->writeq(bufPtr + 112, 0);
    return 0;
}

// ============================================================================
// Signal-frame layout (Linux x86-64 rt_sigframe / ucontext_t).
// ============================================================================
//
// When the kernel delivers a signal it pushes this structure on the user
// stack and sets RIP = handler, RDI = signo, RSI = &siginfo, RDX = &uctx.
// rt_sigreturn(15) reverses it: read the saved cpu state out of the frame
// at the current RSP and restore.
//
// We use the Linux-glibc layout so any handler compiled for x86-64 Linux
// can read it directly. Field offsets and sizes:
//
//   off  +0   rt_sigframe header                       (8 bytes pretty + pad)
//             — actually just the saved restorer addr (8 bytes), kernel
//               relies on the handler to RET into it; we always set RIP
//               from the saved RIP in mcontext on sigreturn so the header
//               is informational only.
//   off  +8   siginfo_t (128 bytes)
//   off +136  ucontext_t
//      +0     uc_flags          (8)
//      +8     uc_link           (8)
//      +16    uc_stack          (24: ss_sp, ss_flags+pad, ss_size)
//      +40    uc_mcontext.gregs[23]  — see gregs index table below
//                                       (23 * 8 = 184 bytes)
//      +224   uc_mcontext.fpregs (8 — pointer; we set 0 → no FPU restore)
//      +232   uc_mcontext.__reserved[8]  (64 bytes — zeroed)
//      +296   uc_sigmask        (8 — single qword, rest of 128-byte sigset
//                                    region is zero-padded)
//      +424   __fpregs_mem      (we don't write this for now)
//
// Total frame size we allocate on the stack: 16-aligned, round up to 1024.
// (Linux's actual frame is ~600 bytes with fpregs_mem; we shrink because
// we never populate fpregs.)
//
// gregs[] index, matching <sys/ucontext.h> REG_* enum:
//   0:R8   1:R9   2:R10  3:R11  4:R12  5:R13  6:R14  7:R15
//   8:RDI  9:RSI 10:RBP 11:RBX 12:RDX 13:RAX 14:RCX 15:RSP
//  16:RIP 17:EFL 18:CSGSFS 19:ERR 20:TRAPNO 21:OLDMASK 22:CR2
#define X64_SIGFRAME_SIZE          1024
#define X64_UCONTEXT_OFF_IN_FRAME  136   // after siginfo
#define X64_MCONTEXT_OFF_IN_UCTX    40
#define X64_GREGS_OFF_IN_UCTX       X64_MCONTEXT_OFF_IN_UCTX
#define X64_SIGMASK_OFF_IN_UCTX    296

// Indices into mcontext.gregs[23].
enum X64Greg {
    X64_GREG_R8 = 0, X64_GREG_R9, X64_GREG_R10, X64_GREG_R11,
    X64_GREG_R12, X64_GREG_R13, X64_GREG_R14, X64_GREG_R15,
    X64_GREG_RDI, X64_GREG_RSI, X64_GREG_RBP, X64_GREG_RBX,
    X64_GREG_RDX, X64_GREG_RAX, X64_GREG_RCX, X64_GREG_RSP,
    X64_GREG_RIP, X64_GREG_EFL, X64_GREG_CSGSFS, X64_GREG_ERR,
    X64_GREG_TRAPNO, X64_GREG_OLDMASK, X64_GREG_CR2,
};

// Build a signal frame on the stack at `framePtr` capturing cpu's full
// gpr/rflags/rip/sigmask state. Caller has already aligned the stack and
// reserved X64_SIGFRAME_SIZE bytes. Returns the address of the ucontext_t
// within the frame (handler receives this as RDX).
static U64 buildSignalFrame(CPU64* cpu, U64 framePtr) {
    // Zero the entire frame first (siginfo, padding, gaps).
    for (U64 i = 0; i < X64_SIGFRAME_SIZE; i += 8) {
        cpu->memory->writeq(framePtr + i, 0);
    }
    U64 uctxPtr   = framePtr + X64_UCONTEXT_OFF_IN_FRAME;
    U64 mctxPtr   = uctxPtr  + X64_MCONTEXT_OFF_IN_UCTX;
    U64 gregsPtr  = mctxPtr; // gregs starts at mcontext
    // uc_stack: copy the registered altstack so the handler can re-arm if needed.
    cpu->memory->writeq(uctxPtr + 16, cpu->sigAltStack.ssSp);
    cpu->memory->writed(uctxPtr + 24, cpu->sigAltStack.ssFlags);
    cpu->memory->writeq(uctxPtr + 32, cpu->sigAltStack.ssSize);
    // gregs[]
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R8,  cpu->reg[X64_R8].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R9,  cpu->reg[X64_R9].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R10, cpu->reg[X64_R10].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R11, cpu->reg[X64_R11].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R12, cpu->reg[X64_R12].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R13, cpu->reg[X64_R13].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R14, cpu->reg[X64_R14].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_R15, cpu->reg[X64_R15].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RDI, cpu->reg[X64_RDI].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RSI, cpu->reg[X64_RSI].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RBP, cpu->reg[X64_RBP].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RBX, cpu->reg[X64_RBX].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RDX, cpu->reg[X64_RDX].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RAX, cpu->reg[X64_RAX].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RCX, cpu->reg[X64_RCX].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RSP, cpu->reg[X64_RSP].u64);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_RIP, cpu->rip);
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_EFL, (U64)cpu->rflags);
    // CSGSFS: low 16 = CS (we don't model segments → 0x33 USER_CS),
    //         next 16 = GS, next 16 = FS, top 16 = ss. Approximate.
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_CSGSFS,
                        (U64)0x33 | ((U64)0x2B << 48));
    // ERR/TRAPNO/CR2 = 0 (no fault triggered this delivery)
    // OLDMASK = caller's sigmask before this delivery (we set it to current)
    cpu->memory->writeq(gregsPtr + 8 * X64_GREG_OLDMASK, cpu->sigMask);
    // fpregs pointer = 0 (we don't snapshot XMM/x87 here yet — the next
    // increment after delivery wiring lands)
    cpu->memory->writeq(mctxPtr + 184, 0);
    // uc_sigmask
    cpu->memory->writeq(uctxPtr + X64_SIGMASK_OFF_IN_UCTX, cpu->sigMask);
    return uctxPtr;
}

// Restore cpu state from a signal frame whose ucontext_t lives at the
// current RSP. The kernel's invariant after rt_sigreturn is that RSP
// points one byte *past* the saved-rsp slot (i.e. the frame has been
// popped); we read everything we need, then write cpu->rip and cpu->reg
// values en masse. mcontext lives at RSP + (X64_MCONTEXT_OFF_IN_UCTX).
//
// Returns 0 on success, negative errno otherwise. The kernel actually
// returns the saved RAX as the syscall result so the user code sees the
// pre-signal RAX restored; we mirror that by writing RAX last and
// signalling "skip the normal RAX-as-return-value path" via a special
// sentinel (the caller checks cpu->yield).
static U64 restoreSignalFrame(CPU64* cpu, U64 uctxPtr) {
    U64 mctxPtr   = uctxPtr + X64_MCONTEXT_OFF_IN_UCTX;
    U64 gregsPtr  = mctxPtr;
    cpu->reg[X64_R8].u64  = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R8);
    cpu->reg[X64_R9].u64  = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R9);
    cpu->reg[X64_R10].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R10);
    cpu->reg[X64_R11].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R11);
    cpu->reg[X64_R12].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R12);
    cpu->reg[X64_R13].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R13);
    cpu->reg[X64_R14].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R14);
    cpu->reg[X64_R15].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_R15);
    cpu->reg[X64_RDI].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RDI);
    cpu->reg[X64_RSI].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RSI);
    cpu->reg[X64_RBP].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RBP);
    cpu->reg[X64_RBX].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RBX);
    cpu->reg[X64_RDX].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RDX);
    cpu->reg[X64_RCX].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RCX);
    cpu->reg[X64_RSP].u64 = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RSP);
    cpu->rip              = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RIP);
    cpu->rflags           = (U32)cpu->memory->readq(gregsPtr + 8 * X64_GREG_EFL);
    cpu->sigMask          = cpu->memory->readq(uctxPtr   + X64_SIGMASK_OFF_IN_UCTX);
    // RAX is restored last — the kernel returns RAX as the syscall return
    // value so the user-visible RAX is the *pre-signal* value, not whatever
    // rt_sigreturn would compute.
    U64 savedRax = cpu->memory->readq(gregsPtr + 8 * X64_GREG_RAX);
    return savedRax;
}

// SA_ONSTACK = 0x08000000 in glibc's <bits/sigaction.h>. When set, the
// handler runs on the registered sigaltstack instead of the user stack.
#define X64_SA_ONSTACK   0x08000000

// Synchronously deliver `sig` to `cpu` (the same thread). Models the
// kernel's "queue a signal that's pending immediately" path for the
// single-thread case — i.e. raise(), abort(), pthread_kill(self,...).
//
// Caller has already validated the signal number and confirmed that
// cpu->sigActions[sig].installed && handler is a real function pointer
// (not SIG_DFL/SIG_IGN).
//
// Mechanics:
//   1. Pick the frame stack: altstack if SA_ONSTACK set and altstack is
//      live, otherwise current RSP minus a redzone (128 bytes per SysV).
//   2. Round down to 16-byte alignment, subtract frame size.
//   3. Build the ucontext_t at frame_base via buildSignalFrame.
//   4. Set cpu->reg[X64_RSP] = frame_base, RIP = handler, RDI = sig,
//      RSI = 0 (no siginfo), RDX = &uctx, R10 = 0, R8/R9 = 0.
//   5. Mask the signal in cpu->sigMask for the duration of handler
//      execution (kernel adds sig to mask unless SA_NODEFER set).
//   6. The handler ends with `ret`-to-restorer or `syscall(rt_sigreturn)`,
//      which (a) hits our rt_sigreturn case → restoreSignalFrame → state
//      restored, including sigMask.
//
// Returns true on successful delivery (handler will run next), false if
// the handler slot is not installed / is SIG_DFL/SIG_IGN.
static bool deliverSignalSync(CPU64* cpu, U32 sig) {
    if (sig < 1 || sig > 64) return false;
    CPU64::SigAction& sa = cpu->sigActions[sig];
    if (!sa.installed) return false;
    if (sa.handler == 0 /*SIG_DFL*/ || sa.handler == 1 /*SIG_IGN*/) return false;

    // Pick stack: SA_ONSTACK requires a non-DISABLED altstack.
    U64 baseSp;
    if ((sa.flags & X64_SA_ONSTACK) && cpu->sigAltStack.ssSp != 0 &&
        (cpu->sigAltStack.ssFlags & 2 /*SS_DISABLE*/) == 0) {
        baseSp = cpu->sigAltStack.ssSp + cpu->sigAltStack.ssSize;
    } else {
        baseSp = cpu->reg[X64_RSP].u64 - 128; // red zone
    }
    // 16-align then reserve the frame.
    U64 frameBase = (baseSp - X64_SIGFRAME_SIZE) & ~(U64)15;

    U64 uctxPtr = buildSignalFrame(cpu, frameBase);

    // Mask the signal during handler execution (unless SA_NODEFER=0x40000000).
    if ((sa.flags & 0x40000000) == 0) {
        cpu->sigMask |= (1ULL << (sig - 1));
    }
    cpu->sigMask |= sa.mask;

    // Hand off to the handler.
    cpu->reg[X64_RSP].setU64(uctxPtr);  // sigreturn reads from RSP
    cpu->reg[X64_RDI].setU64(sig);      // arg1: signal number
    cpu->reg[X64_RSI].setU64(0);        // arg2: siginfo (we don't synth one)
    cpu->reg[X64_RDX].setU64(uctxPtr);  // arg3: ucontext pointer
    cpu->rip = sa.handler;

    return true;
}

// Map an x86-64 Linux syscall number to a human-readable name. Used only by
// the unimplemented-syscall log path — when running real glibc binaries, the
// first thing you want to see is "which syscall is missing", not "#291".
// Covers both syscalls we already handle (useful for trace) and the obvious
// gaps that are most likely to surface during early Wine/glibc bring-up.
// Returns "?" for unknown numbers; the caller still logs the raw #N.
static const char* x64SyscallName(U64 nr) {
    switch (nr) {
        case 0: return "read";
        case 1: return "write";
        case 2: return "open";
        case 3: return "close";
        case 4: return "stat";
        case 5: return "fstat";
        case 6: return "lstat";
        case 7: return "poll";
        case 8: return "lseek";
        case 9: return "mmap";
        case 10: return "mprotect";
        case 11: return "munmap";
        case 12: return "brk";
        case 13: return "rt_sigaction";
        case 14: return "rt_sigprocmask";
        case 15: return "rt_sigreturn";
        case 16: return "ioctl";
        case 17: return "pread64";
        case 18: return "pwrite64";
        case 19: return "readv";
        case 20: return "writev";
        case 21: return "access";
        case 22: return "pipe";
        case 23: return "select";
        case 24: return "sched_yield";
        case 25: return "mremap";
        case 28: return "madvise";
        case 32: return "dup";
        case 33: return "dup2";
        case 34: return "pause";
        case 35: return "nanosleep";
        case 36: return "getitimer";
        case 38: return "setitimer";
        case 39: return "getpid";
        case 41: return "socket";
        case 42: return "connect";
        case 43: return "accept";
        case 44: return "sendto";
        case 45: return "recvfrom";
        case 46: return "sendmsg";
        case 47: return "recvmsg";
        case 53: return "socketpair";
        case 56: return "clone";
        case 57: return "fork";
        case 58: return "vfork";
        case 59: return "execve";
        case 60: return "exit";
        case 61: return "wait4";
        case 62: return "kill";
        case 63: return "uname";
        case 72: return "fcntl";
        case 79: return "getcwd";
        case 80: return "chdir";
        case 89: return "readlink";
        case 90: return "chmod";
        case 91: return "fchmod";
        case 96: return "gettimeofday";
        case 98: return "getrusage";
        case 99: return "sysinfo";
        case 102: return "getuid";
        case 104: return "getgid";
        case 107: return "geteuid";
        case 108: return "getegid";
        case 110: return "getppid";
        case 111: return "getpgrp";
        case 121: return "getpgid";
        case 124: return "getsid";
        case 127: return "rt_sigpending";
        case 128: return "rt_sigtimedwait";
        case 129: return "rt_sigqueueinfo";
        case 130: return "rt_sigsuspend";
        case 131: return "sigaltstack";
        case 137: return "statfs";
        case 138: return "fstatfs";
        case 158: return "arch_prctl";
        case 186: return "gettid";
        case 202: return "futex";
        case 203: return "sched_setaffinity";
        case 204: return "sched_getaffinity";
        case 217: return "getdents64";
        case 218: return "set_tid_address";
        case 228: return "clock_gettime";
        case 229: return "clock_getres";
        case 230: return "clock_nanosleep";
        case 231: return "exit_group";
        case 232: return "epoll_wait";
        case 233: return "epoll_ctl";
        case 234: return "tgkill";
        case 257: return "openat";
        case 262: return "newfstatat";
        case 273: return "set_robust_list";
        case 290: return "eventfd2";
        case 291: return "epoll_create1";
        case 293: return "pipe2";
        case 302: return "prlimit64";
        case 318: return "getrandom";
        case 334: return "rseq";
        case 435: return "clone3";
        default:  return "?";
    }
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
        case X64_SYS_rt_sigaction:
            ret = sys_rt_sigaction64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_rt_sigprocmask:
            ret = sys_rt_sigprocmask64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_sigaltstack:
            ret = sys_sigaltstack64(cpu, a1, a2);
            break;
        case X64_SYS_set_robust_list:
        case X64_SYS_ioctl:
        case X64_SYS_madvise:
        case X64_SYS_chdir:
            // ld-linux makes these calls before main; safe to no-op.
            ret = 0;
            break;
        case X64_SYS_mremap:
            // mremap(old, oldlen, newlen, flags, newaddr). v1: return old
            // address — glibc malloc only resizes when MREMAP_MAYMOVE is set
            // and tolerates a noop if no growth happens. This is wrong but
            // surfaces obvious failures at the malloc level, not later.
            ret = a1;
            break;
        case X64_SYS_dup:
            if (cpu->thread && cpu->thread->process) {
                U32 newFd = cpu->thread->process->dup((U32)a1);
                ret = (S32)newFd < 0 ? (U64)(S64)(S32)newFd : (U64)newFd;
            } else {
                ret = (U64)-K_ENOSYS;
            }
            break;
        case X64_SYS_dup2:
            if (cpu->thread && cpu->thread->process) {
                U32 newFd = cpu->thread->process->dup2((FD)a1, (FD)a2);
                ret = (S32)newFd < 0 ? (U64)(S64)(S32)newFd : (U64)newFd;
            } else {
                ret = (U64)-K_ENOSYS;
            }
            break;
        case X64_SYS_getcwd: {
            // getcwd(buf, size) — copy current directory string out. Returns
            // a pointer to buf on success, -ERANGE if size is too small.
            if (!a1 || a2 == 0) { ret = (U64)-K_EFAULT; break; }
            BString cwd = cpu->thread->process->currentDirectory;
            if (!cwd.length()) cwd = B("/");
            U64 need = (U64)cwd.length() + 1;
            if (need > a2) { ret = (U64)-34; /* -ERANGE */ break; }
            cpu->memory->memcpyToGuest(a1, cwd.c_str(), need);
            ret = a1;
            break;
        }
        case X64_SYS_fcntl:
            // Minimal F_GETFD/F_SETFD/F_GETFL/F_DUPFD handling for ld-linux.
            // F_GETFD=1, F_SETFD=2, F_GETFL=3, F_SETFL=4, F_DUPFD=0,
            // F_DUPFD_CLOEXEC=1030. Returning 0 for the get-ops claims "no
            // flags set, no CLOEXEC", which matches our reality.
            if (a2 == 0 || a2 == 1030) { // F_DUPFD / F_DUPFD_CLOEXEC
                U32 newFd = cpu->thread->process->dup((U32)a1);
                ret = (S32)newFd < 0 ? (U64)(S64)(S32)newFd : (U64)newFd;
            } else {
                ret = 0;
            }
            break;
        case X64_SYS_tgkill: {
            // tgkill(tgid, tid, sig). Single-thread world: any tid that
            // matches our own gets the signal delivered synchronously to
            // ourselves. Wrong-tid targets fall back to ESRCH so callers
            // can detect a stale tid (matches kernel behaviour).
            U64 ourTid = cpu->thread ? (U64)cpu->thread->id : 1;
            U32 sig    = (U32)a3;
            if (sig == 0) {
                // sig=0 is the "is this tid alive?" probe — answer "yes"
                // for our own tid, "no" otherwise. Don't deliver.
                ret = (a2 == ourTid) ? 0 : (U64)-K_ESRCH;
                break;
            }
            if (a2 != ourTid) {
                ret = (U64)-K_ESRCH;
                break;
            }
            if (deliverSignalSync(cpu, sig)) {
                // Handler will run next; ret=0 (kernel reports success
                // *before* handler runs, and the syscall return value is
                // overwritten by RAX-restore at rt_sigreturn anyway).
                ret = 0;
            } else {
                // No handler installed / SIG_DFL / SIG_IGN. For most
                // signals this should terminate the process; for v1 we
                // log and return 0 so abort()-paths can be observed.
                klog_fmt("ksyscall64: tgkill self sig=%u — no handler installed", sig);
                ret = 0;
            }
            break;
        }
        case X64_SYS_rt_sigreturn: {
            // x86-64 ABI: at the moment rt_sigreturn is invoked, RSP points
            // at the ucontext_t of the frame the kernel built when it
            // delivered the signal. We read all gprs/rip/rflags/sigmask
            // out of it and overwrite our state. The saved RAX becomes
            // the syscall return value (kernel restores the pre-signal
            // RAX, *not* a sigreturn status).
            if (!cpu->memory) { ret = (U64)-K_EFAULT; break; }
            U64 uctxPtr = cpu->reg[X64_RSP].u64;
            ret = restoreSignalFrame(cpu, uctxPtr);
            // restoreSignalFrame wrote rip/rsp/gprs (all except RAX) directly.
            // The dispatcher writes `ret` (savedRax) into RAX after the break,
            // completing the restore. The cpu->rip we just wrote is final —
            // SYSCALL only advances RIP *before* ksyscall64() is invoked, so
            // our mid-handler write to cpu->rip survives.
            break;
        }
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
            ret = sys_stat_path64(cpu, a1, a2, true);
            break;
        case X64_SYS_lstat:
            ret = sys_stat_path64(cpu, a1, a2, false);
            break;
        case X64_SYS_newfstatat:
            ret = sys_newfstatat64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_statfs:
            // a1 = path (ignored — same lie for every fs), a2 = struct statfs*
            ret = sys_statfs64_common(cpu, a2);
            break;
        case X64_SYS_fstatfs:
            // a1 = fd (ignored), a2 = struct statfs*
            ret = sys_statfs64_common(cpu, a2);
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
        case X64_SYS_access: {
            // access(path, mode) — resolve path; we don't model EUID perms yet
            // so existence is the only check (mode bits ignored).
            char path[1024] = {0};
            cpu->memory->memcpyFromGuest(path, a1, sizeof(path) - 1);
            std::shared_ptr<FsNode> n = Fs::getNodeFromLocalPath(
                cpu->thread->process->currentDirectory, BString::copy(path), true);
            ret = n ? 0 : (U64)-2;
            break;
        }
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
            ret = sys_futex64(cpu, a1, (U32)a2, (U32)a3);
            break;
        case X64_SYS_poll:
            ret = 0; // timeout — nothing ready
            break;
        case X64_SYS_sched_yield:
            ret = 0;
            break;
        case X64_SYS_sched_getaffinity:
            ret = sys_sched_getaffinity64(cpu, a1, a2, a3);
            break;
        case X64_SYS_sched_setaffinity:
            ret = sys_sched_setaffinity64(cpu, a1, a2, a3);
            break;
        case X64_SYS_kill: {
            // kill(pid, sig). pid==0 means "current process group"; pid==our
            // pid (or -1 for "all processes we can signal") means us.
            // Single-process world: any of those paths target us. sig==0
            // is a permission probe; succeed silently.
            U32 sig = (U32)a2;
            if (sig == 0) { ret = 0; break; }
            // For pid <= 0 or pid == our pid: deliver to self.
            U64 ourPid = cpu->thread && cpu->thread->process ?
                         (U64)cpu->thread->process->id : 1;
            S64 spid = (S64)a1;
            if (spid > 0 && (U64)spid != ourPid) {
                ret = (U64)-K_ESRCH;
                break;
            }
            if (deliverSignalSync(cpu, sig)) {
                ret = 0;
            } else {
                klog_fmt("ksyscall64: kill self sig=%u — no handler installed", sig);
                ret = 0;
            }
            break;
        }
        case X64_SYS_rt_sigtimedwait:
            ret = sys_rt_sigtimedwait64(cpu, a1, a2, a3, a4);
            break;
        case X64_SYS_rt_sigsuspend:
            ret = sys_rt_sigsuspend64(cpu, a1, a2);
            break;
        case X64_SYS_rt_sigpending:
            ret = sys_rt_sigpending64(cpu, a1, a2);
            break;
        case X64_SYS_rt_sigqueueinfo:
            // rt_sigqueueinfo(tgid, sig, info) — like kill but with siginfo.
            // We don't deliver, so report success the same way kill does.
            ret = 0;
            break;
        case X64_SYS_pause:
            // pause() blocks till any signal; we never deliver, so the
            // glibc-compatible answer is -EINTR (caller's loop retries).
            ret = (U64)-K_EINTR;
            break;
        case X64_SYS_wait4:
            // No children to reap (single-process world).
            ret = (U64)-K_ECHILD;
            break;
        case X64_SYS_clone:
            // Real thread/process creation needs KThread64 (deferred). Log
            // so we know when a binary tries — return -ENOSYS so glibc's
            // pthread_create surfaces the failure cleanly instead of looping.
            klog_fmt("ksyscall64: clone(flags=0x%llx) not implemented",
                     (unsigned long long)a1);
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_getitimer:
        case X64_SYS_setitimer:
            // Interval timers are rarely used by modern glibc (timer_create
            // is preferred); explicit ENOSYS keeps callers honest.
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_clone3:
            // Newer glibc tries clone3 first then falls back to clone.
            // Returning -ENOSYS by name (not via default) lets the fallback
            // path engage cleanly.
            klog_fmt("ksyscall64: clone3(args=0x%llx, size=%llu) not implemented",
                     (unsigned long long)a1,
                     (unsigned long long)a2);
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_pipe:
        case X64_SYS_pipe2:
            // No real pipe infra yet; glibc's posix_spawn and popen will
            // observe ENOSYS and either skip or surface a clean failure.
            // a1 (pipefd[2]) should be a writable user pointer if set.
            if (a1 == 0) { ret = (U64)-K_EFAULT; break; }
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_eventfd2:
            // Used by glibc for thread-pool wakeups, by GLib mainloop, etc.
            // Real impl needs an FD allocator that can wire poll/read.
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_epoll_create1:
            // Real epoll set needs FD + poll integration. ENOSYS pushes
            // callers onto their poll/select fallback path.
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_epoll_ctl:
            // Cannot exist meaningfully without epoll_create1 succeeding;
            // EBADF matches what a closed epoll fd would yield.
            ret = (U64)-K_EBADF;
            break;
        case X64_SYS_epoll_wait:
            // Same story — no valid epoll fd can exist yet.
            ret = (U64)-K_EBADF;
            break;
        case X64_SYS_getdents64:
            // Directory iteration needs FsOpenNode + KMemory64 plumbing.
            // EBADF on the assumption no dir was successfully opened yet
            // (open() of a directory currently fails earlier).
            if (a2 == 0) { ret = (U64)-K_EFAULT; break; }
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_pwrite64:
            // pwrite64(fd, buf, count, offset). No real impl; ENOSYS is
            // safer than silent zero-write.
            if (a2 == 0) { ret = (U64)-K_EFAULT; break; }
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_readv:
            // Scatter-gather read. Same shape: needs iovec walker. ENOSYS
            // until we add it — callers fall back to plain read().
            if (a2 == 0) { ret = (U64)-K_EFAULT; break; }
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_select:
            // select(nfds, readfds, writefds, exceptfds, timeout). Without
            // real fd polling we cannot honor it; ENOSYS surfaces cleanly.
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_chmod:
        case X64_SYS_fchmod:
            // No-op success: rootfs is effectively read-only for our purpose
            // and glibc's installer paths frequently call chmod on temp
            // files. Returning 0 avoids spurious install-time failures
            // without actually touching anything.
            ret = 0;
            break;
        case X64_SYS_gettimeofday: {
            // struct timeval { U64 sec; U64 usec; }
            if (a1) {
                U64 us = KSystem::getSystemTimeAsMicroSeconds();
                cpu->memory->writeq(a1,     us / 1000000ULL);
                cpu->memory->writeq(a1 + 8, us % 1000000ULL);
            }
            if (a2) {
                // struct timezone — minutes_west, dsttime. Zero both.
                cpu->memory->writed(a2,     0);
                cpu->memory->writed(a2 + 4, 0);
            }
            ret = 0;
            break;
        }
        case X64_SYS_getrusage:
            // struct rusage is large; for v1 just zero-fill 144 bytes.
            // glibc only inspects ru_utime and ru_stime on the startup path.
            if (a2) cpu->memory->memsetGuest(a2, 0, 144);
            ret = 0;
            break;
        case X64_SYS_sysinfo:
            // struct sysinfo — zero-fill the standard 64-byte layout.
            // ld-linux doesn't actually read these; some binaries call it
            // anyway as part of /proc startup probes.
            if (a1) cpu->memory->memsetGuest(a1, 0, 112);
            ret = 0;
            break;
        case X64_SYS_getppid:
            ret = cpu->thread && cpu->thread->process ? cpu->thread->process->parentId : 1;
            break;
        case X64_SYS_getpgrp:
        case X64_SYS_getpgid:
        case X64_SYS_getsid:
            ret = cpu->thread ? cpu->thread->id : 1;
            break;
        case X64_SYS_clock_getres:
            // 1 ns resolution claim. Some libc time helpers use this to size
            // a struct timespec field.
            if (a2) {
                cpu->memory->writeq(a2,     0);
                cpu->memory->writeq(a2 + 8, 1);
            }
            ret = 0;
            break;
        case X64_SYS_nanosleep:
        case X64_SYS_clock_nanosleep:
            // No-op sleep. Real Wine workloads will need pacing here, but
            // for ld.so startup it's fine to return immediately.
            ret = 0;
            break;
        case X64_SYS_rseq:
            // restartable-sequences registration. glibc 2.35+ calls this on
            // every thread start. Pretend it's not supported so glibc falls
            // back to plain mutexes.
            ret = (U64)-K_ENOSYS;
            break;
        case X64_SYS_exit:
        case X64_SYS_exit_group:
            ret = sys_exit64(cpu, a1);
            break;
        default:
            klog_fmt("ksyscall64: unimplemented syscall #%llu (%s) at RIP=0x%llx — RDI=0x%llx RSI=0x%llx RDX=0x%llx R10=0x%llx R8=0x%llx R9=0x%llx",
                     (unsigned long long)nr,
                     x64SyscallName(nr),
                     (unsigned long long)cpu->rip,
                     (unsigned long long)a1,
                     (unsigned long long)a2,
                     (unsigned long long)a3,
                     (unsigned long long)a4,
                     (unsigned long long)a5,
                     (unsigned long long)a6);
            ret = (U64)-K_ENOSYS;
            break;
    }

    cpu->reg[X64_RAX].setU64(ret);
}

#endif // BOXEDWINE_GUEST_X64
