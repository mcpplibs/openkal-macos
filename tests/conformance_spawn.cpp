// What a started program receives: the three streams and the directories it was
// granted, and nothing else (SPEC.md clause 7.13).
//
// Each case starts a program and has the PROGRAM report what it received, since
// the claim is about the started image and not about the caller. Two programs
// are used. `/bin/sh' needs nothing to survive the start, so any descriptor
// above 2 it holds was conveyed by the start. This test itself, started again
// with `--grant-child', reads its preopens through `kal_fs_preopen', which is
// how a granted directory is defined to arrive.
//
// The three names declared below are the system's own and are reached without
// its headers, as src/unwind.cpp reaches dyld: `_NSGetExecutablePath' is how a
// program on this system learns where it is, and `realpath' makes that a name
// the root preopen can resolve. `dup2' places a descriptor WITHOUT the
// close-on-exec flag, which is the descriptor a caller inherits and did not grant.
import openkal.types;
import openkal.fs;
import openkal.stream;
import openkal.process;

extern "C" int   _NSGetExecutablePath(char* buf, unsigned* size);
extern "C" char* realpath(const char* path, char* resolved);
extern "C" int   dup2(int from, int to);

namespace {

int failures = 0;

kal_uintptr len(const char* s) { kal_uintptr n = 0; while (s[n]) ++n; return n; }

bool same(const char* a, kal_uintptr n, const char* b) {
    if (len(b) != n) return false;
    for (kal_uintptr i = 0; i < n; ++i) if (a[i] != b[i]) return false;
    return true;
}

void say(const char* s) { kal::write(kal::err(), s, len(s)); }

void check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    say("FAIL: "); say(what); say("\n");
}

const char kMarker[] = "okm-grant-marker";

kal_dir working() { return kal::fs::working(); }

kal_dir root() {
    for (kal_uintptr i = 0; i < kal_fs_preopen_count(); ++i) {
        kal_dir d{}; char n[8]; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, n, sizeof n, &l) == kal_ok && l == 1 && n[0] == '/') return d;
    }
    return kal_dir{};
}

bool write_marker(kal_dir d, const char* text) {
    kal_file f{};
    const auto flags = kal::fs::open::write | kal::fs::open::create | kal::fs::open::truncate;
    if (kal::fs::open_file(d, kMarker, sizeof kMarker - 1, flags, &f) != kal_ok) return false;
    const kal_intptr r = kal_stream_write(kal_fs_stream(f), text, len(text));
    kal_fs_close_file(f);
    return r == static_cast<kal_intptr>(len(text));
}

// --- the started side --------------------------------------------------------
//
// argv: --grant-child <count|ambient> {<name> <expect>}
// <expect> is the content of the marker file in that directory, or `@<entry>'
// for a directory entry that must exist there. The status names the first
// disagreement, so that a failure in the caller says which one it was.
int grant_child(char** argv) {
    const kal_uintptr n = kal_fs_preopen_count();
    if (same(argv[2], len(argv[2]), "ambient")) {
        kal_dir d{}; char nm[8]; kal_uintptr l = 0;
        if (n != 2) return 10;
        if (kal_fs_preopen(1, &d, nm, sizeof nm, &l) != kal_ok || l != 1 || nm[0] != '/') return 11;
        return 0;
    }
    kal_uintptr want = 0;
    for (const char* c = argv[2]; *c; ++c) want = want * 10 + static_cast<kal_uintptr>(*c - '0');
    if (n != want) return 20;
    for (kal_uintptr i = 0; i < want; ++i) {
        const char* name = argv[3 + 2 * i];
        const char* expect = argv[4 + 2 * i];
        kal_dir d{}; char nm[256]; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, nm, sizeof nm, &l) != kal_ok) return 30 + static_cast<int>(i);
        if (!same(nm, l, name)) return 40 + static_cast<int>(i);
        if (expect[0] == '@') {
            kal_node_info info = kal::fs::info_for_caller();
            if (kal_fs_info(d, expect + 1, len(expect + 1), kal::fs::field::kind, 0, &info) != kal_ok
                || info.kind != kal_node_directory)
                return 50 + static_cast<int>(i);
        } else {
            kal_file f{}; char buf[64];
            if (kal::fs::open_file(d, kMarker, sizeof kMarker - 1, kal::fs::open::read, &f) != kal_ok)
                return 60 + static_cast<int>(i);
            const kal_intptr r = kal_stream_read(kal_fs_stream(f), buf, sizeof buf);
            kal_fs_close_file(f);
            if (r < 0 || !same(buf, static_cast<kal_uintptr>(r), expect))
                return 70 + static_cast<int>(i);
        }
    }
    return 0;
}

