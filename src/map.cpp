#include "sys.h"
#include "handle.h"
#include <openkal/map.h>

// openkal.map on this kernel (openkal 0.18).
//
// The BSD calls: mmap, msync, munmap. The region is the address the kernel
// returned and the length the caller passed, so nothing is recorded.
//
// THE FILE'S LENGTH AND KIND ARE ASKED FIRST, with fstat64: the kernel maps
// beyond a file's end, and some devices, and the specification refuses both at
// the call. No PROT_EXEC: instructions run only from memory openkal.exec
// published, and this kernel's code signing would refuse an executable mapping
// of an unsigned file in any case.

extern "C" {

int kal_map_file(kal_file f, kal_u64 offset, kal_uintptr length, kal_uintptr how, void** out) {
    if (out == nullptr) return kal_err_invalid;
    *out = nullptr;
    const int fd = okm::unpack(f.h);
    if (fd < 0 || length == 0 || offset % KAL_MAP_ALIGNMENT != 0
        || (how & ~(KAL_MAP_WRITE | KAL_MAP_SHARED)) != 0)
        return kal_err_invalid;
    okm::kstat64 st{};
    const okm_long s = okm::sys(okm::nr_fstat64, fd, reinterpret_cast<okm_long>(&st));
    if (okm::failed(s)) return okm::translate(s);
    if ((okm::stat_mode(st) & okm::s_ifmt) != okm::s_ifreg) return kal_err_not_supported;
    const kal_u64 size = st.size > 0 ? static_cast<kal_u64>(st.size) : 0;
    if (offset > size || static_cast<kal_u64>(length) > size - offset) return kal_err_invalid;
    const okm_long prot  = okm::prot_read | ((how & KAL_MAP_WRITE) ? okm::prot_write : 0);
    const okm_long flags = (how & KAL_MAP_SHARED) ? okm::map_shared : okm::map_private;
    const okm_long r = okm::sys(okm::nr_mmap, 0, static_cast<okm_long>(length), prot, flags, fd,
                                static_cast<okm_long>(offset));
    if (okm::failed(r)) {
        if (-r == okm::e_nodev) return kal_err_not_supported;
        return okm::translate(r);
    }
    *out = reinterpret_cast<void*>(r);
    return kal_ok;
}

int kal_map_sync(void* region, kal_uintptr length) {
    if (region == nullptr || length == 0) return kal_err_invalid;
    // MS_ASYNC: the unified buffer cache is what a read of the file reads, so
    // the visibility the specification asks for needs no wait for the medium.
    const okm_long r = okm::sys(okm::nr_msync, reinterpret_cast<okm_long>(region),
                                static_cast<okm_long>(length), okm::ms_async);
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

void kal_map_release(void* region, kal_uintptr length) {
    if (region == nullptr || length == 0) return;
    (void)okm::sys(okm::nr_munmap, reinterpret_cast<okm_long>(region), static_cast<okm_long>(length));
}

}
