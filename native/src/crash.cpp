// Logs the GML call stack when the game is about to crash, so a fault inside the runtime can be traced to
// the script that caused it. Does not handle the exception: the game crashes as it would have.
#include "crash.h"

#include <windows.h>

#include "log.h"
#include "yyc.h"

namespace coop {

namespace {

constexpr int kMaxFrames = 12;

struct StackFrame {
    StackFrame* prev;
    const char* name;
    int line;
};

void LogGmlStack() {
    __try {
        const StackFrame* frame = *yyc::At<StackFrame**>(yyc::va::StackTraceTop);
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
    const auto address = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    Log("crash: exception 0x%08lX at VA %08X; GML stack:", code, static_cast<unsigned>(address - yyc::g_imageDelta));
    LogGmlStack();
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void CrashLoggerInstall() { AddVectoredExceptionHandler(1, OnException); }

}  // namespace coop
