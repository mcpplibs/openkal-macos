#include "sys.h"
#include "handle.h"
#include <openkal/fs.h>
#include <openkal/memory.h>

namespace {

// The directories this implementation supplies. A hosted system does not
// confine an ordinary program, so it supplies both the directory it was started
// in and the whole file system; an implementation that confined a program would
// supply fewer, and the program could not tell the difference except by finding
// that a directory it wanted was absent.
//
// The working directory is reported under the name the environment knows it by,
// which is its absolute path rather than ".". A C library above openkal must
// both resolve an absolute name and report one, and a name of "." leaves it
// able to do only the first.
struct preopen { const char* name; kal_uintptr len; okm_uptr handle; };

char g_cwd[1024];

preopen* table(kal_uintptr* count) {
    static preopen t[2];
    static bool opened = false;
    if (!opened) {
        opened = true;
        // The name of the working directory. This kernel has no call that
        // reports it: the directory is opened and asked what name it was
        // reached by, which is the operation this system supplies instead and
        // is what its own C library uses.
        okm_uptr cwd_len = 0;
        {
            const okm_long fd = okm::sys(okm::nr_openat, okm::at_fdcwd,
                                         reinterpret_cast<okm_long>("."),
                                         okm::o_rdonly | okm::o_directory, 0);
            if (!okm::failed(fd)) {
                const okm_long r = okm::sys(okm::nr_fcntl, fd, okm::f_getpath,
                                            reinterpret_cast<okm_long>(g_cwd));
                if (!okm::failed(r)) cwd_len = okm::length(g_cwd);
                okm::sys(okm::nr_close, fd);
            }
        }
        if (cwd_len == 0) { g_cwd[0] = '.'; g_cwd[1] = '\0'; cwd_len = 1; }

        const okm_long fd0 = okm::sys(okm::nr_openat, okm::at_fdcwd,
                                      reinterpret_cast<okm_long>("."),
                                      okm::o_rdonly | okm::o_directory | okm::o_cloexec, 0);
        const okm_long fd1 = okm::sys(okm::nr_openat, okm::at_fdcwd,
                                      reinterpret_cast<okm_long>("/"),
                                      okm::o_rdonly | okm::o_directory | okm::o_cloexec, 0);
        t[0] = { g_cwd, cwd_len, okm::failed(fd0) ? 0u : okm::pack(static_cast<int>(fd0)) };
        t[1] = { "/",   1,       okm::failed(fd1) ? 0u : okm::pack(static_cast<int>(fd1)) };
    }
    if (count) *count = 2;
    return t;
}

int kind_of(okm_u32 mode) {
    switch (mode & okm::s_ifmt) {
        case okm::s_ifreg: return kal_node_file;
        case okm::s_ifdir: return kal_node_directory;
        case okm::s_iflnk: return kal_node_link;
        default:           return kal_node_other;
    }
}

// The properties of the volume a descriptor is on. kal_fs_props answers with
// this, and so does the one field of an enquiry that is a volume's property
// rather than a node's.
//
// AN ENQUIRY TAKING THE RESOURCE, BECAUSE EVERY POSITION IS A PROPERTY OF THE
// FORMAT. This kernel names the format in words rather than by a number, so
// that is what is consulted. For a format it does not recognise, what is
// claimed is the set that cannot be wrong: a modification time is reported for
// every volume it mounts, and `renameat' within one directory is atomic by
// POSIX.
kal_uintptr volume_props(int fd) {
    // Locks and capacity are answered by this system's VFS for every format
    // beneath it, so they are in the conservative set rather than switched on
    // by name below.
    const kal_uintptr conservative =
        KAL_FS_PROP_MODIFIED_TIME | KAL_FS_PROP_ATOMIC_RENAME
        | KAL_FS_PROP_LOCKS | KAL_FS_PROP_CAPACITY;
    if (fd < 0) return 0;

    okm::kstatfs64 sf{};
    const okm_long r = okm::sys(okm::nr_fstatfs64, fd, reinterpret_cast<okm_long>(&sf));
    if (okm::failed(r)) return conservative;

    const auto named = [&](const char* w) {
        for (int i = 0; i < 16; ++i) {
            if (sf.f_fstypename[i] != w[i]) return false;
            if (w[i] == '\0') return true;
        }
        return false;
    };

    // Nodes whose content is another name, on a format that stores a mode.
    // This is the ordinary volume of this system.
    if (named("apfs") || named("hfs"))
        return conservative | KAL_FS_PROP_LINKS | KAL_FS_PROP_MAKE_LINKS
             | KAL_FS_PROP_EXECUTABLE;

    // The automounter's own namespace: links, and no mode of its own.
    if (named("autofs"))
        return conservative | KAL_FS_PROP_LINKS | KAL_FS_PROP_MAKE_LINKS;

    // A case-sensitive volume with such nodes: a disk image formatted that
    // way, or a network volume presenting one. What a mode means there depends
    // on a server this implementation does not control, so it is not claimed.
    if (named("nfs") || named("smbfs") || named("webdav"))
        return conservative | KAL_FS_PROP_LINKS | KAL_FS_PROP_MAKE_LINKS;

    // The FAT family stores neither a case distinction nor a node that names
    // another, nor a mode. `symlinkat' on such a volume reports a refusal, and
    // this is where a caller learns it before it tries.
    if (named("msdos") || named("exfat")) return conservative;

    return conservative;
}

// Writes no more of the structure than the caller says exists on its side, and
// reports which fields it filled. Every field but `executable' is filled
// unconditionally: one call answers all of them on this kernel, so selecting
// would cost a branch and save nothing.
//
// `executable' is the exception, because it is a property of the volume as
// well as of the node: a format that stores no mode has nothing honest to
// report, and reporting the mount's own choice of bits would be answering for
// the volume rather than the node. The volume is consulted only when the
// caller wants the field, and only for a file --- kal_fs_props already answers
// whether it is worth asking.
void fill_info(const okm::kstat64& st, kal_u32 wanted, kal_node_info* out,
              int volume_fd) {
    const kal_u32 self = out->self_size;
    kal_node_info v{};
    v.self_size   = self;
    v.present     = KAL_INFO_ALL & ~KAL_INFO_EXECUTABLE;
    v.size        = static_cast<kal_u64>(st.size);
    v.modified_ns = static_cast<kal_u64>(st.mtime_sec) * 1000000000u
                  + static_cast<kal_u64>(st.mtime_nsec);
    // Opaque to a caller, which may compare it and may not read it. The device
    // and the inode are this kernel's answer and not the interface's shape.
    v.identity[0] = st.dev;
    v.identity[1] = st.ino;
    v.kind        = kind_of(okm::stat_mode(st));
    v.writable    = (okm::stat_mode(st) & 0200u) != 0 ? 1 : 0;

    // A caller whose structure ends before the field is not given the
    // position, and is not written to beyond what it stated (clause 4.2).
    const kal_u32 reaches = static_cast<kal_u32>(
        __builtin_offsetof(kal_node_info, executable) + sizeof v.executable);
    if ((wanted & KAL_INFO_EXECUTABLE) != 0 && self >= reaches
        && v.kind == kal_node_file
        && (volume_props(volume_fd) & KAL_FS_PROP_EXECUTABLE) != 0) {
        v.present    |= KAL_INFO_EXECUTABLE;
        v.executable  = (okm::stat_mode(st) & 0111u) != 0 ? 1 : 0;
    }

    const kal_u32 n = self < sizeof v ? self : (kal_u32)sizeof v;
    okm::copy(out, &v, n);
}

void fill_absent(kal_node_info* out) {
    const kal_u32 self = out->self_size;
    kal_node_info v{};
    v.self_size = self;
    v.present   = KAL_INFO_KIND;
    v.kind      = kal_node_absent;
    const kal_u32 n = self < sizeof v ? self : (kal_u32)sizeof v;
    okm::copy(out, &v, n);
}

// The caller must state how much of the structure exists on its side.
bool info_ok(const kal_node_info* out) {
    return out != nullptr && out->self_size >= sizeof(kal_u32) * 2;
}

// Copies a name into a caller's buffer and reports the length it HAS.
kal_uintptr put_name(const char* src, kal_uintptr n,
                     char* out, kal_uintptr cap, kal_uintptr* len) {
    if (out != nullptr && cap != 0) okm::copy(out, src, n < cap ? n : cap);
    if (len) *len = n;
    return n;
}

// Enumeration reads the kernel's own directory records. It holds a descriptor
// of its own, obtained by opening the directory through itself: a duplicate
// would share the position with the handle the caller holds, so two
// enumerations of one directory would consume each other's entries.
struct listing {
    int      fd;
    okm_i64  position;
    okm_uptr used;
    okm_uptr at;
    char     buf[8192];
};

}  // namespace

