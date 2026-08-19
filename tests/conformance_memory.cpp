// Conformance: openkal.memory.
import openkal.memory;
import openkal.stream;

int main() {
    void* p = kal::alloc(128, 16);
    if (p == nullptr) return 1;
    if ((reinterpret_cast<kal_uintptr>(p) % 16) != 0) return 1;

    auto* b = static_cast<unsigned char*>(p);
    for (int i = 0; i < 128; ++i) b[i] = static_cast<unsigned char>(i);
    for (int i = 0; i < 128; ++i) if (b[i] != static_cast<unsigned char>(i)) return 1;
    kal::free(p, 128, 16);

    // A zero-sized request yields a null pointer rather than a region that
    // cannot be used.
    if (kal::alloc(0, 8) != nullptr) return 1;

    const char ok[] = "openkal-linux: memory conformance\n";
    kal::write(kal::out(), ok, sizeof(ok) - 1);
    return 0;
}
