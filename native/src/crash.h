#pragma once

namespace coop {

// Logs the GML call stack on access violations (the crash itself is left to the game), and the text of every GML
// error the game reports (its error.log) while it runs.
void CrashLoggerInstall();

}  // namespace coop
