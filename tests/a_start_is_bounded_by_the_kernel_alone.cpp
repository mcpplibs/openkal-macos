// A start takes the vectors its caller gives (SPEC.md clause 7.6, version 0.26).
//
// Until 0.24.0 this implementation refused an argument vector or an environment
// of more than 512 entries and reported it as memory exhausted. Neither vector
// has a fixed buffer, and the kernel bounds the bytes of both together
// (ARG_MAX, one mebibyte), not their entries. A build tool reached the bound
// with a compile command of 520 definitions, and a development environment's
// build phase with 546 variables; both read as a machine out of memory.
//
// Each case has a shell report what it received, since the claim is about the
// started program.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
import openkal.fs;
import openkal.types;
import openkal.process;
import mcpplibs.mtest;

using namespace mcpplibs::mtest;

namespace {

kal_dir root() {
    for (kal_uintptr i = 0; i < kal_fs_preopen_count(); ++i) {
        kal_dir d{}; char n[8]; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, n, sizeof n, &l) == kal_ok && l == 1 && n[0] == '/') return d;
    }
    return kal_dir{};
}

// Starts /bin/sh with the vectors given; the answer of the start, and the
// status where it started.
int start_shell(const std::vector<std::string>& args, const std::vector<std::string>& env, int& status) {
    std::vector<const char*> argv, envp;
    std::vector<kal_uintptr> alens, elens;
    for (auto const& a : args) { argv.push_back(a.c_str()); alens.push_back(a.size()); }
    for (auto const& e : env)  { envp.push_back(e.c_str()); elens.push_back(e.size()); }
    const kal_spawn how{ root(), kal::fs::working(), nullptr, nullptr, 0, 0 };
    kal_process p{};
    int rc = kal_err_invalid;
    for (const char* path : { "bin/sh", "usr/bin/sh" }) {
        rc = kal_process_spawn(&how, path, std::strlen(path), argv.data(), alens.data(), argv.size(),
                               envp.data(), elens.data(), envp.size(), nullptr, &p);
        if (rc != kal_err_not_found) break;
    }
    if (rc != kal_ok) return rc;
    int terminated = -1;
    kal_process_wait(p, &status, &terminated);
    kal_process_close(p);
    if (terminated != 0) status = -1;
    return kal_ok;
}

}  // namespace

UTest a_start_is_bounded_by_the_kernel_alone_test{"a_start_is_bounded_by_the_kernel_alone", [] {
    // 2,000 arguments after the script: the shell counts them. 2000 mod 256 is 208.
    {
        std::vector<std::string> args{ "sh", "-c", "exit $(($# % 256))", "sh" };
        for (int i = 0; i < 2000; ++i) args.push_back("a");
        int status = -1;
        const int rc = start_shell(args, {}, status);
        expect(rc == kal_ok) << "a vector of 2,004 elements starts (answer " << rc << ")";
        expect(status == 208) << "the started program counts 2,000 arguments (status " << status << ")";
    }
    // 2,000 variables, about 200 KiB: the shell reads the first and the last.
    {
        std::vector<std::string> env;
        const std::string value(90, 'v');
        for (int i = 0; i < 2000; ++i) env.push_back("OKV" + std::to_string(i) + "=" + value + std::to_string(i));
        const std::vector<std::string> args{ "sh", "-c",
            "[ \"${OKV0%0}\" = \"" + value + "\" ] && [ \"${OKV1999%1999}\" = \"" + value + "\" ] && exit 77; exit 1" };
        int status = -1;
        const int rc = start_shell(args, env, status);
        expect(rc == kal_ok) << "an environment of 2,000 variables starts (answer " << rc << ")";
        expect(status == 77) << "the started program reads the first and the last variable (status " << status << ")";
    }
    // More than the kernel takes for both vectors together (ARG_MAX): the
    // request does not fit, which is not a machine without memory.
    {
        const std::vector<std::string> args{ "sh", "-c", "exit 0", "sh", std::string(2 * 1024 * 1024, 'x') };
        int status = -1;
        const int rc = start_shell(args, {}, status);
        expect(rc == kal_err_no_space) << "a start the kernel refuses for its size is no space (answer " << rc << ")";
    }
}};
