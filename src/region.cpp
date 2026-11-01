// openkal.region (openkal 0.20): a range of the address space reserved first
// and backed as it is used.
//
// THIS KERNEL DOES NOT CHARGE BACKING APART FROM RESERVATION: a private
// anonymous mapping may be made larger than the memory the machine has, and a
// page is found when it is first written. KAL_REGION_PROP_OVERCOMMIT is
// therefore claimed, a reservation is a mapping that may be used at once, and
// `kal_region_commit' makes no call.
//
// RETURNING THE BACKING IS A FRESH MAPPING PLACED OVER THE PART, not advice. The
// advice this kernel offers for "these pages are not needed" leaves their
// contents in place until it wants the memory, and the specification promises
// zero after `kal_region_decommit' and a later `kal_region_commit'; a fixed
// anonymous mapping over the part releases the pages at once and reads as zero.
// Discarding is the advice (MADV_FREE): the contents are not specified there,
// and the kernel takes the pages back when it wants them.
#include "sys.h"
#include <openkal/memory.h>
#include <openkal/region.h>

namespace {

okm_uptr granularity() { return static_cast<okm_uptr>(kal_memory_granularity()); }
bool multiple(okm_uptr v, okm_uptr g) { return (v & (g - 1)) == 0; }

okm_long map_at(void* at, okm_uptr len, okm_long extra) {
    return okm::sys(okm::nr_mmap, reinterpret_cast<okm_long>(at), static_cast<okm_long>(len),
                    okm::prot_read | okm::prot_write, okm::map_private | okm::map_anon | extra, -1, 0);
}

}  // namespace

extern "C" {

void* kal_region_reserve(kal_uintptr size, kal_uintptr align, kal_uintptr flags) {
    const okm_uptr g = granularity();
    if (size == 0 || !multiple(size, g) || align < g || (align & (align - 1)) != 0) return nullptr;
    if ((flags & ~KAL_REGION_COMMIT) != 0) return nullptr;
    const okm_uptr total = size + align - g;
    if (total < size) return nullptr;
    const okm_long r = map_at(nullptr, total, 0);
    if (okm::failed(r)) return nullptr;
    const okm_uptr base = static_cast<okm_uptr>(r);
    const okm_uptr at = (base + align - 1) & ~(align - 1);
    if (at > base) okm::sys(okm::nr_munmap, static_cast<okm_long>(base), static_cast<okm_long>(at - base));
    const okm_uptr end = at + size, mapped_end = base + total;
    if (mapped_end > end) okm::sys(okm::nr_munmap, static_cast<okm_long>(end), static_cast<okm_long>(mapped_end - end));
    return reinterpret_cast<void*>(at);
}

int kal_region_commit(void* addr, kal_uintptr len) {
    const okm_uptr g = granularity(), a = reinterpret_cast<okm_uptr>(addr);
    if (addr == nullptr || !multiple(a, g) || !multiple(len, g)) return kal_err_invalid;
    return kal_ok;
}

int kal_region_decommit(void* addr, kal_uintptr len) {
    const okm_uptr g = granularity(), a = reinterpret_cast<okm_uptr>(addr);
    if (addr == nullptr || !multiple(a, g) || !multiple(len, g)) return kal_err_invalid;
    if (len == 0) return kal_ok;
    const okm_long r = map_at(addr, len, okm::map_fixed);
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

int kal_region_discard(void* addr, kal_uintptr len) {
    const okm_uptr g = granularity(), a = reinterpret_cast<okm_uptr>(addr);
    if (addr == nullptr || !multiple(a, g) || !multiple(len, g)) return kal_err_invalid;
    if (len == 0) return kal_ok;
    const okm_long r = okm::sys(okm::nr_madvise, static_cast<okm_long>(a), static_cast<okm_long>(len),
                                okm::madv_free);
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

void kal_region_release(void* addr, kal_uintptr size) {
    if (addr == nullptr || size == 0) return;
    okm::sys(okm::nr_munmap, reinterpret_cast<okm_long>(addr), static_cast<okm_long>(size));
}

kal_uintptr kal_region_props(void) { return KAL_REGION_PROP_OVERCOMMIT; }

}
