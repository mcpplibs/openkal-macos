// Conformance: openkal.terminal upon a pseudo terminal this test makes.
//
// WHAT IS OBSERVED IS THE BYTE, because that is what mcpplibs/openkal#48 was
// about: the return key reached a program in raw mode as a line feed, the same
// byte Ctrl+J produces, and nothing above this interface could tell the two
// apart again. The conformance suite cannot make a terminal --- openkal has no
// operation for one --- so the position is observed here, through the kernel's
// own table of terminals, which is what clause 7.13 does for handles.
#include <fcntl.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

import openkal.types;
import openkal.stream;
import openkal.terminal;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    ++failures;
    const char pre[] = "FAIL: ";
    kal::write(kal::err(), pre, sizeof(pre) - 1);
    kal_uintptr n = 0; while (what[n]) ++n;
    kal::write(kal::err(), what, n);
    kal::write(kal::err(), "\n", 1);
}

constexpr kal_uintptr kRaw = kal::terminal::pass_control.bits;   // line assembly and echo off
constexpr tcflag_t kAlters = ISTRIP | INLCR | IGNCR | ICRNL;

// What the terminal delivers for the bytes the other side writes.
int deliver(int master, kal_stream s, const char* in, int n, unsigned char* out) {
    if (write(master, in, n) != n) return -1;
    int got = 0;
    while (got < n) {
        const kal_intptr r = kal_stream_read(s, out + got, n - got);
        if (r <= 0) break;
        got += static_cast<int>(r);
    }
    return got;
}
}

int main() {
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        kal::write(kal::err(), "no pseudo terminal; not observed\n", 33);
        return 0;
    }
    const int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    check(slave >= 0, "the pseudo terminal opens");
    const kal_stream s{ static_cast<kal_uintptr>(slave) };

    // As the kernel makes one: the return key is rewritten, so the position
    // reads clear.
    kal_uintptr m = ~kal_uintptr{0};
    check(kal_terminal_get_mode(s, &m) == kal_ok && (m & kal::terminal::pass_input.bits) == 0,
          "a terminal that rewrites the return key reads as altering input");

    // Raw, and nothing altered: the return key is a carriage return and Ctrl+J
    // is a line feed. This is the pair the report could not tell apart.
    check(kal_terminal_set_mode(s, kRaw | kal::terminal::pass_input.bits) == kal_ok, "raw input is requested");
    check(kal_terminal_get_mode(s, &m) == kal_ok && (m & kal::terminal::pass_input.bits) != 0,
          "the position reads as set once established");
    termios t{};
    tcgetattr(slave, &t);
    check((t.c_iflag & kAlters) == 0, "every mechanism that alters input is off");
    unsigned char b[4] = {};
    check(deliver(master, s, "\r\n", 2, b) == 2 && b[0] == 0x0d && b[1] == 0x0a,
          "the return key and Ctrl+J arrive as two different bytes");

    // Cleared again: what this kernel ordinarily does, and only that.
    check(kal_terminal_set_mode(s, kRaw) == kal_ok, "altered input is requested");
    tcgetattr(slave, &t);
    check((t.c_iflag & ICRNL) != 0 && (t.c_iflag & (ISTRIP | INLCR | IGNCR)) == 0,
          "clearing the position restores the rewriting of the return key alone");
    check(deliver(master, s, "\r", 1, b) == 1 && b[0] == 0x0a,
          "with the position clear the return key is rewritten as before");

    // A position already at the requested value is not written. ISTRIP set by
    // someone else keeps the position clear, and asking for it clear again
    // leaves ISTRIP and the rest exactly as they were.
    tcgetattr(slave, &t);
    t.c_iflag = (t.c_iflag & ~ICRNL) | ISTRIP;
    tcsetattr(slave, TCSANOW, &t);
    check(kal_terminal_get_mode(s, &m) == kal_ok && (m & kal::terminal::pass_input.bits) == 0,
          "one mechanism in effect is enough to read as altering input");
    check(kal_terminal_set_mode(s, kRaw) == kal_ok, "the same mode is requested again");
    tcgetattr(slave, &t);
    check((t.c_iflag & ISTRIP) != 0 && (t.c_iflag & ICRNL) == 0,
          "a position at its requested value is not written");

    // Independent of line assembly, with the consequence terminal.h states: a
    // carriage return no longer ends a line, the line feed does.
    check(kal_terminal_set_mode(s, kal::terminal::line_edit.bits | kal::terminal::pass_input.bits) == kal_ok,
          "line assembly with nothing altered is requested");
    unsigned char line[8] = {};
    check(deliver(master, s, "ab\r\n", 4, line) == 4 && line[2] == 0x0d && line[3] == 0x0a,
          "with line assembly on, the carriage return is part of the line");

    close(slave); close(master);
    if (failures == 0) kal::write(kal::out(), "openkal-macos: the terminal's alteration of input\n", 51);
    return failures == 0 ? 0 : 1;
}
