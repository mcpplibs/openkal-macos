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
// Whether SIGPIPE was ignored HERE, at the default this program was started
// with: a program it starts is given the default back (process.cpp).
bool   g_pipe_ignored_here = false;

void record(int argc, char** argv, char** envp) {
    g_argc = argc; g_argv = argv; g_envp = envp;
}

}  // namespace okm

namespace {

// A PROGRAM ABOVE openkal SHALL NOT BE ENDED BY SOMETHING openkal NEVER TOLD
// IT ABOUT. openkal defines no signals, and `kal_stream_write' is required to
// REPORT that the far end of a stream is gone --- while this kernel delivers
// SIGPIPE, whose default action ends the program instead.
//
// A C library above answers `signal(SIGPIPE, SIG_IGN)' truthfully, because
// openkal has no signals and there is nothing for it to set; the program is then
// killed anyway, by a mechanism no layer between it and here can name. Ignored
// at this level because this is the only level that can. Found on the other
// implementation, fixed on both --- a divergence here would be the same defect
// with a different exit status.
//
// Not a policy about signals in general: this is the one an ordinary openkal
// operation provokes.
//
// AND IT IS NOT FIXED HERE YET, WHICH IS RECORDED RATHER THAN LEFT TO BE
// DISCOVERED. openkal-linux ignores it in one call. This kernel's `sigaction'
// takes a `struct __sigaction' carrying a TRAMPOLINE that its C library
// supplies, and a disposition installed with the wrong shape is the kind of
// mistake that shows up as a program dying in a way nobody can trace --- which is
// the defect this note is about, arrived at from the other side.
//
// ⇒ MEASURED SINCE (0.15): the stop word installs a handler through this
// kernel's `sigaction' with this implementation's own trampoline, and the
// conformance suite raises the signal and returns from it. Ignoring needs no
// trampoline at all --- the kernel never enters one for an ignored signal ---
// so the shape is the measured one and the trampoline field is left empty.
//
// ONLY WHERE NOTHING ELSE HAS CHOSEN. A program whose own runtime installed a
// handler for SIGPIPE keeps it; a disposition still at its default is the one
// that ends the program without telling it, and that is the one replaced.
//
// AND IT IS THIS PROGRAM'S, NOT PASSED ON. An ignored disposition survives
// `execve', so a program started from here would begin with SIGPIPE ignored and
// a shell it runs could not undo it. `kal_process_spawn' gives the default back
// to the program it starts when the ignore was made here (process.cpp).
void ignore_sigpipe() {
    struct { void (*handler)(int); void* tramp; unsigned int mask; int flags; } act{};
    struct { void (*handler)(int); unsigned int mask; int flags; } old{};
    if (okm::failed(okm::sys(46 /* sigaction */, 13 /* SIGPIPE */, 0, reinterpret_cast<okm_long>(&old))))
        return;
    if (old.handler != nullptr) return;                 // not SIG_DFL
    act.handler = reinterpret_cast<void (*)(int)>(1);   // SIG_IGN
    if (!okm::failed(okm::sys(46 /* sigaction */, 13, reinterpret_cast<okm_long>(&act), 0)))
        okm::g_pipe_ignored_here = true;
}
[[gnu::constructor(101)]] void capture(int argc, char** argv, char** envp) {
    if (okm::g_argv == nullptr) okm::record(argc, argv, envp);
    ignore_sigpipe();
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

// THE IMAGE THE KERNEL STARTED (0.17): what `proc_pidpath' answers, the
// kernel's record of the file it mapped (`proc_info', PROC_PIDPATHINFO);
// where that is refused, `executable_path=' from the vector this kernel passes
// after the environment, which is the name the kernel was given.
// A program built for its environment was loaded by nothing the interface
// names (clause 12: only an image has a loader to name).
kal_intptr kal_env_loader(char* out, kal_uintptr cap) {
    (void)out; (void)cap;
    return -kal_err_not_found;
}

kal_intptr kal_env_image(char* out, kal_uintptr cap) {
    char buf[4096];
    const okm_long n = okm::sys(okm::nr_proc_info, 2 /* PROC_INFO_CALL_PIDINFO */,
                                okm::sys(okm::nr_getpid), 11 /* PROC_PIDPATHINFO */, 0,
                                reinterpret_cast<okm_long>(buf), sizeof buf);
    if (!okm::failed(n) && n > 0) {
        kal_uintptr len = 0; while (len < sizeof buf && buf[len] != '\0') ++len;
        if (len > 0) return give(buf, len, out, cap);
    }
    if (okm::g_envp != nullptr) {
        char** a = okm::g_envp;
        while (*a) ++a;
        for (++a; *a; ++a) {
            const char key[] = "executable_path=";
            kal_uintptr i = 0;
            while (key[i] != '\0' && (*a)[i] == key[i]) ++i;
            if (key[i] == '\0') return give(*a + i, okm::length(*a + i), out, cap);
        }
    }
    return -kal_err_not_found;
}

// For conventions only, as the declaration restricts it.
kal_intptr kal_env_system(char* out, kal_uintptr cap) {
    return give("macos", 5, out, cap);
}

}
