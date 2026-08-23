// Where this image's unwind tables are — answered by the image, because there
// is no loader to ask.
//
// ⭐ WHY THIS IS THIS PACKAGE'S JOB. libunwind, on this object format, asks
// `_dyld_find_unwind_sections`, which Apple's libSystem implements by consulting
// the dynamic loader's table of loaded images. A program above openkal is not
// loaded by that loader — it is one image, and the question "which image is this
// address in, and where are its tables" has a constant answer.
//
// So the function is supplied here, beside the two names `port/libSystem.tbd`
// records as genuinely borrowed. ⚠️ It is NOT in the stub: the stub lists names
// this system provides, and this is a name this system provides that openkal
// answers differently — which makes it an implementation, not a borrowing.
//
// ⚠️ Measured 2026-08-23, cross-linking for this system from Linux with the
// whole C++ runtime above openkal: `undefined symbol: _dyld_find_unwind_sections`
// was the last name standing after the compiler runtime and emulated
// thread-local storage were resolved.
// ⚠️ NO INCLUDES. This package's other sources take none either, and the reason
// is the same one src/sys.h records: a header found here would be the C library
// of whichever machine is building, and this package describes the machine
// being built FOR. Measured while adding this file: `#include <stddef.h>`
// reached the toolchain's own libc++ shim and stopped on `'__config_site' file
// not found` — a per-installation file generated for the host.
//
// The two widths used below are the compiler's own, which is where openkal's
// `types.h` takes them from for the same reason.
using okm_uptr = __UINTPTR_TYPE__;

namespace {

// The linker synthesises a symbol for the bounds of any section that exists.
// ⚠️ WEAK, because a program with no exceptions has no `__eh_frame`, and a
// strong reference to an absent section is a link error in exactly the programs
// that were right not to have one.
extern "C" {
extern const char __eh_frame_start[]
    __asm("section$start$__TEXT$__eh_frame") __attribute__((weak));
extern const char __eh_frame_end[]
    __asm("section$end$__TEXT$__eh_frame") __attribute__((weak));
extern const char __unwind_info_start[]
    __asm("section$start$__TEXT$__unwind_info") __attribute__((weak));
extern const char __unwind_info_end[]
    __asm("section$end$__TEXT$__unwind_info") __attribute__((weak));

// The image's own header. Every Mach-O program has one, and the linker names it.
extern const char __dso_handle[] __attribute__((weak));
}

// libunwind's shape, reproduced rather than included: the declaration lives in
// its own header behind `__APPLE__`, and this package does not depend on
// libunwind. ⚠️ The order and widths are the contract; a disagreement here
// writes through a wrong member rather than failing to compile.
struct dyld_unwind_sections {
    const void* mh;
    const void* dwarf_section;
    okm_uptr    dwarf_section_length;
    const void* compact_unwind_section;
    okm_uptr    compact_unwind_section_length;
};

}  // namespace

extern "C" bool _dyld_find_unwind_sections(void*, dyld_unwind_sections* info) {
    if (info == nullptr) return false;

    // ⚠️ The address is ignored, and that is correct rather than lazy: there is
    // one image. A caller asking about an address outside it would be asking
    // about memory no loader here ever mapped.
    info->mh = static_cast<const void*>(__dso_handle);

    const bool haveDwarf = __eh_frame_start != nullptr
                        && __eh_frame_end > __eh_frame_start;
    info->dwarf_section        = haveDwarf ? __eh_frame_start : nullptr;
    info->dwarf_section_length =
        haveDwarf ? static_cast<okm_uptr>(__eh_frame_end - __eh_frame_start) : 0;

    const bool haveCompact = __unwind_info_start != nullptr
                          && __unwind_info_end > __unwind_info_start;
    info->compact_unwind_section = haveCompact ? __unwind_info_start : nullptr;
    info->compact_unwind_section_length =
        haveCompact
            ? static_cast<okm_uptr>(__unwind_info_end - __unwind_info_start)
            : 0;

    // ⚠️ False when there is nothing to find, not "true with zero length".
    // libunwind reads the return value to decide whether to look further, and
    // reporting a section of length zero as present makes it stop looking.
    return haveDwarf || haveCompact;
}
