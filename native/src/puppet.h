// The other players' characters, each shown as a real objPlayer instance whose own logic is switched off.
// Enemies target them like any parAlly; each is drawn by objPlayer's Draw_0 with PlayerFind() pointing at it.
// Puppets are keyed by the player they show (net::PeerId: 0 is the host).
#pragma once

#include <vector>

#include "net.h"
#include "yyc.h"

namespace coop::puppet {

struct State {
    float x, y;
    float imageIndex, xscale, yscale, depth;
    int sprite;
    int tool;  // item in hand, -4 = none
};

using LocalStepFn = void (*)(yyc::CInstance* localPlayer);

// Creates the hooks (enabled by the caller). `onLocalStep` runs after the local player's Step_0,
// `beforeLocalStep1` before its Step_1 (the begin step that turns hp < 1 into a game over).
bool InstallHooks(LocalStepFn onLocalStep, LocalStepFn beforeLocalStep1);

// Host with guests: an enemy asking for "the player" (PlayerFind) gets the nearest of the local player and the
// puppets, so it chases and attacks whoever is closest.
void SetEnemiesSeeAll(bool on);

// Guest: familiars (lantern, halo, droids) come with the host's world and are the host's: they follow the
// host's puppet.
void SetFamiliarsFollowHost(bool on);

// Player `player`'s puppet, or nullptr when there is none.
yyc::CInstance* Instance(net::PeerId player);
// Every puppet there is, in the order they were created.
std::vector<yyc::CInstance*> Instances();
bool IsPuppet(const yyc::CInstance* instance);

// While acting, PlayerFind() returns player `player`'s puppet: game code run in between treats that player as
// "the player". Nests (the outermost player stays); every Begin needs an End. Without a puppet for `player`
// nothing changes.
void BeginActing(net::PeerId player);
void EndActing();
bool IsActing();
// The player being acted as (only meaningful while IsActing()).
net::PeerId ActingPlayer();
// Lets something run as the local player in the middle of acting; ResumeActing takes what it returned.
struct Acting {
    int depth;
    int actorId;
};
Acting PauseActing();
void ResumeActing(const Acting& acting);

// Moves player `player`'s puppet toward `target`, creating it next to the local player if needed.
void Update(yyc::CInstance* localPlayer, net::PeerId player, const State& target);
void Destroy(yyc::CInstance* localPlayer, net::PeerId player);
void DestroyAll(yyc::CInstance* localPlayer);

}  // namespace coop::puppet
