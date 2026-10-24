// Conformance: a context started in a copy of the space can start a context.
//
// mcpplibs/openkal-musl#55: in a copy made by kal_space_start, the first
// thread the copy asked for failed with EAGAIN, while the original started one.
// The duplication primitive alone leaves the system library's state the
// original's, and this system's `fork' prepares it around the primitive; the
// implementation now does what `fork' does. Observed through the status, the
// only channel a separate space has: 3 is a context started, joined and given
// its least stack size in the copy, and any other value is the step that
// failed.
import openkal.types;
import openkal.task;
import openkal.space;
import openkal.process;
import openkal.stream;
import openkal.abort;

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

volatile int g_ran = 0;
void ran(void*) { g_ran = 1; }

void in_the_copy(void*) {
    kal_task t{};
    if (kal_task_start(ran, nullptr, &t) != kal_ok) kal_exit(4);
    if (kal_task_join(t) != kal_ok || g_ran != 1) kal_exit(5);
    kal_task s{};
    if (kal_task_start_sized(ran, nullptr, 16u << 20, &s) != kal_ok) kal_exit(6);
    kal_task_join(s);
    kal_exit(3);
}

alignas(16) unsigned char g_stack[64 * 1024];
}

int main() {
    for (int round = 0; round < 3; ++round) {
        kal_process p{};
        const int rc = kal_space_start(in_the_copy, nullptr, g_stack + sizeof g_stack, &p);
        check(rc == kal_ok, "a copy of the space starts");
        if (rc != kal_ok) break;
        int status = 0, terminated = 0;
        check(kal_process_wait(p, &status, &terminated) == kal_ok, "the copy is waited for");
        kal_process_close(p);
        check(terminated == 0 && status == 3, "the copy started a context, joined it, and started one with a least stack size");
        if (status != 3) {
            char line[] = "copy status: X\n";
            line[13] = static_cast<char>('0' + (status % 10));
            kal::write(kal::err(), line, sizeof line - 1);
        }
    }
    if (failures == 0) kal::write(kal::out(), "openkal-macos: a copy of the space starts contexts\n", 51);
    return failures == 0 ? 0 : 1;
}
