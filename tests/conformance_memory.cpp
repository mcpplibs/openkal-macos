// Conformance: openkal.memory.
import openkal.memory;
import openkal.stream;
import mcpplibs.mtest;

using namespace mcpplibs::mtest;

UTest conformance_memory_test{"conformance_memory", [] {
    void* p = kal::alloc(128, 16);
    if (p == nullptr) { fail("p == nullptr"); return; }
    if ((reinterpret_cast<kal_uintptr>(p) % 16) != 0) { fail("(reinterpret_cast<kal_uintptr>(p) % 16) != 0"); return; }
    auto* b = static_cast<unsigned char*>(p);
    for (int i = 0; i < 128; ++i) b[i] = static_cast<unsigned char>(i);
    for (int i = 0; i < 128; ++i) if (b[i] != static_cast<unsigned char>(i)) { fail("each byte written is read back"); return; }
    kal::free(p, 128, 16);

    // A zero-sized request yields a null pointer rather than a region that
    // cannot be used.
    if (kal::alloc(0, 8) != nullptr) { fail("kal::alloc(0, 8) != nullptr"); return; }
    const char ok[] = "openkal-macos: memory conformance\n";
    kal::write(kal::out(), ok, sizeof(ok) - 1);
}};
