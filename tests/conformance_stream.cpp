// Conformance: openkal.stream.
//
// The suite is two-sided. It verifies that what the implementation provides
// behaves as specified, and it verifies that what the implementation does not
// provide is genuinely absent. The second half is the one that is usually
// omitted, and it is the half that keeps a capability claim from diverging from
// the code behind it.
import openkal.stream;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        const char pre[] = "FAIL: ";
        kal::write(kal::err(), pre, sizeof(pre) - 1);
        kal_uintptr n = 0; while (what[n] != '\0') ++n;
        kal::write(kal::err(), what, n);
        kal::write(kal::err(), "\n", 1);
    }
}

}  // namespace

int main() {
    // The standard streams are distinct and usable.
    check(kal::in().h  != kal::out().h, "stdin and stdout are distinct");
    check(kal::out().h != kal::err().h, "stdout and stderr are distinct");

    // A write transfers the whole buffer or reports why it could not. The
    // specification excludes a successful partial transfer, so a conforming
    // result reports either the full count or a non-zero error.
    const char msg[] = "openkal-linux: conformance\n";
    // ⭐ ONE SIGNED WORD: the count, or the negated condition when no byte
    // moved. A caller never inspects two things to learn one thing.
    const kal_intptr r = kal::write(kal::out(), msg, sizeof(msg) - 1);
    check(r >= 0, "write reports success");
    check(r == static_cast<kal_intptr>(sizeof(msg) - 1),
          "write transfers the whole buffer");

    // An invalid handle is reported rather than accepted.
    const kal_intptr bad = kal::write(kal::stream{ 0x7fffffff }, msg, 1);
    check(bad < 0, "an invalid handle is rejected");

    // Flushing an unbuffered stream succeeds.
    check(kal::flush(kal::out()) == kal_ok, "flush succeeds");

    // The negative half of the claim is verified statically, outside this
    // program, by comparing the exported names of the implementation against
    // the specification's list. Version 0.2 defines no optional operation, so
    // there is nothing here for a compile-time assertion to check.

    return failures == 0 ? 0 : 1;
}
