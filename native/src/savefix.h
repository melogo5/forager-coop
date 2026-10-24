// A stopgap, not a fix: in a session SaveGame sometimes leaves out instances that are asleep on no sleep list
// (docs/coop/PLAYTEST5.md) - the zone file then has empty records or a list shorter than its count, and the world
// never loads again. Before the game loads a world (LoadGame), its zone files are checked and repaired as
// tools/gametest/cinder_fix.py does: empty records go, a short list ends where the zone's next fields begin. What the
// save left out stays lost; the broken file is kept next to it (<name>.broken).
#pragma once

namespace coop::savefix {

// Creates the hook (enabled by the caller).
bool InstallHooks();

}  // namespace coop::savefix
