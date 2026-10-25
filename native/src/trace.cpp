// Dev tracer: hooks every function listed in coop_trace.txt (next to Forager.exe, lines "name<TAB>hexVA",
// same format as decompiled/code/index.tsv) and logs each distinct GML call chain that reaches it.
// Signature-agnostic: the detour only records the call and jumps on to the original.
#include "trace.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "log.h"
#include "yyc.h"

namespace coop {

namespace {

constexpr int kMaxTraced = 64;
constexpr int kMaxChainDepth = 6;
constexpr int kMaxLoggedChains = 4096;
constexpr int kNameLen = 96;
constexpr wchar_t kTraceFile[] = L"coop_trace.txt";

// YYC pushes one of these per GML function: {previous frame, function name, current line}.
using yyc::StackFrame;

char g_names[kMaxTraced][kNameLen] = {};
unsigned g_calls[kMaxTraced] = {};
uint32_t g_seenChains[kMaxLoggedChains] = {};
int g_seenCount = 0;
int g_tracedCount = 0;

uint32_t Fnv1a(uint32_t hash, const char* text) {
    for (; *text != '\0'; ++text) hash = (hash ^ static_cast<uint8_t>(*text)) * 16777619u;
    return hash;
}

bool MarkSeen(uint32_t hash) {
    for (int i = 0; i < g_seenCount; ++i) {
        if (g_seenChains[i] == hash) return false;
    }
    if (g_seenCount < kMaxLoggedChains) g_seenChains[g_seenCount++] = hash;
    return true;
}

void BuildChain(char* out, size_t size) {
    out[0] = '\0';
    const StackFrame* frame = yyc::TopFrame();
    for (int depth = 0; frame != nullptr && depth < kMaxChainDepth; ++depth, frame = frame->prev) {
        const size_t used = strlen(out);
        if (frame->name == nullptr || used + 8 >= size) break;
        snprintf(out + used, size - used, " <- %s:%d", frame->name, frame->line);
    }
}

}  // namespace

// Referenced from the naked stubs, so it needs external C linkage.
extern "C" void* g_traceOriginal[kMaxTraced] = {};

extern "C" void __cdecl TraceEnter(int slot) {
    ++g_calls[slot];
    __try {
        char chain[768];
        BuildChain(chain, sizeof(chain));
        uint32_t hash = Fnv1a(2166136261u, g_names[slot]);
        hash = Fnv1a(hash, chain);
        if (MarkSeen(hash)) Log("TRACE %s (call #%u)%s", g_names[slot], g_calls[slot], chain);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("TRACE %s: stack walk failed", g_names[slot]);
    }
}

// One naked stub per slot: keep every register and flag, record the call, tail-jump to the original.
#define TRACE_STUB(n)                                        \
    extern "C" __declspec(naked) void TraceStub##n() {       \
        __asm { pushad }                                     \
        __asm { pushfd }                                     \
        __asm { push n }                                     \
        __asm { call TraceEnter }                            \
        __asm { add esp, 4 }                                 \
        __asm { popfd }                                      \
        __asm { popad }                                      \
        __asm { jmp dword ptr [g_traceOriginal + n * 4] }    \
    }
TRACE_STUB(0) TRACE_STUB(1) TRACE_STUB(2) TRACE_STUB(3) TRACE_STUB(4) TRACE_STUB(5) TRACE_STUB(6) TRACE_STUB(7)
TRACE_STUB(8) TRACE_STUB(9) TRACE_STUB(10) TRACE_STUB(11) TRACE_STUB(12) TRACE_STUB(13) TRACE_STUB(14) TRACE_STUB(15)
TRACE_STUB(16) TRACE_STUB(17) TRACE_STUB(18) TRACE_STUB(19) TRACE_STUB(20) TRACE_STUB(21) TRACE_STUB(22) TRACE_STUB(23)
TRACE_STUB(24) TRACE_STUB(25) TRACE_STUB(26) TRACE_STUB(27) TRACE_STUB(28) TRACE_STUB(29) TRACE_STUB(30) TRACE_STUB(31)
TRACE_STUB(32) TRACE_STUB(33) TRACE_STUB(34) TRACE_STUB(35) TRACE_STUB(36) TRACE_STUB(37) TRACE_STUB(38) TRACE_STUB(39)
TRACE_STUB(40) TRACE_STUB(41) TRACE_STUB(42) TRACE_STUB(43) TRACE_STUB(44) TRACE_STUB(45) TRACE_STUB(46) TRACE_STUB(47)
TRACE_STUB(48) TRACE_STUB(49) TRACE_STUB(50) TRACE_STUB(51) TRACE_STUB(52) TRACE_STUB(53) TRACE_STUB(54) TRACE_STUB(55)
TRACE_STUB(56) TRACE_STUB(57) TRACE_STUB(58) TRACE_STUB(59) TRACE_STUB(60) TRACE_STUB(61) TRACE_STUB(62) TRACE_STUB(63)

