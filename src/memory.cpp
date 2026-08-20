#include "sys.h"
#include <openkal/memory.h>

// Clause 7.3 requires that where the environment already provides an
// allocator, this one be built upon it and not beside it. The environment of
// this implementation is the kernel, and the kernel provides mappings rather
// than an allocator, so an allocator is what this file supplies.
//
// The hazard the clause names is nevertheless worth checking against rather
// than dismissing. It is two independent claimants upon one region, on a
// system whose heap grows by extending a single region --- that is, `brk'. No
// allocation here uses `brk'. A program that contains both this allocator and
// a C library's therefore has two allocators drawing on disjoint mappings, and
// neither can shorten the other's.
//
// Version 0.2 called the host C library's `malloc'. That is correct for a
// program that borrows nothing and wrong for a program that supplies its own:
// the program defines `malloc' too, so the call resolves to it, and it in turn
// calls this implementation. See src/sys.h.

namespace {


constexpr okm_uptr kPage      = 4096;
constexpr okm_uptr kMinBlock  = 16;
constexpr okm_uptr kMaxSmall  = 32768;
constexpr okm_uptr kChunk     = 1u << 20;
constexpr int      kClasses   = 12;   // 16, 32, ... 32768

// Contention is rare --- the allocations of a C library above this one are
// served by that library's own allocator, and this one sees page-granular
// requests --- so a lock that spins is adequate and needs no suspension
// primitive, which would make openkal.memory depend upon openkal.task.
struct spin {
    volatile int held = 0;
    void lock() {
        while (__atomic_exchange_n(&held, 1, __ATOMIC_ACQUIRE)) okm::relax();
    }
    void unlock() { __atomic_store_n(&held, 0, __ATOMIC_RELEASE); }
};

spin  g_lock;
void* g_free[kClasses];

int class_of(okm_uptr n) {
    okm_uptr s = kMinBlock; int c = 0;
    while (s < n) { s <<= 1; ++c; }
    return c;
}
okm_uptr size_of(int c) { return kMinBlock << c; }

void* map(okm_uptr bytes) {
    const okm_long r = okm::sys(okm::nr_mmap, 0, static_cast<okm_long>(bytes),
                                okm::prot_read | okm::prot_write,
                                okm::map_private | okm::map_anon, -1, 0);
    if (okm::failed(r)) return nullptr;
    return reinterpret_cast<void*>(r);
}

void unmap(void* p, okm_uptr bytes) {
    okm::sys(okm::nr_munmap, reinterpret_cast<okm_long>(p), static_cast<okm_long>(bytes));
}

okm_uptr round_up(okm_uptr n, okm_uptr to) { return (n + to - 1) & ~(to - 1); }

// A chunk is page-aligned and a block size is a power of two no larger than a
// chunk, so a block at a multiple of its size is aligned to its size. That is
// why an alignment request is satisfied by choosing a larger class rather than
// by padding: no metadata is needed, and none is kept.
bool refill(int c) {
    const okm_uptr bs = size_of(c);
    auto* base = static_cast<unsigned char*>(map(kChunk));
    if (!base) return false;
    for (okm_uptr off = kChunk; off >= bs; off -= bs) {
        auto* b = base + off - bs;
        *reinterpret_cast<void**>(b) = g_free[c];
        g_free[c] = b;
    }
    return true;
}

}  // namespace

extern "C" {

void* kal_alloc(kal_uintptr size, kal_uintptr align) {
    if (size == 0) return nullptr;
    if (align < kMinBlock) align = kMinBlock;

    okm_uptr want = size < align ? align : size;
    if (want <= kMaxSmall) {
        const int c = class_of(want);
        g_lock.lock();
        if (!g_free[c] && !refill(c)) { g_lock.unlock(); return nullptr; }
        void* b = g_free[c];
        g_free[c] = *reinterpret_cast<void**>(b);
        g_lock.unlock();
        return b;
    }

    const okm_uptr bytes = round_up(size, kPage);
    if (align <= kPage) return map(bytes);

    // An alignment wider than a page is satisfied by mapping more and
    // recording, immediately before the region returned, what must be
    // released. The record is reachable because the returned address is at
    // least sixteen bytes above the mapping's base by construction.
    const okm_uptr total = bytes + align;
    auto* base = static_cast<unsigned char*>(map(total));
    if (!base) return nullptr;
    auto  addr = reinterpret_cast<okm_uptr>(base) + 16;
    addr = (addr + align - 1) & ~(align - 1);
    auto* user = reinterpret_cast<unsigned char*>(addr);
    reinterpret_cast<okm_uptr*>(user)[-1] = total;
    reinterpret_cast<okm_uptr*>(user)[-2] = reinterpret_cast<okm_uptr>(base);
    return user;
}

// The size and alignment are those passed to the allocation. The interface
// carries them so that an implementation need keep no record of its own, and
// this one keeps none: the class is recomputed from the size, and only the
// over-aligned case --- where the returned address is not the mapping's ---
// records anything at all.
void kal_free(void* p, kal_uintptr size, kal_uintptr align) {
    if (!p || size == 0) return;
    if (align < kMinBlock) align = kMinBlock;

    const okm_uptr want = size < align ? align : size;
    if (want <= kMaxSmall) {
        const int c = class_of(want);
        g_lock.lock();
        *reinterpret_cast<void**>(p) = g_free[c];
        g_free[c] = p;
        g_lock.unlock();
        return;
    }

    if (align <= kPage) { unmap(p, round_up(size, kPage)); return; }

    auto* user = static_cast<unsigned char*>(p);
    const okm_uptr total = reinterpret_cast<okm_uptr*>(user)[-1];
    auto*  base = reinterpret_cast<void*>(reinterpret_cast<okm_uptr*>(user)[-2]);
    unmap(base, total);
}

}
