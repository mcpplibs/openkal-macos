// openkal.debug and openkal.prof for a program of this environment (0.23):
// the fault reported by what ends the program, an address named, and samples
// taken (SPEC.md clause 11, entry 56).
//
// A HANDLER HERE IS ENTERED THROUGH THIS IMPLEMENTATION'S OWN TRAMPOLINE
// (`okm_sigtramp', process.cpp), which exists on 64-bit ARM; on the other
// architecture neither the report nor samples are claimed. What runs in a
// handler asks the kernel directly and allocates nothing, and reads a frame
// only inside the stack of the context it interrupted.
#include "sys.h"
#include <openkal/debug.h>
#include <openkal/prof.h>
#include <openkal/task.h>

extern "C" {
void*         _pthread_self(void);
void*         pthread_get_stackaddr_np(void* thread);
unsigned long pthread_get_stacksize_np(void* thread);
#if defined(__aarch64__)
void okm_sigtramp(void);
#endif
void okm_fault_handler(int signo, void* info, void* uc);
void okm_sample_handler(int signo, void* info, void* uc);
}

namespace {

#if defined(__aarch64__)
constexpr bool kHandlers = true;
#else
constexpr bool kHandlers = false;
#endif

// --- writing without a C library ------------------------------------------
struct line {
    char b[512]; okm_uptr n = 0;
    line& s(const char* t) { while (*t && n + 1 < sizeof b) b[n++] = *t++; return *this; }
    line& s(const char* t, okm_uptr len) { for (okm_uptr i = 0; i < len && n + 1 < sizeof b; ++i) b[n++] = t[i]; return *this; }
    line& hex(okm_uptr v) {
        char d[18]; int k = 0;
        do { d[k++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
        s("0x");
        while (k) { if (n + 1 < sizeof b) b[n++] = d[--k]; else --k; }
        return *this;
    }
    line& dec(okm_uptr v) {
        char d[24]; int k = 0;
        do { d[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (k) { if (n + 1 < sizeof b) b[n++] = d[--k]; else --k; }
        return *this;
    }
    void out(int fd) { okm::write_all(fd, b, n); n = 0; }
};

// --- where an address lies: the region the kernel holds, and its file -------
//
// `proc_info' (PROC_PIDREGIONPATHINFO): the region at or after the address,
// the offset of its start in its file, and the file's name.
struct where_t { okm_uptr start = 0, offset = 0; char file[1024]; okm_uptr flen = 0; };

bool locate(okm_uptr addr, where_t& w) {
    alignas(8) unsigned char info[1272] = {};
    static okm_long self = 0;
    if (self == 0) self = okm::sys(okm::nr_getpid);
    const okm_long r = okm::sys(okm::nr_proc_info, 2 /* PROC_INFO_CALL_PIDINFO */, self,
                                8 /* PROC_PIDREGIONPATHINFO */, static_cast<okm_long>(addr),
                                reinterpret_cast<okm_long>(info), sizeof info);
    if (okm::failed(r) || r < static_cast<okm_long>(sizeof info)) return false;
    okm_u64 offset = 0, start = 0, size = 0;
    okm::copy(&offset, info + 16, 8);
    okm::copy(&start, info + 80, 8);
    okm::copy(&size, info + 88, 8);
    if (addr < start || addr >= start + size) return false;
    const char* path = reinterpret_cast<const char*>(info + 248);
    w.start = start; w.offset = offset; w.flen = 0;
    while (w.flen + 1 < sizeof w.file && path[w.flen]) { w.file[w.flen] = path[w.flen]; ++w.flen; }
    w.file[w.flen] = '\0';
    return w.flen != 0;
}

bool pread_all(int fd, void* out, okm_uptr n, okm_u64 at) {
    okm_uptr got = 0;
    while (got < n) {
        const okm_long r = okm::sys(okm::nr_pread, fd, reinterpret_cast<okm_long>(static_cast<char*>(out) + got),
                                    static_cast<okm_long>(n - got), static_cast<okm_long>(at + got));
        if (okm::interrupted(r)) continue;
        if (r <= 0) return false;
        got += static_cast<okm_uptr>(r);
    }
    return true;
}

// --- the symbol that covers an offset: of an image (ELF) or of a program of
// this system (Mach-O) -------------------------------------------------------
struct ehdr64 { unsigned char ident[16]; okm_u32 tv; okm_u32 version; okm_u64 entry, phoff, shoff;
                okm_u32 flags; unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct phdr64 { okm_u32 type, flags; okm_u64 offset, vaddr, paddr, filesz, memsz, align; };
struct shdr64 { okm_u32 name, type; okm_u64 flags, addr, offset, size; okm_u32 link, info; okm_u64 align, entsize; };
struct sym64  { okm_u32 name; unsigned char info, other; unsigned short shndx; okm_u64 value, size; };
struct nlist64 { okm_u32 strx; unsigned char type, sect; unsigned short desc; okm_u64 value; };

bool name_from(int fd, okm_u64 at, char* name, okm_uptr cap, okm_uptr& len, bool drop_underscore) {
    okm_uptr n = 0;
    char c = 1;
    okm_u64 k = at;
    if (drop_underscore && pread_all(fd, &c, 1, k) && c == '_') ++k;
    while (n + 1 < cap && pread_all(fd, &c, 1, k + n) && c) name[n++] = c;
    name[n] = '\0';
    len = n;
    return n != 0;
}

bool elf_symbol(int fd, okm_uptr off, char* name, okm_uptr cap, okm_uptr& len, okm_uptr& delta) {
    ehdr64 eh{};
    if (!pread_all(fd, &eh, sizeof eh, 0)) return false;
    okm_uptr vaddr = ~okm_uptr{0};
    for (unsigned i = 0; i < eh.phnum; ++i) {
        phdr64 ph{};
        if (!pread_all(fd, &ph, sizeof ph, eh.phoff + i * sizeof ph)) return false;
        if (ph.type == 1 && off >= ph.offset && off < ph.offset + ph.filesz) { vaddr = off - ph.offset + ph.vaddr; break; }
    }
    if (vaddr == ~okm_uptr{0}) return false;
    shdr64 table{}, strings{};
    bool have = false;
    static const int kinds[2] = { 2, 11 };
    for (int want : kinds) {
        for (unsigned i = 0; i < eh.shnum && !have; ++i) {
            shdr64 sh{};
            if (!pread_all(fd, &sh, sizeof sh, eh.shoff + i * sizeof sh)) return false;
            if (static_cast<int>(sh.type) != want) continue;
            if (!pread_all(fd, &strings, sizeof strings, eh.shoff + sh.link * sizeof sh)) return false;
            table = sh; have = true;
        }
        if (have) break;
    }
    if (!have || table.entsize != sizeof(sym64)) return false;
    okm_u64 best = 0; okm_u32 best_name = 0; bool any = false;
    sym64 chunk[128];
    const okm_u64 count = table.size / sizeof(sym64);
    for (okm_u64 at = 0; at < count; at += 128) {
        const okm_u64 n = count - at < 128 ? count - at : 128;
        if (!pread_all(fd, chunk, n * sizeof(sym64), table.offset + at * sizeof(sym64))) break;
        for (okm_u64 k = 0; k < n; ++k) {
            const sym64& sy = chunk[k];
            if ((sy.info & 15) != 2 && (sy.info & 15) != 1) continue;
            if (sy.shndx == 0 || sy.value > vaddr) continue;
            if (sy.size != 0 && vaddr >= sy.value + sy.size) continue;
            if (!any || sy.value > best) { best = sy.value; best_name = sy.name; any = true; }
        }
    }
    if (!any) return false;
    delta = vaddr - best;
    return name_from(fd, strings.offset + best_name, name, cap, len, false);
}

bool macho_symbol(int fd, okm_uptr off, char* name, okm_uptr cap, okm_uptr& len, okm_uptr& delta) {
    okm_u32 head[8] = {};
    if (!pread_all(fd, head, sizeof head, 0) || head[0] != 0xfeedfacfu) return false;
    const okm_u32 ncmds = head[4];
    okm_u64 at = 32;
    okm_uptr vaddr = ~okm_uptr{0};
    okm_u32 symoff = 0, nsyms = 0, stroff = 0;
    for (okm_u32 i = 0; i < ncmds && i < 512; ++i) {
        okm_u32 lc[2] = {};
        if (!pread_all(fd, lc, sizeof lc, at)) return false;
        if (lc[0] == 0x19 /* LC_SEGMENT_64 */) {
            okm_u64 seg[4] = {};   // vmaddr, vmsize, fileoff, filesize
            if (!pread_all(fd, seg, sizeof seg, at + 8 + 16)) return false;
            if (seg[3] != 0 && off >= seg[2] && off < seg[2] + seg[3]) vaddr = off - seg[2] + seg[0];
        } else if (lc[0] == 0x2 /* LC_SYMTAB */) {
            okm_u32 st[4] = {};
            if (!pread_all(fd, st, sizeof st, at + 8)) return false;
            symoff = st[0]; nsyms = st[1]; stroff = st[2];
        }
        if (lc[1] == 0) return false;
        at += lc[1];
    }
    if (vaddr == ~okm_uptr{0} || nsyms == 0) return false;
    okm_u64 best = 0; okm_u32 best_name = 0; bool any = false;
    nlist64 chunk[128];
    for (okm_u32 k0 = 0; k0 < nsyms; k0 += 128) {
        const okm_u32 n = nsyms - k0 < 128 ? nsyms - k0 : 128;
        if (!pread_all(fd, chunk, n * sizeof(nlist64), symoff + static_cast<okm_u64>(k0) * sizeof(nlist64))) break;
        for (okm_u32 k = 0; k < n; ++k) {
            const nlist64& nl = chunk[k];
            if ((nl.type & 0xe0) != 0 || (nl.type & 0x0e) != 0x0e) continue;   // a symbol of a section
            if (nl.value > vaddr) continue;
            if (!any || nl.value > best) { best = nl.value; best_name = nl.strx; any = true; }
        }
    }
    if (!any) return false;
    delta = vaddr - best;
    return name_from(fd, stroff + best_name, name, cap, len, true);
}

bool symbol_at(const char* file, okm_uptr off, char* name, okm_uptr cap, okm_uptr& len, okm_uptr& delta) {
    const okm_long fd = okm::sys(okm::nr_openat, okm::at_fdcwd, reinterpret_cast<okm_long>(file),
                                 okm::o_rdonly | okm::o_cloexec, 0);
    if (okm::failed(fd)) return false;
    const bool ok = elf_symbol(static_cast<int>(fd), off, name, cap, len, delta)
                 || macho_symbol(static_cast<int>(fd), off, name, cap, len, delta);
    okm::sys(okm::nr_close, fd);
    return ok;
}

// --- frames ----------------------------------------------------------------------
struct frame_regs { okm_uptr pc = 0, fp = 0, sp = 0; };

frame_regs from_context(void* uc) {
    frame_regs r{};
#if defined(__aarch64__)
    // ucontext_t: onstack, sigmask, stack (24), link, mcsize, then a pointer
    // to the machine context: the exception state (16), then x0..x28, fp, lr,
    // sp, pc.
    unsigned char* mc = nullptr;
    okm::copy(&mc, static_cast<unsigned char*>(uc) + 48, sizeof mc);
    if (mc) {
        okm::copy(&r.fp, mc + 16 + 29 * 8, 8);
        okm::copy(&r.sp, mc + 16 + 31 * 8, 8);
        okm::copy(&r.pc, mc + 16 + 32 * 8, 8);
    }
#else
    (void)uc;
#endif
    return r;
}

// The chain of frame pointers, read only inside the interrupted context's
// stack, which is mapped wherever a chain that is not broken points.
int walk(frame_regs r, okm_uptr* pcs, int max) {
    int n = 0;
    if (r.pc && n < max) pcs[n++] = r.pc;
    void* self = _pthread_self();
    const okm_uptr top = self ? reinterpret_cast<okm_uptr>(pthread_get_stackaddr_np(self)) : 0;
    const okm_uptr size = self ? pthread_get_stacksize_np(self) : 0;
    const okm_uptr bottom = top > size ? top - size : 0;
    okm_uptr fp = r.fp;
    for (int i = 0; i < 256 && n < max; ++i) {
        if (fp == 0 || (fp & 7) != 0 || fp < r.sp || fp < bottom || fp + 16 > top) break;
        okm_uptr pair[2];
        okm::copy(pair, reinterpret_cast<const void*>(fp), sizeof pair);
        if (pair[1] == 0) break;
        pcs[n++] = pair[1] & 0x0000ffffffffffffull;   // a return address without its signature bits
        if (pair[0] <= fp) break;
        fp = pair[0];
    }
    return n;
}

void describe_frame(line& l, okm_uptr pc) {
    where_t w;
    if (!locate(pc, w)) return;
    const okm_uptr off = pc - w.start + w.offset;
    l.s(" ").s(w.file, w.flen).s("+").hex(off);
    char name[256]; okm_uptr nlen = 0, delta = 0;
    if (symbol_at(w.file, off, name, sizeof name, nlen, delta)) l.s(" ").s(name, nlen).s("+").hex(delta);
}

const char* fault_name(int signo) {
    switch (signo) {
        case 11: return "an access to memory the program may not make (SIGSEGV)";
        case 10: return "an access the memory cannot serve (SIGBUS)";
        case 4:  return "an instruction the program may not execute (SIGILL)";
        case 8:  return "an arithmetic fault (SIGFPE)";
        default: return "a fault";
    }
}

struct siginfo_head { int signo, errnum, code, pid; okm_u32 uid; int status; okm_uptr addr; };

// --- installing a handler through the kernel's own call ---------------------------
struct macos_sigaction {
    void (*handler)(int, void*, void*);
    void (*tramp)(void);
    unsigned int mask;
    int flags;
};
constexpr int sa_onstack = 0x1, sa_restart = 0x2, sa_resethand = 0x4, sa_siginfo = 0x40;
constexpr okm_long nr_sigaction = 46;

bool install(int signo, void (*handler)(int, void*, void*), int flags, macos_sigaction* before) {
#if defined(__aarch64__)
    macos_sigaction act{ handler, &okm_sigtramp, 0, flags };
    return !okm::failed(okm::sys(nr_sigaction, signo, reinterpret_cast<okm_long>(&act),
                                 reinterpret_cast<okm_long>(before)));
#else
    (void)signo; (void)handler; (void)flags; (void)before;
    return false;
#endif
}

alignas(16) unsigned char g_alt_stack[256 * 1024];

// INSTALLED AT INCEPTION, on a stack of its own; the handler is the default
// again once it has run, so the fault repeats and the program ends as this
// system ends it, its report kept.
[[gnu::constructor(103)]] void install_fault_report() {
    if (!kHandlers) return;
    struct { void* sp; okm_uptr size; int flags; } ss{ g_alt_stack, sizeof g_alt_stack, 0 };
    okm::sys(okm::nr_sigaltstack, reinterpret_cast<okm_long>(&ss), 0);
    static const int faults[4] = { 11, 10, 4, 8 };
    for (int signo : faults) {
        struct { void* handler; unsigned int mask; int flags; } old{};
        if (okm::failed(okm::sys(nr_sigaction, signo, 0, reinterpret_cast<okm_long>(&old)))) continue;
        if (old.handler != nullptr) continue;   // the disposition the program was started with is its own
        install(signo, &okm_fault_handler, sa_siginfo | sa_onstack | sa_resethand, nullptr);
    }
}

// --- samples ---------------------------------------------------------------------
struct ring {
    kal_prof_sample* slots = nullptr;
    volatile okm_uptr* ready = nullptr;
    okm_uptr cap = 0;
    volatile okm_uptr written = 0;
    volatile okm_uptr taken = 0;
    volatile int on = 0;
};
ring g_ring;
macos_sigaction g_prof_before{};

kal_u64 monotonic();

}  // namespace

extern "C" kal_u64 clock_gettime_nsec_np(int clock);

namespace {
kal_u64 monotonic() { return clock_gettime_nsec_np(4 /* CLOCK_UPTIME_RAW */); }
}

extern "C" void okm_fault_handler(int signo, void* info, void* uc) {
    const auto* si = static_cast<const siginfo_head*>(info);
    line l;
    l.s("openkal: fault: ").s(fault_name(signo)).s(" at ").hex(si ? si->addr : 0).s("\n");
    l.out(2);
    okm_uptr pcs[64];
    const int n = walk(from_context(uc), pcs, 64);
    for (int i = 0; i < n; ++i) {
        l.s("openkal:   #").dec(static_cast<okm_uptr>(i)).s(" ").hex(pcs[i]);
        describe_frame(l, i == 0 ? pcs[i] : pcs[i] - 1);
        l.s("\n");
        l.out(2);
    }
}

extern "C" void okm_sample_handler(int, void*, void* uc) {
    if (!__atomic_load_n(&g_ring.on, __ATOMIC_ACQUIRE)) return;
    const okm_uptr at = __atomic_load_n(&g_ring.written, __ATOMIC_RELAXED);
    if (at - __atomic_load_n(&g_ring.taken, __ATOMIC_ACQUIRE) >= g_ring.cap) return;
    const okm_uptr mine = __atomic_fetch_add(&g_ring.written, 1, __ATOMIC_ACQ_REL);
    if (mine - __atomic_load_n(&g_ring.taken, __ATOMIC_ACQUIRE) >= g_ring.cap) return;
    kal_prof_sample& s = g_ring.slots[mine % g_ring.cap];
    okm_uptr pcs[KAL_PROF_SAMPLE_DEPTH];
    const int n = walk(from_context(uc), pcs, KAL_PROF_SAMPLE_DEPTH);
    s.time_ns = monotonic();
    s.context = kal_task_current();
    s.depth = static_cast<kal_u32>(n);
    s.reserved = 0;
    for (int i = 0; i < n; ++i) s.pc[i] = pcs[i];
    __atomic_store_n(&g_ring.ready[mine % g_ring.cap], mine + 1, __ATOMIC_RELEASE);
}

extern "C" {

kal_uintptr kal_debug_props(void) { return kHandlers ? KAL_DEBUG_PROP_FAULT_REPORT : 0; }

kal_intptr kal_debug_symbolize(kal_uintptr address, char* out, kal_uintptr cap, kal_uintptr* start) {
    if (address == 0) return -static_cast<kal_intptr>(kal_err_not_found);
    where_t w;
    if (!locate(address, w)) return -static_cast<kal_intptr>(kal_err_not_found);
    const okm_uptr off = address - w.start + w.offset;
    char name[1024]; okm_uptr nlen = 0, delta = 0;
    if (!symbol_at(w.file, off, name, sizeof name, nlen, delta))
        return -static_cast<kal_intptr>(kal_err_not_found);
    if (out && cap) okm::copy(out, name, nlen < cap ? nlen : cap);
    if (start) *start = address - delta;
    return static_cast<kal_intptr>(nlen);
}

// THIS IMPLEMENTATION DOES NOT MEASURE A PROGRAM ITSELF: a loader that binds
// an image does (clause 12.3).
int  kal_prof_active(void) { return 0; }
int  kal_prof_counter(const char*, kal_uintptr, const volatile kal_u64*) { return kal_ok; }
void kal_prof_mark(const char*, kal_uintptr, int) {}

kal_uintptr kal_prof_props(void) { return kHandlers ? KAL_PROF_PROP_SAMPLES : 0; }

int kal_prof_sample_begin(kal_u32 per_second) {
    if (!kHandlers) return kal_err_not_supported;
    if (per_second == 0 || per_second > 10000) return kal_err_invalid;
    if (__atomic_load_n(&g_ring.on, __ATOMIC_ACQUIRE)) return kal_err_again;
    if (!g_ring.slots) {
        constexpr okm_uptr cap = 65536;
        const okm_uptr bytes = cap * sizeof(kal_prof_sample) + cap * sizeof(okm_uptr);
        const okm_long m = okm::sys(okm::nr_mmap, 0, static_cast<okm_long>(bytes),
                                    okm::prot_read | okm::prot_write, okm::map_private | okm::map_anon, -1, 0);
        if (okm::failed(m)) return kal_err_no_memory;
        g_ring.slots = reinterpret_cast<kal_prof_sample*>(m);
        g_ring.ready = reinterpret_cast<volatile okm_uptr*>(m + static_cast<okm_long>(cap * sizeof(kal_prof_sample)));
        g_ring.cap = cap;
    }
    if (!install(27 /* SIGPROF */, &okm_sample_handler, sa_siginfo | sa_restart, &g_prof_before))
        return kal_err_not_supported;
    __atomic_store_n(&g_ring.on, 1, __ATOMIC_RELEASE);
    const okm_long us = static_cast<okm_long>(1000000 / per_second);
    // struct itimerval: interval then value, each a timeval (seconds, then
    // microseconds in an int padded to eight bytes).
    struct { okm_i64 is; int ius; int p1; okm_i64 vs; int vus; int p2; } it{ us / 1000000, static_cast<int>(us % 1000000), 0,
                                                                          us / 1000000, static_cast<int>(us % 1000000), 0 };
    if (okm::failed(okm::sys(okm::nr_setitimer, 2 /* ITIMER_PROF */, reinterpret_cast<okm_long>(&it), 0))) {
        __atomic_store_n(&g_ring.on, 0, __ATOMIC_RELEASE);
        okm::sys(nr_sigaction, 27, reinterpret_cast<okm_long>(&g_prof_before), 0);
        return kal_err_not_supported;
    }
    return kal_ok;
}

kal_intptr kal_prof_sample_take(kal_prof_sample* out, kal_uintptr count) {
    if (out == nullptr && count != 0) return -static_cast<kal_intptr>(kal_err_invalid);
    if (!g_ring.slots) return 0;
    kal_uintptr n = 0;
    while (n < count) {
        const okm_uptr t = __atomic_load_n(&g_ring.taken, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&g_ring.ready[t % g_ring.cap], __ATOMIC_ACQUIRE) != t + 1) break;
        out[n++] = g_ring.slots[t % g_ring.cap];
        __atomic_store_n(&g_ring.taken, t + 1, __ATOMIC_RELEASE);
    }
    return static_cast<kal_intptr>(n);
}

int kal_prof_sample_end(void) {
    if (!__atomic_load_n(&g_ring.on, __ATOMIC_ACQUIRE)) return kal_err_invalid;
    struct { okm_i64 is; int ius; int p1; okm_i64 vs; int vus; int p2; } stop{};
    okm::sys(okm::nr_setitimer, 2, reinterpret_cast<okm_long>(&stop), 0);
    __atomic_store_n(&g_ring.on, 0, __ATOMIC_RELEASE);
    okm::sys(nr_sigaction, 27, reinterpret_cast<okm_long>(&g_prof_before), 0);
    return kal_ok;
}

}  // extern "C"
