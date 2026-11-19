// What an observation of a unit answers (SPEC.md clause 7.18, version 0.26).
//
// A unit here is a process group, and its reference names the boot session it
// was made in; this kernel has one process space. The cases alter a real
// reference one field at a time, which is how a reference from elsewhere reads
// to this program.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
import openkal.types;
import openkal.process;
import mcpplibs.mtest;

using namespace mcpplibs::mtest;

namespace {

std::vector<std::string> fields(const std::string& r) {
    std::vector<std::string> out(1);
    for (char c : r) { if (c == ';') out.emplace_back(); else out.back() += c; }
    return out;
}
std::string joined(const std::vector<std::string>& f) {
    std::string out;
    for (std::size_t i = 0; i < f.size(); ++i) { if (i) out += ';'; out += f[i]; }
    return out;
}

// What `open' answers, and the state an observation gives where it opens.
struct answer { int open = 0; kal_u32 state = kal::process::job_state::unknown; };
answer ask(const std::string& reference) {
    answer a; kal_job job{};
    a.open = kal_process_job_open(reference.data(), reference.size(), &job);
    if (a.open == kal_ok) kal_process_job_observe(job, reference.data(), reference.size(), &a.state);
    return a;
}

}  // namespace

UTest a_unit_reference_names_its_boot_session_test{"a_unit_reference_names_its_boot_session", [] {
    expect(fatal(kal::process::has(kal::process::job_observe)))
        << "this environment lets a reference be made, so observation is claimed";

    // A child declares a unit, reports its reference and stays until told.
    int to_parent[2], to_child[2];
    expect(fatal(pipe(to_parent) == 0 && pipe(to_child) == 0)) << "pipes";
    const pid_t child = fork();
    if (child == 0) {
        close(to_parent[0]); close(to_child[1]);
        kal_job job{}; char reference[256]; kal_uintptr used = 0;
        const int rc = kal_process_job_create(&job, reference, sizeof reference, &used);
        if (rc != kal_ok) used = 0;
        (void)!write(to_parent[1], reference, used);
        close(to_parent[1]);
        char c; (void)!read(to_child[0], &c, 1);
        _exit(0);
    }
    close(to_parent[1]); close(to_child[0]);
    std::string reference(256, '\0');
    const ssize_t n = read(to_parent[0], reference.data(), reference.size());
    reference.resize(n > 0 ? static_cast<std::size_t>(n) : 0);
    auto f = fields(reference);
    expect(fatal(f.size() == 4 && f[0] == "M2")) << "the reference is `M2;group;birth;boot': " << reference;

    expect(ask(reference).open == kal_ok && ask(reference).state == kal::process::job_state::occupied)
        << "a unit with a running member is occupied";

    {   // Another boot session: every program of it has ended.
        auto g = f; g[3] = "00000000-0000-0000-0000-000000000000";
        const auto a = ask(joined(g));
        expect(a.open == kal_ok && a.state == kal::process::job_state::empty) << "a reference of another boot session is empty (open " << a.open << ", state " << a.state << ")";
    }
    {   // The number is another program's: the kernel gave it again, which it
        // does only once no program holds it as its group.
        auto g = f; g[2] = std::to_string(std::stoull(f[2]) + 1);
        const auto a = ask(joined(g));
        expect(a.open == kal_ok && a.state == kal::process::job_state::empty) << "a unit whose number now names a program of another start is empty (state " << a.state << ")";
    }

    close(to_child[1]);
    int status = 0; waitpid(child, &status, 0);
    const auto after = ask(reference);
    expect(after.open == kal_ok && after.state == kal::process::job_state::empty) << "a unit whose members have ended is empty (state " << after.state << ")";
}};
