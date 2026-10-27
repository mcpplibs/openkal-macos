// A file this machine cannot start is "not a program" (kal_err_not_program),
// whichever of this kernel's values says so (0.15.1): ENOEXEC for a file that
// is no image, EBADARCH for an image built for a processor this machine does
// not have. Before, only ENOEXEC was translated, and an image of another
// processor --- the case a runner asks about --- arrived as an input/output
// error.
//
// The foreign image is a Mach-O header naming a processor that does not exist;
// the kernel refuses it by its header before reading anything else.
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
import openkal.types;
import openkal.fs;
import openkal.process;

namespace {
int failures = 0;
void check(bool held, const char* what) {
    if (!held) { std::printf("FAIL: %s\n", what); ++failures; }
}

kal_dir root() {
    for (kal_uintptr i = 0; i < kal_fs_preopen_count(); ++i) {
        kal_dir d{}; char n[8]; kal_uintptr l = 0;
        if (kal_fs_preopen(i, &d, n, sizeof n, &l) == kal_ok && l == 1 && n[0] == '/') return d;
    }
    return kal_dir{};
}

// The refusal a start of `path' (absolute) meets.
int start(const char* path) {
    const kal_dir r = root();
    const char* argv[] = { path };
    const kal_uintptr lens[] = { std::strlen(path) };
    const kal_spawn how{ r, r, nullptr, nullptr, 0, 0 };
    kal_process p{};
    const int e = kal_process_spawn(&how, path + 1, std::strlen(path + 1), argv, lens, 1,
                                    nullptr, nullptr, 0, nullptr, &p);
    if (e == kal_ok) {
        int status = 0, terminated = 0;
        kal_process_wait(p, &status, &terminated);
    }
    return e;
}

// A file of `bytes', executable, at a fresh name under /tmp.
bool place(char* path, const unsigned char* bytes, std::size_t n) {
    const int fd = ::mkstemp(path);
    if (fd < 0) return false;
    const bool written = ::write(fd, bytes, n) == static_cast<ssize_t>(n);
    ::fchmod(fd, 0755);
    ::close(fd);
    return written;
}
}  // namespace

int main() {
    // mach_header_64: magic, a processor type no machine has, a subtype, MH_EXECUTE.
    const unsigned char foreign[32] = {
        0xcf, 0xfa, 0xed, 0xfe,   0x45, 0x23, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,   0x02, 0x00, 0x00, 0x00,
    };
    char image[] = "/tmp/okm-foreign-XXXXXX";
    check(place(image, foreign, sizeof foreign), "a foreign image is written");
    check(start(image) == kal_err_not_program, "an image of a processor this machine lacks is not a program");
    ::unlink(image);

    const unsigned char text[] = { 'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e', '\n' };
    char plain[] = "/tmp/okm-plain-XXXXXX";
    check(place(plain, text, sizeof text), "a file of text is written");
    const int e = start(plain);
    // A file with no `#!' and no image header: the kernel's ENOEXEC.
    check(e == kal_err_not_program, "a file that is no image is not a program");
    ::unlink(plain);

    if (failures == 0) std::printf("openkal-macos: a file this machine cannot start is not a program\n");
    return failures == 0 ? 0 : 1;
}
