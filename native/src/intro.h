#pragma once

namespace coop {

// Skips the Humble Bundle and HopFrog splash screens at startup when coop_skip_intro.txt is next to Forager.exe
// (or COOP_SKIP_INTRO is set). Requires MH_Initialize(); hooks are enabled by the caller.
bool IntroInstall();

}  // namespace coop
