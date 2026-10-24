// Conformance: the least stack size a started context is given (openkal 0.16),
// and the structure this implementation composes to ask for it.
//
// WHAT IS OBSERVED IS ROOM, for the reason the Linux implementation gives: the
// report this answers (mcpplibs/openkal-musl#48) asked for a mebibyte and was
// given less, and a size reported and not provided would pass a test that only
// read the number back.
//
// AND THE STRUCTURE IS COMPARED WITH THE SYSTEM'S OWN. Beneath a C library the
// thread attributes are composed by src/task.cpp rather than by
// `pthread_attr_init', whose name that library defines. The layout is the
// system library's and is not documented, so it is checked here, on every run,
// against what the system library itself writes; a system that changed it turns
// this red rather than starting threads with a structure it does not recognise.
#include <pthread.h>
#include <stdio.h>
#include <string.h>

import openkal.types;
import openkal.task;
import openkal.stream;

namespace okm {
struct thread_attr { unsigned long long word[8]; };
void compose_thread_attr(thread_attr* a, __UINTPTR_TYPE__ stack, __UINTPTR_TYPE__ guard);
}

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

long recurse(long n);
long (*volatile g_recurse)(long) = recurse;
long recurse(long n) {
    volatile char pad[1000];
    pad[0] = static_cast<char>(n);
    if (n == 0) return 0;
    return g_recurse(n - 1) + pad[0];
}

struct probe { kal_uintptr ask; kal_uintptr told; volatile int done; };

void room(void* p) {
    auto* q = static_cast<probe*>(p);
    void* base = nullptr; kal_uintptr size = 0;
    if (kal_task_stack(&base, &size) == kal_ok) q->told = size;
    g_recurse(static_cast<long>(q->ask / 1024 * 3 / 4));
    __atomic_store_n(&q->done, 1, __ATOMIC_RELEASE);
}

void dump(const char* what, const void* p) {
    const auto* w = static_cast<const unsigned long long*>(p);
    fprintf(stderr, "%s:", what);
    for (int i = 0; i < 8; ++i) fprintf(stderr, " %016llx", w[i]);
    fprintf(stderr, "\n");
}
}

int main() {
    static_assert(sizeof(pthread_attr_t) == sizeof(okm::thread_attr), "the structure is the system's size");
    const unsigned long sizes[] = { 8ul << 20, 64ul << 20 };
    for (unsigned long size : sizes) {
        pthread_attr_t sys;
        pthread_attr_init(&sys);
        pthread_attr_setstacksize(&sys, size);
        pthread_attr_setguardsize(&sys, 256u * 1024u);
        okm::thread_attr mine;
        okm::compose_thread_attr(&mine, size, 256u * 1024u);
        const bool same = memcmp(&sys, &mine, sizeof mine) == 0;
        check(same, "the composed thread attributes are the system library's own");
        if (!same) { dump("system  ", &sys); dump("composed", &mine); }
        pthread_attr_destroy(&sys);
    }

    const kal_uintptr asks[] = { 1u << 20, 16u << 20, 64u << 20 };
    for (kal_uintptr ask : asks) {
        probe q{ ask, 0, 0 };
        kal_task t{};
        check(kal_task_start_sized(room, &q, ask, &t) == kal_ok, "a context with a least stack size starts");
        kal_task_join(t);
        check(q.told >= ask, "the context is told at least the size asked for");
        check(q.done == 1, "the context has the room it asked for");
    }
    // Asking for nothing gives the default, the same 8 MiB as the other two.
    probe d{ 0, 0, 0 };
    kal_task t{};
    check(kal_task_start(room, &d, &t) == kal_ok, "a context starts");
    kal_task_join(t);
    check(d.told >= (8u << 20), "a context that asks for nothing is given 8 MiB");

    kal_task never{};
    check(kal_task_start_sized(room, nullptr, ~kal_uintptr{0} - 4095, &never) == kal_err_no_memory,
          "a stack larger than the address space is reported as exhaustion");
    if (failures == 0) kal::write(kal::out(), "openkal-macos: a least stack size is room\n", 42);
    return failures == 0 ? 0 : 1;
}
