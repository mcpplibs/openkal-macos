// The stop word given back, and a closed stream reported rather than fatal
// (openkal 0.17, process.h; K2, K8).
//
//   1. (tests/stop_word_ignored.cpp: an ignored request stays ignored, K2);
//   2. ending as requested is reported to the starter as the request (G4);
//   3. writing to a stream whose far end is gone is an error the program is
//      told, not SIGPIPE ending it (K8).
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
import openkal.types;
import openkal.env;
import openkal.stream;
import openkal.process;
import mcpplibs.mtest;

using namespace mcpplibs::mtest;

namespace {
void check(bool held, const char* what) { expect(held) << what; }

#if defined(__aarch64__)
// Starts this program again, the image `self', as the child `name' and answers
// its wait status.
int run_copy(const char* self, const char* name) {
    const std::vector<std::string> command = child_command(name);
    std::vector<char*> argv;
    for (const auto& a : command) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const pid_t copy = ::fork();
    if (copy == 0) {
        ::execv(self, argv.data());
        ::_exit(93);
    }
    int st = 0;
    ::waitpid(copy, &st, 0);
    return st;
}
#endif
}

child hang_up{"hang-up", [](std::vector<std::string>) {
    const kal_u32* word = kal_process_stop_requested();
    if (word == nullptr) return 90;
    ::kill(::getpid(), SIGHUP);
    for (int i = 0; i < 1000 && *word == 0; ++i) ::usleep(1000);
    if (*word == 0) return 91;
    kal_process_end_as_requested();
    return 92;
}};

child end_as_requested{"end-as-requested", [](std::vector<std::string>) {
    const kal_u32* word = kal_process_stop_requested();
    if (word == nullptr) return 90;
    ::kill(::getpid(), SIGTERM);
    for (int i = 0; i < 1000 && *word == 0; ++i) ::usleep(1000);
    if (*word == 0) return 91;
    kal_process_end_as_requested();
    return 92;
}};

UTest stop_word_and_sigpipe_test{"stop_word_and_sigpipe", [] {

    {
        kal_stream mine{}, theirs{};
        check(kal_process_channel(&mine, &theirs) == kal_ok, "a channel is made");
        kal_process_channel_close(mine);
        const kal_intptr r = kal_stream_write(theirs, "x", 1);
        check(r < 0, "writing to a stream whose reader is gone is reported, and the program lives");
        kal_process_channel_close(theirs);
    }

    const kal_u32* word = kal_process_stop_requested();
#if defined(__aarch64__)
    check(word != nullptr, "the stop word is armed");
    char self[4096] = {};
    const kal_intptr n = kal_env_image(self, sizeof self - 1);
    check(n > 0, "the program's own image is named");
    int st = run_copy(self, "end-as-requested");
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
          "a program that ends as requested is reported to its starter as ended by the request");
    st = run_copy(self, "hang-up");
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGHUP,
          "a hang-up is taken over as a request, and the program ends as it would have (0.15.1)");
#else
    (void)word;
#endif
    std::printf("openkal-macos: the stop word given back, and a closed stream reported\n");
}};