// --- the starting side -------------------------------------------------------

char g_self[1024];

// Starts this test again under the root preopen and answers the status it
// finished with, or -1 when the start itself was refused.
int run_child(const kal_preopen* grants, kal_uintptr count,
              const char* const* args, kal_uintptr nargs,
              const char** envp = nullptr, kal_uintptr envc = 0) {
    const char* argv[16] = { "conformance_spawn", "--grant-child" };
    kal_uintptr lens[16] = { 17, 13 };
    for (kal_uintptr i = 0; i < nargs; ++i) { argv[2 + i] = args[i]; lens[2 + i] = len(args[i]); }
    kal_uintptr elens[4] = {};
    for (kal_uintptr i = 0; i < envc; ++i) elens[i] = len(envp[i]);
    const kal_spawn how{ root(), working(), nullptr, grants, count, 0 };
    kal_process p{};
    const char* rel = g_self + 1;
    if (kal_process_spawn(&how, rel, len(rel), argv, lens, 2 + nargs,
                          envp, elens, envc, nullptr, &p) != kal_ok)
        return -1;
    int status = -1, terminated = -1;
    kal_process_wait(p, &status, &terminated);
    kal_process_close(p);
    return terminated == 0 ? status : -1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 && same(argv[1], len(argv[1]), "--grant-child")) return grant_child(argv);

    char raw[1024]; unsigned size = sizeof raw;
    const bool named = _NSGetExecutablePath(raw, &size) == 0 && realpath(raw, g_self) != nullptr;
    check(named && g_self[0] == '/' && g_self[1] != '\0', "the test can name its own program");
    check(root().h != 0, "a directory covering the file system is supplied");

    // Two directories with distinct contents, so that a grant arriving under
    // the wrong name is told apart from one arriving under the right name.
    const char* da = "okm-grant-a.tmp";
    const char* db = "okm-grant-b.tmp";
    kal_fs_mkdir(working(), da, len(da));
    kal_fs_mkdir(working(), db, len(db));
    kal_dir a{}, b{};
    check(kal_fs_open_dir(working(), da, len(da), &a) == kal_ok
          && kal_fs_open_dir(working(), db, len(db), &b) == kal_ok,
          "two directories are made");
    check(write_marker(a, "A") && write_marker(b, "B"), "each holds its own marker");

    // Named grants arrive in order, under their names, as the directories named.
    // The second name holds both separators the variable uses.
    {
        const kal_preopen g[2] = { { a, "/first", 6 }, { b, "b;2,x", 5 } };
        const char* args[] = { "2", "/first", "A", "b;2,x", "B" };
        check(run_child(g, 2, args, 5) == 0,
              "granted directories arrive in order, under their names, as the directories named");
    }
    {
        const kal_preopen g[2] = { { b, "/b", 2 }, { a, "/a", 2 } };
        const char* args[] = { "2", "/b", "B", "/a", "A" };
        check(run_child(g, 2, args, 5) == 0, "and in the other order");
    }

    // The caller's own preopens occupy the numbers grants are placed at. Placing
    // one at a time where it stood made the second of these a copy of the first,
    // and made the first, already at its own position, never arrive at all.
    {
        const kal_preopen g[2] = { { root(), "/", 1 }, { working(), "/work", 5 } };
        const char* args[] = { "2", "/", "@usr", "/work", "@okm-grant-a.tmp" };
        check(run_child(g, 2, args, 5) == 0,
              "grants whose sources occupy each other's positions arrive as themselves");
        const kal_preopen h[2] = { { working(), "/work", 5 }, { root(), "/", 1 } };
        const char* hargs[] = { "2", "/work", "@okm-grant-a.tmp", "/", "@usr" };
        check(run_child(h, 2, hargs, 5) == 0, "and in the other order");
    }

    // A count of zero is a request for no preopens, which is different from not
    // asking. Not asking leaves what the implementation supplies by default.
    {
        const kal_preopen none[1] = {};
        const char* zero[] = { "0" };
        check(run_child(none, 0, zero, 1) == 0, "a count of zero starts a program with no preopens");
        const char* ambient[] = { "ambient" };
        check(run_child(nullptr, 0, ambient, 1) == 0,
              "not asking leaves the directories supplied by default");
    }

    // A value of the variable the caller supplies is not forwarded, whether or
    // not it names the process: it is the implementation's to write.
    {
        const char* env[] = { "KAL_PREOPENS=0000000001;3,1,/" };
        const char* ambient[] = { "ambient" };
        check(run_child(nullptr, 0, ambient, 1, env, 1) == 0,
              "a variable the caller supplies is not taken for a grant");
    }

    // A name travels in the environment, which cannot carry a zero byte.
    {
        const kal_preopen g[1] = { { a, "x\0y", 3 } };
        const char* args[] = { "1", "x", "A" };
        check(run_child(g, 1, args, 3) == -1, "a name holding a zero byte is refused");
    }

    // An ordinary program needs nothing to survive the start, so it receives
    // nothing above the three streams --- including a descriptor this process
    // holds without the close-on-exec flag, which is placed at 77 for the
    // purpose. The glob is expanded before the loop, so the descriptor the shell
    // had open to list the directory is gone by the time it is tested.
    {
        kal_stream mine{}, theirs{};
        const bool planted = kal_process_channel(&mine, &theirs) == kal_ok
                          && dup2(static_cast<int>(theirs.h), 77) == 77;
        check(planted, "a descriptor without the close-on-exec flag is held");

        const char* body = "for f in /dev/fd/*; do [ -e \"$f\" ] || continue; "
                           "[ \"${f##*/}\" -gt 2 ] && { echo \"inherited $f\" >&2; exit 1; }; "
                           "done; exit 0";
        const char* sargv[3] = { "sh", "-c", body };
        const kal_uintptr slens[3] = { 2, 2, len(body) };
        const char* paths[2] = { "bin/sh", "usr/bin/sh" };
        const kal_spawn how{ root(), working(), nullptr, nullptr, 0, 0 };
        kal_process p{};
        int rc = kal_err_invalid;
        for (int i = 0; i < 2 && rc != kal_ok; ++i)
            rc = kal_process_spawn(&how, paths[i], len(paths[i]), sargv, slens, 3,
                                   nullptr, nullptr, 0, nullptr, &p);
        check(rc == kal_ok, "an ordinary program is started");
        if (rc == kal_ok) {
            int status = -1, terminated = -1;
            kal_process_wait(p, &status, &terminated);
            kal_process_close(p);
            check(terminated == 0 && status == 0,
                  "an ordinary program receives no descriptor above the three streams");
        }
        if (planted) {
            kal_process_channel_close(kal_stream{ 77 });
            kal_process_channel_close(mine);
            kal_process_channel_close(theirs);
        }
    }

    kal_fs_remove(a, kMarker, sizeof kMarker - 1);
    kal_fs_remove(b, kMarker, sizeof kMarker - 1);
    kal_fs_close_dir(a);
    kal_fs_close_dir(b);
    kal_fs_remove(working(), da, len(da));
    kal_fs_remove(working(), db, len(db));

    say("openkal-macos: what a started program receives\n");
    return failures == 0 ? 0 : 1;
}
