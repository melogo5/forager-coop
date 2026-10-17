#pragma once

namespace coop {

// Logs the GML call stack on access violations (the crash itself is left to the game).
void CrashLoggerInstall();

}  // namespace coop
