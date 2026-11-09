// The ignore of SIGPIPE is this program's own (src/env.cpp, K8) and is not passed
// on (0.15): a program started through `kal_process_spawn' begins with SIGPIPE
// as this program was started with --- the default for an ordinary start, so a
// started shell's `yes | head' ends by the signal; and still ignored for a
// program that was itself started with it ignored.
//
// The disposition a program was started with is observed before openkal's own
// initialiser runs: an initialiser of priority 100 runs before one of 101. Both
// starts are made here, with the disposition set explicitly: what this test
// itself was started with is the harness's (a CI runner starts its steps with
// SIGPIPE ignored).
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
import openkal.types;
import openkal.env;
import openkal.fs;
import openkal.process;
import mcpplibs.mtest;

using namespace mcpplibs::mtest;

namespace {
struct sigaction g_started{};
[[gnu::constructor(100)]] void observe() { ::sigaction(SIGPIPE, nullptr, &g_started); }

kal_dir root() {
    for (kal_uintptr i = 0; i < kal_fs_preopen_count(); ++i) {
        kal_dir d{}; char n[8]; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, n, sizeof n, &l) == kal_ok && l == 1 && n[0] == '/') return d;
    }
    return kal_dir{};
}

// This program's own image, as the environment named it.
std::string self_image() {
    char self[4096];
    const kal_intptr n = kal_env_image(self, sizeof self - 1);
    if (n <= 0 || self[0] != '/') return {};
    return std::string(self, static_cast<std::size_t>(n));
}

// This program again, as the child `report', through openkal, asked how it was
// started: 7 ignored, 0 default, -1 not started.
int started_child_reports() {
    const std::string self = self_image();
    if (self.empty()) return -1;
    const std::vector<std::string> command = child_command("report");
    std::vector<const char*> argv;
    std::vector<kal_uintptr> lens;
    for (const auto& a : command) { argv.push_back(a.c_str()); lens.push_back(a.size()); }
    const kal_dir r = root();
    const kal_spawn how{ r, r, nullptr, nullptr, 0, 0 };
    kal_process p{};
    const int e = kal_process_spawn(&how, self.c_str() + 1, self.size() - 1, argv.data(), lens.data(),
                                    argv.size(), nullptr, nullptr, 0, nullptr, &p);
    int status = -1, terminated = 0;
    if (e == kal_ok) kal_process_wait(p, &status, &terminated);
    return e == kal_ok && terminated == 0 ? status : -1;
}

// This program again, as the child `mode', started with SIGPIPE as
// `disposition' says: 0 when the program it then starts through openkal
// reported what was expected.
int started_with(void (*disposition)(int), const char* mode) {
    const std::string self = self_image();
    const std::vector<std::string> command = child_command(mode);
    std::vector<char*> argv;
    for (const auto& a : command) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
        ::signal(SIGPIPE, disposition);
        ::execv(self.c_str(), argv.data());
        ::_exit(93);
    }
    int st = 0;
    ::waitpid(child, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
}  // namespace

child report{"report", [](std::vector<std::string>) {
    return g_started.sa_handler == SIG_IGN ? 7 : 0;
}};

child started_default{"started-default", [](std::vector<std::string>) {
    struct sigaction now{};
    ::sigaction(SIGPIPE, nullptr, &now);
    if (now.sa_handler != SIG_IGN) return 2;   // this program's own writes are not ended by it
    return started_child_reports() == 0 ? 0 : 1;
}};

child started_ignoring{"started-ignoring", [](std::vector<std::string>) {
    return started_child_reports() == 7 ? 0 : 1;
}};

UTest sigpipe_not_passed_on_test{"sigpipe_not_passed_on", [] {
    if (self_image().empty()) { fail("the program's own image is named"); return; }
    const int d = started_with(SIG_DFL, "started-default");
    expect(ne(d, 2)) << "this program's own writes are not ended by SIGPIPE";
    expect(eq(d, 0)) << "a program started with SIGPIPE at its default starts others with it at its default";
    expect(eq(started_with(SIG_IGN, "started-ignoring"), 0))
        << "a program started with SIGPIPE ignored passes the ignore on, as it was given";
}};
