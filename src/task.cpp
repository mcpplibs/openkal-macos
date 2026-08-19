#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
import openkal.task;
import openkal.types;

// ⚠️ This platform has no suspension primitive that a program may use. The
// operation openkal declares as the boundary is therefore constructed here from
// a mutex and a condition variable, which is the reverse of the arrangement on
// a system that provides the primitive: there the synchronisation objects are
// built from the primitive, and here the primitive is built from them.
//
// The construction is recorded rather than concealed because it is the one
// place in this implementation that resembles a compatibility layer. It is
// admitted because the alternative is worse in both directions: an interface
// offering mutexes would oblige an implementation whose environment has none to
// construct them, and every C library above openkal would then be built upon a
// facility it does not need. One shared pair of objects serves every address,
// which costs contention that a program suspending on distinct addresses would
// not otherwise pay, and which is invisible to a caller.
namespace {

int translate(int e) {
    switch (e) {
        case EINVAL: case ESRCH: case EFAULT: return kal_err_invalid;
        case EAGAIN:                          return kal_err_again;
        case ENOMEM:                          return kal_err_no_memory;
        case EPERM:                           return kal_err_permission;
        case ENOSYS:                          return kal_err_not_supported;
        default:                              return kal_err_io;
    }
}

pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;

struct trampoline { void (*entry)(void*); void* arg; };

void* run(void* p) {
    trampoline* t = static_cast<trampoline*>(p);
    void (*entry)(void*) = t->entry;
    void* arg = t->arg;
    ::free(t);
    entry(arg);
    return nullptr;
}

}  // namespace

extern "C" {

int kal_task_start(void (*entry)(void*), void* arg, kal_task* out) {
    if (entry == nullptr || out == nullptr) return kal_err_invalid;
    trampoline* t = static_cast<trampoline*>(::malloc(sizeof(trampoline)));
    if (t == nullptr) return kal_err_no_memory;
    t->entry = entry; t->arg = arg;
    pthread_t id{};
    const int rc = ::pthread_create(&id, nullptr, run, t);
    if (rc != 0) { ::free(t); return translate(rc); }
    *out = kal_task{ reinterpret_cast<kal_uintptr>(id) };
    return kal_ok;
}

int kal_task_join(kal_task h) {
    const int rc = ::pthread_join(reinterpret_cast<pthread_t>(h.h), nullptr);
    return rc == 0 ? kal_ok : translate(rc);
}

void kal_task_yield(void) { ::sched_yield(); }

kal_uintptr kal_task_current(void) {
    return reinterpret_cast<kal_uintptr>(::pthread_self());
}

int kal_task_wait(const __UINT32_TYPE__* word, __UINT32_TYPE__ expected,
                  __UINT64_TYPE__ timeout_ns) {
    ::pthread_mutex_lock(&g_lock);
    // The comparison occurs while the lock is held, and the wait releases it
    // atomically, so the value cannot change unobserved between the two. That
    // is the property the interface requires, and it is why the comparison
    // cannot be performed by the caller.
    if (__atomic_load_n(word, __ATOMIC_SEQ_CST) != expected) {
        ::pthread_mutex_unlock(&g_lock);
        return kal_ok;
    }
    int rc = 0;
    if (timeout_ns == 0) {
        rc = ::pthread_cond_wait(&g_cond, &g_lock);
    } else {
        timespec now{};
        clock_gettime(CLOCK_REALTIME, &now);
        __UINT64_TYPE__ total = static_cast<__UINT64_TYPE__>(now.tv_nsec) + timeout_ns;
        timespec until{ now.tv_sec + static_cast<time_t>(total / 1000000000u),
                        static_cast<long>(total % 1000000000u) };
        rc = ::pthread_cond_timedwait(&g_cond, &g_lock, &until);
    }
    ::pthread_mutex_unlock(&g_lock);
    if (rc == 0) return kal_ok;
    if (rc == ETIMEDOUT) return kal_err_again;
    return translate(rc);
}

int kal_task_wake(const __UINT32_TYPE__*, kal_uintptr count, kal_uintptr* woken) {
    ::pthread_mutex_lock(&g_lock);
    // One pair of objects serves every address, so a wake reaches contexts
    // suspended upon other addresses as well. They observe that their own
    // condition still holds and suspend again, which the interface permits: it
    // requires a caller to re-examine its condition after waking.
    if (count == 1) ::pthread_cond_signal(&g_cond);
    else if (count > 1) ::pthread_cond_broadcast(&g_cond);
    ::pthread_mutex_unlock(&g_lock);
    if (woken) *woken = count;
    return kal_ok;
}

const kal_uintptr kal_task_props =
    kal::task::prop_preemptive | kal::task::prop_parallel
  | kal::task::prop_wait_timeout;

}
