#include "sys.h"
#include <openkal/time.h>

// Time on this system.
//
// The wall source is a kernel call. The monotonic source is not: this system
// counts elapsed time in a unit of the processor's, and converting it is done
// in a library rather than in the kernel. The conversion is therefore asked for
// through a name no C library defines --- which is the property that makes a
// name usable from here at all, and which was measured on the system rather
// than remembered.
//
// Suspension is the interesting one. This kernel has no call that suspends for
// a duration: what its C library uses is a wait upon a semaphore that does not
// exist in a program carrying no other runtime. So the wait this implementation
// already needs for openkal.task is used, upon an address nothing ever wakes.
// That is not a substitute for a sleep --- it is a sleep, expressed with the
// operation this system has.

extern "C" {
// Elapsed nanoseconds from an unspecified origin. A name no C library defines.
kal_u64 clock_gettime_nsec_np(int clock);
}

namespace {
constexpr int kMonotonicRaw = 4;   // this system's own numbering
}

extern "C" {

kal_duration kal_time_monotonic(void) {
    return clock_gettime_nsec_np(kMonotonicRaw);
}

kal_duration kal_time_wall(void) {
    okm::ktimeval tv{ 0, 0, 0 };
    okm::sys(okm::nr_gettimeofday, reinterpret_cast<okm_long>(&tv), 0, 0);
    return static_cast<kal_duration>(tv.sec) * 1000000000u
         + static_cast<kal_duration>(tv.usec) * 1000u;
}

kal_duration kal_time_monotonic_granularity(void) {
    // Measured rather than declared: the source is read until it changes, and
    // the difference is what it can resolve. A granularity that was asserted
    // rather than observed would be a claim about the machine this was written
    // on rather than about the one it is running on.
    static kal_duration remembered = 0;
    if (remembered) return remembered;
    const kal_duration first = kal_time_monotonic();
    kal_duration next = first;
    for (int i = 0; i < 1000000 && next == first; ++i) next = kal_time_monotonic();
    remembered = next > first ? next - first : 1;
    return remembered;
}

void kal_time_sleep(kal_duration ns) {
    if (ns == 0) return;
    const kal_duration target = kal_time_monotonic() + ns;
    static volatile okm_u32 never = 0;
    for (;;) {
        const kal_duration now = kal_time_monotonic();
        if (now >= target) return;
        const kal_duration remaining = target - now;
        // The unit this system takes is the microsecond. A remainder shorter
        // than one is waited out against the source rather than rounded away:
        // the specification requires that the call not return early, and
        // rounding down is returning early.
        const okm_u32 microseconds = static_cast<okm_u32>(remaining / 1000u);
        if (microseconds == 0) { okm::relax(); continue; }
        okm::sys(okm::nr_ulock_wait,
                 static_cast<okm_long>(okm::ul_compare_and_wait | okm::ulf_no_errno),
                 reinterpret_cast<okm_long>(const_cast<okm_u32*>(&never)),
                 0, static_cast<okm_long>(microseconds));
    }
}

// This system's monotonic source continues while the machine is suspended,
// which is the opposite of the Linux implementation. The position exists for
// exactly this divergence: a program measuring an interval across a suspension
// obtains different answers from the two, and no operation could report which
// it is dealing with.
kal_uintptr kal_time_props(void) { return
    KAL_TIME_PROP_WALL_AVAILABLE | KAL_TIME_PROP_SLEEP_PRECISE; }

}
