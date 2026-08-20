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
        const okm_long n = okm::sys(okm::nr_getcwd, reinterpret_cast<okm_long>(g_cwd),
                                    static_cast<okm_long>(sizeof g_cwd));
        okm_uptr cwd_len = 0;
        if (!okm::failed(n)) cwd_len = okm::length(g_cwd);
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

void fill_info(const okm::kstat64& st, kal_node_info* out) {
    *out = kal_node_info{
        static_cast<kal_uintptr>(st.size),
        static_cast<kal_u64>(st.mtime_sec) * 1000000000u
            + static_cast<kal_u64>(st.mtime_nsec),
        kind_of(okm::stat_mode(st)),
        (okm::stat_mode(st) & 0200u) != 0 ? 1 : 0,
    };
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

int kal_fs_preopen(kal_uintptr index, kal_dir* out, const char** name, kal_uintptr* len) {
    kal_uintptr n = 0;
    preopen* t = table(&n);
    if (index >= n || out == nullptr) return kal_err_invalid;
    if (t[index].handle == 0) return kal_err_permission;
    *out = kal_dir{ t[index].handle };
    if (name) *name = t[index].name;
    if (len)  *len  = t[index].len;
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

int kal_fs_open_file(kal_dir base, const char* name, kal_uintptr len,
                     int write, int create, kal_file* out) {
    kal_uintptr flags = KAL_OPEN_READ;
    if (write)  flags |= KAL_OPEN_WRITE;
    if (create) flags |= KAL_OPEN_WRITE | KAL_OPEN_CREATE | KAL_OPEN_TRUNCATE;
    return kal_fs_open(base, name, len, flags, out);
}

void kal_fs_close_dir(kal_dir d) {
    const int fd = okm::unpack(d.h);
    if (fd >= 0) { okm::retire(d.h); okm::sys(okm::nr_close, fd); }
}

void kal_fs_close_file(kal_file f) {
    const int fd = okm::unpack(f.h);
    if (fd >= 0) { okm::retire(f.h); okm::sys(okm::nr_close, fd); }
}

kal_uintptr kal_fs_stream(kal_file f) {
    const int fd = okm::unpack(f.h);
    return fd < 0 ? 0u : static_cast<kal_uintptr>(fd);
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

int kal_fs_info(kal_dir base, const char* name, kal_uintptr len, kal_node_info* out) {
    const int b = okm::unpack(base.h);
    if (b < 0 || out == nullptr || !okm::acceptable(name, len)) return kal_err_invalid;
    okm::terminated t(name, len); if (!t.ok) return kal_err_invalid;
    okm::kstat64 st{};
    const okm_long r = okm::sys(okm::nr_fstatat64, b, reinterpret_cast<okm_long>(t.buf),
                                reinterpret_cast<okm_long>(&st), okm::at_symlink_nofollow);
    if (okm::failed(r)) {
        // Clause 7.7: enquiry about a name that does not exist is answered, not
        // refused. A component of the name that is not a directory is the same
        // answer, because the name still refers to nothing.
        if (r == -okm::e_noent || r == -okm::e_notdir) {
            *out = kal_node_info{ 0, 0, kal_node_absent, 0 };
            return kal_ok;
        }
        return okm::translate(r);
    }
    fill_info(st, out);
    return kal_ok;
}

int kal_fs_file_info(kal_file f, kal_node_info* out) {
    const int fd = okm::unpack(f.h);
    if (fd < 0 || out == nullptr) return kal_err_invalid;
    okm::kstat64 st{};
    const okm_long r = okm::sys(okm::nr_fstat64, fd, reinterpret_cast<okm_long>(&st));
    if (okm::failed(r)) return okm::translate(r);
    fill_info(st, out);
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

int kal_fs_list_next(kal_dir, kal_uintptr* iter, const char** name,
                     kal_uintptr* len, int* kind) {
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
                if (name) *name = nullptr;
                if (len)  *len  = 0;
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
        if (name) *name = e->name;
        if (len)  *len  = e->namlen;
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
const kal_uintptr kal_fs_props =
    KAL_FS_PROP_MODIFIED_TIME | KAL_FS_PROP_ATOMIC_RENAME;

}
