// openkal.random on this system --- getentropy(2).
//
// ⭐ THE NUMBER CAME FROM THE MACHINE, NOT FROM MEMORY. `.github/workflows/
// numbers.yml` reads every number this implementation uses out of the SDK's own
// `sys/syscall.h`, on both runners this repository targets, and both answered
// `SYS_getentropy 500`. That workflow exists because a number recalled rather
// than read is a number that is right until the day it is not.
//
// ⚠️ AND NOT `arc4random_buf`, WHICH IS WHAT libc++ WOULD REACH FOR HERE.
// That name is in libSystem, and reaching into libSystem is what this backend
// exists to avoid: it issues this kernel's calls directly, as the note in
// `sys.h` records. `getentropy` is the call underneath.
//
// ⚠️ THE KERNEL CAPS A CALL AT 256 BYTES. That is this system's limit and not
// this interface's, so the loop below turns it into the all-or-nothing
// `kal_random_fill` promises.
#include "sys.h"
#include <openkal/random.h>

extern "C" int kal_random_fill(void* out, kal_uintptr len) {
    if (len == 0) return kal_ok;
    if (out == nullptr) return kal_err_invalid;

    auto* p = static_cast<unsigned char*>(out);
    kal_uintptr filled = 0;
    while (filled < len) {
        const kal_uintptr chunk = (len - filled) > 256 ? 256 : (len - filled);
        const okm_long r = okm::sys(okm::nr_getentropy,
                                    reinterpret_cast<okm_long>(p + filled),
                                    static_cast<okm_long>(chunk), 0, 0);
        if (r < 0) {
            // ⚠️ The buffer is not restored, and the contract says it need not
            // be: a failed fill leaves it unspecified rather than unchanged.
            if (r == -4 /* EINTR */) continue;
            return kal_err_io;
        }
        filled += chunk;
    }
    return kal_ok;
}

// Neither blocking nor hardware. This kernel's generator is seeded before a
// process runs, so there is no wait to report; and whether the seed came from a
// hardware source is not something this backend can observe.
extern "C" const kal_uintptr kal_random_props = 0;
