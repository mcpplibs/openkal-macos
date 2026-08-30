#include "sys.h"
#include <openkal/env.h>

namespace okm {

// The vectors this system passes to a program's entry point. They are recorded
// by whichever of the two entrances the program has: this implementation's own
// entry, when the program carries no other runtime, and otherwise an
// initialiser, which this system calls with the same three arguments.
int    g_argc = 0;
char** g_argv = nullptr;
char** g_envp = nullptr;

void record(int argc, char** argv, char** envp) {
    g_argc = argc; g_argv = argv; g_envp = envp;
}

}  // namespace okm

namespace {

// ⚠️⚠️ A PROGRAM ABOVE openkal SHALL NOT BE ENDED BY SOMETHING openkal NEVER TOLD
// IT ABOUT. openkal defines no signals, and `kal_stream_write' is required to
// REPORT that the far end of a stream is gone --- while this kernel delivers
// SIGPIPE, whose default action ends the program instead.
//
// ⭐ A C library above answers `signal(SIGPIPE, SIG_IGN)' truthfully, because
// openkal has no signals and there is nothing for it to set; the program is then
// killed anyway, by a mechanism no layer between it and here can name. Ignored
// at this level because this is the only level that can. Found on the other
// implementation, fixed on both --- a divergence here would be the same defect
// with a different exit status.
//
// ⚠️ Not a policy about signals in general: this is the one an ordinary openkal
// operation provokes.
//
// ⚠️⚠️ AND IT IS NOT FIXED HERE YET, WHICH IS RECORDED RATHER THAN LEFT TO BE
// DISCOVERED. openkal-linux ignores it in one call. This kernel's `sigaction'
// takes a `struct __sigaction' carrying a TRAMPOLINE that its C library
// supplies, and a disposition installed with the wrong shape is the kind of
// mistake that shows up as a program dying in a way nobody can trace --- which is
// the defect this note is about, arrived at from the other side.
//
// ⇒ It is left until it can be MEASURED on this system. This repository already
// refuses to claim a facility it has not exercised, and a signal disposition
// installed by guesswork is exactly that. The consequence meanwhile is stated:
// a program above this implementation that writes to a stream whose far end has
// gone is ended by SIGPIPE rather than told, and no layer between it and here
// can name what happened.
[[gnu::constructor(101)]] void capture(int argc, char** argv, char** envp) {
    if (okm::g_argv == nullptr) okm::record(argc, argv, envp);
}
}  // namespace

extern "C" {

kal_uintptr kal_env_arg_count(void) { return static_cast<kal_uintptr>(okm::g_argc); }

// EVERY VALUE IS COPIED INTO THE CALLER'S BUFFER. These answered with a pointer
// into this implementation's own storage, which is meaningful only while the
// implementation shares the caller's address space. Each reports the length the
// value HAS, so a caller with a large enough buffer is done in one call and one
// that wants to size first passes a capacity of zero.
namespace {
kal_intptr give(const char* v, kal_uintptr n, char* out, kal_uintptr cap) {
    if (out != nullptr && cap != 0) okm::copy(out, v, n < cap ? n : cap);
    return static_cast<kal_intptr>(n);
}
}  // namespace

kal_intptr kal_env_arg(kal_uintptr index, char* out, kal_uintptr cap) {
    if (index >= static_cast<kal_uintptr>(okm::g_argc)) return -kal_err_not_found;
    const char* s = okm::g_argv[index];
    return give(s, okm::length(s), out, cap);
}

kal_intptr kal_env_var(const char* name, kal_uintptr name_len,
                       char* out, kal_uintptr cap) {
    if (name == nullptr) return -kal_err_invalid;
    for (char** e = okm::g_envp; e && *e; ++e) {
        const char* entry = *e;
        kal_uintptr i = 0;
        while (i < name_len && entry[i] != '\0' && entry[i] == name[i]) ++i;
        if (i == name_len && entry[i] == '=') {
            const char* v = entry + name_len + 1;
            return give(v, okm::length(v), out, cap);
        }
    }
    // A name that is not there is distinct from one whose value is empty.
    return -kal_err_not_found;
}

kal_uintptr kal_env_var_count(void) {
    kal_uintptr n = 0; for (char** e = okm::g_envp; e && *e; ++e) ++n; return n;
}

// The NAME at a position. The value is then obtained by kal_env_var: an
// operation answering both needs two buffers, two capacities and two lengths,
// and its second half is kal_env_var written again.
kal_intptr kal_env_var_at(kal_uintptr index, char* out, kal_uintptr cap) {
    kal_uintptr n = 0;
    for (char** e = okm::g_envp; e && *e; ++e, ++n) {
        if (n != index) continue;
        const char* entry = *e;
        kal_uintptr i = 0; while (entry[i] != '\0' && entry[i] != '=') ++i;
        return give(entry, i, out, cap);
    }
    return -kal_err_not_found;
}

}
