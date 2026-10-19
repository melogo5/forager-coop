// The peer's character, shown as a real objPlayer instance whose own logic is switched off.
// Enemies target it like any parAlly; it is drawn by objPlayer's Draw_0 with PlayerFind() pointing at it.
#pragma once

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

// Host with a guest: an enemy asking for "the player" (PlayerFind) gets the nearer of the local player and the
// puppet, so it chases and attacks whoever is closer.
void SetEnemiesSeeBoth(bool on);

// The puppet instance, or nullptr when there is none.
yyc::CInstance* Instance();
// While acting, PlayerFind() returns the puppet: game code run in between treats the peer as "the player".
// Nests; every Begin needs an End.
void BeginActing();
void EndActing();
bool IsActing();
// Lets something run as the local player in the middle of acting; returns what ResumeActing needs.
int PauseActing();
void ResumeActing(int depth);

// Moves the puppet toward `target`, creating it next to the local player if needed.
void Update(yyc::CInstance* localPlayer, const State& target);
void Destroy(yyc::CInstance* localPlayer);

}  // namespace coop::puppet
