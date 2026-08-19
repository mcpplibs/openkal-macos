// Handle construction shared by the interfaces whose handles are owned.
//
// The specification requires that a released handle not be treated as valid,
// and recommends dividing the word into an index and a generation. That is what
// this file does. It is not a translation table: the descriptor is recovered
// arithmetically from the word, and the array holds only generations, so the
// implementation retains the property clause 7.1 requires.
#pragma once
#include <stdint.h>

namespace okl {

inline constexpr int kMaxDescriptor = 65536;

inline unsigned* generations() {
    static unsigned g[kMaxDescriptor];
    return g;
}

inline uintptr_t pack(int fd) {
    if (fd < 0 || fd >= kMaxDescriptor) return 0;
    return (static_cast<uintptr_t>(generations()[fd]) << 32)
         | (static_cast<uintptr_t>(fd) + 1u);
}

// Returns the descriptor, or -1 if the word does not name a live one.
inline int unpack(uintptr_t h) {
    const int fd = static_cast<int>(h & 0xffffffffu) - 1;
    if (fd < 0 || fd >= kMaxDescriptor) return -1;
    if (static_cast<unsigned>(h >> 32) != generations()[fd]) return -1;
    return fd;
}

inline void retire(uintptr_t h) {
    const int fd = unpack(h);
    if (fd >= 0) ++generations()[fd];
}

}  // namespace okl
