#include <stdio.h>      // renameat
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include "handle.h"
import openkal.fs;
import openkal.types;

namespace {

int translate(int e) {
    switch (e) {
        case EBADF: case EINVAL: case EFAULT: case ENOENT: case ENOTDIR: return kal_err_invalid;
        case EAGAIN:                                     return kal_err_again;
        case ENOMEM:                                     return kal_err_no_memory;
        case ENOSPC: case EFBIG: case EDQUOT:            return kal_err_no_space;
        case EACCES: case EPERM: case EROFS:             return kal_err_permission;
        case ENOSYS: case ENOTSUP:                       return kal_err_not_supported;
        default:                                         return kal_err_io;
    }
}

// A name is a single component or a sequence separated by a forward slash. It
// shall not begin with a separator and shall not contain a component that
// ascends: a program able to ascend from the directory it was given would not
// be confined by having been given it.
bool acceptable(const char* name, kal_uintptr len) {
    if (len == 0 || name[0] == '/') return false;
    kal_uintptr start = 0;
    for (kal_uintptr i = 0; i <= len; ++i) {
        if (i == len || name[i] == '/') {
            const kal_uintptr n = i - start;
            if (n == 0) return false;
            if (n == 2 && name[start] == '.' && name[start + 1] == '.') return false;
            start = i + 1;
        }
    }
    return true;
}

// The operations take a counted name; the system calls take a terminated one.
// A bounded copy is the whole of the adaptation, and it is not a compatibility
// layer: it converts a representation and reconstructs no namespace.
struct terminated {
    char  buf[4096];
    bool  ok;
    terminated(const char* s, kal_uintptr n) : ok(n < sizeof(buf)) {
        if (ok) { for (kal_uintptr i = 0; i < n; ++i) buf[i] = s[i]; buf[n] = '\0'; }
    }
};

}  // namespace

