// Conformance: openkal.env and openkal.time.
import openkal.env;
import openkal.time;
import openkal.stream;
import openkal.process;

namespace {
int failures = 0;

// This program's processor time, or ~0 where the interface does not say.
kal_u64 processor_ns() {
    kal_process me{};
    if (kal_process_self(&me) != kal_ok) return ~0ull;
    kal_program_info info{};
    info.self_size = sizeof info;
    const bool ok = kal_process_info(me, kal::process::info::cpu, &info) == kal_ok
                 && (info.present & kal::process::info::cpu);
    kal_process_close(me);
    return ok ? info.cpu_ns : ~0ull;
}
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
    // A program always receives the name it was started with, even where the
    // environment has none, in which case it is empty rather than absent.
    check(kal::env::arg_count() >= 1, "at least the program name is present");
    // THE VALUE IS COPIED AND THE LENGTH REPORTED IS THE VALUE'S OWN, so a
    // capacity of zero asks for the length without writing.
    char buf[1024];
    check(kal::env::arg(0, buf, sizeof buf) >= 0, "argument zero is readable");
    check(kal::env::arg(0, nullptr, 0) == kal::env::arg(0, buf, sizeof buf),
          "a capacity of zero reports the same length as a copy");

    // A variable that is certain to exist under the harness, and one that is
    // certain not to. Both halves are asserted, because a lookup that always
    // succeeded and one that always failed would each satisfy only one.
    check(kal::env::var("PATH", 4, buf, sizeof buf) > 0, "an existing variable is found");
    check(kal::env::var("OPENKAL_ABSENT_VARIABLE", 23, buf, sizeof buf)
              == -kal_err_not_found,
          "an absent variable is reported absent");
    check(kal_env_var_count() > 0, "the set can be enumerated");

    // The monotonic source does not decrease, and it advances.
    const auto a = kal::time::monotonic();
    kal::time::sleep(2 * 1000 * 1000);          // two milliseconds
    const auto b = kal::time::monotonic();
    check(b >= a, "the monotonic source does not decrease");
    check(b - a >= 2 * 1000 * 1000, "suspension lasts at least as long as requested");
    check(kal::time::granularity() > 0, "the granularity is reported");

    // A SUSPENSION IS NOT PROCESSOR TIME (0.21.1). The wait this implementation
    // suspends with failed at once for a word whose page was not yet present,
    // and a sleep was a spin of the processor for its whole length.
    const kal_u64 c0 = processor_ns();
    kal::time::sleep(200 * 1000 * 1000);
    const kal_u64 c1 = processor_ns();
    check(c0 != ~0ull && c1 != ~0ull && c1 - c0 < 50 * 1000 * 1000,
          "200 ms asleep is under 50 ms of processor time");

    // The wall source is claimed by this implementation, so it must report a
    // time after the specification was written rather than zero.
    check(kal::time::has(kal::time::wall_available), "the wall source is claimed");
    check(kal::time::wall() > 1700000000ull * 1000000000ull,
          "the wall source reports a plausible time");

    const char ok[] = "openkal-macos: env and time conformance\n";
    kal::write(kal::out(), ok, sizeof(ok) - 1);
    return failures == 0 ? 0 : 1;
}