extern "C" {

kal_uintptr kal_fs_preopen_count(void) { kal_uintptr n = 0; table(&n); return n; }

int kal_fs_preopen(kal_uintptr index, kal_dir* out,
                   char* name_out, kal_uintptr name_cap, kal_uintptr* name_len) {
    kal_uintptr n = 0;
    preopen* t = table(&n);
    if (index >= n || out == nullptr) return kal_err_invalid;
    if (t[index].handle == 0) return kal_err_permission;
    *out = kal_dir{ t[index].handle };
    put_name(t[index].name, t[index].len, name_out, name_cap, name_len);
    return kal_ok;
}

int kal_fs_open_dir(kal_dir base, const char* name, kal_uintptr len, kal_dir* out) {
    const int b = okm::unpack(base.h);
    if (b < 0 || out == nullptr || !okm::acceptable(name, len)) return kal_err_invalid;
    okm::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    const okm_long fd = okm::sys(okm::nr_openat, b, reinterpret_cast<okm_long>(t.buf),
                                 okm::o_rdonly | okm::o_directory | okm::o_cloexec, 0);
    if (okm::failed(fd)) return okm::translate(fd);
    *out = kal_dir{ okm::pack(static_cast<int>(fd)) };
    return kal_ok;
}

int kal_fs_open(kal_dir base, const char* name, kal_uintptr len,
                kal_uintptr flags, kal_file* out) {
    const int b = okm::unpack(base.h);
    if (b < 0 || out == nullptr || !okm::acceptable(name, len)) return kal_err_invalid;
    okm::terminated t(name, len); if (!t.ok) return kal_err_invalid;

    const bool r = (flags & KAL_OPEN_READ)  != 0;
    const bool w = (flags & KAL_OPEN_WRITE) != 0;
    okm_long f = w ? (r ? okm::o_rdwr : okm::o_wronly) : okm::o_rdonly;
    f |= okm::o_cloexec;
    if (flags & KAL_OPEN_CREATE)    f |= okm::o_creat;
    if (flags & KAL_OPEN_EXCLUSIVE) f |= okm::o_excl;
    if (flags & KAL_OPEN_TRUNCATE)  f |= okm::o_trunc;
    if (flags & KAL_OPEN_APPEND)    f |= okm::o_append;

    const okm_long fd = okm::sys(okm::nr_openat, b, reinterpret_cast<okm_long>(t.buf), f, 0666);
    if (okm::failed(fd)) return okm::translate(fd);
    *out = kal_file{ okm::pack(static_cast<int>(fd)) };
    return kal_ok;
}

void kal_fs_close_dir(kal_dir d) {
    const int fd = okm::unpack(d.h);
    if (fd >= 0) { okm::retire(d.h); okm::sys(okm::nr_close, fd); }
}

void kal_fs_close_file(kal_file f) {
    const int fd = okm::unpack(f.h);
    if (fd >= 0) { okm::retire(f.h); okm::sys(okm::nr_close, fd); }
}

kal_uintptr kal_fs_max_name(void) { return okm::max_name; }

kal_stream kal_fs_stream(kal_file f) {
    const int fd = okm::unpack(f.h);
    return kal_stream{ fd < 0 ? 0u : static_cast<kal_uintptr>(fd) };
}

int kal_fs_seek(kal_file f, kal_i64 offset, int whence, kal_u64* result) {
    const int fd = okm::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    int w = 0;
    if (whence == KAL_SEEK_CURRENT) w = 1;
    else if (whence == KAL_SEEK_END) w = 2;
    const okm_long r = okm::sys(okm::nr_lseek, fd, static_cast<okm_long>(offset), w);
    if (okm::failed(r)) return okm::translate(r);
    if (result) *result = static_cast<kal_u64>(r);
    return kal_ok;
}

int kal_fs_truncate(kal_file f, kal_u64 size) {
    const int fd = okm::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    const okm_long r = okm::sys(okm::nr_ftruncate, fd, static_cast<okm_long>(size));
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

int kal_fs_info(kal_dir base, const char* name, kal_uintptr len,
                kal_uintptr flags, kal_u32 wanted, kal_node_info* out) {
    const int b = okm::unpack(base.h);
    if (b < 0 || !info_ok(out) || !okm::acceptable(name, len)) return kal_err_invalid;
    okm::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    okm::kstat64 st{};
    // RESOLVES BY DEFAULT, SO THAT ASKING AND OPENING ANSWER THE SAME QUESTION.
    // This implementation asked with AT_SYMLINK_NOFOLLOW always while
    // `kal_fs_open' resolved, so a name referring to a node whose content is
    // another name was reported as that node while opening it reached a file.
    const okm_long at = (flags & KAL_FS_NO_RESOLVE) ? okm::at_symlink_nofollow : 0;
    const okm_long r = okm::sys(okm::nr_fstatat64, b, reinterpret_cast<okm_long>(t.buf),
                                reinterpret_cast<okm_long>(&st), at);
    if (okm::failed(r)) {
        // Clause 7.7: enquiry about a name that does not exist is answered, not
        // refused. A component of the name that is not a directory is the same
        // answer, and so is a node whose content names something absent when
        // the enquiry resolves.
        if (r == -okm::e_noent || r == -okm::e_notdir || r == -okm::e_loop) {
            fill_absent(out);
            return kal_ok;
        }
        return okm::translate(r);
    }
    fill_info(st, wanted, out, b);
    return kal_ok;
}

int kal_fs_file_info(kal_file f, kal_u32 wanted, kal_node_info* out) {
    const int fd = okm::unpack(f.h);
    if (fd < 0 || !info_ok(out)) return kal_err_invalid;
    okm::kstat64 st{};
    const okm_long r = okm::sys(okm::nr_fstat64, fd, reinterpret_cast<okm_long>(&st));
    if (okm::failed(r)) return okm::translate(r);
    fill_info(st, wanted, out, fd);
    return kal_ok;
}

int kal_fs_set_modified(kal_file f, kal_u64 modified_ns) {
    const int fd = okm::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    // This kernel's call takes both times and takes them in microseconds, so
    // two things are true of it that are not true of the interface: the access
    // time cannot be left alone, and a nanosecond cannot be expressed.
    //
    // The access time is therefore read and written back, which is the nearest
    // thing to leaving it alone that the call permits, and the value written is
    // the one the file already had. The nanoseconds below a microsecond are
    // lost, which is why the conformance suite compares whole seconds: an
    // interface that required nanoseconds of every environment would be
    // requiring a resolution three of the environments openkal is implemented
    // on do not agree upon.
    okm::kstat64 st{};
    okm_long r = okm::sys(okm::nr_fstat64, fd, reinterpret_cast<okm_long>(&st));
    if (okm::failed(r)) return okm::translate(r);

    okm::ktimeval times[2];
    times[0].sec  = st.atime_sec;
    times[0].usec = static_cast<int>(st.atime_nsec / 1000);
    times[0].pad  = 0;
    times[1].sec  = static_cast<okm_i64>(modified_ns / 1000000000u);
    times[1].usec = static_cast<int>((modified_ns % 1000000000u) / 1000u);
    times[1].pad  = 0;

    r = okm::sys(okm::nr_futimes, fd, reinterpret_cast<okm_long>(times));
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

// The modification time of a NAME, including a directory. Version 0.10.
//
// Expressed as opening the name and using the operation above's own call,
// because this system's time-setting call takes a descriptor. Opening for
// READING is enough for it and is what lets a DIRECTORY be reached --- which is
// the whole reason this declaration exists: the file-taking form takes a
// `kal_file' and a directory is a `kal_dir', so this interface had no route to
// a directory's time at all.
int kal_fs_set_modified_at(kal_dir base, const char* name, kal_uintptr len,
                           kal_u64 modified_ns) {
    const int b = okm::unpack(base.h);
    if (b < 0 || !okm::acceptable(name, len)) return kal_err_invalid;
    okm::terminated t(name, len); if (!t.ok) return kal_err_invalid;

    const okm_long fd = okm::sys(okm::nr_openat, b,
                                 reinterpret_cast<okm_long>(t.buf),
                                 okm::o_rdonly | okm::o_cloexec, 0);
    if (okm::failed(fd)) return okm::translate(fd);

    okm::kstat64 st{};
    okm_long r = okm::sys(okm::nr_fstat64, fd, reinterpret_cast<okm_long>(&st));
    if (!okm::failed(r)) {
        okm::ktimeval times[2];
        times[0].sec  = st.atime_sec;
        times[0].usec = static_cast<int>(st.atime_nsec / 1000);
        times[0].pad  = 0;
        times[1].sec  = static_cast<okm_i64>(modified_ns / 1000000000u);
        times[1].usec = static_cast<int>((modified_ns % 1000000000u) / 1000u);
        times[1].pad  = 0;
        r = okm::sys(okm::nr_futimes, fd, reinterpret_cast<okm_long>(times));
    }
    okm::sys(okm::nr_close, fd);
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

// Whether a NAME's node may be started as a program. Version 0.13.
//
// Recorded as the mode's execute bits, which is what this system's own tools
// read to decide the same question. The interface states one property and not
// a permission: setting it grants every class of caller that may already read
// the node, and clearing it clears all three classes at once. `kal_fs_props'
// is asked first, so a volume that stores no mode is refused rather than
// reporting success and changing nothing.
int kal_fs_set_executable_at(kal_dir base, const char* name, kal_uintptr len,
                             int executable) {
    const int b = okm::unpack(base.h);
    if (b < 0 || !okm::acceptable(name, len)) return kal_err_invalid;
    okm::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    if ((volume_props(b) & KAL_FS_PROP_EXECUTABLE) == 0) return kal_err_not_supported;

    // Resolves, as the enquiry does by default, so the node changed is the
    // node an ordinary caller would have been told about.
    okm::kstat64 st{};
    okm_long r = okm::sys(okm::nr_fstatat64, b, reinterpret_cast<okm_long>(t.buf),
                          reinterpret_cast<okm_long>(&st), 0);
    if (okm::failed(r)) return okm::translate(r);
    const int k = kind_of(okm::stat_mode(st));
    if (k == kal_node_directory) return kal_err_is_directory;
    if (k != kal_node_file) return kal_err_invalid;

    const unsigned mode = okm::stat_mode(st) & 07777u;
    const unsigned next = executable != 0 ? (mode | ((mode & 0444u) >> 2))
                                          : (mode & ~0111u);
    if (next == mode) return kal_ok;
    r = okm::sys(okm::nr_fchmodat, b, reinterpret_cast<okm_long>(t.buf),
                static_cast<okm_long>(next), 0);
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

// --- exclusion upon a range of a file ---------------------------------------
//
// THE OPEN-FILE FORM. This system's oldest record lock is held by the process
// and is released when that process closes any descriptor for the node; openkal
// states the holder as the `kal_file', and `F_OFD_*' is exactly that.
static int lock_range(kal_file f, kal_u64 start, kal_u64 len,
                      short type, bool wait) {
    const int fd = okm::unpack(f.h);
    if (fd < 0) return kal_err_invalid;

    okm::kflock fl{};
    fl.l_start  = static_cast<okm_i64>(start);
    fl.l_len    = static_cast<okm_i64>(len);   // zero means to the end, as here
    fl.l_pid    = 0;
    fl.l_type   = type;
    fl.l_whence = okm::seek_set;

    const okm_long cmd = wait ? okm::f_ofd_setlkw : okm::f_ofd_setlk;
    okm_long r;
    do {
        r = okm::sys(okm::nr_fcntl, fd, cmd, reinterpret_cast<okm_long>(&fl));
    } while (r == -okm::e_intr);
    if (!okm::failed(r)) return kal_ok;

    // TWO VALUES MEAN ONE THING HERE, AND openkal NAMES ONE OF THEM. The
    // standard this call comes from reports a range another holder has as
    // EITHER of two values and leaves the choice to the system. openkal says
    // `kal_err_again', which a caller polls upon; the other translates to
    // `permission', which a caller reads as "asking again will not help".
    // Narrowed to the attempt that does not wait, where alone the two carry
    // this meaning.
    if (!wait && (-r == okm::e_acces || -r == okm::e_again)) return kal_err_again;
    return okm::translate(r);
}

int kal_fs_lock(kal_file f, kal_u64 start, kal_u64 len, kal_uintptr mode) {
    const bool shared    = (mode & KAL_LOCK_SHARED)    != 0;
    const bool exclusive = (mode & KAL_LOCK_EXCLUSIVE) != 0;
    if (shared == exclusive) return kal_err_invalid;
    return lock_range(f, start, len,
                      shared ? okm::lock_read : okm::lock_write,
                      (mode & KAL_LOCK_WAIT) != 0);
}

int kal_fs_unlock(kal_file f, kal_u64 start, kal_u64 len) {
    return lock_range(f, start, len, okm::lock_unlock, false);
}

// How much the volume holds, in bytes. `f_bavail' rather than `f_bfree': the
// question is what THIS program could use.
int kal_fs_capacity(kal_dir d, kal_u64* total, kal_u64* available) {
    const int fd = okm::unpack(d.h);
    if (fd < 0) return kal_err_invalid;
    okm::kstatfs64 sf{};
    const okm_long r = okm::sys(okm::nr_fstatfs64, fd, reinterpret_cast<okm_long>(&sf));
    if (okm::failed(r)) return okm::translate(r);
    const kal_u64 unit = static_cast<kal_u64>(sf.f_bsize);
    if (total)     *total     = static_cast<kal_u64>(sf.f_blocks) * unit;
    if (available) *available = static_cast<kal_u64>(sf.f_bavail) * unit;
    return kal_ok;
}

int kal_fs_mkdir(kal_dir base, const char* name, kal_uintptr len) {
    const int b = okm::unpack(base.h);
    if (b < 0 || !okm::acceptable(name, len)) return kal_err_invalid;
    okm::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    const okm_long r = okm::sys(okm::nr_mkdirat, b, reinterpret_cast<okm_long>(t.buf), 0777);
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

int kal_fs_remove(kal_dir base, const char* name, kal_uintptr len) {
    const int b = okm::unpack(base.h);
    if (b < 0 || !okm::acceptable(name, len)) return kal_err_invalid;
    okm::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    okm_long r = okm::sys(okm::nr_unlinkat, b, reinterpret_cast<okm_long>(t.buf), 0);
    if (!okm::failed(r)) return kal_ok;
    // One operation removes a name, and the kernel distinguishes two kinds of
    // name where this interface does not.
    if (r == -okm::e_isdir || r == -okm::e_perm) {
        const okm_long d = okm::sys(okm::nr_unlinkat, b, reinterpret_cast<okm_long>(t.buf),
                                    okm::at_removedir);
        if (!okm::failed(d)) return kal_ok;
        r = d;
    }
    return okm::translate(r);
}

int kal_fs_rename(kal_dir from, const char* a, kal_uintptr alen,
                  kal_dir to, const char* b, kal_uintptr blen) {
    const int f = okm::unpack(from.h), t2 = okm::unpack(to.h);
    if (f < 0 || t2 < 0 || !okm::acceptable(a, alen) || !okm::acceptable(b, blen))
        return kal_err_invalid;
    okm::terminated ta(a, alen), tb(b, blen);
    if (!ta.ok || !tb.ok) return kal_err_invalid;
    const okm_long r = okm::sys(okm::nr_renameat, f, reinterpret_cast<okm_long>(ta.buf),
                                t2, reinterpret_cast<okm_long>(tb.buf));
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

int kal_fs_list_begin(kal_dir d, kal_uintptr* iter) {
    const int fd = okm::unpack(d.h);
    if (fd < 0 || iter == nullptr) return kal_err_invalid;
    const okm_long own = okm::sys(okm::nr_openat, fd, reinterpret_cast<okm_long>("."),
                                  okm::o_rdonly | okm::o_directory | okm::o_cloexec, 0);
    if (okm::failed(own)) return okm::translate(own);
    auto* s = static_cast<listing*>(kal_alloc(sizeof(listing), alignof(listing)));
    if (s == nullptr) { okm::sys(okm::nr_close, own); return kal_err_no_memory; }
    s->fd = static_cast<int>(own); s->position = 0; s->used = 0; s->at = 0;
    *iter = reinterpret_cast<kal_uintptr>(s);
    return kal_ok;
}

int kal_fs_list_next(kal_dir, kal_uintptr* iter,
                     char* name_out, kal_uintptr name_cap,
                     kal_uintptr* name_len, int* kind) {
    if (iter == nullptr || *iter == 0) return kal_err_invalid;
    auto* s = reinterpret_cast<listing*>(*iter);
    for (;;) {
        if (s->at >= s->used) {
            const okm_long r = okm::sys(okm::nr_getdirentries64, s->fd,
                                        reinterpret_cast<okm_long>(s->buf),
                                        static_cast<okm_long>(sizeof s->buf),
                                        reinterpret_cast<okm_long>(&s->position));
            if (okm::interrupted(r)) continue;
            if (okm::failed(r) || r == 0) {
                okm::sys(okm::nr_close, s->fd);
                kal_free(s, sizeof(listing), alignof(listing));
                *iter = 0;
                if (name_len) *name_len = 0;
                return okm::failed(r) ? okm::translate(r) : kal_ok;
            }
            s->used = static_cast<okm_uptr>(r);
            s->at = 0;
        }
        auto* e = reinterpret_cast<okm::kdirent64*>(s->buf + s->at);
        s->at += e->reclen;
        // The two entries that name the directory and its parent are omitted.
        // They exist to support ascent, which this interface does not offer.
        if (e->name[0] == '.' && (e->name[1] == '\0'
            || (e->name[1] == '.' && e->name[2] == '\0'))) continue;
        put_name(e->name, e->namlen, name_out, name_cap, name_len);
        if (kind) *kind = e->type == okm::dt_dir ? kal_node_directory
                        : e->type == okm::dt_reg ? kal_node_file
                        : e->type == okm::dt_lnk ? kal_node_link : kal_node_other;
        return kal_ok;
    }
}

// Names on this system are compared without regard to case on the file system
// it is ordinarily installed on. A program that creates two names differing
// only in case succeeds on the Linux implementation and not on this one, and
// the position reports it in advance, which no operation could.
kal_uintptr kal_fs_props(kal_dir d) { return volume_props(okm::unpack(d.h)); }

// Nodes whose content is another name.
int kal_fs_link_create(kal_dir base, const char* name, kal_uintptr len,
                       const char* target, kal_uintptr target_len,
                       kal_uintptr flags) {
    // The target is content rather than a name this interface resolves, so it
    // is not passed through `acceptable' --- which would refuse one that
    // ascends, and one that ascends is the ordinary case for a relative target.
    (void)flags;   // this kernel does not distinguish a link to a directory
    const int b = okm::unpack(base.h);
    if (b < 0 || !okm::acceptable(name, len) || target == nullptr) return kal_err_invalid;
    okm::terminated n(name, len);            if (!n.ok) return kal_err_invalid;
    okm::terminated tgt(target, target_len); if (!tgt.ok) return kal_err_invalid;
    const okm_long r = okm::sys(okm::nr_symlinkat, reinterpret_cast<okm_long>(tgt.buf),
                                b, reinterpret_cast<okm_long>(n.buf));
    return okm::failed(r) ? okm::translate(r) : kal_ok;
}

kal_intptr kal_fs_link_read(kal_dir base, const char* name, kal_uintptr len,
                            char* out, kal_uintptr cap) {
    const int b = okm::unpack(base.h);
    if (b < 0 || !okm::acceptable(name, len)) return -kal_err_invalid;
    okm::terminated t(name, len); if (!t.ok) return -kal_err_invalid;

    // The kernel truncates into the buffer it is given and does not report the
    // length the content has, so a caller asking for the length --- a capacity
    // of zero --- is served from a buffer of this implementation's own.
    char own[okm::max_name + 1];
    char* dst = (out != nullptr && cap != 0) ? out : own;
    okm_uptr room = (out != nullptr && cap != 0) ? cap : sizeof own;
    okm_long r = okm::sys(okm::nr_readlinkat, b, reinterpret_cast<okm_long>(t.buf),
                          reinterpret_cast<okm_long>(dst), static_cast<okm_long>(room));
    if (okm::failed(r)) return -okm::translate(r);

    if (static_cast<okm_uptr>(r) == room && room < sizeof own) {
        const okm_long full = okm::sys(okm::nr_readlinkat, b,
                                       reinterpret_cast<okm_long>(t.buf),
                                       reinterpret_cast<okm_long>(own),
                                       static_cast<okm_long>(sizeof own));
        if (!okm::failed(full)) r = full;
    }
    return static_cast<kal_intptr>(r);
}

}