namespace {

#define STUB_PTR(n) reinterpret_cast<void*>(&TraceStub##n)
void* const kStubs[kMaxTraced] = {
    STUB_PTR(0), STUB_PTR(1), STUB_PTR(2), STUB_PTR(3), STUB_PTR(4), STUB_PTR(5), STUB_PTR(6), STUB_PTR(7),
    STUB_PTR(8), STUB_PTR(9), STUB_PTR(10), STUB_PTR(11), STUB_PTR(12), STUB_PTR(13), STUB_PTR(14), STUB_PTR(15),
    STUB_PTR(16), STUB_PTR(17), STUB_PTR(18), STUB_PTR(19), STUB_PTR(20), STUB_PTR(21), STUB_PTR(22), STUB_PTR(23),
    STUB_PTR(24), STUB_PTR(25), STUB_PTR(26), STUB_PTR(27), STUB_PTR(28), STUB_PTR(29), STUB_PTR(30), STUB_PTR(31),
    STUB_PTR(32), STUB_PTR(33), STUB_PTR(34), STUB_PTR(35), STUB_PTR(36), STUB_PTR(37), STUB_PTR(38), STUB_PTR(39),
    STUB_PTR(40), STUB_PTR(41), STUB_PTR(42), STUB_PTR(43), STUB_PTR(44), STUB_PTR(45), STUB_PTR(46), STUB_PTR(47),
    STUB_PTR(48), STUB_PTR(49), STUB_PTR(50), STUB_PTR(51), STUB_PTR(52), STUB_PTR(53), STUB_PTR(54), STUB_PTR(55),
    STUB_PTR(56), STUB_PTR(57), STUB_PTR(58), STUB_PTR(59), STUB_PTR(60), STUB_PTR(61), STUB_PTR(62), STUB_PTR(63)};

FILE* OpenTraceList() {
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (len == 0 || len >= MAX_PATH || slash == nullptr) return nullptr;
    if ((slash - path) + 1 + wcslen(kTraceFile) >= MAX_PATH) return nullptr;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), kTraceFile);
    FILE* file = nullptr;
    return _wfopen_s(&file, path, L"r") == 0 ? file : nullptr;
}

void AddTrace(const char* name, uint32_t va) {
    if (g_tracedCount >= kMaxTraced) {
        Log("trace: more than %d entries, ignoring %s", kMaxTraced, name);
        return;
    }
    const int slot = g_tracedCount;
    void* target = yyc::At<void*>(va);
    const MH_STATUS status = MH_CreateHook(target, kStubs[slot], &g_traceOriginal[slot]);
    if (status != MH_OK) {
        Log("trace: hook %s @ %p failed: %s", name, target, MH_StatusToString(status));
        return;
    }
    strncpy_s(g_names[slot], name, _TRUNCATE);
    ++g_tracedCount;
}

}  // namespace

int TraceInstall() {
    FILE* list = OpenTraceList();
    if (list == nullptr) return 0;
    char line[256];
    while (fgets(line, sizeof(line), list) != nullptr) {
        char name[kNameLen] = {};
        unsigned va = 0;
        if (line[0] == '#' || sscanf_s(line, "%95s %x", name, static_cast<unsigned>(sizeof(name)), &va) != 2) {
            continue;
        }
        AddTrace(name, va);
    }
    fclose(list);
    Log("trace: %d functions hooked", g_tracedCount);
    return g_tracedCount;
}

}  // namespace coop
