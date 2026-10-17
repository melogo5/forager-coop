// Plain-text log next to Forager.exe: coop_native_<pid>.log, lines "[t=12.345s] message".
#pragma once

namespace coop {

void LogOpen();
void LogClose();
void Log(const char* format, ...);

}  // namespace coop
