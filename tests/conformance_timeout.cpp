// Conformance: openkal.timeout, and in particular that a bounded transfer waits
// upon the stream it then transfers upon.
//
// THIS IS THE OBSERVATION THAT WAS MISSING while kal_timeout_read and
// kal_timeout_write waited upon the descriptor below the one they moved. Nothing
// in this package examined either operation: there was no test for openkal.timeout
// at all, and the specification's own suite examines them through observations
// that a wait upon the wrong descriptor satisfies -- a bounded read of the
// standard input is permitted to expire, so an implementation that expires for
// the wrong reason is indistinguishable there from one that is right.
import openkal.types;
import openkal.stream;
import openkal.process;
import openkal.timeout;

namespace {

int failures = 0;

void say(const char* s) {
    kal_uintptr n = 0; while (s[n]) ++n;
    kal::write(kal::err(), s, n);
}

void say_num(int v) {
    char b[12]; int i = 12;
    if (v == 0) b[--i] = '0';
    while (v > 0) { b[--i] = static_cast<char>('0' + v % 10); v /= 10; }
    kal::write(kal::err(), b + i, static_cast<kal_uintptr>(12 - i));
}

void check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    say("FAIL: "); say(what); say("\n");
}

// WHY SIXTEEN CHANNELS AND NOT ONE. Under the defect the wait was performed upon
// descriptor N-1, and whether that expires depends on what occupies N-1. For a
// channel made after another, N-1 is the previous channel's writing end, upon
// which input is never reported, so the read expires while bytes sit in the
// stream that was asked for. One channel would be answering about whichever
// descriptor happened to precede it; sixteen make the answer a property of the
// implementation rather than of the process that ran it.
//
// AND WHY THIS RUNS BEFORE ANYTHING ELSE. The mistaken decode succeeded only
// while the generation recorded for N-1 was still zero, so it corrected itself
// for every index at which an owned handle had already been released. Placed
// after anything that creates and releases a file, a listener or a datagram,
// this would examine reused descriptors and hold upon the defect.
void bounded_transfer_waits_on_the_stream() {
    constexpr int n = 16;
    kal_stream mine[n]{}, theirs[n]{};
    int made = 0;

    for (; made < n; ++made) {
        if (kal_process_channel(&mine[made], &theirs[made]) != kal_ok) break;
        const char byte = 'x';
        if (kal_stream_write(theirs[made], &byte, 1) != 1) break;
    }
    check(made == n, "sixteen channels are created and each is written to");

    int transferred = 0;
    for (int i = 0; i < made; ++i) {
        char buf[1] = {};
        // One millisecond. Every one of these streams has a byte waiting, so a
        // correct implementation does not reach the bound at all; the bound is
        // present so that a wait upon the wrong descriptor ends the run rather
        // than hanging it.
        if (kal_timeout_read(mine[i], buf, 1, 1000000) == 1 && buf[0] == 'x')
            ++transferred;
    }
    // The count rather than the first failure. Sixteen distinguishes a correct
    // implementation from one answering about a neighbouring descriptor; the
    // first failure alone would not say which.
    if (transferred != made) {
        say("  "); say_num(transferred); say(" of "); say_num(made);
        say(" bounded reads transferred\n");
    }
    check(transferred == made,
          "a bounded read of a stream that has bytes waiting transfers them");

    // The mirror, and it is a control rather than a discriminator: a stream with
    // nothing in it expires under both readings. It is here so that the
    // observation above cannot be satisfied by an implementation that never
    // waits at all.
    kal_stream empty_mine{}, empty_theirs{};
    if (kal_process_channel(&empty_mine, &empty_theirs) == kal_ok) {
        char buf[1] = {};
        check(kal_timeout_read(empty_mine, buf, 1, 1000000) == -kal_err_again,
              "a bounded read of a stream with nothing waiting expires");
        kal_process_channel_close(empty_theirs);
        kal_process_channel_close(empty_mine);
    }

    for (int i = 0; i < made; ++i) {
        kal_process_channel_close(theirs[i]);
        kal_process_channel_close(mine[i]);
    }
}

void the_rest_of_the_interface() {
    check(kal_timeout_granularity() > 0,
          "the granularity is a positive number of nanoseconds");

    // A transfer of zero bytes does not wait and is not bounded.
    check(kal_timeout_write(kal_stdout(), "", 0, 1) >= 0,
          "a bounded transfer of zero bytes succeeds");

    // A bound of zero denotes no bound, which is the convention kal_task_wait
    // establishes. Observed by writing, which does not wait; a run that waited
    // without bound would never report.
    check(kal_timeout_write(kal_stdout(), "", 0, 0) >= 0,
          "a bound of zero is accepted and denotes no bound");
}

}  // namespace

int main() {
    bounded_transfer_waits_on_the_stream();
    the_rest_of_the_interface();

    if (failures == 0) say("openkal-macos: timeout conformance\n");
    return failures == 0 ? 0 : 1;
}
