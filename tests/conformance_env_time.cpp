// Conformance: openkal.env and openkal.time.
import openkal.env;
import openkal.time;
import openkal.stream;

namespace {
int failures = 0;
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
    kal_uintptr n = 0;
    check(kal::env::arg(0, &n) != nullptr, "argument zero is readable");

    // A variable that is certain to exist under the harness, and one that is
    // certain not to. Both halves are asserted, because a lookup that always
    // succeeded and one that always failed would each satisfy only one.
    kal_uintptr vlen = 0;
    const char* path = kal::env::var("PATH", 4, &vlen);
    check(path != nullptr && vlen > 0, "an existing variable is found");
    check(kal::env::var("OPENKAL_ABSENT_VARIABLE", 23, &vlen) == nullptr,
          "an absent variable is reported absent");
    check(kal_env_var_count() > 0, "the set can be enumerated");

    // The monotonic source does not decrease, and it advances.
    const auto a = kal::time::monotonic();
    kal::time::sleep(2 * 1000 * 1000);          // two milliseconds
    const auto b = kal::time::monotonic();
    check(b >= a, "the monotonic source does not decrease");
    check(b - a >= 2 * 1000 * 1000, "suspension lasts at least as long as requested");
    check(kal::time::granularity() > 0, "the granularity is reported");

    // The wall source is claimed by this implementation, so it must report a
    // time after the specification was written rather than zero.
    check(kal::time::has(kal::time::prop_wall_available), "the wall source is claimed");
    check(kal::time::wall() > 1700000000ull * 1000000000ull,
          "the wall source reports a plausible time");

    const char ok[] = "openkal-linux: env and time conformance\n";
    kal::write(kal::out(), ok, sizeof(ok) - 1);
    return failures == 0 ? 0 : 1;
}
