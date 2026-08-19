#include <spawn.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include "handle.h"
import openkal.process;
import openkal.types;

extern "C" char** environ;

namespace {

int translate(int e) {
    switch (e) {
        case EBADF: case EINVAL: case ENOENT: return kal_err_invalid;
        case ENOMEM:                          return kal_err_no_memory;
        case EACCES: case EPERM:              return kal_err_permission;
        case ENOSYS: case ENOTSUP:            return kal_err_not_supported;
        default:                              return kal_err_io;
    }
}

// A bounded copy of a counted string, as in the file system implementation.
struct terminated {
    char buf[4096];
    bool ok;
    terminated(const char* s, kal_uintptr n) : ok(n < sizeof(buf)) {
        if (ok) { for (kal_uintptr i = 0; i < n; ++i) buf[i] = s[i]; buf[n] = '\0'; }
    }
};

// The counted arrays the interface takes are converted to the terminated arrays
// the environment takes. The conversion is bounded and is released on return.
struct vector {
    char*  storage[256];
    char*  slots[257];
    kal_uintptr used = 0;
    bool   ok = true;

    bool add(const char* s, kal_uintptr n) {
        if (used >= 256 || n >= 4096) { ok = false; return false; }
        char* p = static_cast<char*>(::malloc(n + 1));
        if (p == nullptr) { ok = false; return false; }
        for (kal_uintptr i = 0; i < n; ++i) p[i] = s[i];
        p[n] = '\0';
        storage[used] = p; slots[used] = p; ++used; slots[used] = nullptr;
        return true;
    }
    ~vector() { for (kal_uintptr i = 0; i < used; ++i) ::free(storage[i]); }
};

}  // namespace

extern "C" {

int kal_process_spawn(kal_dir base,
                      const char* path, kal_uintptr path_len,
                      const char** argv, const kal_uintptr* argv_lens, kal_uintptr argc,
                      const char** envp, const kal_uintptr* envp_lens, kal_uintptr envc,
                      const kal_spawn_streams* streams,
                      kal_process* out) {
    const int b = okl::unpack(base.h);
    if (b < 0 || out == nullptr) return kal_err_invalid;
    terminated p(path, path_len);
    if (!p.ok) return kal_err_invalid;

    vector args, envs;
    args.add(p.buf, path_len);                       // the started program's own name
    for (kal_uintptr i = 0; i < argc; ++i) args.add(argv[i], argv_lens[i]);
    for (kal_uintptr i = 0; i < envc; ++i) envs.add(envp[i], envp_lens[i]);
    if (!args.ok || !envs.ok) return kal_err_no_memory;

    // The started program's working directory is the directory supplied here.
    // The environment offers this as an attribute of the spawn rather than as a
    // change to the caller, which is what makes it usable from several contexts.
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return kal_err_io;
    // ⚠️ This platform's spawn has no action that sets the working directory of
    // the started program. The directory is therefore applied to the caller
    // around the spawn, which is correct for a single-context program and is
    // not correct in general.
    //
    // The difference is recorded rather than hidden. It is the kind of
    // divergence a second implementation exists to expose: an interface whose
    // parameter one platform honours as an attribute and another can only
    // approximate is an interface that has assumed a mechanism.
    const int saved = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (::fchdir(b) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        if (saved >= 0) ::close(saved);
        return translate(errno);
    }
    if (streams != nullptr) {
        if (streams->in  != 0) posix_spawn_file_actions_adddup2(&actions, static_cast<int>(streams->in),  0);
        if (streams->out != 0) posix_spawn_file_actions_adddup2(&actions, static_cast<int>(streams->out), 1);
        if (streams->err != 0) posix_spawn_file_actions_adddup2(&actions, static_cast<int>(streams->err), 2);
    }

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, p.buf, &actions, nullptr,
                               args.slots, envc == 0 ? environ : envs.slots);
    if (saved >= 0) { (void)::fchdir(saved); ::close(saved); }
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) return translate(rc);

    *out = kal_process{ static_cast<kal_uintptr>(pid) };
    return kal_ok;
}

int kal_process_wait(kal_process h, int* status, int* terminated_by_environment) {
    if (h.h == 0) return kal_err_invalid;
    int st = 0;
    for (;;) {
        const pid_t r = ::waitpid(static_cast<pid_t>(h.h), &st, 0);
        if (r < 0) { if (errno == EINTR) continue; return translate(errno); }
        break;
    }
    if (WIFEXITED(st)) {
        if (status) *status = WEXITSTATUS(st);
        if (terminated_by_environment) *terminated_by_environment = 0;
    } else {
        if (status) *status = WIFSIGNALED(st) ? WTERMSIG(st) : -1;
        if (terminated_by_environment) *terminated_by_environment = 1;
    }
    return kal_ok;
}

int kal_process_terminate(kal_process h) {
    if (h.h == 0) return kal_err_invalid;
    return ::kill(static_cast<pid_t>(h.h), SIGTERM) == 0 ? kal_ok : translate(errno);
}

// Releasing the handle does not affect the program. A program that has not been
// waited for continues, and this environment collects it when the caller exits.
void kal_process_close(kal_process) { }

const kal_uintptr kal_process_props =
    kal::process::prop_terminate | kal::process::prop_stream_passing
  | kal::process::prop_exit_status;

}
