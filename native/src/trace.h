#pragma once

namespace coop {

// Hooks the functions listed in coop_trace.txt (if present) for call-chain logging.
// Requires MH_Initialize(); hooks are enabled by the caller. Returns the number of functions hooked.
int TraceInstall();

}  // namespace coop
