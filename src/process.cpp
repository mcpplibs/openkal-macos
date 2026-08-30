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
// openkal 0.11: a started program that forms a job of its own.
constexpr okm_long nr_setpgid = 82;
constexpr okm_long nr_getpgid = 151;

}  // namespace

extern "C" {

// Starting a program. One function since openkal 0.11, where three declarations
// became one and their modifiers became positions in `kal_spawn'.
//
// ⚠️⚠️ AND THIS IMPLEMENTATION IS WHERE THE MISSING MODIFIER WAS ALREADY VISIBLE,
// which is worth recording rather than quietly fixing.
//
// This kernel has no `execveat', so a program named relative to a directory has
// always been started by entering that directory first --- the `fchdir(base)'
// below used to be the whole story. So a started program's working directory WAS
// `base' here, and on the other kernel it was whatever that implementation
// happened to be in. ⭐ Same openkal calls, two different observable answers,
// and neither was wrong because the specification said nothing.
//
// ⇒ 0.11 gives the caller a second directory, and both implementations now enter
// the one the caller named. The divergence is gone because the thing that caused
// it --- a property nobody had declared --- is declared.
int kal_process_spawn(const kal_spawn* how,
                      const char* path, kal_uintptr path_len,
                      const char** argv, const kal_uintptr* argv_lens, kal_uintptr argc,
                      const char** envp, const kal_uintptr* envp_lens, kal_uintptr envc,
                      const kal_spawn_streams* streams,
                      kal_process* out) {
    if (how == nullptr || out == nullptr) return kal_err_invalid;

    const int b = okm::unpack(how->base.h);
    const int w = okm::unpack(how->work.h);
    if (b < 0 || w < 0) return kal_err_invalid;
    if (!okm::acceptable(path, path_len)) return kal_err_invalid;
    if (how->grant_count > 0 && how->grants == nullptr) return kal_err_invalid;

    // ⚠️ A LIFETIME THIS KERNEL CANNOT BIND IS REFUSED BEFORE ANYTHING STARTS.
    // The reason is unchanged from 0.10 and is stated at kal_process_props: the
    // binding must hold however the caller ends, including when it is killed
    // outright, and what this system offers instead is a WATCH, which needs a
    // live context to notice. Composing it would move the defect from "refused"
    // to "works except when it matters".
    constexpr kal_uintptr can = KAL_SPAWN_OWN_JOB;
    if (how->flags & ~can) return kal_err_not_supported;

    okm::terminated p(path, path_len);
    if (!p.ok) return kal_err_invalid;

    vector args, envs;
    if (!args.build(argv, argv_lens, argc)) return kal_err_no_memory;
    if (!envs.build(envp, envp_lens, envc)) return kal_err_no_memory;

    constexpr kal_uintptr max_grants = 16;
    if (how->grant_count > max_grants) return kal_err_invalid;
    int granted[max_grants];
    for (kal_uintptr i = 0; i < how->grant_count; ++i) {
        granted[i] = okm::unpack(how->grants[i].dir.h);
        if (granted[i] < 0) return kal_err_invalid;
    }

    // ⭐ THE PROGRAM'S NAME IS MADE ABSOLUTE BEFORE THE DIRECTORY MOVES, because
    // with no `execveat' the two things `base' and `work' now mean cannot both be
    // served by one `fchdir'. `F_GETPATH' answers the path of an open directory,
    // which src/fs.cpp already relies on for the same reason: this kernel has no
    // call that reports a working directory, so a path is obtained from the
    // descriptor that names it.
    char whole[1024];
    kal_uintptr n = 0;
    if (p.buf[0] == '/') {
        while (p.buf[n] && n < sizeof whole - 1) { whole[n] = p.buf[n]; ++n; }
    } else {
        const okm_long r = okm::sys(okm::nr_fcntl, b, okm::f_getpath,
                                    reinterpret_cast<okm_long>(whole));
        if (okm::failed(r)) return okm::translate(r);
        while (whole[n] && n < sizeof whole - 1) ++n;
        if (n && whole[n - 1] != '/' && n < sizeof whole - 1) whole[n++] = '/';
        for (kal_uintptr i = 0; p.buf[i] && n < sizeof whole - 1; ++i) whole[n++] = p.buf[i];
    }
    if (n >= sizeof whole - 1) return kal_err_invalid;
    whole[n] = '\0';

    const okm_long in = streams ? static_cast<okm_long>(streams->in.h)  : 0;
    const okm_long ou = streams ? static_cast<okm_long>(streams->out.h) : 0;
    const okm_long er = streams ? static_cast<okm_long>(streams->err.h) : 0;

    const bool job = (how->flags & KAL_SPAWN_OWN_JOB) != 0;

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

        for (kal_uintptr i = 0; i < how->grant_count; ++i) {
            const okm_long want = static_cast<okm_long>(3 + i);
            if (granted[i] != want) okm::sys(okm::nr_dup2, granted[i], want);
        }

        // The directory the program RUNS in --- `whole' already carries where it
        // is named from, so this no longer has to serve both.
        okm::sys(nr_fchdir, w);

        if (job) okm::sys(nr_setpgid, 0, 0);

        okm::sys(okm::nr_execve, reinterpret_cast<okm_long>(whole),
                 reinterpret_cast<okm_long>(args.slots),
                 reinterpret_cast<okm_long>(envs.slots));
        for (;;) okm::sys(okm::nr_exit, 127);
    }

    *out = kal_process{ static_cast<kal_uintptr>(child) };
    return kal_ok;
}

