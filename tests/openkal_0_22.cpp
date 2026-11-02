// What openkal-macos 0.20 changes beside openkal 0.22, as this kernel does it.
//
// THE NULL DEVICE AT A POSITION IS THE SYSTEM'S OWN: a started program finds a
// character device at standard input and standard error, reads the end of input
// there and has every write accepted (openkal 0.22, clause 7.15). The device is
// opened in the duplicate that becomes the program, so the caller holds no
// descriptor for it afterwards.
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
import openkal.types;
import openkal.env;
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

int descriptors() {
    int n = 0;
    for (int fd = 0; fd < 1024; ++fd) n += ::fcntl(fd, F_GETFD) != -1;
    return n;
}

char g_self[4096];
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--null") == 0) {
        struct stat in{}, err{};
        if (::fstat(0, &in) != 0 || !S_ISCHR(in.st_mode)) return 41;
        if (::fstat(2, &err) != 0 || !S_ISCHR(err.st_mode)) return 42;
        if (in.st_rdev != err.st_rdev) return 43;
        char b[8];
        if (::read(0, b, sizeof b) != 0) return 44;
        if (::write(2, "discarded", 9) != 9) return 45;
        return 0;
    }
    const kal_intptr n = kal_env_image(g_self, sizeof g_self - 1);
    if (n <= 0 || g_self[0] != '/') { std::printf("FAIL: the program's own image is named\n"); return 1; }
    g_self[n] = 0;

    const kal_dir r = root();
    const char* args[] = { g_self, "--null" };
    const kal_uintptr lens[] = { std::strlen(g_self), 6 };
    const kal_spawn how{ r, r, nullptr, nullptr, 0, (kal::process::null_in_flag | kal::process::null_err_flag).bits };
    const int before = descriptors();
    kal_process p{};
    const int e = kal_process_spawn(&how, g_self + 1, std::strlen(g_self + 1), args, lens, 2,
                                    nullptr, nullptr, 0, nullptr, &p);
    check(e == kal_ok, "a program is started with the null device at two positions");
    check(descriptors() == before, "and the caller holds no descriptor for the device");
    int status = -1, terminated = 0;
    if (e == kal_ok) {
        kal_process_wait(p, &status, &terminated);
        kal_process_close(p);
    }
    check(terminated == 0 && status == 0,
          "the started program finds one character device there, reads its end and writes to it");
    if (status > 0) std::printf("  the started program reported %d\n", status);
    if (failures == 0) std::printf("openkal_0_22: ok\n");
    return failures == 0 ? 0 : 1;
}
