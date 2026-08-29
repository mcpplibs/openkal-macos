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

// A STREAM HANDLE IS A DESCRIPTOR AND IS NOT DECODED. stream.cpp states it in
// terms, kal_stream_read and kal_stream_write take it as one, and so must the
// wait that precedes them: the wait and the transfer that follows it have to
// name the same object or the wait answers about something else.
//
// THIS FILE USED TO DECODE IT AS AN OWNED HANDLE, AND THE DECODE SUCCEEDED.
// handle.h packs an owned handle as (generation << 32) | (fd + 1), so a bare
// descriptor N has exactly the shape of a packed handle naming N-1, and
// okm::unpack accepts it whenever generation N-1 is still zero -- which is
// every index at which no owned handle has yet been released. The wait was
// therefore performed upon descriptor N-1 and the transfer upon N.
//
// The former reading tested `unpack` and fell back when it failed, on the
// stated ground that the standard streams are not packed. That ground is
// correct and the conclusion drawn from it was not: NO stream handle is packed
// here, and of the three standard ones only kal_stdin, whose handle is zero,
// fails to decode. kal_stdout decoded to descriptor 0 and kal_stderr to 1.
//
// What a caller observed was an expiry for a stream that had bytes waiting, or
// a wait without bound inside an operation that states one, according to what
// happened to occupy the descriptor below. Both are functions of the process's
// descriptor history, so the same program answered differently when run alone
// and when run after something else -- and the index healed for good once an
// owned handle at N-1 had been released, because that advances the generation
// and makes the decode fail correctly.
//
// Found in openkal-linux, whose timeout.cpp is this file's counterpart and
// carried the same four call sites with the same two wrong.
int stream_fd(kal_stream s) { return static_cast<int>(s.h); }

}  // namespace

extern "C" {

kal_intptr kal_timeout_read(kal_stream s, void* buf, kal_uintptr len, kal_u64 ns) {
    // A transfer of zero bytes does not wait and is not bounded. Waiting first
    // would turn a call that always succeeds into one that can expire.
    if (len == 0) return 0;

    if (const int rc = await(stream_fd(s), static_cast<short>(okm::poll_in), ns);
        rc != kal_ok)
        return -rc;
    return kal_stream_read(s, buf, len);
}

kal_intptr kal_timeout_write(kal_stream s, const void* buf, kal_uintptr len, kal_u64 ns) {
    if (len == 0) return 0;

    if (const int rc = await(stream_fd(s), static_cast<short>(okm::poll_out), ns);
        rc != kal_ok)
        return -rc;
    return kal_stream_write(s, buf, len);
}

// THE TWO OPERATIONS BELOW DO DECODE, AND THAT IS NOT AN INCONSISTENCY WITH THE
// TWO ABOVE. A listener and a datagram are owned: their handles are made by
// okm::pack and released by okm::retire, so the decode is the operation that
// recovers the descriptor and its failure is how a released handle is refused.
// A stream is borrowed and carries no generation. The four call sites divide
// exactly along that line, and the two that were wrong were the two that had a
// borrowed handle in hand.
int kal_timeout_accept(kal_net_listener l, kal_u64 ns, kal_net_conn* out) {
    if (out == nullptr) return kal_err_invalid;
    const int fd = okm::unpack(l.h);
    if (fd < 0) return kal_err_invalid;

    if (const int rc = await(fd, static_cast<short>(okm::poll_in), ns); rc != kal_ok)
        return rc;
    return kal_net_accept(l, out);
}

kal_intptr kal_timeout_recv_from(kal_datagram d, void* buf, kal_uintptr len,
                                 kal_endpoint* from, kal_u64 ns) {
    const int fd = okm::unpack(d.h);
    if (fd < 0) return -kal_err_invalid;

    if (const int rc = await(fd, static_cast<short>(okm::poll_in), ns); rc != kal_ok)
        return -rc;
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
kal_u64 kal_timeout_granularity(void) { return 1000000u; }

}  // extern "C"
