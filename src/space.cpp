#include "sys.h"
#include <openkal/space.h>

// openkal.space upon this kernel's duplication primitive.
//
// THE PRIMITIVE IS ALREADY IN src/sys.h AND WAS ALREADY IN USE: openkal.process
// starts a program by duplicating this one and replacing the duplicate, so the
// call, its two return values and the reason the second one matters were
// written down before this interface existed. What is added here is the other
// use of the same primitive --- the one where the duplicate is not replaced.
//
// ⚠️ THE DUPLICATE IS DISTINGUISHED BY THE SECOND VALUE THE CALL RETURNS AND
// NOT BY THE FIRST. Both images receive a process identifier in the first
// register on this system: the original receives the duplicate's and the
// duplicate receives the ORIGINAL's, so an implementation that tested the first
// against zero --- which is what the other kernel's convention would suggest ---
// would decide that neither image was the duplicate. src/sys.h records the
// measurement beside the call.

extern "C" {

int kal_space_start(void (*entry)(void*), void* arg, void* stack_top,
                    kal_process* out) {
    if (entry == nullptr || out == nullptr) return kal_err_invalid;

    // THE STACK ARGUMENT IS ACCEPTED AND IGNORED, AND THE HEADER STATES THAT AS
    // ONE OF THE TWO PERMITTED BEHAVIOURS. This kernel's primitive gives the
    // started context a copy of the caller's own stack, so there is nothing for
    // a caller-supplied one to be used for; a caller cannot observe which of the
    // two occurred and has no decision resting upon it.
    (void)stack_top;

    bool is_duplicate = false;
    const okm_long child = okm::duplicate(is_duplicate);
    if (okm::failed(child)) return okm::translate(child);

    if (is_duplicate) {
        entry(arg);
        // THE ENTRY IS NOT REQUIRED TO RETURN, AND IF IT DOES THE CONTEXT ENDS.
        //
        // Returning from here would return into the duplication's caller in the
        // copied space, which is the whole program running a second time from
        // the middle of this function. Ending the context is the only defined
        // thing to do, and the status says the entry returned rather than
        // choosing one.
        for (;;) okm::sys(okm::nr_exit, 0);
    }

    *out = kal_process{ static_cast<kal_uintptr>(child) };
    return kal_ok;
}

// Both positions hold on this kernel.
//
// The handles accompany the memory: the duplicate receives a copy of the
// descriptor table, so every handle the caller holds is open in the copy at the
// same number, and the packing in handle.h recovers the same descriptor from the
// same word.
//
// The copy is deferred: this kernel maps the pages copy-on-write, so a store to
// copied memory can fail with the machine out of memory after this call has
// already reported success. An implementation cannot undefer that, and stating
// it is what lets a program that cannot tolerate it know which environment it
// is in.
const kal_uintptr kal_space_props =
    KAL_SPACE_PROP_CLONE_HANDLES | KAL_SPACE_PROP_DEFERRED_COPY;

}  // extern "C"
