// Testing aid ("mutation twins"): COOP_DEBUG_MUTANT=<name>[,<name>...] puts a fixed bug back, so the fix's test can
// show it fails without the fix (tools/gametest/mutants.ps1). A test that cannot fail proves nothing.
//   skills     the host acts for a guest with its own skills (playtest 4, bug 7)
//   chests     a chest the guest opened goes away at the host too (bug 9)
//   savelists  no list fixing before a save (bug 10)
//   herbs      dead ids stay on global.herbs (the game stops in HerbProcess)
//   wake       waking leaves ids on the cull lists (the ProcessCulls crash)
//   forget     a destroyed instance stays on cull lists other than its last cell
//   modes      the coop mode screen shows every mode
//   storagelock  the chest open here is sent while open (the other side's lock on it is released)
//   vaultslots   each player's vaults have that player's own number of cells
#pragma once

#include <windows.h>

#include <string>

namespace coop {

inline bool Mutant(const char* name) {
    static const std::string list = [] {
        char text[256] = {};
        GetEnvironmentVariableA("COOP_DEBUG_MUTANT", text, sizeof(text));
        return "," + std::string(text) + ",";
    }();
    return list.find("," + std::string(name) + ",") != std::string::npos;
}

}  // namespace coop
