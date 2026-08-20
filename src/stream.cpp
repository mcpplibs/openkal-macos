#include "sys.h"
#include <openkal/stream.h>

extern "C" {

kal_stream kal_stdin (void) { return kal_stream{0}; }
kal_stream kal_stdout(void) { return kal_stream{1}; }
kal_stream kal_stderr(void) { return kal_stream{2}; }

kal_io_result kal_stream_write(kal_stream s, const void* buf, kal_uintptr len) {
    const auto* p = static_cast<const unsigned char*>(buf);
    kal_uintptr done = 0;
    while (done < len) {
        const okm_long r = okm::sys(okm::nr_write, static_cast<okm_long>(s.h),
                                    reinterpret_cast<okm_long>(p + done),
                                    static_cast<okm_long>(len - done));
        // An interrupted call is retried rather than reported. Clause 7.5: a
        // caller cannot distinguish this condition from a genuine failure
        // without knowledge of the environment, and an implementation that
        // reports it produces short writes on any system that delivers
        // signals --- a failure a test suite is unlikely to reproduce.
        if (okm::interrupted(r)) continue;
        if (okm::failed(r)) return { done, okm::translate(r) };
        if (r == 0) break;
        done += static_cast<kal_uintptr>(r);
    }
    return { done, done == len ? kal_ok : kal_err_io };
}

kal_io_result kal_stream_read(kal_stream s, void* buf, kal_uintptr len) {
    for (;;) {
        const okm_long r = okm::sys(okm::nr_read, static_cast<okm_long>(s.h),
                                    reinterpret_cast<okm_long>(buf),
                                    static_cast<okm_long>(len));
        if (okm::interrupted(r)) continue;
        if (okm::failed(r)) return { 0, okm::translate(r) };
        // A short read is reported as it occurred. Unlike a short write it
        // carries information the caller requires: zero denotes end of input.
        return { static_cast<kal_uintptr>(r), kal_ok };
    }
}

int kal_stream_flush(kal_stream s) {
    const okm_long r = okm::sys(okm::nr_fsync, static_cast<okm_long>(s.h));
    if (!okm::failed(r)) return kal_ok;
    // A stream that cannot be committed --- a terminal, a pipe --- has nothing
    // to commit, and reporting that as a failure would oblige every caller to
    // distinguish it from a failure to reach a medium.
    if (r == -okm::e_inval || r == -okm::e_notty || r == -okm::e_badf
        || r == -okm::e_opnotsupp) return kal_ok;
    return okm::translate(r);
}

kal_uintptr kal_stream_props(kal_stream s) {
    // The enquiry this kernel offers is an attempt to read a terminal's
    // settings: it succeeds for a terminal and reports that the stream is not
    // one otherwise. It is the same question every C library asks before
    // choosing a buffering discipline, and it is asked here so that the library
    // above need not know which environment it is upon.
    unsigned char termios[128] = { 0 };
    // TIOCGETA, which this system spells with the size of the structure in the
    // request itself.
    constexpr okm_long tiocgeta = 0x40000000L | (72L << 16) | ('t' << 8) | 19;
    const okm_long r = okm::sys(okm::nr_ioctl, static_cast<okm_long>(s.h), tiocgeta,
                                reinterpret_cast<okm_long>(termios));
    return okm::failed(r) ? kal_uintptr{0} : KAL_STREAM_PROP_INTERACTIVE;
}

}
