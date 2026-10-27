// The other players' characters, each shown as a real objPlayer instance whose own logic is switched off.
// Enemies target them like any parAlly; each is drawn by objPlayer's Draw_0 with PlayerFind() pointing at it.
// Puppets are keyed by the player they show (net::PeerId: 0 is the host).
#pragma once

#include <vector>

#include "look.h"
#include "net.h"
#include "yyc.h"

namespace coop::puppet {

// How the player's tool and body move on a hit: objPlayer's own variables, which its Step_0 sets at a hit and its
// Step_2 eases back. A puppet runs neither, so they come from its player. On the wire as is (PlayerStateMsg).
#pragma pack(push, 1)
struct Swing {
    float toolSwing;         // the tool's turn, degrees: -120 or 140 at a hit, back to 0
    float squashX, squashY;  // objPlayer.xscale/yscale: squashed at a hit, back to 1
    float facing;            // which side the tool is held on
    float angle;
};
#pragma pack(pop)

// The local player's swing now.
Swing ReadSwing(yyc::CInstance* player);

struct State {
    float x, y;
    float imageIndex, xscale, yscale, depth;
    int sprite;
    int tool;  // item in hand, -4 = none
    float hp, hpMax;
    char name[16];  // shown over the puppet, with its hearts; not 0-terminated when all 16 are used
    look::Look look;  // what the player wears
    Swing swing;
};

using LocalStepFn = void (*)(yyc::CInstance* localPlayer);

// Creates the hooks (enabled by the caller). `onLocalStep` runs after the local player's Step_0,
// `beforeLocalStep1` before its Step_1 (the begin step that turns hp < 1 into a game over).
bool InstallHooks(LocalStepFn onLocalStep, LocalStepFn beforeLocalStep1);

// Host with guests: an enemy asking for "the player" (PlayerFind) gets the nearest of the local player and the
// puppets, so it chases and attacks whoever is closest.
void SetEnemiesSeeAll(bool on);

// While held, the local player's own Step_0 does not run (the coop's step callback still does): it stands while
// the skill tree is open in the world, with no movement or "stuck" checks of its own.
void HoldLocalPlayer(bool hold);

// An objPlayer that is neither the local player nor a puppet (the skill tree's avatar): none of its events run,
// as for a puppet, and it is not taken for the local player.
void SetInert(yyc::CInstance* instance, bool inert);

// Guest: familiars (lantern, halo, droids) come with the host's world and are the host's: they follow the
// host's puppet.
void SetFamiliarsFollowHost(bool on);

// A familiar with a known owner follows that player: kOwnSelf - the local player (the guest's own droid), else
// that peer's puppet (the host's copy of a guest's droid). kNoOwner forgets it.
constexpr int kOwnSelf = -1;
constexpr int kNoOwner = -2;
void SetFamiliarOwner(int familiarId, int owner);

// Player `player`'s puppet, or nullptr when there is none.
yyc::CInstance* Instance(net::PeerId player);
// Every puppet there is, in the order they were created.
std::vector<yyc::CInstance*> Instances();
bool IsPuppet(const yyc::CInstance* instance);

// True while a puppet's Create_0 runs (its gear goes to itself through PlayerFind).
bool IsCreatingPuppet();

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
