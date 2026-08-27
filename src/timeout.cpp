#include "sys.h"
#include "handle.h"
#include "endpoint.h"
#include <openkal/timeout.h>
#include <openkal/time.h>

// openkal.timeout upon poll(2) and wait4(2).
//
// THE BOUND IS APPLIED BEFORE THE OPERATION AND NOT DURING IT. `poll' reports
// whether a descriptor would transfer without blocking, so a bounded read is a
// bounded wait for readiness followed by the ordinary read. This is what the
// environment already does at the point of the call, which is why clause 6.3
// records readiness notification as the alternative that was NOT adopted: an
// interface reporting readiness would oblige an implementation to maintain a set
// and a context of its own, and this one obliges it to maintain nothing.
//
// The bound is therefore upon the WAIT and not upon the transfer. A read that
// becomes ready within the bound and then transfers slowly is not interrupted,
// which is the behaviour every environment's own bounded read has.
//
// ⚠️ THIS KERNEL'S `poll' STATES ITS BOUND IN MILLISECONDS, and `ppoll' --- the
// call the other kernel uses, which takes nanoseconds --- does not exist here.
// The granularity this interface reports is therefore a millisecond, which is
// what the environment can distinguish rather than what would be convenient.

namespace {

// A duration of zero denotes no bound, which is the convention kal_task_wait
// establishes. `poll' expresses that with a negative number.
//
// ⚠️ A BOUND SHORTER THAN A MILLISECOND ROUNDS UP TO ONE AND NOT DOWN TO NONE.
// Rounding down would turn a wait into a poll, and the header is explicit: a
// caller that asks for less is not refused and does not get less.
int bound_ms(kal_u64 ns) {
    if (ns == 0) return -1;
    okm_u64 ms = ns / 1000000ull;
    if (ms == 0) ms = 1;
    if (ms > 0x7fffffffull) ms = 0x7fffffffull;
    return static_cast<int>(ms);
}

// Waits for one descriptor. Reports kal_ok when it is ready, kal_err_again when
// the bound expired, and a translated error otherwise.
int await(int fd, short events, kal_u64 ns) {
    okm::kpollfd p{ fd, events, 0 };
    const int ms = bound_ms(ns);

    const okm_long r = okm::sys(okm::nr_poll, reinterpret_cast<okm_long>(&p), 1, ms);
    // AN INTERRUPTED WAIT IS NOT RETRIED WITH THE WHOLE BOUND AGAIN.
    //
    // Retrying with the original duration would make the bound restart at every
    // signal, so a program on a system that delivers them regularly would wait
    // without end while appearing to be bounded. The call leaves nothing behind
    // to resume from, and the honest report is that the operation did not
    // complete.
    if (okm::interrupted(r)) return kal_err_again;
    if (okm::failed(r)) return okm::translate(r);
    if (r == 0) return kal_err_again;   // the bound expired
    return kal_ok;
}

}  // namespace

extern "C" {

kal_io_result kal_timeout_read(kal_stream s, void* buf, kal_uintptr len, kal_u64 ns) {
    // A transfer of zero bytes does not wait and is not bounded. Waiting first
    // would turn a call that always succeeds into one that can expire.
    if (len == 0) return { 0, kal_ok };

    const int fd = okm::unpack(s.h);
    // THE STANDARD STREAMS ARE NOT PACKED HANDLES. openkal.stream reports them
    // as the descriptors themselves, so a word that does not unpack is taken to
    // be one of those rather than being refused.
    const int use = (fd >= 0) ? fd : static_cast<int>(s.h);

    if (const int rc = await(use, static_cast<short>(okm::poll_in), ns); rc != kal_ok)
        return { 0, rc };
    return kal_stream_read(s, buf, len);
}

kal_io_result kal_timeout_write(kal_stream s, const void* buf, kal_uintptr len, kal_u64 ns) {
    if (len == 0) return { 0, kal_ok };

    const int fd  = okm::unpack(s.h);
    const int use = (fd >= 0) ? fd : static_cast<int>(s.h);

    if (const int rc = await(use, static_cast<short>(okm::poll_out), ns); rc != kal_ok)
        return { 0, rc };
    return kal_stream_write(s, buf, len);
}

int kal_timeout_accept(kal_net_listener l, kal_u64 ns, kal_net_conn* out) {
    if (out == nullptr) return kal_err_invalid;
    const int fd = okm::unpack(l.h);
    if (fd < 0) return kal_err_invalid;

    if (const int rc = await(fd, static_cast<short>(okm::poll_in), ns); rc != kal_ok)
        return rc;
    return kal_net_accept(l, out);
}

kal_io_result kal_timeout_recv_from(kal_datagram d, void* buf, kal_uintptr len,
                                    kal_endpoint* from, kal_u64 ns) {
    const int fd = okm::unpack(d.h);
    if (fd < 0) return { 0, kal_err_invalid };

    if (const int rc = await(fd, static_cast<short>(okm::poll_in), ns); rc != kal_ok)
        return { 0, rc };
    return kal_datagram_recv_from(d, buf, len, from);
}

int kal_timeout_wait_process(kal_process p, kal_u64 ns, int* status, int* terminated) {
    if (p.h == 0) return kal_err_invalid;

    // WNOHANG AND A POLLING LOOP, BECAUSE THIS KERNEL HAS NO BOUNDED WAIT FOR A
    // CHILD EITHER. `wait4' blocks or does not wait at all; there is no bound.
    //
    // The alternative is a handler for the signal a child's end raises, which
    // is process-wide state: an implementation that installed one would take a
    // facility away from the program above it. The loop is what the environment
    // permits, and the interval is the granularity this interface reports so
    // that the polling cost is stated rather than hidden.
    constexpr okm_u64 interval_ns = 1000000ull;   // one millisecond
    okm_u64 waited = 0;

    for (;;) {
        int st = 0;
        const okm_long r = okm::sys(okm::nr_wait4, static_cast<okm_long>(p.h),
                                    reinterpret_cast<okm_long>(&st),
                                    okm::wnohang, 0);
        if (okm::interrupted(r)) continue;
        if (okm::failed(r)) return okm::translate(r);

        if (r != 0) {
            const int signalled = st & 0x7f;
            if (signalled == 0) {
                if (status) *status = (st >> 8) & 0xff;
                if (terminated) *terminated = 0;
            } else {
                if (status) *status = signalled;
                if (terminated) *terminated = 1;
            }
            return kal_ok;
        }

        if (ns != 0 && waited >= ns) return kal_err_again;

        // openkal.time's own suspension, rather than a second spelling of it.
        // This implementation provides that interface and time.cpp records what
        // this system's suspension is expressed with; reproducing it here would
        // be the same decision written twice.
        kal_time_sleep(interval_ns);
        waited += interval_ns;
    }
}

// The bound this kernel distinguishes. `poll' takes milliseconds and there is
// no call here that takes less, so a millisecond is what an implementation can
// honestly report --- and it is also the interval the child-waiting loop above
// polls at, so a caller asking for less is not told a number one of the
// operations cannot meet.
const kal_uintptr kal_timeout_granularity_ns = 1000000u;

}  // extern "C"
