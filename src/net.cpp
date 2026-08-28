#include "sys.h"
#include "handle.h"
#include "endpoint.h"
#include <openkal/net.h>

// openkal.net upon this kernel's socket calls.
//
// A CONNECTION IS AN OWNED HANDLE AND THE STREAM IS BORROWED FROM IT, exactly as
// kal_file and kal_fs_stream are here. The owned handle carries a generation so
// that a released one stops being valid, which clause 7.2 requires; the stream
// it yields is the bare descriptor, because that is what openkal.stream's
// transfer operations take.
//
// ⚠️ TWO DIFFERENCES FROM THE OTHER KERNEL, BOTH IN THE SHAPE OF THE CALLS
// RATHER THAN IN WHAT THEY DO.
//
//   There is no `accept4' and no flag upon `socket' that closes a descriptor
//   across a spawn, so close-on-exec is set afterwards with `fcntl'. The two
//   steps are not equivalent to one under a concurrent spawn, and process.cpp
//   already records that this implementation states the difference rather than
//   concealing it.
//
//   A socket address carries its own length in its first byte. endpoint.h
//   writes it; a structure copied from the other kernel would put the family
//   where this one reads a length.

namespace {

int fd_of(kal_net_conn c)     { return okm::unpack(c.h); }
int fd_of(kal_net_listener l) { return okm::unpack(l.h); }

void close_on_exec(okm_long fd) {
    okm::sys(okm::nr_fcntl, fd, okm::f_setfd, okm::fd_cloexec);
}

int report_address(okm_long call, int fd, kal_endpoint* out) {
    if (out == nullptr) return kal_err_invalid;
    if (fd < 0) return kal_err_invalid;
    okm::ksockaddr_storage ss{};
    okm_u32 len = static_cast<okm_u32>(sizeof ss);
    const okm_long r = okm::sys(call, fd, reinterpret_cast<okm_long>(&ss),
                                reinterpret_cast<okm_long>(&len));
    if (okm::failed(r)) return okm::translate(r);
    return okm::from_kernel(ss, *out);
}

}  // namespace

