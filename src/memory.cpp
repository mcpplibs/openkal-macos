#include <stdlib.h>
#include <stddef.h>
import openkal.memory;

extern "C" {

// Built upon the C library's allocator rather than beside it, as the
// specification requires. The alternative places two claimants on one region
// of memory, and the C library's formatted output already draws on its own
// allocator, so the second claimant would not be idle.
void* kal_alloc(kal_uintptr size, kal_uintptr align) {
    if (size == 0) return nullptr;
    if (align <= alignof(::max_align_t)) return ::malloc(size);
    // aligned_alloc requires the size to be a multiple of the alignment.
    const kal_uintptr rounded = (size + align - 1) / align * align;
    return ::aligned_alloc(align, rounded);
}

// The size and alignment are discarded. They are carried by the interface for
// the benefit of implementations whose allocators require them; on a platform
// whose allocator records its own metadata the parameters cost nothing.
void kal_free(void* p, kal_uintptr, kal_uintptr) { ::free(p); }

}
