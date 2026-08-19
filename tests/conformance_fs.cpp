// Conformance: openkal.fs.
//
// The interface is relative to a directory throughout, and the suite exercises
// that property directly: a name that would ascend is rejected, which is what
// makes a program confinable by the directory it was given.
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
}

int main() {
    check(kal::fs::preopen_count() >= 1, "the environment supplies at least one directory");
    auto root = kal::fs::working();
    check(root.h != 0, "the working directory is the first entry");

    // Each supplied directory carries the name the environment gives it.
    kal_dir d0{}; const char* n0 = nullptr; kal_uintptr l0 = 0;
    check(kal_fs_preopen(0, &d0, &n0, &l0) == kal_ok && n0 != nullptr && l0 > 0,
          "a supplied directory carries a name");
    kal_dir beyond{};
    check(kal_fs_preopen(kal::fs::preopen_count(), &beyond, nullptr, nullptr) != kal_ok,
          "an index beyond the set is refused");

    // Creation, writing, reading back, and removal.
    kal_file f{};
    check(kal_fs_open_file(root, "okl_probe.txt", 13, 1, 1, &f) == kal_ok,
          "a file is created");
    const kal_stream s{ kal_fs_stream(f) };
    const char payload[] = "conformance";
    check(kal::write(s, payload, sizeof(payload) - 1).e == kal_ok, "the file is written");

    __UINT64_TYPE__ pos = 0;
    check(kal_fs_seek(f, 0, kal::fs::seek_set, &pos) == kal_ok && pos == 0,
          "the file is repositioned");
    char back[32] = {};
    const auto r = kal::read(s, back, sizeof(back));
    check(r.e == kal_ok && r.n == sizeof(payload) - 1, "the file reads back");
    for (kal_uintptr i = 0; i < sizeof(payload) - 1; ++i)
        check(back[i] == payload[i], "the contents match");
    kal_fs_close_file(f);

    // Enquiry reports what was written, and reports absence without failing.
    kal_node_info info{};
    check(kal_fs_info(root, "okl_probe.txt", 13, &info) == kal_ok, "enquiry succeeds");
    check(info.kind == kal_node_file, "the node is a file");
    check(info.size == sizeof(payload) - 1, "the size is reported");
    kal_node_info absent{};
    check(kal_fs_info(root, "okl_absent", 10, &absent) == kal_ok
          && absent.kind == kal_node_absent,
          "an absent name is reported absent rather than as a failure");

    // A directory, an enumeration that finds what was placed in it, and removal.
    check(kal_fs_mkdir(root, "okl_dir", 7) == kal_ok, "a directory is created");
    kal_dir d{};
    check(kal_fs_open_dir(root, "okl_dir", 7, &d) == kal_ok, "the directory opens");
    kal_file inner{};
    check(kal_fs_open_file(d, "inner", 5, 1, 1, &inner) == kal_ok, "a file is created within");
    kal_fs_close_file(inner);
    kal_uintptr iter = 0; bool found = false;
    check(kal_fs_list_begin(d, &iter) == kal_ok, "enumeration begins");
    for (;;) {
        const char* name = nullptr; kal_uintptr len = 0; int kind = 0;
        if (kal_fs_list_next(d, &iter, &name, &len, &kind) != kal_ok) break;
        if (name == nullptr) break;
        if (len == 5 && name[0] == 'i') found = true;
    }
    check(found, "enumeration finds the entry");

    // ⚠️ The property the interface exists to have: a name that ascends is
    // refused, so a program cannot leave the directory it was given.
    kal_dir escape{};
    check(kal_fs_open_dir(d, "..", 2, &escape) != kal_ok, "an ascending name is refused");
    check(kal_fs_open_dir(d, "../..", 5, &escape) != kal_ok, "a compound ascent is refused");
    check(kal_fs_open_dir(d, "/etc", 4, &escape) != kal_ok, "an absolute name is refused");

    // A released handle is not valid, which the specification requires.
    kal_fs_close_dir(d);
    kal_file after{};
    check(kal_fs_open_file(d, "inner", 5, 0, 0, &after) != kal_ok,
          "a released handle is not treated as valid");

    check(kal_fs_remove(root, "okl_dir/inner", 13) == kal_ok, "the inner file is removed");
    check(kal_fs_remove(root, "okl_dir", 7) == kal_ok, "the directory is removed");
    check(kal_fs_remove(root, "okl_probe.txt", 13) == kal_ok, "the file is removed");

    const char ok[] = "openkal-linux: file system conformance\n";
    kal::write(kal::out(), ok, sizeof(ok) - 1);
    return failures == 0 ? 0 : 1;
}
