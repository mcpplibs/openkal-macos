// The ignore of SIGPIPE is this program's own (src/env.cpp, K8) and is not passed
// on (0.15): a program started through `kal_process_spawn' begins with SIGPIPE
// as this program was started with --- the default for an ordinary start, so a
// started shell's `yes | head' ends by the signal; and still ignored for a
// program that was itself started with it ignored.
//
// The disposition a program was started with is observed before openkal's own
// initialiser runs: an initialiser of priority 100 runs before one of 101.
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
import openkal.types;
import openkal.env;
import openkal.fs;
import openkal.process;

namespace {
struct sigaction g_started{};
[[gnu::constructor(100)]] void observe() { ::sigaction(SIGPIPE, nullptr, &g_started); }

int failures = 0;
void check(bool held, const char* what) {
    if (!held) { std::printf("FAIL: %s\n", what); ++failures; }
}

kal_dir root() {
    for (kal_uintptr i = 0; i < kal_fs_preopen_count(); ++i) {
        kal_dir d{}; char n[8]; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, n, sizeof n, &l) == kal_ok && l == 1 && n[0] == '/') return d;
    }
    return kal_dir{};
}

char g_self[4096];

// This program again, through openkal, asked how it was started: 7 ignored,
// 0 default.
int started_child_reports() {
    const kal_dir r = root();
    const char* argv[] = { g_self, "--report" };
    const kal_uintptr lens[] = { std::strlen(g_self), 8 };
    const kal_spawn how{ r, r, nullptr, nullptr, 0, 0 };
    kal_process p{};
    const int e = kal_process_spawn(&how, g_self + 1, std::strlen(g_self + 1), argv, lens, 2,
                                    nullptr, nullptr, 0, nullptr, &p);
    int status = -1, terminated = 0;
    if (e == kal_ok) kal_process_wait(p, &status, &terminated);
    return e == kal_ok && terminated == 0 ? status : -1;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--report") == 0)
        return g_started.sa_handler == SIG_IGN ? 7 : 0;
    const kal_intptr n = kal_env_image(g_self, sizeof g_self - 1);
    if (n <= 0 || g_self[0] != '/') { std::printf("FAIL: the program's own image is named\n"); return 1; }
    g_self[n] = 0;
    if (argc > 1 && std::strcmp(argv[1], "--started-ignoring") == 0)
        return started_child_reports() == 7 ? 0 : 1;

    struct sigaction now{};
    ::sigaction(SIGPIPE, nullptr, &now);
    check(now.sa_handler == SIG_IGN, "this program's own writes are not ended by SIGPIPE");
    check(started_child_reports() == 0, "a started program begins with SIGPIPE at its default");

    const pid_t child = ::fork();
    if (child == 0) {
        ::signal(SIGPIPE, SIG_IGN);
        ::execl(g_self, g_self, "--started-ignoring", nullptr);
        ::_exit(93);
    }
    int st = 0;
    ::waitpid(child, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "a program started with SIGPIPE ignored passes the ignore on, as it was given");

    if (failures == 0) std::printf("openkal-macos: SIGPIPE ignored here, not passed on\n");
    return failures == 0 ? 0 : 1;
}
