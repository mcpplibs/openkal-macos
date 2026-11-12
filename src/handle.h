// Handle construction shared by the interfaces whose handles are owned.
//
// The specification requires that a released handle not be treated as valid,
// and recommends dividing the word into an index and a generation. That is what
// this file does. It is not a translation table: the descriptor is recovered
// arithmetically from the word, and the array holds only generations, so the
// implementation retains the property clause 7.1 requires.
#pragma once
#include "sys.h"

namespace okm {

inline constexpr int kMaxDescriptor = 65536;

inline unsigned* generations() {
    static unsigned g[kMaxDescriptor];
    return g;
}

// A generation is read by the context that opens a descriptor and advanced by
// the one that releases it, and the two are different contexts whenever a
// number is reused across them, so both go through the atomic operations.
inline unsigned generation(int fd) {
    return __atomic_load_n(&generations()[fd], __ATOMIC_RELAXED);
}

inline okm_uptr pack(int fd) {
    if (fd < 0 || fd >= kMaxDescriptor) return 0;
    return (static_cast<okm_uptr>(generation(fd)) << 32)
         | (static_cast<okm_uptr>(fd) + 1u);
}

// Returns the descriptor, or -1 if the word does not name a live one.
//
// THIS ACCEPTS A WORD THAT WAS NEVER PACKED, AND SILENTLY. A bare descriptor N
// has the shape of a packed handle naming N-1 whose generation is still zero,
// so this returns N-1 for it rather than -1. Nothing here can tell the two
// apart: the word is one machine word and carries no tag.
//
// The consequence is that a handle of the OTHER discipline must never reach
// this function. openkal.stream's handles are bare descriptors (stream.cpp
// states why), and src/timeout.cpp used to pass one here and wait upon the
// descriptor below the one it then transferred upon. Owned handles --- kal_file,
// kal_dir, kal_net_listener, kal_net_conn, kal_datagram --- are the whole of
// this function's domain.
inline int unpack(okm_uptr h) {
    const int fd = static_cast<int>(h & 0xffffffffu) - 1;
    if (fd < 0 || fd >= kMaxDescriptor) return -1;
    if (static_cast<unsigned>(h >> 32) != generation(fd)) return -1;
    return fd;
}

// RETIRE, THEN CLOSE. The kernel gives a closed number to the next open in any
// context. A handle retired after its descriptor was closed leaves a window in
// which another context opens the number, packs it with the generation the
// handle being released still has, and is then left with a handle this retire
// makes stale: its stream reads and writes nothing of its own, and its
// descriptor is never closed (openkal-linux#53, the same construction). Every
// release in this implementation is `release(h)`, which keeps the order in one
// place.
//
// One release of a handle advances its generation; a second release of the
// same word, in this context or another, finds it advanced and closes nothing,
// so it cannot close a descriptor opened since.
inline bool retire(okm_uptr h) {
    const int fd = static_cast<int>(h & 0xffffffffu) - 1;
    if (fd < 0 || fd >= kMaxDescriptor) return false;
    unsigned held = static_cast<unsigned>(h >> 32);
    return __atomic_compare_exchange_n(&generations()[fd], &held, held + 1u, false,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

inline void release(okm_uptr h) {
    const int fd = static_cast<int>(h & 0xffffffffu) - 1;
    if (retire(h)) sys(nr_close, fd);
}

// What a stream enquiry answers for a handle that does not name a live
// descriptor. A stream is a bare descriptor here (stream.cpp), so zero is
// standard input: a released handle answered with zero was read from and
// written to as standard input. No descriptor has this value, and every
// transfer upon it reports `kal_err_invalid`.
inline constexpr okm_uptr kNoStream = ~static_cast<okm_uptr>(0);

inline okm_uptr stream_word(okm_uptr h) {
    const int fd = unpack(h);
    return fd < 0 ? kNoStream : static_cast<okm_uptr>(fd);
}

}  // namespace okm
