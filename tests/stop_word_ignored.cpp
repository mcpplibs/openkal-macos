// A request the starter ignored stays ignored once the stop word is armed
// (openkal 0.17, the take-over rule; K2).
#include <csignal>
#include <cstdio>
#include <unistd.h>
import openkal.types;
import openkal.process;

int main() {
#if defined(__aarch64__)
    std::signal(SIGINT, SIG_IGN);
    const kal_u32* word = kal_process_stop_requested();
    if (word == nullptr) { std::printf("FAIL: the stop word is armed\n"); return 1; }
    ::kill(::getpid(), SIGINT);
    ::usleep(20000);
    if (*word != 0) { std::printf("FAIL: an ignored request was delivered\n"); return 1; }
    ::kill(::getpid(), SIGTERM);
    for (int i = 0; i < 1000 && *word == 0; ++i) ::usleep(1000);
    if (*word == 0) { std::printf("FAIL: a request after arming sets the word\n"); return 1; }
#endif
    std::printf("openkal-macos: an ignored request stays ignored\n");
    return 0;
}
