// Logs the GML call stack when the game is about to crash, so a fault inside the runtime can be traced to
// the script that caused it. Does not handle the exception: the game crashes as it would have.
// A GML error ("UH OH!" window) is no exception: the game's catch_error extension writes its text and GML stack to
// %LOCALAPPDATA%\Forager\error.log, overwritten by the next one; a watcher copies each into our log.
#include "crash.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <string>

#include "log.h"
#include "yyc.h"

namespace coop {

namespace {

constexpr int kMaxFrames = 12;
constexpr DWORD kErrorLogPollMs = 500;

std::wstring ErrorLogPath() {
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return length > 0 && length < MAX_PATH ? std::wstring(local) + L"\\Forager\\error.log" : std::wstring();
}

ULONGLONG LastWrite(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

void LogErrorFile(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::istringstream lines(text.str());
    std::string line;
    Log("crash: the game reported an error (error.log):");
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.find_first_not_of("#-_") == std::string::npos) continue;  // its separator lines
        Log("crash:   %s", line.c_str());
    }
}

DWORD WINAPI WatchErrorLog(LPVOID) {
    const std::wstring path = ErrorLogPath();
    if (path.empty()) return 0;
    ULONGLONG seen = LastWrite(path);  // what is there now is from an earlier run
    for (;;) {
        Sleep(kErrorLogPollMs);
        const ULONGLONG written = LastWrite(path);
        if (written == 0 || written == seen) continue;
        seen = written;
        Sleep(kErrorLogPollMs);  // let the game finish writing it
        LogErrorFile(path);
    }
}

using yyc::StackFrame;

void LogGmlStack() {
    __try {
        const StackFrame* frame = yyc::TopFrame();
        for (int i = 0; frame != nullptr && i < kMaxFrames; ++i, frame = frame->prev) {
            Log("crash:   #%d %s:%d", i, frame->name != nullptr ? frame->name : "?", frame->line);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("crash:   (GML stack unreadable)");
    }
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // A fault while walking the GML stack comes back here first (vectored handlers run before __except).
    static thread_local bool busy = false;
    if (busy) return EXCEPTION_CONTINUE_SEARCH;
    busy = true;
    const auto address = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    Log("crash: exception 0x%08lX at VA %08X; GML stack:", code, static_cast<unsigned>(address - yyc::g_imageDelta));
    LogGmlStack();
    busy = false;
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void CrashLoggerInstall() {
    AddVectoredExceptionHandler(1, OnException);
    if (const HANDLE thread = CreateThread(nullptr, 0, WatchErrorLog, nullptr, 0, nullptr)) CloseHandle(thread);
}

}  // namespace coop
