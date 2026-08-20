#include "sys.h"
#include <openkal/abort.h>

extern "C" {

[[noreturn]] void kal_abort(const char* msg, kal_uintptr len) {
    if (msg != nullptr && len != 0) okm::write_all(2, msg, len);
    // The kernel's own means of stopping a program whose state has been
    // declared impossible. A status that no program returns is what makes it
    // distinguishable from an ordinary end, which is what the specification
    // requires of it.
    for (;;) okm::sys(okm::nr_exit, 134);
}

// Termination is immediate. Clause 7.8: registered exit handlers and static
// destructors shall not run, and a caller must be able to reason about what
// executes after the call. This kernel's own call ends the program without
// running anything, which is why it is the one used.
[[noreturn]] void kal_exit(int code) {
    for (;;) okm::sys(okm::nr_exit, code);
}

}
