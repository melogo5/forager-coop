// One skill tree for everyone (docs/coop/SKILLS.md): a skill learned by any player is learned by all - shown so in
// every tree, and its effects everywhere. The tree is the host's global.skillUnlocked (the world save keeps it; a
// joining guest gets it with the world). Whoever buys a skill sends the tree, with the ItemEdit/StructureEdit calls
// the purchase made (objSkill Step_0: items, recipes and structures it unlocks); the others merge the tree and make
// the same calls. The host passes a guest's tree on to the other guests.
// Skill points stay each player's own: a point per shared level-up, minus what that player bought. The host keeps a
// guest's count next to its character (saveDataN/coop_guests/<name>.skills) and gives it back on joining.
#pragma once

#include <string>

#include "net.h"
#include "yyc.h"

namespace coop::skills {

// Creates the hooks (enabled by the caller): ItemEdit and StructureEdit, recorded while a skill is bought.
bool InstallHooks();

// From the objSkill Step_0 hook: a skill bought now is bought in a coop session; record what it does.
void RecordPurchases(bool on);

// Host, game thread: guest `peer` has introduced itself as `name`; sends it what is stored for it.
void HostOnGuest(const std::string& name, net::PeerId peer, yyc::CInstance* localPlayer);

// Game thread, every local player step while connected (both sides).
void Tick(yyc::CInstance* localPlayer, bool host);

// Game thread: handles a skills message; false if not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Forgets the session (the guest re-applies on joining again).
void Reset();

}  // namespace coop::skills
