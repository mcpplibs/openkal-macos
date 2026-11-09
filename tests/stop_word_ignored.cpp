// A request the starter ignored stays ignored once the stop word is armed
// (openkal 0.17, the take-over rule; K2).
#include <csignal>
#include <cstdio>
#include <unistd.h>
import openkal.types;
import openkal.process;
import mcpplibs.mtest;

using namespace mcpplibs::mtest;

UTest stop_word_ignored_test{"stop_word_ignored", [] {
#if defined(__aarch64__)
    std::signal(SIGINT, SIG_IGN);
    const kal_u32* word = kal_process_stop_requested();
    if (word == nullptr) { fail("the stop word is armed"); return; }
    ::kill(::getpid(), SIGINT);
    ::usleep(20000);
    if (*word != 0) { fail("an ignored request was delivered"); return; }
    ::kill(::getpid(), SIGTERM);
    for (int i = 0; i < 1000 && *word == 0; ++i) ::usleep(1000);
    if (*word == 0) { fail("a request after arming sets the word"); return; }
#endif
    std::printf("openkal-macos: an ignored request stays ignored\n");
}};
