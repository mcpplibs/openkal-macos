#include <stdint.h>
import openkal.env;

namespace {

int          g_argc = 0;
char**       g_argv = nullptr;
char**       g_envp = nullptr;

// The environment supplies these to a constructor. The alternative, reading the
// pseudo-file the kernel provides, would require a file system before the
// program has one and would report the arguments as they were at inception
// rather than as the program received them.
[[gnu::constructor]] void capture(int argc, char** argv, char** envp) {
    g_argc = argc; g_argv = argv; g_envp = envp;
}

kal_uintptr length(const char* s) {
    kal_uintptr n = 0; while (s && s[n] != '\0') ++n; return n;
}

}  // namespace

extern "C" {

kal_uintptr kal_env_arg_count(void) { return static_cast<kal_uintptr>(g_argc); }

const char* kal_env_arg(kal_uintptr index, kal_uintptr* len) {
    if (index >= static_cast<kal_uintptr>(g_argc)) { if (len) *len = 0; return nullptr; }
    const char* s = g_argv[index];
    if (len) *len = length(s);
    return s;
}

const char* kal_env_var(const char* name, kal_uintptr name_len, kal_uintptr* value_len) {
    for (char** e = g_envp; e && *e; ++e) {
        const char* entry = *e;
        kal_uintptr i = 0;
        while (i < name_len && entry[i] != '\0' && entry[i] == name[i]) ++i;
        if (i == name_len && entry[i] == '=') {
            const char* v = entry + name_len + 1;
            if (value_len) *value_len = length(v);
            return v;
        }
    }
    if (value_len) *value_len = 0;
    return nullptr;
}

kal_uintptr kal_env_var_count(void) {
    kal_uintptr n = 0; for (char** e = g_envp; e && *e; ++e) ++n; return n;
}

const char* kal_env_var_at(kal_uintptr index, kal_uintptr* name_len,
                           const char** value, kal_uintptr* value_len) {
    kal_uintptr n = 0;
    for (char** e = g_envp; e && *e; ++e, ++n) {
        if (n != index) continue;
        const char* entry = *e;
        kal_uintptr i = 0; while (entry[i] != '\0' && entry[i] != '=') ++i;
        if (name_len) *name_len = i;
        const char* v = entry[i] == '=' ? entry + i + 1 : entry + i;
        if (value)     *value = v;
        if (value_len) *value_len = length(v);
        return entry;
    }
    return nullptr;
}

}
