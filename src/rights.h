// A directory handle with less (openkal 0.23, `kal_fs_dir_attenuate',
// SPEC.md clause 7.16): what a handle may not do, what its holder stated, and
// what it records.
//
// A table indexed by descriptor, consulted by every operation of openkal.fs that
// takes a directory. It is empty in a program that attenuates nothing, and the
// one test of `g_any' is then all an operation pays.
#pragma once
#include "sys.h"

namespace okm::rights {

inline constexpr okm_u32 read_only  = 1u;   // KAL_DIR_READ_ONLY
inline constexpr okm_u32 unchanging = 2u;   // KAL_DIR_UNCHANGING
inline constexpr okm_u32 recorded   = 4u;   // KAL_DIR_RECORDED

extern volatile bool g_any;

// The rights a directory descriptor carries; zero for one that carries none.
okm_u32 of(int fd);

// A new descriptor for the directory `fd' names, carrying the rights of `fd'
// and `add'; `record' is the holder's file for `recorded'. Returns a kal_err
// value, and writes the new descriptor to `out' on kal_ok.
int attenuate(int fd, okm_u32 add, int record, int* out);

// A directory `child' opened beneath `parent' by `name': it carries what the
// parent carries. Returns false when the table has no room, and the caller then
// refuses the open rather than hand out a directory with more than its base.
bool derive(int parent, int child, const char* name, okm_uptr len);

// The descriptor is being closed.
void forget(int fd);

// What a name reached beneath `fd' was found to be, for a recorded handle:
//   'r' read, 'w' written, 's' asked about and present, 'a' absent, 'l' listed.
// A name of length zero is the directory itself.
void note(int fd, char kind, const char* name, okm_uptr len);

// Whether a handle that states nothing changes beneath it knows `name' to be
// absent, from what it has listed --- without asking the kernel.
bool known_absent(int fd, const char* name, okm_uptr len);

// Something beneath was changed through a handle of this tree: what was listed
// may no longer hold.
void changed(int fd);

// THE CONVEYANCE TO A STARTED PROGRAM. What a granted descriptor carries, for
// the environment variable a start writes; and, in the started program, a
// preopen received with rights.
bool conveyed(int fd, okm_u32* flags, int* record, const char** prefix, okm_uptr* prefix_len);
void adopt(int fd, okm_u32 flags, int record, const char* prefix, okm_uptr prefix_len);

// The program is ending: every record is written, and ended with `e'.
void end_of_program();

}  // namespace okm::rights
