// Program startup, for a program that carries no runtime of its own.
//
// Where a program already carries a runtime, that runtime receives control from
// the loader and this file is not compiled. Where it does not, the entry point
// is named in the image rather than found under a fixed symbol, so the name
// below is stated on the link line.
//
// It is short here and long on the other kernel, and the difference is a
// property of the environment. This system's loader passes the arguments, the
// named values and one further vector to the entry point as ordinary arguments,
// and it has already established thread-local storage. There is no stack layout
// to parse and no register to install.
#ifdef OKM_STANDALONE

#include "sys.h"
#include <openkal/abort.h>

namespace okm {
void record(int argc, char** argv, char** envp);
}

extern "C" {

int main(int, char**, char**);

// The name the hand-over from the first object to a C library already has. It
// is weak: a program written directly against openkal has none, and then this
// file calls main itself.
[[gnu::weak]] int __libc_start_main(int (*)(int, char**, char**), int, char**,
                                    void (*)(), void (*)(), void (*)());

int okm_start(int argc, char** argv, char** envp, char** apple);

int okm_start(int argc, char** argv, char** envp, char** apple) {
    (void)apple;
    okm::record(argc, argv, envp);

    if (__libc_start_main != nullptr) {
        __libc_start_main(main, argc, argv, nullptr, nullptr, nullptr);
        // A C library's hand-over does not return. Reaching here means one did,
        // and continuing would run the program a second time.
        kal_exit(127);
    }

    kal_exit(main(argc, argv, envp));
    return 0;
}

}

#endif  // OKM_STANDALONE