extern "C" {

int kal_net_connect(const kal_endpoint* to, kal_net_conn* out) {
    if (to == nullptr || out == nullptr) return kal_err_invalid;
    const okm_long family = okm::family_of(*to);
    if (family < 0) return kal_err_invalid;

    okm::ksockaddr_storage ss{};
    okm_u32 len = 0;
    if (const int rc = okm::to_kernel(*to, ss, len); rc != kal_ok) return rc;

    const okm_long fd = okm::sys(okm::nr_socket, family, okm::sock_stream,
                                 okm::ipproto_tcp);
    if (okm::failed(fd)) return okm::translate(fd);
    close_on_exec(fd);

    for (;;) {
        const okm_long r = okm::sys(okm::nr_connect, fd,
                                    reinterpret_cast<okm_long>(&ss),
                                    static_cast<okm_long>(len));
        if (okm::interrupted(r)) continue;
        if (okm::failed(r)) {
            okm::sys(okm::nr_close, fd);
            return okm::translate(r);
        }
        break;
    }

    out->h = okm::pack(static_cast<int>(fd));
    if (out->h == 0) { okm::sys(okm::nr_close, fd); return kal_err_no_memory; }
    return kal_ok;
}

int kal_net_listen(const kal_endpoint* local, kal_net_listener* out) {
    if (local == nullptr || out == nullptr) return kal_err_invalid;
    const okm_long family = okm::family_of(*local);
    if (family < 0) return kal_err_invalid;

    okm::ksockaddr_storage ss{};
    okm_u32 len = 0;
    if (const int rc = okm::to_kernel(*local, ss, len); rc != kal_ok) return rc;

    const okm_long fd = okm::sys(okm::nr_socket, family, okm::sock_stream,
                                 okm::ipproto_tcp);
    if (okm::failed(fd)) return okm::translate(fd);
    close_on_exec(fd);

    // SO_REUSEADDR, because a listener whose predecessor is in the kernel's
    // lingering state would otherwise be refused for a reason that has nothing
    // to do with the caller. A program restarted within the linger interval is
    // the ordinary case, not an unusual one.
    {
        const int on = 1;
        okm::sys(okm::nr_setsockopt, fd, okm::sol_socket, okm::so_reuseaddr,
                 reinterpret_cast<okm_long>(&on),
                 static_cast<okm_long>(sizeof on));
    }

    if (const okm_long r = okm::sys(okm::nr_bind, fd,
                                    reinterpret_cast<okm_long>(&ss),
                                    static_cast<okm_long>(len));
        okm::failed(r)) {
        okm::sys(okm::nr_close, fd);
        return okm::translate(r);
    }

    // The backlog the kernel is asked for. A number rather than a name, because
    // this interface does not expose one and a caller has no way to state it.
    if (const okm_long r = okm::sys(okm::nr_listen, fd, 128); okm::failed(r)) {
        okm::sys(okm::nr_close, fd);
        return okm::translate(r);
    }

    out->h = okm::pack(static_cast<int>(fd));
    if (out->h == 0) { okm::sys(okm::nr_close, fd); return kal_err_no_memory; }
    return kal_ok;
}

int kal_net_accept(kal_net_listener l, kal_net_conn* out) {
    if (out == nullptr) return kal_err_invalid;
    const int fd = fd_of(l);
    if (fd < 0) return kal_err_invalid;

    for (;;) {
        const okm_long r = okm::sys(okm::nr_accept, fd, 0, 0);
        if (okm::interrupted(r)) continue;
        if (okm::failed(r)) return okm::translate(r);
        close_on_exec(r);
        out->h = okm::pack(static_cast<int>(r));
        if (out->h == 0) { okm::sys(okm::nr_close, r); return kal_err_no_memory; }
        return kal_ok;
    }
}

kal_stream kal_net_stream(kal_net_conn c) {
    // The bare descriptor, for the reason kal_fs_stream gives: openkal.stream's
    // operations take whatever the environment's transfer calls take, and a
    // packed word is not that.
    const int fd = fd_of(c);
    return kal_stream{ fd < 0 ? 0u : static_cast<kal_uintptr>(fd) };
}

int kal_net_peer(kal_net_conn c, kal_endpoint* out) {
    return report_address(okm::nr_getpeername, fd_of(c), out);
}

int kal_net_local(kal_net_conn c, kal_endpoint* out) {
    return report_address(okm::nr_getsockname, fd_of(c), out);
}

int kal_net_listener_local(kal_net_listener l, kal_endpoint* out) {
    return report_address(okm::nr_getsockname, fd_of(l), out);
}

int kal_net_shutdown(kal_net_conn c, int direction) {
    const int fd = fd_of(c);
    if (fd < 0) return kal_err_invalid;

    // The kernel numbers the directions from zero and this interface from one,
    // so the mapping is written out rather than arithmetic upon the argument. A
    // direction this interface does not define is refused rather than passed
    // through, because the kernel would read an unknown number as SHUT_RD.
    okm_long how;
    switch (direction) {
        case KAL_SHUT_READ:  how = 0; break;
        case KAL_SHUT_WRITE: how = 1; break;
        case KAL_SHUT_BOTH:  how = 2; break;
        default: return kal_err_invalid;
    }

    const okm_long r = okm::sys(okm::nr_shutdown, fd, how);
    if (okm::failed(r)) return okm::translate(r);
    return kal_ok;
}

void kal_net_close(kal_net_conn c) {
    const int fd = fd_of(c);
    if (fd < 0) return;
    okm::sys(okm::nr_close, fd);
    okm::retire(c.h);
}

void kal_net_close_listener(kal_net_listener l) {
    const int fd = fd_of(l);
    if (fd < 0) return;
    okm::sys(okm::nr_close, fd);
    okm::retire(l.h);
}

// Both positions hold on this kernel: it speaks IPv6 and its `shutdown' ends
// transfer in one direction while the other continues.
kal_uintptr kal_net_props(void) { return KAL_NET_PROP_IPV6 | KAL_NET_PROP_HALFCLOSE; }

}  // extern "C"
