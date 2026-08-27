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

// Positions within lflag. This kernel's values, which are not the other's.
constexpr okm_ulong t_echo   = 0x00000008u;
constexpr okm_ulong t_icanon = 0x00000100u;

kal_uintptr mode_of(const oktermios& t) {
    kal_uintptr m = 0;
    if ((t.lflag & t_icanon) != 0) m |= KAL_TERM_LINE_EDIT;
    if ((t.lflag & t_echo)   != 0) m |= KAL_TERM_ECHO;
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

    if ((mode & KAL_TERM_LINE_EDIT) != 0) t.lflag |=  t_icanon;
    else                                  t.lflag &= ~t_icanon;
    if ((mode & KAL_TERM_ECHO) != 0)      t.lflag |=  t_echo;
    else                                  t.lflag &= ~t_echo;

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
