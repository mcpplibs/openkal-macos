#include "sys.h"
#include <openkal/terminal.h>

// openkal.terminal upon this kernel's terminal ioctls.
//
// THE REQUESTS ENCODE THE SIZE OF THE STRUCTURE THEY CARRY, which is why they
// are written out here rather than named from a header: the number is a property
// of this kernel's layout, and a header that stated it would belong to a C
// library this implementation does not have. kal_stream_props already spells
// TIOCGETA the same way and for the same reason.

namespace {

// This kernel's terminal settings, in this kernel's layout. Four flag words of a
// machine word each, twenty control characters, and two speeds. The size is
// seventy-two bytes, which is the number the requests below carry.
struct oktermios {
    okm_ulong     iflag;
    okm_ulong     oflag;
    okm_ulong     cflag;
    okm_ulong     lflag;
    unsigned char cc[20];
    okm_ulong     ispeed;
    okm_ulong     ospeed;
};
static_assert(sizeof(oktermios) == 72,
              "the request numbers below carry this size");

struct okwinsize {
    unsigned short row;
    unsigned short col;
    unsigned short xpixel;
    unsigned short ypixel;
};
static_assert(sizeof(okwinsize) == 8, "the request number below carries this size");

// _IOR('t', 19, struct termios) and _IOW('t', 20, struct termios), and
// _IOR('t', 104, struct winsize).
constexpr okm_long tiocgeta   = 0x40000000L | (72L << 16) | ('t' << 8) | 19;
constexpr okm_long tiocseta   = 0x80000000L | (72L << 16) | ('t' << 8) | 20;
constexpr okm_long tiocgwinsz = 0x40000000L | (8L  << 16) | ('t' << 8) | 104;

// Positions within lflag and iflag, and the two entries of cc that decide how
// long a read waits. This kernel's values, which are not the other's.
constexpr okm_ulong t_echo   = 0x00000008u;
constexpr okm_ulong t_isig   = 0x00000080u;
constexpr okm_ulong t_icanon = 0x00000100u;
constexpr okm_ulong t_iexten = 0x00000400u;
constexpr okm_ulong t_ixon   = 0x00000200u;   // iflag
constexpr unsigned  v_min = 16, v_time = 17;

// KAL_TERM_PASS_CONTROL IS READ FROM THREE FLAGS AND NOT FROM ISIG. The
// position states that the environment reserves NO keystroke, so it is set only
// where every mechanism by which this kernel reserves one is off: ISIG for the
// interrupt and its neighbours, IXON for the pair that stops and starts output,
// IEXTEN for the one that takes the next keystroke literally. A terminal upon
// which some of them had been released reads as clear and is restored to the
// set this kernel ordinarily reserves, which is the cost the specification
// records beside the position.
kal_uintptr mode_of(const oktermios& t) {
    kal_uintptr m = 0;
    if ((t.lflag & t_icanon) != 0) m |= KAL_TERM_LINE_EDIT;
    if ((t.lflag & t_echo)   != 0) m |= KAL_TERM_ECHO;
    if ((t.lflag & (t_isig | t_iexten)) == 0 &&
        (t.iflag & t_ixon) == 0)   m |= KAL_TERM_PASS_CONTROL;
    return m;
}

int get_termios(kal_stream s, oktermios& out) {
    const okm_long r = okm::sys(okm::nr_ioctl, static_cast<okm_long>(s.h),
                                tiocgeta, reinterpret_cast<okm_long>(&out));
    // A stream that is not a terminal is reported as unsupported rather than
    // having this kernel's own classification passed through.
    if (okm::failed(r)) return kal_err_not_supported;
    return kal_ok;
}

}  // namespace

