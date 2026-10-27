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
#include <unistd.h>
#include <sys/wait.h>
import openkal.types;
import openkal.env;
import openkal.stream;
import openkal.process;

namespace {
int failures = 0;
void check(bool held, const char* what) {
    if (!held) { std::printf("FAIL: %s\n", what); ++failures; }
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--hang-up") == 0) {
        const kal_u32* word = kal_process_stop_requested();
        if (word == nullptr) return 90;
        ::kill(::getpid(), SIGHUP);
        for (int i = 0; i < 1000 && *word == 0; ++i) ::usleep(1000);
        if (*word == 0) return 91;
        kal_process_end_as_requested();
        return 92;
    }
    if (argc > 1 && std::strcmp(argv[1], "--end-as-requested") == 0) {
        const kal_u32* word = kal_process_stop_requested();
        if (word == nullptr) return 90;
        ::kill(::getpid(), SIGTERM);
        for (int i = 0; i < 1000 && *word == 0; ++i) ::usleep(1000);
        if (*word == 0) return 91;
        kal_process_end_as_requested();
        return 92;
    }

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
    const pid_t child = ::fork();
    if (child == 0) {
        ::execl(self, argv[0], "--end-as-requested", nullptr);
        ::_exit(93);
    }
    int st = 0;
    ::waitpid(child, &st, 0);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
          "a program that ends as requested is reported to its starter as ended by the request");
    const pid_t hung = ::fork();
    if (hung == 0) {
        ::execl(self, argv[0], "--hang-up", nullptr);
        ::_exit(93);
    }
    st = 0;
    ::waitpid(hung, &st, 0);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGHUP,
          "a hang-up is taken over as a request, and the program ends as it would have (0.15.1)");
#else
    (void)word; (void)argv;
#endif
    if (failures == 0) std::printf("openkal-macos: the stop word given back, and a closed stream reported\n");
    return failures == 0 ? 0 : 1;
}
