// The system call interface of this kernel, and the one name beyond it.
//
// openkal is a contract and says nothing about what else a program contains.
// An implementation is selected by the program, and the program may itself
// supply the facilities the implementation would otherwise borrow; if it does,
// and the names agree, the implementation's calls resolve to the program's and
// the program's resolve back to the implementation. The recursion is unbounded
// and it is not visible in either side's source.
//
// Version 0.2 of this implementation borrowed the C library of the host. That
// is a correct implementation of openkal for a program that borrows nothing,
// and it is wrong for a program that supplies its own --- which the
// specification permits and clause 1 names first among the consumers it
// expects.
//
// This implementation therefore issues the kernel's own calls. One name is
// still taken from the system, and it is chosen for the property that matters
// here: `pthread_create_from_mach_thread' is a name no C library defines, so a
// program that defines every ordinary one still leaves it reachable. What it
// does could not be done by a system call --- creating an execution context on
// this system means arranging state a kernel call does not arrange --- and the
// alternative, reproducing that arrangement here, would be reproducing a part
// of this system inside an implementation of openkal.
//
// Which names are reachable and which are not was measured on the system rather
// than remembered: .github/workflows/probe.yml asked it, and its answers are
// what this file is written against.
#pragma once

using okm_long  = long;
using okm_ulong = unsigned long;
using okm_u64   = unsigned long long;
using okm_i64   = long long;
using okm_u32   = unsigned;
using okm_uptr  = __UINTPTR_TYPE__;

