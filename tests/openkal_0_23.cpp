// What openkal-macos 0.21 changes beside openkal 0.23, as this kernel does it.
//
// A RECORD ACROSS A START: a program granted a recorded handle appends what it
// reached to its caller's record, and its end. A CONFINED PROGRAM IS HELD BY
// THIS SYSTEM'S SANDBOX: one that writes or reads without this interface ---
// this system's own C library --- is refused outside its grants. A PREFERENCE
// reaches the started program.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
import openkal.types;
import openkal.env;
import openkal.fs;
import openkal.process;
import openkal.abort;

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

char g_self[4096];
char g_dir[] = "/tmp/okm-0.21-XXXXXX";

int start(const char* errand, const kal_preopen* grants, kal_uintptr n, kal_uintptr flags) {
    const kal_dir r = root();
    const char* args[] = { g_self, errand };
    const kal_uintptr lens[] = { std::strlen(g_self), std::strlen(errand) };
    const kal_spawn how{ r, r, nullptr, grants, n, flags };
    kal_process p{};
    if (kal_process_spawn(&how, g_self + 1, std::strlen(g_self + 1), args, lens, 2, nullptr, nullptr, 0,
                          nullptr, &p) != kal_ok)
        return -1;
    int status = -1, terminated = 0;
    kal_process_wait(p, &status, &terminated);
    kal_process_close(p);
    return terminated ? -2 : status;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--read") == 0) {
        // The first preopen is the recorded grant: read a file, ask for an
        // absent one, and end through kal_exit so the record is ended.
        kal_dir d{}; kal_uintptr l = 0;
        if (kal_fs_preopen(0, &d, nullptr, 0, &l) != kal_ok) kal_exit(51);
        kal_file f{};
        if (kal_fs_open(d, "present.txt", 11, kal::fs::open::read.bits, &f) != kal_ok) kal_exit(52);
        kal_fs_close_file(f);
        kal_node_info i{}; i.self_size = sizeof i;
        kal_fs_info(d, "absent.txt", 10, 0, kal::fs::field::kind, &i);
        kal_exit(0);
    }
    if (argc > 1 && std::strcmp(argv[1], "--write-raw") == 0) {
        // Past openkal: this system's own library is asked to create a file in
        // the read-only grant, and to read a file not granted at all.
        const int a = ::open("/tmp/okm-0.21-raw.tmp", O_CREAT | O_WRONLY, 0600);
        if (a >= 0) { ::unlink("/tmp/okm-0.21-raw.tmp"); return 61; }
        const int b = ::open("/etc/hosts", O_RDONLY);
        if (b >= 0) return 62;
        return 60;
    }
    if (argc > 1 && std::strcmp(argv[1], "--resources") == 0) return 70 + kal_env_resources();

    const kal_intptr n = kal_env_image(g_self, sizeof g_self - 1);
    if (n <= 0 || g_self[0] != '/') { std::printf("FAIL: the program's own image is named\n"); return 1; }
    g_self[n] = 0;
    if (!mkdtemp(g_dir)) { std::printf("FAIL: a directory for the test\n"); return 1; }
    char path[256];
    std::snprintf(path, sizeof path, "%s/present.txt", g_dir);
    { const int fd = ::open(path, O_CREAT | O_WRONLY, 0600); (void)!::write(fd, "x", 1); ::close(fd); }

    // A record across a start.
    kal_dir dir{};
    check(kal_fs_open_dir(root(), g_dir + 1, std::strlen(g_dir + 1), &dir) == kal_ok, "the test's directory is opened");
    kal_file rec{};
    check(kal_fs_open(dir, "record", 6, (kal::fs::open::write | kal::fs::open::create | kal::fs::open::truncate).bits, &rec) == kal_ok,
          "a record file is opened");
    kal_dir_rights rights{};
    rights.self_size = sizeof rights;
    rights.flags = (kal::fs::dir_recorded | kal::fs::dir_read_only).bits;
    rights.record = rec;
    kal_dir rd{};
    check(kal_fs_dir_attenuate(dir, &rights, &rd) == kal_ok, "the directory is attenuated, recorded");
    const kal_preopen grant{ rd, "/granted", 8 };
    check(start("--read", &grant, 1, 0) == 0, "a program granted the recorded handle reads through it");
    kal_fs_close_dir(rd);
    kal_fs_close_file(rec);
    char buf[4096] = {};
    std::snprintf(path, sizeof path, "%s/record", g_dir);
    const int fd = ::open(path, O_RDONLY);
    const long got = fd >= 0 ? ::read(fd, buf, sizeof buf) : -1;
    if (fd >= 0) ::close(fd);
    auto holds = [&](const char* entry) {
        const std::size_t len = std::strlen(entry) + 1;
        for (long i = 0; i + static_cast<long>(len) <= got; ++i)
            if (std::memcmp(buf + i, entry, len) == 0 && (i == 0 || buf[i - 1] == 0)) return true;
        return false;
    };
    check(holds("rpresent.txt"), "the started program's read is in its caller's record");
    check(holds("aabsent.txt"), "and so is the name it found absent");
    check(holds("e"), "and the record of the program that ended is ended");

    // Confinement, through this system's sandbox.
    if (kal::process::has(kal::process::confine)) {
        kal_dir_rights ro{};
        ro.self_size = sizeof ro;
        ro.flags = kal::fs::dir_read_only.bits;
        // /tmp and the program's own directory, read-only; what a program of
        // this system reads to start (its libraries in /System and /usr/lib)
        // the rules allow, and /etc is not granted.
        static const char* const trees[] = { "private/tmp" };
        kal_preopen grants[4];
        static char names[4][1024];
        kal_uintptr count = 0;
        for (const char* t : trees) {
            kal_dir d{}, a{};
            if (kal_fs_open_dir(root(), t, std::strlen(t), &d) != kal_ok) continue;
            if (kal_fs_dir_attenuate(d, &ro, &a) != kal_ok) continue;
            std::snprintf(names[count], sizeof names[count], "/%s", t);
            grants[count] = kal_preopen{ a, names[count], std::strlen(names[count]) };
            ++count;
        }
        {
            char parent[4096];
            std::strcpy(parent, g_self);
            *std::strrchr(parent, '/') = 0;
            kal_dir d{}, a{};
            if (kal_fs_open_dir(root(), parent + 1, std::strlen(parent + 1), &d) == kal_ok
                && kal_fs_dir_attenuate(d, &ro, &a) == kal_ok) {
                std::snprintf(names[count], sizeof names[count], "%s", parent);
                grants[count] = kal_preopen{ a, names[count], std::strlen(names[count]) };
                ++count;
            }
        }
        const int s = start("--write-raw", grants, count, kal::process::confine_flag.bits);
        check(s == 60, "a confined program is refused by the kernel what its grants do not allow, without openkal");
        if (s != 60) std::printf("  the confined program reported %d\n", s);
    } else {
        std::printf("  confinement is not claimed here\n");
    }

    // A preference reaches the started program.
    check(start("--resources", nullptr, 0, kal::process::economy_flag.bits) == 72,
          "a program started for economy reads economy");
    check(start("--resources", nullptr, 0, kal::process::performance_flag.bits) == 71,
          "and one started for performance reads performance");

    std::snprintf(path, sizeof path, "%s/present.txt", g_dir); ::unlink(path);
    std::snprintf(path, sizeof path, "%s/record", g_dir); ::unlink(path);
    ::rmdir(g_dir);
    if (failures == 0) std::printf("openkal_0_23: ok\n");
    return failures == 0 ? 0 : 1;
}
