#include "sys.h"
#include <openkal/exec.h>

// openkal.exec on this system.
//
// A mapping obtained writable and made executable afterwards, which is the
// order the interface requires and the only order this system permits: a
// mapping that is writable and executable at once is refused here, and the
// interface states the narrower contract for exactly that reason.
//
// ⚠️⚠️ CLAUSE 6.5 NAMES THIS SYSTEM AND THIS INTERFACE IN TERMS, and what it
// says is that availability may be settled by how the artifact is PRODUCED ---
// a signed declaration applied after the link, by whoever produces the
// artifact. That would make the interface a feature of this package rather than
// a part of it.
//
// ⭐ THIS IMPLEMENTATION PROVIDES IT UNCONDITIONALLY, AND THE READING IS A
// MEASUREMENT RATHER THAN A CHOICE. The declaration clause 6.5 describes is
// what a program needs to obtain memory that is writable and executable AT THE
// SAME TIME --- `MAP_JIT' and the pair of calls that flip one region between
// the two. It is not what a program needs to obtain memory that is writable,
// then made executable and no longer writable, which is the whole of what this
// interface offers. tests/conformance_exec.cpp is where that distinction stops
// being an argument: it reserves a region, writes an instruction sequence into
// it, publishes it, CALLS it, and compares what it returned.
//
// ⇒ If that test fails on this system, the reading above is wrong and the
// remedy is clause 6.5's: a feature of this package, provided at dependency
// resolution, together with whatever produces the artifact. The test is here so
// that the question is answered by the system rather than by this comment.
//
// ⚠️ THE INSTRUCTION CACHE IS INVALIDATED HERE AS WELL, AND THAT DOES NOT MOVE
// THE OBLIGATION. A processor with separate caches for data and instructions
// has just had bytes written through the data path that it is about to fetch
// through the instruction path, and nothing in `mprotect' makes the second path
// see the first's writes. The specification places the maintenance upon the
// PROGRAM --- the conformance suite performs it itself and says why: the
// program is the party that knows which bytes it wrote --- so what happens here
// is help rather than conformance, and a program that also does it pays for one
// redundant sequence.
//
// ⭐ IT IS DONE HERE AND NOT IN openkal-linux, WHICH IS AN ASYMMETRY WITH A
// REASON RATHER THAN AN OVERSIGHT. On this system's two architectures the
// builtin expands to nothing (x86_64) or to the maintenance sequence inline
// (aarch64), so it costs nothing to link. On riscv64 it becomes a CALL into the
// compiler's support library, and openkal-linux declines to acquire that
// dependency for an operation the specification does not require of it.
// openkal-linux/src/exec.cpp records the same reasoning from the other side.

namespace {

constexpr okm_uptr kPage = 4096;

// ⚠️ THE PAGE IS 16384 BYTES ON ONE OF THIS SYSTEM'S TWO ARCHITECTURES. Rounding
// to the smaller number is still correct --- the kernel rounds up to its own
// granularity, and a region reserved as 4096 occupies a whole page of whatever
// size --- but a caller freeing with the size it reserved must reach the same
// number, which it does because both go through the same rounding.
okm_uptr round_up(okm_uptr n, okm_uptr to) { return (n + to - 1) & ~(to - 1); }

}  // namespace

extern "C" {

void* kal_exec_alloc(kal_uintptr size) {
    if (size == 0) return nullptr;
    const okm_uptr bytes = round_up(static_cast<okm_uptr>(size), kPage);
    const okm_long r = okm::sys(okm::nr_mmap, 0, static_cast<okm_long>(bytes),
                                okm::prot_read | okm::prot_write,
                                okm::map_private | okm::map_anon, -1, 0);
    if (okm::failed(r)) return nullptr;
    return reinterpret_cast<void*>(r);
}

int kal_exec_publish(void* p, kal_uintptr size) {
    if (p == nullptr || size == 0) return kal_err_invalid;
    const okm_uptr bytes = round_up(static_cast<okm_uptr>(size), kPage);
    const okm_long r = okm::sys(okm::nr_mprotect, reinterpret_cast<okm_long>(p),
                                static_cast<okm_long>(bytes),
                                okm::prot_read | okm::prot_exec);
    if (okm::failed(r)) return okm::translate(r);

    auto* begin = static_cast<char*>(p);
    __builtin___clear_cache(begin, begin + bytes);
    return kal_ok;
}

void kal_exec_free(void* p, kal_uintptr size) {
    if (p == nullptr || size == 0) return;
    const okm_uptr bytes = round_up(static_cast<okm_uptr>(size), kPage);
    okm::sys(okm::nr_munmap, reinterpret_cast<okm_long>(p),
             static_cast<okm_long>(bytes));
}

// A published region may NOT be reserved for writing again on this system, and
// the position is withheld accordingly. Asking this kernel to make an executable
// mapping writable is the case it refuses, which is the whole reason the
// interface separates the two states; a caller that must change published bytes
// reserves a second region and abandons the first, which is what the header
// says a zero here means.
const kal_uintptr kal_exec_props = 0;

}  // extern "C"
