// A handle is retired before its descriptor is closed (0.23.1; openkal-linux#53, the same construction).
//
// The kernel gives a closed number to the next open in any context. Until
// 0.23.1 a connection, a listener and a datagram socket were closed first and
// retired second: a file opened by another context between the two took the
// number with the generation the socket's handle still had, and the retire then
// made the file's handle stale. Its stream was answered as zero, standard
// input, so its reads returned nothing of the file and its descriptor was never
// closed. Measured by the report on four processors: about 150 such files a
// second.
//
// One context accepts connections and closes them, one connects and closes, and
// this one opens, reads and closes a file. Every read returns the file.
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
import openkal.types;
import openkal.fs;
import openkal.net;
import openkal.stream;
import mcpplibs.mtest;

using namespace mcpplibs::mtest;

namespace {

// The working directory, which is the first directory the environment supplies.
kal_dir root() {
    kal_dir d{}; char n[4096]; kal_uintptr l = 0;
    return kal_fs_preopen(0, &d, n, sizeof n, &l) == kal_ok ? d : kal_dir{};
}

constexpr char kName[] = "okl_retire_probe.txt";
constexpr char kText[] = "the file's bytes";   // 16

}  // namespace

UTest a_released_handle_is_stale_before_its_number_is_free{
    "a_released_handle_is_stale_before_its_number_is_free", [] {
    const kal_dir dir = root();
    expect(fatal(dir.h != 0)) << "the environment supplies the working directory";
    {
        kal_file f{};
        expect(fatal(kal_fs_open(dir, kName, sizeof kName - 1,
                                 (kal::fs::open::write | kal::fs::open::create | kal::fs::open::truncate).bits,
                                 &f) == kal_ok));
        expect(fatal(kal_stream_write(kal_fs_stream(f), kText, 16) == 16));
        kal_fs_close_file(f);
    }

    kal_endpoint any{};
    any.addr[0] = 127; any.addr[3] = 1; any.addr_len = 4;
    kal_net_listener lis{};
    expect(fatal(kal_net_listen(&any, &lis) == kal_ok));
    kal_endpoint at{};
    expect(fatal(kal_net_listener_local(lis, &at) == kal_ok));

    std::atomic<bool> stop{false};
    std::atomic<long> served{0};
    std::thread accepting([&] {
        for (;;) {
            kal_net_conn c{};
            if (kal_net_accept(lis, &c) != kal_ok) break;
            kal_stream_write(kal_net_stream(c), "okl", 3);
            kal_net_close(c);
            served.fetch_add(1, std::memory_order_relaxed);
            if (stop.load(std::memory_order_relaxed)) break;
        }
    });
    std::thread connecting([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            kal_net_conn c{};
            if (kal_net_connect(&at, &c) == kal_ok) kal_net_close(c);
        }
        // One more, so the accepting context leaves its wait and sees the word.
        kal_net_conn last{};
        if (kal_net_connect(&at, &last) == kal_ok) kal_net_close(last);
    });

    long opened = 0, stale = 0, short_reads = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until) {
        for (int i = 0; i < 256; ++i) {
            kal_file f{};
            if (kal_fs_open(dir, kName, sizeof kName - 1, kal::fs::open::read.bits, &f) != kal_ok) continue;
            ++opened;
            char buf[32]{};
            const kal_intptr n = kal_stream_read(kal_fs_stream(f), buf, sizeof buf);
            if (n < 0) ++stale;
            else if (n != 16 || std::memcmp(buf, kText, 16) != 0) ++short_reads;
            kal_fs_close_file(f);
        }
    }
    stop.store(true, std::memory_order_relaxed);
    connecting.join();
    accepting.join();
    kal_net_close_listener(lis);

    expect(gt(opened, 0L));
    expect(gt(served.load(), 0L)) << "no connection was closed beside the opens; the test measured nothing";
    expect(eq(stale, 0L)) << "files whose handle was stale when its stream was taken, of " << opened;
    expect(eq(short_reads, 0L)) << "files whose read was not the file, of " << opened;

    // A released handle's stream is no stream: a transfer upon it is refused,
    // and is not a transfer upon standard input.
    kal_file f{};
    expect(fatal(kal_fs_open(dir, kName, sizeof kName - 1, kal::fs::open::read.bits, &f) == kal_ok));
    kal_fs_close_file(f);
    char one = 0;
    expect(eq(kal_stream_read(kal_fs_stream(f), &one, 1), static_cast<kal_intptr>(-kal_err_invalid)));
    // And a second release of it closes nothing: the number may already be
    // another's.
    kal_file g{};
    expect(fatal(kal_fs_open(dir, kName, sizeof kName - 1, kal::fs::open::read.bits, &g) == kal_ok));
    kal_fs_close_file(f);
    char buf[32]{};
    expect(eq(kal_stream_read(kal_fs_stream(g), buf, sizeof buf), static_cast<kal_intptr>(16)));
    kal_fs_close_file(g);

    kal_fs_remove(dir, kName, sizeof kName - 1);
}};
