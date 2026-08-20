#include "sys.h"
#include "handle.h"
#include <openkal/process.h>
#include <openkal/memory.h>

// A program image that has been started.
//
// This system has no operation that starts a program relative to a directory,
// so the directory is entered by the duplicate before it replaces itself --- a
// duplicate that exists for the length of two calls, is not a resource the
// caller receives, and is exactly what openkal declines to offer for that
// reason. The caller's own working directory is not touched, which is the
// property the interface requires and which an implementation that entered the
// directory before duplicating would not have.

namespace {

constexpr kal_uintptr kMaxEntries = 512;

// The counted arrays the interface takes become the terminated arrays this
// kernel takes. Every allocation happens before the program is duplicated, so
// that the duplicate performs nothing but a few calls: a duplicate of a program
// that has more than one execution context may hold a lock no context in it
// will release.
struct vector {
    char** slots = nullptr;
    char*  bytes = nullptr;
    okm_uptr slots_bytes = 0;
    okm_uptr bytes_bytes = 0;
    bool   ok = true;

    bool build(const char** items, const kal_uintptr* lens, kal_uintptr n) {
        if (n > kMaxEntries) { ok = false; return false; }
        okm_uptr total = 0;
        for (kal_uintptr i = 0; i < n; ++i) total += lens[i] + 1;
        slots_bytes = (n + 1) * sizeof(char*);
        bytes_bytes = total == 0 ? 1 : total;
        slots = static_cast<char**>(kal_alloc(slots_bytes, alignof(char*)));
        bytes = static_cast<char*>(kal_alloc(bytes_bytes, 1));
        if (!slots || !bytes) { ok = false; return false; }
        okm_uptr at = 0;
        for (kal_uintptr i = 0; i < n; ++i) {
            okm::copy(bytes + at, items[i], lens[i]);
            bytes[at + lens[i]] = '\0';
            slots[i] = bytes + at;
            at += lens[i] + 1;
        }
        slots[n] = nullptr;
        return true;
    }

    ~vector() {
        if (slots) kal_free(slots, slots_bytes, alignof(char*));
        if (bytes) kal_free(bytes, bytes_bytes, 1);
    }
};

constexpr okm_long nr_fchdir = 13;

}  // namespace

extern "C" {

int kal_process_spawn(kal_dir base,
                      const char* path, kal_uintptr path_len,
                      const char** argv, const kal_uintptr* argv_lens, kal_uintptr argc,
                      const char** envp, const kal_uintptr* envp_lens, kal_uintptr envc,
                      const kal_spawn_streams* streams,
                      kal_process* out) {
    const int b = okm::unpack(base.h);
    if (b < 0 || out == nullptr) return kal_err_invalid;
    if (!okm::acceptable(path, path_len)) return kal_err_invalid;
    okm::terminated p(path, path_len);
    if (!p.ok) return kal_err_invalid;

    // The vector is passed unaltered. Clause 7.6: argv[0] is the name the
    // started program observes as its own, and it is the caller's to choose ---
    // the started program reads it through kal_env_arg(0), so a caller that did
    // not supply it could not predict what the program would read.
    vector args, envs;
    if (!args.build(argv, argv_lens, argc)) return kal_err_no_memory;
    if (!envs.build(envp, envp_lens, envc)) return kal_err_no_memory;

    const okm_long in = streams ? static_cast<okm_long>(streams->in)  : 0;
    const okm_long ou = streams ? static_cast<okm_long>(streams->out) : 0;
    const okm_long er = streams ? static_cast<okm_long>(streams->err) : 0;

    bool is_duplicate = false;
    const okm_long child = okm::duplicate(is_duplicate);
    if (okm::failed(child)) return okm::translate(child);

    // The duplicate is distinguished by the second value the call returns and
    // not by the first: both images receive the same first value here. The
    // reason, and what happens to an implementation that tests the first alone,
    // are in src/sys.h beside the call.
    if (is_duplicate) {
        if (in != 0) okm::sys(okm::nr_dup2, in, 0);
        if (ou != 0) okm::sys(okm::nr_dup2, ou, 1);
        if (er != 0) okm::sys(okm::nr_dup2, er, 2);
        okm::sys(nr_fchdir, b);
        okm::sys(okm::nr_execve, reinterpret_cast<okm_long>(p.buf),
                 reinterpret_cast<okm_long>(args.slots),
                 reinterpret_cast<okm_long>(envs.slots));
        for (;;) okm::sys(okm::nr_exit, 127);
    }

    *out = kal_process{ static_cast<kal_uintptr>(child) };
    return kal_ok;
}

int kal_process_wait(kal_process h, int* status, int* terminated_by_environment) {
    if (h.h == 0) return kal_err_invalid;
    int st = 0;
    for (;;) {
        const okm_long r = okm::sys(okm::nr_wait4, static_cast<okm_long>(h.h),
                                    reinterpret_cast<okm_long>(&st), 0, 0);
        if (okm::interrupted(r)) continue;
        if (okm::failed(r)) return okm::translate(r);
        break;
    }
    // The encoding is the kernel's: the low seven bits name the signal that
    // ended the program and are zero when it ended by returning, in which case
    // the next eight bits are what it returned.
    const int signalled = st & 0x7f;
    if (signalled == 0) {
        if (status) *status = (st >> 8) & 0xff;
        if (terminated_by_environment) *terminated_by_environment = 0;
    } else {
        if (status) *status = signalled;
        if (terminated_by_environment) *terminated_by_environment = 1;
    }
    return kal_ok;
}

int kal_process_terminate(kal_process h) {
    if (h.h == 0) return kal_err_invalid;
    const okm_long r = okm::sys(okm::nr_kill, static_cast<okm_long>(h.h), 15 /* SIGTERM */);
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

// Releasing the handle does not affect the program. A program that has not been
// waited for continues, and this environment collects it when the caller exits.
void kal_process_close(kal_process) { }

const kal_uintptr kal_process_props =
    KAL_PROCESS_PROP_TERMINATE | KAL_PROCESS_PROP_STREAM_PASSING
  | KAL_PROCESS_PROP_EXIT_STATUS;

}