namespace okm {

// --- the calling convention --------------------------------------------------
//
// A failure is reported by the carry flag rather than by a negative result,
// which is the difference a reader coming from the other kernel will look for
// first. The number returned when the flag is set is the error value itself.

#if defined(__aarch64__)

inline okm_long sys(okm_long n, okm_long a = 0, okm_long b = 0, okm_long c = 0,
                    okm_long d = 0, okm_long e = 0, okm_long f = 0) {
    register okm_long x16 __asm__("x16") = n;
    register okm_long x0 __asm__("x0") = a;
    register okm_long x1 __asm__("x1") = b;
    register okm_long x2 __asm__("x2") = c;
    register okm_long x3 __asm__("x3") = d;
    register okm_long x4 __asm__("x4") = e;
    register okm_long x5 __asm__("x5") = f;
    okm_long failed;
    __asm__ __volatile__("svc #0x80\n\tcset %1, cs"
                         : "+r"(x0), "=r"(failed)
                         : "r"(x16), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                         : "memory", "cc");
    return failed ? -x0 : x0;
}

#elif defined(__x86_64__)

// The number carries a class in its high bits; the ordinary calls are class 2.
inline okm_long sys(okm_long n, okm_long a = 0, okm_long b = 0, okm_long c = 0,
                    okm_long d = 0, okm_long e = 0, okm_long f = 0) {
    okm_long r;
    unsigned char failed;
    register okm_long r10 __asm__("r10") = d;
    register okm_long r8  __asm__("r8")  = e;
    register okm_long r9  __asm__("r9")  = f;
    __asm__ __volatile__("syscall"
                         : "=a"(r), "=@ccc"(failed)
                         : "a"(n | 0x2000000L), "D"(a), "S"(b), "d"(c),
                           "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "memory", "cc");
    return failed ? -r : r;
}

#else
#error "openkal-macos supports x86_64 and arm64"
#endif

// Duplicating the calling image, which is the one call whose result does not
// fit the convention above.
//
// This kernel reports which image is which in the *second* register: both
// receive the same first value, and the second is zero in the original and one
// in the duplicate. That is the BSD convention, and it is not the other
// kernel's --- there the duplicate is told by receiving a first value of zero.
// An implementation that tested the first value alone would have both images
// take the original's branch, so the duplicate would carry on running the
// program instead of replacing itself, and the original would wait for a
// program that never starts.
//
// This system's own C library hides the difference by forcing the duplicate's
// first value to zero in its wrapper. There is no wrapper here, so the
// difference is met rather than hidden.
inline okm_long duplicate(bool& is_duplicate) {
#if defined(__aarch64__)
    register okm_long x16 __asm__("x16") = 2;   // fork
    register okm_long x0 __asm__("x0") = 0;
    register okm_long x1 __asm__("x1") = 0;
    okm_long failed;
    __asm__ __volatile__("svc #0x80\n\tcset %2, cs"
                         : "+r"(x0), "+r"(x1), "=r"(failed)
                         : "r"(x16)
                         : "memory", "cc");
    is_duplicate = x1 != 0;
    return failed ? -x0 : x0;
#else
    okm_long first, second;
    unsigned char failed;
    __asm__ __volatile__("syscall"
                         : "=a"(first), "=d"(second), "=@ccc"(failed)
                         : "a"(2L | 0x2000000L)
                         : "rcx", "r11", "memory", "cc");
    is_duplicate = second != 0;
    return failed ? -first : first;
#endif
}

// Creating a pipe, which is the second call whose result does not fit the
// convention above and does so for the same reason as the first.
//
// This kernel reports BOTH descriptors as return values: the reading end in the
// first register and the writing end in the second. The other kernel takes a
// buffer and fills it. Neither is more natural; what matters is that this file
// meets the difference rather than hiding it, as it does for the duplication
// primitive above.
inline okm_long pipe_pair(okm_long& writing) {
#if defined(__aarch64__)
    register okm_long x16 __asm__("x16") = 42;   // pipe
    register okm_long x0 __asm__("x0") = 0;
    register okm_long x1 __asm__("x1") = 0;
    okm_long failed;
    __asm__ __volatile__("svc #0x80\n\tcset %2, cs"
                         : "+r"(x0), "+r"(x1), "=r"(failed)
                         : "r"(x16)
                         : "memory", "cc");
    writing = x1;
    return failed ? -x0 : x0;
#else
    okm_long first, second;
    unsigned char failed;
    __asm__ __volatile__("syscall"
                         : "=a"(first), "=d"(second), "=@ccc"(failed)
                         : "a"(42L | 0x2000000L)
                         : "rcx", "r11", "memory", "cc");
    writing = second;
    return failed ? -first : first;
#endif
}

// The numbers. They are the same on both architectures this implementation
// supports, which is the reason the table is not per-architecture as it is on
// the other kernel.
enum : okm_long {
    nr_exit = 1, nr_read = 3, nr_write = 4, nr_close = 6, nr_wait4 = 7,
    nr_chdir = 12, nr_getpid = 20, nr_getuid = 24, nr_geteuid = 25,
    nr_kill = 37, nr_dup = 41, nr_getegid = 43, nr_getgid = 47,
    nr_ioctl = 54, nr_execve = 59, nr_umask = 60, nr_pipe = 42,
    nr_munmap = 73, nr_mprotect = 74, nr_madvise = 75,
    nr_dup2 = 90, nr_fsync = 95, nr_gettimeofday = 116,
    nr_readv = 120, nr_writev = 121, nr_ftruncate = 201,
    nr_utimes = 138, nr_futimes = 139,
    nr_getentropy = 500,
    // This kernel has no call that reports the working directory --- the
    // measurement is in .github/workflows/numbers.yml, where SYS___getcwd is
    // absent from the system's own table. What it has instead is an enquiry
    // upon an open file that reports the name it was reached by, and that is
    // what a program on this system uses.
    f_getpath = 50,
    nr_mmap = 197, nr_lseek = 199,
    nr_fcntl = 92, nr_stat64 = 338, nr_fstat64 = 339, nr_lstat64 = 340,
    nr_getdirentries64 = 344, nr_bsdthread_terminate = 361,
    nr_thread_selfid = 372,
    nr_openat = 463, nr_renameat = 465, nr_faccessat = 466,
    nr_fstatat64 = 470, nr_unlinkat = 472, nr_readlinkat = 473,
    nr_mkdirat = 475,
    nr_ulock_wait = 515, nr_ulock_wake = 516,
};

// --- error values, as this kernel returns them -------------------------------
enum : int {
    e_perm = 1, e_noent = 2, e_intr = 4, e_io = 5, e_badf = 9, e_child = 10,
    e_again = 35, e_nomem = 12, e_acces = 13, e_fault = 14, e_busy = 16,
    e_exist = 17, e_xdev = 18, e_nodev = 19, e_notdir = 20, e_isdir = 21,
    e_inval = 22, e_nfile = 23, e_mfile = 24, e_notty = 25, e_fbig = 27,
    e_nospc = 28, e_spipe = 29, e_rofs = 30, e_pipe = 32, e_range = 34,
    e_nametoolong = 63, e_nosys = 78, e_notempty = 66, e_loop = 62,
    // This system has two values for an operation the object does not support,
    // and they are different numbers: 45 is the one a call upon a resource of
    // the wrong kind returns, and 102 is the one a socket returns.
    e_notsup = 45,
    e_timedout = 60, e_connreset = 54, e_dquot = 69, e_opnotsupp = 102,
};

enum : okm_long {
    o_rdonly = 0, o_wronly = 1, o_rdwr = 2,
    o_creat = 0x0200, o_excl = 0x0800, o_trunc = 0x0400, o_append = 0x0008,
    o_directory = 0x100000, o_cloexec = 0x1000000, o_nofollow = 0x0100,
    at_fdcwd = -2, at_removedir = 0x0080, at_symlink_nofollow = 0x0020,
    prot_read = 1, prot_write = 2,
    map_private = 2, map_anon = 0x1000,
};

// The suspension primitive's operation word.
enum : okm_u32 {
    ul_compare_and_wait = 1,
    ulf_no_errno = 0x01000000,
    ulf_wake_all = 0x00000100,
};

inline bool failed(okm_long r) { return r < 0 && r > -4096; }
inline bool interrupted(okm_long r) { return r == -e_intr; }

// The environment's error values, mapped onto the closed set the specification
// defines. A table preserves the naturalness clause 7.1 requires;
// reconstructing a foreign namespace would not.
inline int translate(okm_long r) {
    const int e = static_cast<int>(-r);
    switch (e) {
        case e_badf: case e_inval: case e_fault: case e_nametoolong:
        case e_loop: case e_spipe:                       return 1;  // invalid
        case e_again:                                    return 2;  // again
        case e_nomem:                                    return 4;  // no memory
        case e_nospc: case e_fbig: case e_dquot:         return 5;  // no space
        case e_acces: case e_perm: case e_rofs:          return 6;  // permission
        case e_nosys: case e_opnotsupp: case e_notsup:   return 7;  // unsupported
        case e_pipe: case e_connreset:                   return 8;  // closed
        case e_noent: case e_child: case e_nodev:        return 9;  // not found
        case e_exist: case e_busy:                       return 10; // exists
        case e_notempty:                                 return 11; // not empty
        case e_isdir:                                    return 12; // is directory
        case e_notdir:                                   return 13; // not directory
        default:                                         return 3;  // io
    }
}

// --- this kernel's structure layouts -----------------------------------------

struct kstat64 {
    okm_u32 dev;
    okm_u32 mode_pad;      // st_mode is 16 bits followed by 16 of nlink
    okm_u64 ino;
    okm_u32 uid, gid;
    okm_u32 rdev;
    okm_i64 atime_sec, atime_nsec;
    okm_i64 mtime_sec, mtime_nsec;
    okm_i64 ctime_sec, ctime_nsec;
    okm_i64 btime_sec, btime_nsec;
    okm_i64 size;
    okm_i64 blocks;
    okm_u32 blksize;
    okm_u32 flags;
    okm_u32 gen;
    okm_u32 lspare;
    okm_i64 qspare[2];
};

// The two fields the layout above packs together, taken apart.
inline okm_u32 stat_mode(const kstat64& s) { return s.mode_pad & 0xffffu; }
inline okm_u32 stat_nlink(const kstat64& s) { return (s.mode_pad >> 16) & 0xffffu; }

enum : okm_u32 {
    s_ifmt = 0170000, s_ifreg = 0100000, s_ifdir = 0040000, s_iflnk = 0120000,
};

struct ktimeval { okm_i64 sec; int usec; int pad; };

// The record this kernel's directory enumeration produces.
struct kdirent64 {
    okm_u64        ino;
    okm_u64        seekoff;
    unsigned short reclen;
    unsigned short namlen;
    unsigned char  type;
    char           name[];
};

enum : unsigned char { dt_dir = 4, dt_reg = 8, dt_lnk = 10 };

// --- operations used by more than one interface ------------------------------

inline okm_long write_all(int fd, const void* p, okm_uptr n) {
    const auto* b = static_cast<const unsigned char*>(p);
    okm_uptr done = 0;
    while (done < n) {
        const okm_long r = sys(nr_write, fd, reinterpret_cast<okm_long>(b + done),
                               static_cast<okm_long>(n - done));
        if (interrupted(r)) continue;
        if (failed(r)) return r;
        if (r == 0) break;
        done += static_cast<okm_uptr>(r);
    }
    return static_cast<okm_long>(done);
}

// Relinquishing the processor for a moment. This kernel offers no ordinary
// call for it --- what its C library uses is a trap of the other kind --- so
// what is used here is the instruction the processor provides, which is what
// the operation is for.
inline void relax() {
#if defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

// The identity of the calling context.
//
// Version 0.3 read it from the register each architecture reserves for the
// current thread's own record, masking the low bits that carry a processor
// number. That is one instruction and it is wrong for a context this system
// creates by the one route a program carrying no other runtime can use: such a
// context observes zero there. openkal requires the value to be distinct per
// context and stable within one, and zero for every context satisfies neither
// --- and the way it failed was a C library above openkal reading its own
// per-context state through a null pointer, four layers from the register.
//
// The kernel is asked instead. It answers with the identity it gave the
// context, which is never zero and never shared, and the cost is a call rather
// than a load. That is the right trade for a value whose wrongness is not
// detectable by the caller.
inline okm_uptr current_context() {
    const okm_long r = sys(nr_thread_selfid);
    return failed(r) ? 1u : static_cast<okm_uptr>(r);
}

inline okm_uptr length(const char* s) { okm_uptr n = 0; while (s && s[n]) ++n; return n; }

inline void copy(void* d, const void* s, okm_uptr n) {
    auto* a = static_cast<unsigned char*>(d);
    const auto* b = static_cast<const unsigned char*>(s);
    for (okm_uptr i = 0; i < n; ++i) a[i] = b[i];
}

inline void fill(void* d, unsigned char v, okm_uptr n) {
    auto* a = static_cast<unsigned char*>(d);
    for (okm_uptr i = 0; i < n; ++i) a[i] = v;
}

// A name is a single component or a sequence separated by a forward slash. It
// shall not begin with a separator and shall not contain a component that
// ascends: a program able to ascend from the directory it was given would not
// be confined by having been given it. The rule is here rather than in the file
// system implementation because openkal.process names a program the same way.
inline bool acceptable(const char* name, okm_uptr len) {
    if (name == nullptr || len == 0 || name[0] == '/') return false;
    okm_uptr start = 0;
    for (okm_uptr i = 0; i <= len; ++i) {
        if (i == len || name[i] == '/') {
            const okm_uptr n = i - start;
            if (n == 0) return false;
            if (n == 2 && name[start] == '.' && name[start + 1] == '.') return false;
            start = i + 1;
        }
    }
    return true;
}

struct terminated {
    char buf[1024];
    bool ok;
    terminated(const char* s, okm_uptr n) : ok(n < sizeof buf) {
        if (ok) { copy(buf, s, n); buf[n] = '\0'; }
    }
};

}  // namespace okm
