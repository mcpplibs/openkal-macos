// Conformance: openkal.process and openkal.task.
import openkal.process;
import openkal.task;
import openkal.fs;
import openkal.stream;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    const char pre[] = "FAIL: ";
    kal::write(kal::err(), pre, sizeof(pre) - 1);
    kal_uintptr n = 0; while (what[n]) ++n;
    kal::write(kal::err(), what, n);
    kal::write(kal::err(), "\n", 1);
}

// Shared between contexts. The word is what openkal.task suspends upon, and it
// is ordinary memory: the interface adds no object of its own.
volatile __UINT32_TYPE__ g_word = 0;
int g_ran = 0;

void worker(void* arg) {
    g_ran = 1;
    *static_cast<int*>(arg) = 42;
    __atomic_store_n(reinterpret_cast<__UINT32_TYPE__*>(const_cast<__UINT32_TYPE__*>(&g_word)),
                     1u, __ATOMIC_SEQ_CST);
    kal_uintptr woken = 0;
    kal_task_wake(const_cast<const __UINT32_TYPE__*>(&g_word), 1, &woken);
}
}

int main() {
    // --- openkal.process -----------------------------------------------------
    //
    // The program to start is reached through a directory the environment
    // supplied, which is the whole reason the set exists: a program and the
    // program it starts are commonly not beneath one root.
    kal_dir slash{}; const char* nm = nullptr; kal_uintptr nl = 0;
    bool have_root = false;
    for (kal_uintptr i = 0; i < kal::fs::preopen_count(); ++i) {
        kal_dir d{}; const char* n = nullptr; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, &n, &l) != kal_ok) continue;
        if (l == 1 && n[0] == '/') { slash = d; nm = n; nl = l; have_root = true; }
    }
    check(have_root, "a directory covering the file system is supplied");

    if (have_root) {
        // The program that succeeds and the program that fails are at
        // different places on different systems. The test locates them rather
        // than assuming, because assuming would make it a test of one system.
        const char* true_paths[]  = { "bin/true",  "usr/bin/true"  };
        const kal_uintptr true_lens[] = { 8, 12 };
        const char* false_paths[] = { "bin/false", "usr/bin/false" };
        const kal_uintptr false_lens[] = { 9, 13 };

        kal_process p{};
        const char* argv[] = { "openkal" };
        const kal_uintptr lens[] = { 7 };
        int rc = kal_err_invalid;
        for (int i = 0; i < 2 && rc != kal_ok; ++i)
            rc = kal_process_spawn(slash, true_paths[i], true_lens[i], argv, lens, 1,
                                   nullptr, nullptr, 0, nullptr, &p);
        check(rc == kal_ok, "a program is started");
        if (rc == kal_ok) {
            int status = -1, terminated = -1;
            check(kal_process_wait(p, &status, &terminated) == kal_ok, "the program is waited for");
            check(status == 0 && terminated == 0, "the status it finished with is reported");
            kal_process_close(p);
        }

        // A program that finishes with a non-zero status is distinguished from
        // one that succeeded. Without this, a harness that ignored the status
        // would pass.
        kal_process q{};
        const char* qargv[] = { "openkal" };
        int qrc = kal_err_invalid;
        for (int i = 0; i < 2 && qrc != kal_ok; ++i)
            qrc = kal_process_spawn(slash, false_paths[i], false_lens[i], qargv, lens, 1,
                                    nullptr, nullptr, 0, nullptr, &q);
        if (qrc == kal_ok) {
            int status = -1, terminated = -1;
            kal_process_wait(q, &status, &terminated);
            check(status != 0, "a non-zero status is reported as such");
            kal_process_close(q);
        }

        // Clause 7.6: the vector is passed unaltered, and argv[0] is the name
        // the started program observes as its own.
        //
        // The programs started above ignore their arguments, so they cannot
        // distinguish an implementation that passes the vector from one that
        // prepends the path — which is how that defect survived a suite that
        // started programs and read their statuses. A shell does distinguish
        // them: `sh -c <script>` takes $0 from its own argv[0] when no further
        // argument is given, so the script observes the name the caller chose.
        // An implementation that prepended the path would give the shell an
        // extra argument, which it would read as a script file to open, and the
        // status would be non-zero.
        const char* sh_paths[] = { "bin/sh", "usr/bin/sh" };
        const kal_uintptr sh_lens[] = { 6, 10 };
        const char* script = "test \"$0\" = openkal-observed-argv0";
        kal_uintptr script_len = 0; while (script[script_len]) ++script_len;

        kal_process r{};
        const char*       rargv[] = { "openkal-observed-argv0", "-c", script };
        const kal_uintptr rlens[] = { 22, 2, script_len };
        int rrc = kal_err_invalid;
        for (int i = 0; i < 2 && rrc != kal_ok; ++i)
            rrc = kal_process_spawn(slash, sh_paths[i], sh_lens[i], rargv, rlens, 3,
                                    nullptr, nullptr, 0, nullptr, &r);
        check(rrc == kal_ok, "a shell is started");
        if (rrc == kal_ok) {
            int status = -1, terminated = -1;
            kal_process_wait(r, &status, &terminated);
            check(status == 0 && terminated == 0,
                  "the started program observes the argument vector the caller supplied, unaltered");
            kal_process_close(r);
        }
    }

    // A name that ascends is refused here as it is in the file system.
    kal_process bad{};
    check(kal_process_spawn(kal::fs::working(), "../bin/true", 11,
                            nullptr, nullptr, 0, nullptr, nullptr, 0, nullptr, &bad)
          != kal_ok, "an ascending program name is refused");

    // --- openkal.task --------------------------------------------------------
    int written = 0;
    kal_task t{};
    check(kal_task_start(worker, &written, &t) == kal_ok, "an execution context starts");

    // Suspension upon the word, which the worker changes and then wakes. The
    // loop re-examines the condition after waking, because waking is permitted
    // to be spurious and the specification says so.
    while (__atomic_load_n(reinterpret_cast<__UINT32_TYPE__*>(
               const_cast<__UINT32_TYPE__*>(&g_word)), __ATOMIC_SEQ_CST) == 0u) {
        kal_task_wait(const_cast<const __UINT32_TYPE__*>(&g_word), 0u,
                      1000ull * 1000 * 1000);
    }
    check(kal_task_join(t) == kal_ok, "the context is joined");
    check(g_ran == 1 && written == 42, "the context ran and observed shared memory");

    // A wait whose expected value does not match returns rather than suspending,
    // which is what makes the primitive usable without losing a wake.
    __UINT32_TYPE__ other = 5;
    check(kal_task_wait(&other, 6u, 1000ull * 1000) == kal_ok,
          "a wait on a value that already differs returns");

    kal_task_yield();
    check(kal_task_current() != 0, "the calling context has an identity");

    const char ok[] = "openkal-linux: process and task conformance\n";
    kal::write(kal::out(), ok, sizeof(ok) - 1);
    return failures == 0 ? 0 : 1;
}
