#include <unistd.h>
#include <errno.h>
import openkal.stream;

namespace {

// Platform error values are translated, not forwarded. The translation is a
// table; it does not reconstruct a foreign namespace, and the distinction is
// what keeps the implementation free of a compatibility layer.
int translate(int e) {
    switch (e) {
        case EBADF: case EINVAL: case EFAULT:  return kal_err_invalid;
        case EAGAIN:                            return kal_err_again;
        case ENOMEM:                            return kal_err_no_memory;
        case ENOSPC: case EFBIG:                return kal_err_no_space;
        case EACCES: case EPERM:                return kal_err_permission;
        case EPIPE: case ECONNRESET:            return kal_err_closed;
        case ENOTSUP:                           return kal_err_not_supported;
        default:                                return kal_err_io;
    }
}

}  // namespace

extern "C" {

kal_stream kal_stdin (void) { return kal_stream{0}; }
kal_stream kal_stdout(void) { return kal_stream{1}; }
kal_stream kal_stderr(void) { return kal_stream{2}; }

kal_io_result kal_stream_write(kal_stream s, const void* buf, kal_uintptr len) {
    const auto* p = static_cast<const unsigned char*>(buf);
    kal_uintptr done = 0;
    while (done < len) {
        const auto r = ::write(static_cast<int>(s.h), p + done, len - done);
        if (r < 0) {
            // An interrupted call is retried rather than reported. A caller
            // cannot distinguish this condition from a genuine failure without
            // knowledge of the platform, and an implementation that reports it
            // produces short writes on any system that delivers signals ---
            // a failure mode that a test suite is unlikely to reproduce.
            if (errno == EINTR) continue;
            return { done, translate(errno) };
        }
        if (r == 0) break;
        done += static_cast<kal_uintptr>(r);
    }
    return { done, done == len ? kal_ok : kal_err_io };
}

kal_io_result kal_stream_read(kal_stream s, void* buf, kal_uintptr len) {
    for (;;) {
        const auto r = ::read(static_cast<int>(s.h), buf, len);
        if (r < 0) {
            if (errno == EINTR) continue;
            return { 0, translate(errno) };
        }
        // A short read is reported as it occurred. Unlike a short write, it
        // carries information the caller requires: a result of zero denotes
        // end of input.
        return { static_cast<kal_uintptr>(r), kal_ok };
    }
}

int kal_stream_flush(kal_stream) {
    // Descriptors are unbuffered at this level, so the operation has nothing
    // to commit and reports success.
    return kal_ok;
}

}
