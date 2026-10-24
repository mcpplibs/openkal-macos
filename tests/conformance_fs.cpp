// Conformance: openkal.fs.
//
// The interface is relative to a directory throughout, and the suite exercises
// that property directly: a name that would ascend is rejected, which is what
// makes a program confinable by the directory it was given.
#include <fcntl.h>

import openkal.fs;
import openkal.stream;
import openkal.task;

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
}

int main() {
    check(kal::fs::preopen_count() >= 1, "the environment supplies at least one directory");
    auto root = kal::fs::working();
    check(root.h != 0, "the working directory is the first entry");

    // Each supplied directory carries the name the environment gives it.
    kal_dir d0{}; char n0[512]; kal_uintptr l0 = 0;
    check(kal_fs_preopen(0, &d0, n0, sizeof n0, &l0) == kal_ok && l0 > 0,
          "a supplied directory carries a name");
    kal_dir beyond{};
    check(kal_fs_preopen(kal::fs::preopen_count(), &beyond, nullptr, 0, nullptr) != kal_ok,
          "an index beyond the set is refused");

    // Creation, writing, reading back, and removal.
    kal_file f{};
    check(kal_fs_open(root, "okl_probe.txt", 13,
                    (kal::fs::open::read | kal::fs::open::write
                     | kal::fs::open::create | kal::fs::open::truncate).bits, &f) == kal_ok,
          "a file is created");
    const kal_stream s = kal_fs_stream(f);
    const char payload[] = "conformance";
    check(kal::write(s, payload, sizeof(payload) - 1)
              == static_cast<kal_intptr>(sizeof(payload) - 1), "the file is written");

    __UINT64_TYPE__ pos = 0;
    check(kal_fs_seek(f, 0, kal::fs::seek_set, &pos) == kal_ok && pos == 0,
          "the file is repositioned");
    char back[32] = {};
    const kal_intptr r = kal::read(s, back, sizeof(back));
    check(r == static_cast<kal_intptr>(sizeof(payload) - 1), "the file reads back");
    for (kal_uintptr i = 0; i < sizeof(payload) - 1; ++i)
        check(back[i] == payload[i], "the contents match");
    kal_fs_close_file(f);

    // Enquiry reports what was written, and reports absence without failing.
    kal_node_info info{}; info.self_size = sizeof info;
    check(kal_fs_info(root, "okl_probe.txt", 13, 0, kal::fs::field::all, &info) == kal_ok, "enquiry succeeds");
    check(info.kind == kal_node_file, "the node is a file");
    check(info.size == sizeof(payload) - 1, "the size is reported");
    kal_node_info absent{}; absent.self_size = sizeof absent;
    check(kal_fs_info(root, "okl_absent", 10, 0, kal::fs::field::all, &absent) == kal_ok
          && absent.kind == kal_node_absent,
          "an absent name is reported absent rather than as a failure");

    // A directory, an enumeration that finds what was placed in it, and removal.
    check(kal_fs_mkdir(root, "okl_dir", 7) == kal_ok, "a directory is created");
    kal_dir d{};
    check(kal_fs_open_dir(root, "okl_dir", 7, &d) == kal_ok, "the directory opens");
    kal_file inner{};
    check(kal_fs_open(d, "inner", 5,
                    (kal::fs::open::read | kal::fs::open::write
                     | kal::fs::open::create | kal::fs::open::truncate).bits, &inner) == kal_ok, "a file is created within");
    kal_fs_close_file(inner);
    kal_uintptr iter = 0; bool found = false;
    check(kal_fs_list_begin(d, &iter) == kal_ok, "enumeration begins");
    for (;;) {
        char name[512]; kal_uintptr len = 0; int kind = 0;
        if (kal_fs_list_next(d, &iter, name, sizeof name, &len, &kind) != kal_ok) break;
        // The iterator becoming zero is how the end is reported now.
        if (iter == 0) break;
        if (len == 5 && name[0] == 'i') found = true;
    }
    check(found, "enumeration finds the entry");

    // The property the interface exists to have: a name that ascends is
    // refused, so a program cannot leave the directory it was given.
    kal_dir escape{};
    check(kal_fs_open_dir(d, "..", 2, &escape) != kal_ok, "an ascending name is refused");
    check(kal_fs_open_dir(d, "../..", 5, &escape) != kal_ok, "a compound ascent is refused");
    check(kal_fs_open_dir(d, "/etc", 4, &escape) != kal_ok, "an absolute name is refused");

    // A released handle is not valid, which the specification requires.
    kal_fs_close_dir(d);
    kal_file after{};
    check(kal_fs_open(d, "inner", 5, kal::fs::open::read.bits, &after) != kal_ok,
          "a released handle is not treated as valid");

    // An enumeration left part-way is ended by closing the directory that
    // produced it, which fs.h states. Counted in this system's own table: the
    // Linux implementation had the same shape and kept one descriptor for every
    // directory a C library above it had stopped reading early
    // (mcpplibs/openkal-linux#34), and this system's default limit is 256.
    {
        auto open_now = [] { int n = 0; for (int fd = 0; fd < 4096; ++fd) if (fcntl(fd, F_GETFD) != -1) ++n; return n; };
        const int before = open_now();
        for (int i = 0; i < 64; ++i) {
            kal_dir again{}; kal_uintptr it = 0;
            if (kal_fs_open_dir(root, "okl_dir", 7, &again) != kal_ok) { check(false, "the directory opens again"); break; }
            if (kal_fs_list_begin(again, &it) == kal_ok) {
                char name[512]; kal_uintptr len = 0; int kind = 0;
                kal_fs_list_next(again, &it, name, sizeof name, &len, &kind);
            }
            kal_fs_close_dir(again);
        }
        check(open_now() == before, "closing a directory ends an enumeration left part-way");
    }

    // A closed directory is a released handle, and the iterator it produced was
    // released with it (fs.h, version 0.16).
    {
        kal_dir once{}; kal_uintptr it = 0;
        if (kal_fs_open_dir(root, "okl_dir", 7, &once) == kal_ok
            && kal_fs_list_begin(once, &it) == kal_ok) {
            kal_fs_close_dir(once);
            char name[512]; kal_uintptr len = 0; int kind = 0;
            check(kal_fs_list_next(once, &it, name, sizeof name, &len, &kind) == kal_err_invalid,
                  "an enumeration of a closed directory is refused");
        } else {
            check(false, "a directory is enumerated so that its close can be observed");
        }
        kal_dir a{}, b{}; kal_uintptr ia = 0;
        if (kal_fs_open_dir(root, "okl_dir", 7, &a) == kal_ok
            && kal_fs_open_dir(root, "okl_dir", 7, &b) == kal_ok
            && kal_fs_list_begin(a, &ia) == kal_ok) {
            char name[512]; kal_uintptr len = 0; int kind = 0;
            check(kal_fs_list_next(b, &ia, name, sizeof name, &len, &kind) == kal_err_invalid,
                  "an iterator is refused with a directory that did not produce it");
        }
        kal_fs_close_dir(a); kal_fs_close_dir(b);
    }

    // A close that races a read of the same enumeration does not free what the
    // reader is using (clause 6.6).
    {
        struct race { kal_dir d; volatile int done; volatile int told; };
        auto reader = [](void* p) {
            auto* r = static_cast<race*>(p);
            kal_uintptr it = 0;
            if (kal_fs_list_begin(r->d, &it) == kal_ok) {
                for (;;) {
                    char name[512]; kal_uintptr len = 0; int kind = 0;
                    const int e = kal_fs_list_next(r->d, &it, name, sizeof name, &len, &kind);
                    if (e != kal_ok) { r->told = 1; break; }
                    if (it == 0 && kal_fs_list_begin(r->d, &it) != kal_ok) { r->told = 1; break; }
                }
            } else {
                r->told = 1;
            }
            __atomic_store_n(&r->done, 1, __ATOMIC_RELEASE);
        };
        int ended = 0;
        for (int i = 0; i < 200; ++i) {
            race r{};
            if (kal_fs_open_dir(root, "okl_dir", 7, &r.d) != kal_ok) break;
            kal_task t{};
            if (kal_task_start(reader, &r, &t) != kal_ok) { kal_fs_close_dir(r.d); break; }
            for (int spin = 0; spin < i * 10; ++spin) kal_task_yield();
            kal_fs_close_dir(r.d);
            kal_task_join(t);
            ended += r.told;
        }
        check(ended == 200, "an enumeration racing the close of its directory ends and is told so");
    }

    check(kal_fs_remove(root, "okl_dir/inner", 13) == kal_ok, "the inner file is removed");
    check(kal_fs_remove(root, "okl_dir", 7) == kal_ok, "the directory is removed");
    check(kal_fs_remove(root, "okl_probe.txt", 13) == kal_ok, "the file is removed");

    const char ok[] = "openkal-macos: file system conformance\n";
    kal::write(kal::out(), ok, sizeof(ok) - 1);
    return failures == 0 ? 0 : 1;
}
