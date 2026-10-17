#include "log.h"

#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>

namespace coop {

namespace {

constexpr size_t kLogNameMax = 40;  // "coop_native_<pid>.log"

FILE* g_file = nullptr;
ULONGLONG g_startTicks = 0;
CRITICAL_SECTION g_lock;

}  // namespace

void LogOpen() {
    InitializeCriticalSection(&g_lock);
    g_startTicks = GetTickCount64();
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash == nullptr || (slash - path) + 1 + kLogNameMax >= MAX_PATH) return;
    // Per-process name: coop tests run two game instances from the same folder.
    swprintf_s(slash + 1, MAX_PATH - (slash + 1 - path), L"coop_native_%lu.log", GetCurrentProcessId());
    g_file = _wfsopen(path, L"w", _SH_DENYNO);  // readable while the game runs
}

void LogClose() {
    if (g_file != nullptr) {
        fclose(g_file);
        g_file = nullptr;
    }
    DeleteCriticalSection(&g_lock);
}

void Log(const char* format, ...) {
    if (g_file == nullptr) return;
    EnterCriticalSection(&g_lock);
    const double seconds = static_cast<double>(GetTickCount64() - g_startTicks) / 1000.0;
    fprintf(g_file, "[t=%.3fs] ", seconds);
    va_list args;
    va_start(args, format);
    vfprintf(g_file, format, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
    LeaveCriticalSection(&g_lock);
}

}  // namespace coop