extern "C" {

// The directories this implementation supplies. A hosted system does not
// confine an ordinary program, so it supplies both the working directory and
// the whole file system; an implementation that confined a program would supply
// fewer, and the program would not be able to tell the difference except by
// finding that a directory it wanted was absent.
namespace {

struct preopen { const char* name; kal_uintptr len; const char* path; uintptr_t handle; };

preopen* table(kal_uintptr* count) {
    static preopen t[] = {
        { ".", 1, ".", 0 },
        { "/", 1, "/", 0 },
    };
    static bool opened = false;
    if (!opened) {
        for (auto& e : t) {
            const int fd = ::open(e.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            e.handle = okl::pack(fd);
        }
        opened = true;
    }
    if (count) *count = sizeof(t) / sizeof(t[0]);
    return t;
}

}  // namespace

kal_uintptr kal_fs_preopen_count(void) {
    kal_uintptr n = 0; table(&n); return n;
}

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
    const int b = okl::unpack(base.h);
    if (b < 0 || !acceptable(name, len)) return kal_err_invalid;
    terminated t(name, len); if (!t.ok) return kal_err_invalid;
    const int fd = ::openat(b, t.buf, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return translate(errno);
    *out = kal_dir{ okl::pack(fd) };
    return kal_ok;
}

int kal_fs_open_file(kal_dir base, const char* name, kal_uintptr len,
                     int write, int create, kal_file* out) {
    const int b = okl::unpack(base.h);
    if (b < 0 || !acceptable(name, len)) return kal_err_invalid;
    terminated t(name, len); if (!t.ok) return kal_err_invalid;
    int flags = (write ? O_RDWR : O_RDONLY) | O_CLOEXEC;
    if (create) flags |= O_CREAT | O_TRUNC;
    const int fd = ::openat(b, t.buf, flags, 0666);
    if (fd < 0) return translate(errno);
    *out = kal_file{ okl::pack(fd) };
    return kal_ok;
}

void kal_fs_close_dir (kal_dir d)  { const int fd = okl::unpack(d.h); if (fd >= 0) { okl::retire(d.h); ::close(fd); } }
void kal_fs_close_file(kal_file f) { const int fd = okl::unpack(f.h); if (fd >= 0) { okl::retire(f.h); ::close(fd); } }

// A file's stream is the file. The descriptor is what openkal.stream's handle
// holds on this implementation, so no conversion is required and none is
// performed: the two interfaces agree because both are descriptor-shaped here,
// which is a property of this implementation and not of the specification.
kal_uintptr kal_fs_stream(kal_file f) {
    const int fd = okl::unpack(f.h);
    return fd < 0 ? 0u : static_cast<kal_uintptr>(fd);
}

int kal_fs_seek(kal_file f, __INT64_TYPE__ offset, int whence, __UINT64_TYPE__* result) {
    const int fd = okl::unpack(f.h);
    if (fd < 0) return kal_err_invalid;
    int w = SEEK_SET;
    if (whence == kal::fs::seek_current) w = SEEK_CUR;
    else if (whence == kal::fs::seek_end) w = SEEK_END;
    const off_t r = ::lseek(fd, static_cast<off_t>(offset), w);
    if (r < 0) return translate(errno);
    if (result) *result = static_cast<__UINT64_TYPE__>(r);
    return kal_ok;
}

int kal_fs_info(kal_dir base, const char* name, kal_uintptr len, kal_node_info* out) {
    const int b = okl::unpack(base.h);
    if (b < 0 || !acceptable(name, len) || out == nullptr) return kal_err_invalid;
    terminated t(name, len); if (!t.ok) return kal_err_invalid;
    struct stat st{};
    if (::fstatat(b, t.buf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) { *out = kal_node_info{ 0, 0, kal_node_absent, 0 }; return kal_ok; }
        return translate(errno);
    }
    int kind = kal_node_other;
    if (S_ISREG(st.st_mode))       kind = kal_node_file;
    else if (S_ISDIR(st.st_mode))  kind = kal_node_directory;
    else if (S_ISLNK(st.st_mode))  kind = kal_node_link;
    *out = kal_node_info{
        static_cast<kal_uintptr>(st.st_size),
        static_cast<__UINT64_TYPE__>(st.st_mtimespec.tv_sec) * 1000000000u
            + static_cast<__UINT64_TYPE__>(st.st_mtimespec.tv_nsec),
        kind,
        (st.st_mode & S_IWUSR) != 0 ? 1 : 0,
    };
    return kal_ok;
}

int kal_fs_mkdir(kal_dir base, const char* name, kal_uintptr len) {
    const int b = okl::unpack(base.h);
    if (b < 0 || !acceptable(name, len)) return kal_err_invalid;
    terminated t(name, len); if (!t.ok) return kal_err_invalid;
    return ::mkdirat(b, t.buf, 0777) == 0 ? kal_ok : translate(errno);
}

int kal_fs_remove(kal_dir base, const char* name, kal_uintptr len) {
    const int b = okl::unpack(base.h);
    if (b < 0 || !acceptable(name, len)) return kal_err_invalid;
    terminated t(name, len); if (!t.ok) return kal_err_invalid;
    if (::unlinkat(b, t.buf, 0) == 0) return kal_ok;
    if (errno == EISDIR || errno == EPERM) {
        if (::unlinkat(b, t.buf, AT_REMOVEDIR) == 0) return kal_ok;
    }
    return translate(errno);
}

int kal_fs_rename(kal_dir from, const char* a, kal_uintptr alen,
                  kal_dir to,   const char* b, kal_uintptr blen) {
    const int f = okl::unpack(from.h), t2 = okl::unpack(to.h);
    if (f < 0 || t2 < 0 || !acceptable(a, alen) || !acceptable(b, blen)) return kal_err_invalid;
    terminated ta(a, alen), tb(b, blen);
    if (!ta.ok || !tb.ok) return kal_err_invalid;
    return ::renameat(f, ta.buf, t2, tb.buf) == 0 ? kal_ok : translate(errno);
}

// Enumeration holds a directory stream. The iterator word carries its address,
// which is the implementation's own and is never interpreted by a caller.
int kal_fs_list_begin(kal_dir d, kal_uintptr* iter) {
    const int fd = okl::unpack(d.h);
    if (fd < 0 || iter == nullptr) return kal_err_invalid;
    const int dup = ::dup(fd);
    if (dup < 0) return translate(errno);
    DIR* s = ::fdopendir(dup);
    if (s == nullptr) { ::close(dup); return translate(errno); }
    ::rewinddir(s);
    *iter = reinterpret_cast<kal_uintptr>(s);
    return kal_ok;
}

int kal_fs_list_next(kal_dir, kal_uintptr* iter, const char** name,
                     kal_uintptr* len, int* kind) {
    if (iter == nullptr || *iter == 0) return kal_err_invalid;
    DIR* s = reinterpret_cast<DIR*>(*iter);
    for (;;) {
        errno = 0;
        dirent* e = ::readdir(s);
        if (e == nullptr) {
            ::closedir(s); *iter = 0;
            if (name) *name = nullptr;
            if (len)  *len = 0;
            return errno == 0 ? kal_ok : translate(errno);
        }
        // The two entries that name the directory and its parent are omitted.
        // They exist to support ascent, which this interface does not offer.
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0'
            || (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;
        kal_uintptr n = 0; while (e->d_name[n] != '\0') ++n;
        if (name) *name = e->d_name;
        if (len)  *len  = n;
        if (kind) *kind = e->d_type == DT_DIR ? kal_node_directory
                        : e->d_type == DT_REG ? kal_node_file
                        : e->d_type == DT_LNK ? kal_node_link : kal_node_other;
        return kal_ok;
    }
}

// ⚠️ The default file system of this platform compares names without regard to
// case. The property exists because of differences of exactly this kind: a
// program that creates two names differing only in case succeeds on one
// implementation and not on the other, and no operation can report that in
// advance.
const kal_uintptr kal_fs_props =
    kal::fs::prop_modified_time | kal::fs::prop_atomic_rename;

}
