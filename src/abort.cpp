#include <unistd.h>
#include <stdlib.h>
import openkal.abort;

extern "C" {

[[noreturn]] void kal_abort(const char* msg, kal_uintptr len) {
    if (msg != nullptr && len != 0) {
        kal_uintptr done = 0;
        while (done < len) {
            const auto r = ::write(2, msg + done, len - done);
            if (r <= 0) break;
            done += static_cast<kal_uintptr>(r);
        }
    }
    ::abort();
}

// _exit rather than exit. The specification requires immediate termination,
// and exit would run registered handlers and static destructors first. The
// difference is not observable in a small program and becomes observable in a
// large one, which is why it is stated rather than left to judgement.
[[noreturn]] void kal_exit(int code) { ::_exit(code); }

}