extern "C" {

int kal_terminal_get_mode(kal_stream s, kal_uintptr* mode) {
    if (mode == nullptr) return kal_err_invalid;
    oktermios t{};
    const int rc = get_termios(s, t);
    if (rc != kal_ok) return rc;
    *mode = mode_of(t);
    return kal_ok;
}

int kal_terminal_set_mode(kal_stream s, kal_uintptr mode) {
    // READ, MODIFY, WRITE. The structure carries a baud rate and twenty control
    // characters that this interface does not name; composing one from the mode
    // word alone would discard them, and the terminal a program returned to
    // would not be the one it found.
    oktermios t{};
    const int rc = get_termios(s, t);
    if (rc != kal_ok) return rc;
    const kal_uintptr in_effect = mode_of(t);

    if ((mode & KAL_TERM_LINE_EDIT) != 0) t.lflag |=  t_icanon;
    else                                  t.lflag &= ~t_icanon;
    if ((mode & KAL_TERM_ECHO) != 0)      t.lflag |=  t_echo;
    else                                  t.lflag &= ~t_echo;

    // A POSITION WHOSE REQUESTED VALUE IS THE ONE IN EFFECT IS NOT WRITTEN.
    // This position stands for three of the kernel's flags, so establishing it
    // again would settle two mechanisms the caller never asked about: a user
    // who had released the keystroke that stops output keeps it released while
    // a program turns the echo off and back on.
    if (((mode ^ in_effect) & KAL_TERM_PASS_CONTROL) != 0) {
        if ((mode & KAL_TERM_PASS_CONTROL) != 0) {
            t.lflag &= ~(t_isig | t_iexten);
            t.iflag &= ~t_ixon;
        } else {
            t.lflag |=  (t_isig | t_iexten);
            t.iflag |=   t_ixon;
        }
    }

    // AND A MODE IS NOT A WAY TO END THE INPUT. With line assembly off, how
    // long a read waits is decided by VMIN and VTIME rather than by a newline,
    // and a terminal left at VMIN=0 by whatever ran before makes
    // `kal_stream_read' report zero --- which clause 7.4 says denotes the end
    // of the input. A caller that wants a read which gives up asks
    // `kal_timeout_read' for one.
    if ((mode & KAL_TERM_LINE_EDIT) == 0) {
        t.cc[v_min]  = 1;
        t.cc[v_time] = 0;
    }

    // A position this implementation does not distinguish is ignored rather than
    // refused, which clause 6.2 requires: a program compiled against a later
    // revision sets a position this build has never heard of.
    const okm_long w = okm::sys(okm::nr_ioctl, static_cast<okm_long>(s.h),
                                tiocseta, reinterpret_cast<okm_long>(&t));
    if (okm::failed(w)) return kal_err_not_supported;
    return kal_ok;
}

int kal_terminal_size(kal_stream s, kal_uintptr* cols, kal_uintptr* rows) {
    if (cols == nullptr || rows == nullptr) return kal_err_invalid;
    okwinsize w{};
    const okm_long r = okm::sys(okm::nr_ioctl, static_cast<okm_long>(s.h),
                                tiocgwinsz, reinterpret_cast<okm_long>(&w));
    // Both outputs are left untouched, which the interface requires: a serial
    // line answers TIOCGETA and not this, so a caller must be able to tell the
    // two conditions apart.
    if (okm::failed(r)) return kal_err_not_supported;
    *cols = static_cast<kal_uintptr>(w.col);
    *rows = static_cast<kal_uintptr>(w.row);
    return kal_ok;
}

kal_uintptr kal_terminal_props(kal_stream s) {
    kal_uintptr p = 0;

    oktermios t{};
    if (get_termios(s, t) == kal_ok) p |= KAL_TERM_PROP_MODE;

    // The size is asked for rather than derived from the first. Deriving it
    // would make the word claim a facility the very next call refuses.
    okwinsize w{};
    const okm_long r = okm::sys(okm::nr_ioctl, static_cast<okm_long>(s.h),
                                tiocgwinsz, reinterpret_cast<okm_long>(&w));
    if (!okm::failed(r)) p |= KAL_TERM_PROP_SIZE;

    return p;
}

}  // extern "C"