// A channel: a pair of streams of which one end is meant to cross a spawn.
//
// This kernel's `pipe' reports BOTH descriptors as return values rather than
// through a buffer, which is a property of its calling convention and not of the
// call: the second value comes back in the second register. src/sys.h says the
// same thing about the duplication primitive, and for the same reason.
//
// THERE IS NO pipe2 HERE, so close-on-exec is set afterwards with fcntl. Doing
// it in two steps is not equivalent under a concurrent spawn --- another context
// starting a program between the two would inherit the descriptors --- and this
// implementation states that rather than concealing it. A caller that spawns
// from one context, which is what a program using this operation does, is not
// affected.
int kal_process_channel(kal_stream* mine, kal_stream* theirs) {
    if (mine == nullptr || theirs == nullptr) return kal_err_invalid;

    okm_long second = 0;
    const okm_long first = okm::pipe_pair(second);
    if (okm::failed(first)) return okm::translate(first);

    constexpr okm_long f_setfd = 2, fd_cloexec = 1;
    okm::sys(okm::nr_fcntl, first,  f_setfd, fd_cloexec);
    okm::sys(okm::nr_fcntl, second, f_setfd, fd_cloexec);

    // Bare descriptors, because openkal.stream's transfer operations take what
    // this kernel takes. kal_fs_stream reports a file's stream the same way.
    *mine   = kal_stream{ static_cast<kal_uintptr>(first)  };   // the reading end
    *theirs = kal_stream{ static_cast<kal_uintptr>(second) };   // the writing end
    return kal_ok;
}

void kal_process_channel_close(kal_stream s) {
    // The standard streams are borrowed and are numbered 0, 1 and 2; closing one
    // of those through this operation would take a stream away from the whole
    // program.
    const okm_long fd = static_cast<okm_long>(s.h);
    if (fd < 3) return;
    okm::sys(okm::nr_close, fd);
}

// Starting a program that receives exactly the directories named.

// ⭐ REACHES THE WHOLE JOB WHEN THERE IS ONE, AND THE HANDLE CARRIES NOTHING TO
// SAY SO. A program started with KAL_SPAWN_OWN_JOB called `setpgid(0, 0)', so its
// group identifier is its own; one started without it inherited this
// implementation's, which is some other process. `getpgid(pid) == pid'
// distinguishes them exactly.
//
// ⚠️ Without this the flag would do nothing a caller could see: forming the job
// matters only because terminating then reaches what the started program itself
// started.
int kal_process_terminate(kal_process h) {
    if (h.h == 0) return kal_err_invalid;
    const okm_long pid  = static_cast<okm_long>(h.h);
    const okm_long pgid = okm::sys(nr_getpgid, pid);
    const okm_long target = (!okm::failed(pgid) && pgid == pid) ? -pid : pid;
    const okm_long r = okm::sys(okm::nr_kill, target, 15 /* SIGTERM */);
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

// Releasing the handle does not affect the program. A program that has not been
// waited for continues, and this environment collects it when the caller exits.
void kal_process_close(kal_process) { }

// Starting a program whose lifetime is bound to this one's. Version 0.10.
//

kal_uintptr kal_process_props(void) { return
    KAL_PROCESS_PROP_TERMINATE | KAL_PROCESS_PROP_STREAM_PASSING
  | KAL_PROCESS_PROP_EXIT_STATUS
  | KAL_PROCESS_PROP_CHANNEL | KAL_PROCESS_PROP_GRANT_DIR
  | KAL_PROCESS_PROP_OWN_JOB; }

}
