#include "sys.h"
#include <openkal/task.h>
#include <openkal/memory.h>

// Execution contexts, and the primitive they are built upon.
//
// The primitive is a kernel call: this system has the same operation the other
// kernel's futex provides, under a different name and with no shared ancestry,
// and openkal's kal_task_wait is that operation. That two unrelated systems
// offer it is the evidence that it is the shape of the thing rather than the
// shape of one kernel.
//
// Contexts are not a kernel call. Creating one on this system means arranging
// state the kernel does not arrange, and the arrangement belongs to this
// system's thread library rather than to its kernel. Which of that library's
// names may be used is decided by one property: a program above openkal may
// itself define every ordinary name, so only a name no C library defines is
// reachable. `pthread_create_from_mach_thread' is such a name, and the
// measurement that established it is in .github/workflows/probe.yml.
//
// Where the program carries a runtime of its own --- the ordinary arrangement,
// and the default --- there is no such constraint and the ordinary names are
// used, which also avoids the cost the other arrangement pays.

#ifndef OKM_STANDALONE
#include <pthread.h>
#endif

extern "C" {
#ifdef OKM_STANDALONE
int pthread_create_from_mach_thread(void** thread, const void* attr,
                                    void* (*start)(void*), void* arg);
#endif
}

namespace {

struct context {
    void (*entry)(void*);
    void* arg;
#ifdef OKM_STANDALONE
    volatile okm_u32 finished;
#else
    unsigned long thread;
#endif
};

void* run(void* p) {
    auto* c = static_cast<context*>(p);
    c->entry(c->arg);
#ifdef OKM_STANDALONE
    __atomic_store_n(&c->finished, 1u, __ATOMIC_RELEASE);
    okm::sys(okm::nr_ulock_wake,
             static_cast<okm_long>(okm::ul_compare_and_wait | okm::ulf_no_errno
                                 | okm::ulf_wake_all),
             reinterpret_cast<okm_long>(const_cast<okm_u32*>(&c->finished)), 0);
#endif
    return nullptr;
}

int translate_posix(int e) {
    switch (e) {
        case okm::e_inval: case okm::e_fault: return kal_err_invalid;
        case okm::e_again:                    return kal_err_again;
        case okm::e_nomem:                    return kal_err_no_memory;
        case okm::e_perm:                     return kal_err_permission;
        default:                              return kal_err_io;
    }
}

}  // namespace

extern "C" {

int kal_task_start(void (*entry)(void*), void* arg, kal_task* out) {
    if (entry == nullptr || out == nullptr) return kal_err_invalid;
    auto* c = static_cast<context*>(kal_alloc(sizeof(context), alignof(context)));
    if (c == nullptr) return kal_err_no_memory;
    okm::fill(c, 0, sizeof(context));
    c->entry = entry; c->arg = arg;

#ifdef OKM_STANDALONE
    void* thread = nullptr;
    const int rc = pthread_create_from_mach_thread(&thread, nullptr, run, c);
#else
    pthread_t id{};
    const int rc = ::pthread_create(&id, nullptr, run, c);
    if (rc == 0) c->thread = static_cast<unsigned long>(reinterpret_cast<okm_uptr>(id));
#endif
    if (rc != 0) { kal_free(c, sizeof(context), alignof(context)); return translate_posix(rc); }
    *out = kal_task{ reinterpret_cast<kal_uintptr>(c) };
    return kal_ok;
}

int kal_task_join(kal_task h) {
    auto* c = reinterpret_cast<context*>(h.h);
    if (c == nullptr) return kal_err_invalid;
#ifdef OKM_STANDALONE
    // Waited for with the primitive rather than with the thread library's own
    // wait, because that one's name is among the ones a program above may
    // define. The word the context sets before it ends is what is waited upon.
    while (__atomic_load_n(&c->finished, __ATOMIC_ACQUIRE) == 0) {
        okm::sys(okm::nr_ulock_wait,
                 static_cast<okm_long>(okm::ul_compare_and_wait | okm::ulf_no_errno),
                 reinterpret_cast<okm_long>(const_cast<okm_u32*>(&c->finished)), 0, 0);
    }
#else
    const int rc = ::pthread_join(reinterpret_cast<pthread_t>(c->thread), nullptr);
    if (rc != 0) return translate_posix(rc);
#endif
    kal_free(c, sizeof(context), alignof(context));
    return kal_ok;
}

void kal_task_yield(void) { okm::relax(); }

kal_uintptr kal_task_current(void) { return okm::current_context(); }

int kal_task_wait(const kal_u32* word, kal_u32 expected,
                  kal_u64 timeout_ns) {
    // The unit this system takes is the microsecond, and zero means no timeout.
    // A timeout shorter than a microsecond is rounded up rather than down: a
    // wait that returned before the time it was given would make every timed
    // wait above it wrong.
    okm_u32 microseconds = 0;
    if (timeout_ns != 0) {
        const kal_u64 rounded = (timeout_ns + 999u) / 1000u;
        microseconds = rounded > 0xfffffffeu ? 0xfffffffeu : static_cast<okm_u32>(rounded);
    }
    for (;;) {
        const okm_long r = okm::sys(okm::nr_ulock_wait,
                                    static_cast<okm_long>(okm::ul_compare_and_wait
                                                        | okm::ulf_no_errno),
                                    reinterpret_cast<okm_long>(const_cast<kal_u32*>(word)),
                                    static_cast<okm_long>(expected),
                                    static_cast<okm_long>(microseconds));
        if (r >= 0) return kal_ok;
        // The value had already changed, which is a successful outcome: the
        // caller's condition no longer holds and it should re-examine it.
        if (r == -okm::e_again) return kal_ok;
        if (okm::interrupted(r)) continue;
        if (r == -okm::e_timedout) return kal_err_again;
        return okm::translate(r);
    }
}

int kal_task_wake(const kal_u32* word, kal_uintptr count, kal_uintptr* woken) {
    if (count == 0) { if (woken) *woken = 0; return kal_ok; }
    okm_long operation = okm::ul_compare_and_wait | okm::ulf_no_errno;
    if (count > 1) operation |= okm::ulf_wake_all;
    const okm_long r = okm::sys(okm::nr_ulock_wake, operation,
                                reinterpret_cast<okm_long>(const_cast<kal_u32*>(word)), 0);
    // This system reports that nothing was waiting as a failure. Nothing having
    // been waiting is not a failure of the operation, so it is reported as none
    // woken.
    if (okm::failed(r) && r != -okm::e_noent) return okm::translate(r);
    if (woken) *woken = okm::failed(r) ? 0u : count;
    return kal_ok;
}

// A context started here observes the thread-local storage of the toolchain
// that compiled the program: this system's thread library establishes it for
// every context it creates, which is why the position can be reported without
// this implementation doing anything to earn it.
const kal_uintptr kal_task_props =
    KAL_TASK_PROP_PREEMPTIVE | KAL_TASK_PROP_PARALLEL
  | KAL_TASK_PROP_WAIT_TIMEOUT | KAL_TASK_PROP_THREAD_LOCAL;

}
