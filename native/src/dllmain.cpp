// version.dll proxy: forwards the real exports to the system DLL, then installs the coop hooks.
#include <windows.h>

#include "MinHook.h"
#include "crash.h"
#include "intro.h"
#include "log.h"
#include "menu.h"
#include "session.h"
#include "trace.h"
#include "yyc.h"

#define PROXY_EXPORTS(X)            \
    X(GetFileVersionInfoA)          \
    X(GetFileVersionInfoByHandle)   \
    X(GetFileVersionInfoExA)        \
    X(GetFileVersionInfoExW)        \
    X(GetFileVersionInfoSizeA)      \
    X(GetFileVersionInfoSizeExA)    \
    X(GetFileVersionInfoSizeExW)    \
    X(GetFileVersionInfoSizeW)      \
    X(GetFileVersionInfoW)          \
    X(VerFindFileA)                 \
    X(VerFindFileW)                 \
    X(VerInstallFileA)              \
    X(VerInstallFileW)              \
    X(VerLanguageNameA)             \
    X(VerLanguageNameW)             \
    X(VerQueryValueA)               \
    X(VerQueryValueW)

#define DECLARE_REAL(name) static FARPROC g_real_##name = nullptr;
PROXY_EXPORTS(DECLARE_REAL)

// Stubs jump straight to the real function so arguments and return values pass through untouched.
#define DEFINE_STUB(name) \
    extern "C" __declspec(naked) void Proxy_##name() { __asm { jmp dword ptr [g_real_##name] } }
PROXY_EXPORTS(DEFINE_STUB)

namespace {

HMODULE g_realVersion = nullptr;

bool LoadRealVersion() {
    wchar_t path[MAX_PATH] = {};
    const UINT len = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for this 32-bit process
    if (len == 0 || len + 13 >= MAX_PATH) return false;
    wcscat_s(path, L"\\version.dll");
    g_realVersion = LoadLibraryW(path);
    if (g_realVersion == nullptr) return false;
#define RESOLVE(name) g_real_##name = GetProcAddress(g_realVersion, #name);
    PROXY_EXPORTS(RESOLVE)
    return true;
}

// Runs on a worker thread: MinHook suspends threads and allocates, which must not happen under the loader lock.
DWORD WINAPI InstallCoop(LPVOID) {
    coop::LogOpen();
    yyc::InitImageDelta();
    coop::Log("coop_native loaded, image delta 0x%08X", static_cast<unsigned>(yyc::g_imageDelta));
    if (!yyc::IsSupportedBuild()) {
        coop::Log("unsupported Forager.exe build (expected PE timestamp 0x%08X); hooks not installed",
                  yyc::kExpectedTimestamp);
        return 0;
    }
    coop::CrashLoggerInstall();
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        coop::Log("MH_Initialize failed: %s", MH_StatusToString(status));
        return 0;
    }
    const bool sessionOk = coop::SessionInstall();
    const bool menuOk = coop::MenuInstall();
    coop::IntroInstall();
    coop::TraceInstall();
    const MH_STATUS enabled = MH_EnableHook(MH_ALL_HOOKS);
    coop::Log("hooks %s (session %s, menu %s)", enabled == MH_OK ? "enabled" : MH_StatusToString(enabled),
              sessionOk ? "ok" : "incomplete", menuOk ? "ok" : "incomplete");
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (!LoadRealVersion()) return FALSE;
        const HANDLE thread = CreateThread(nullptr, 0, InstallCoop, nullptr, 0, nullptr);
        if (thread != nullptr) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // Only on FreeLibrary; at process exit other threads are gone and unhooking is pointless.
        MH_Uninitialize();
        coop::LogClose();
    }
    return TRUE;
}
