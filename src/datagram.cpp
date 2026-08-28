#include "sys.h"
#include "handle.h"
#include "endpoint.h"
#include <openkal/datagram.h>

// openkal.datagram upon this kernel's socket calls.
//
// A DATAGRAM IS NOT PACKED AS A kal_stream, and the handle type is its own for
// that reason: kal_stream_read reports a count and not a boundary, so reading a
// datagram through it would lose the property that distinguishes this interface.
// The packing is the same, the type is not, and the type is what prevents the
// mistake.

namespace {

int fd_of(kal_datagram d) { return okm::unpack(d.h); }

}  // namespace

extern "C" {

int kal_datagram_open(const kal_endpoint* local, kal_datagram* out) {
    if (out == nullptr) return kal_err_invalid;

    // A null local endpoint asks for one that may send and whose receiving
    // address is unspecified. IPv4 is chosen for it, because a family must be
    // named at the point the socket is made and this is the one every
    // environment that has a network at all provides.
    okm_long family = okm::af_inet;
    if (local != nullptr) {
        family = okm::family_of(*local);
        if (family < 0) return kal_err_invalid;
    }

    const okm_long fd = okm::sys(okm::nr_socket, family, okm::sock_dgram,
                                 okm::ipproto_udp);
    if (okm::failed(fd)) return okm::translate(fd);
    okm::sys(okm::nr_fcntl, fd, okm::f_setfd, okm::fd_cloexec);

    if (local != nullptr) {
        okm::ksockaddr_storage ss{};
        okm_u32 len = 0;
        if (const int rc = okm::to_kernel(*local, ss, len); rc != kal_ok) {
            okm::sys(okm::nr_close, fd);
            return rc;
        }
        if (const okm_long r = okm::sys(okm::nr_bind, fd,
                                        reinterpret_cast<okm_long>(&ss),
                                        static_cast<okm_long>(len));
            okm::failed(r)) {
            okm::sys(okm::nr_close, fd);
            return okm::translate(r);
        }
    }

    out->h = okm::pack(static_cast<int>(fd));
    if (out->h == 0) { okm::sys(okm::nr_close, fd); return kal_err_no_memory; }
    return kal_ok;
}

int kal_datagram_local(kal_datagram d, kal_endpoint* out) {
    if (out == nullptr) return kal_err_invalid;
    const int fd = fd_of(d);
    if (fd < 0) return kal_err_invalid;

    okm::ksockaddr_storage ss{};
    okm_u32 len = static_cast<okm_u32>(sizeof ss);
    const okm_long r = okm::sys(okm::nr_getsockname, fd,
                                reinterpret_cast<okm_long>(&ss),
                                reinterpret_cast<okm_long>(&len));
    if (okm::failed(r)) return okm::translate(r);
    return okm::from_kernel(ss, *out);
}

kal_intptr kal_datagram_send_to(kal_datagram d, const void* buf, kal_uintptr len,
                                const kal_endpoint* to) {
    const int fd = fd_of(d);
    if (fd < 0 || to == nullptr) return -kal_err_invalid;

    okm::ksockaddr_storage ss{};
    okm_u32 addrlen = 0;
    if (const int rc = okm::to_kernel(*to, ss, addrlen); rc != kal_ok)
        return -rc;

    for (;;) {
        const okm_long r = okm::sys(okm::nr_sendto, fd,
                                    reinterpret_cast<okm_long>(buf),
                                    static_cast<okm_long>(len), 0,
                                    reinterpret_cast<okm_long>(&ss),
                                    static_cast<okm_long>(addrlen));
        if (okm::interrupted(r)) continue;
        if (okm::failed(r)) return -okm::translate(r);

        // A MESSAGE IS SENT WHOLE OR NOT AT ALL, which is what this interface
        // states. The kernel reports a count anyway; a count short of the length
        // would mean the medium had split the message, which for a datagram
        // socket it does not do. Reporting the short count as success would give
        // a caller a partial send this interface says cannot occur, so it is
        // reported as a failure of the medium instead.
        const kal_uintptr n = static_cast<kal_uintptr>(r);
        return n == len ? static_cast<kal_intptr>(n) : -kal_err_io;
    }
}

kal_intptr kal_datagram_recv_from(kal_datagram d, void* buf, kal_uintptr len,
                                  kal_endpoint* from) {
    const int fd = fd_of(d);
    if (fd < 0) return -kal_err_invalid;

    okm::ksockaddr_storage ss{};
    okm_u32 addrlen = static_cast<okm_u32>(sizeof ss);

    for (;;) {
        const okm_long r = okm::sys(okm::nr_recvfrom, fd,
                                    reinterpret_cast<okm_long>(buf),
                                    static_cast<okm_long>(len), 0,
                                    reinterpret_cast<okm_long>(&ss),
                                    reinterpret_cast<okm_long>(&addrlen));
        if (okm::interrupted(r)) continue;
        if (okm::failed(r)) return -okm::translate(r);

        // THE COUNT REPORTED IS WHAT WAS PLACED IN THE BUFFER, not what was
        // sent. Without MSG_TRUNC the kernel already reports the former, which
        // is what this interface requires: a caller that trusted the larger
        // number would read beyond its own buffer.
        if (from != nullptr) {
            // A sender whose family this implementation does not know leaves the
            // endpoint zeroed rather than partly filled. The transfer still
            // happened and is reported; what is unknown is who sent it.
            if (okm::from_kernel(ss, *from) != kal_ok) {
                for (auto& b : from->addr) b = 0;
                from->addr_len = 0;
                from->port     = 0;
            }
        }
        return static_cast<kal_intptr>(r);
    }
}

void kal_datagram_close(kal_datagram d) {
    const int fd = fd_of(d);
    if (fd < 0) return;
    okm::sys(okm::nr_close, fd);
    okm::retire(d.h);
}

// Broadcast is not claimed. The kernel provides it only after SO_BROADCAST has
// been set, and this interface has no operation that would set it; a word
// claiming a facility no operation reaches is the disagreement clause 6.2 exists
// to prevent.
kal_uintptr kal_datagram_props(void) { return KAL_DGRAM_PROP_IPV6; }

}  // extern "C"
