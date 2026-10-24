// Testing aid ("mutation twins"): COOP_DEBUG_MUTANT=<name>[,<name>...] puts a fixed bug back, so the fix's test can
// show it fails without the fix (tools/gametest/mutants.ps1). A test that cannot fail proves nothing.
//   skills     a tree from another player is ignored (each keeps its own skill tree)
//   chests     chests are the game's own: one key opens a key chest for its opener only
//   savelists  no list fixing before a save (bug 10)
//   savefix    a broken zone file is loaded as it is
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
