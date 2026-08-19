#include <time.h>
#include <errno.h>
import openkal.time;

extern "C" {

kal_duration kal_time_monotonic(void) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<kal_duration>(ts.tv_sec) * 1000000000u
         + static_cast<kal_duration>(ts.tv_nsec);
}

kal_duration kal_time_wall(void) {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<kal_duration>(ts.tv_sec) * 1000000000u
         + static_cast<kal_duration>(ts.tv_nsec);
}

kal_duration kal_time_monotonic_granularity(void) {
    timespec ts{};
    if (clock_getres(CLOCK_MONOTONIC, &ts) != 0) return 1;
    const auto ns = static_cast<kal_duration>(ts.tv_sec) * 1000000000u
                  + static_cast<kal_duration>(ts.tv_nsec);
    return ns == 0 ? 1 : ns;
}

void kal_time_sleep(kal_duration ns) {
    timespec req{ static_cast<time_t>(ns / 1000000000u),
                  static_cast<long>(ns % 1000000000u) };
    // The specification requires that the call not return early, so an
    // interruption resumes the remainder rather than reporting it.
    while (nanosleep(&req, &req) != 0 && errno == EINTR) { }
}

// ⚠️ The monotonic source on this system continues to advance while the machine
// is suspended, which is the opposite of the other implementation. A program
// measuring an interval across a suspension obtains different answers from the
// two, and no operation reports which it is dealing with. That is what the
// property is for, and this divergence is the clearest evidence in either
// implementation that the property was necessary rather than decorative.
const kal_uintptr kal_time_props =
    kal::time::prop_wall_available | kal::time::prop_sleep_precise;

}
