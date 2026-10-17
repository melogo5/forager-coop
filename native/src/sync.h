// World changes after the guest has loaded the host's world.
//  host -> guest: world objects spawned/destroyed (resources, plants, structures, drops, enemies)
//  guest -> host: the guest's hits (applied on the host as the guest's puppet), world objects the guest
//                 consumed locally (picked-up drops)
//  host -> guest: XP the guest earned
// The guest runs no world spawning of its own.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::sync {

// Creates the script hooks (enabled by the caller). Builtin hooks are added later by Tick, once the
// builtin table is filled.
bool InstallHooks(bool (*isHost)(), bool (*isGuest)());

// Game thread, every local player step while in a world.
void Tick(yyc::CInstance* localPlayer);

// Game thread: applies a sync message; false if `message` is not a sync message.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Guest: the local replica of a host instance (0 if none), and creating one.
int ReplicaOf(int hostId);
int SpawnReplica(yyc::CInstance* self, int hostId, int object, double x, double y, double depth);
// Guest: binds an existing local instance (e.g. loaded from the same save) to a host instance.
void AdoptReplica(int hostId, int localId);
bool IsReplica(int localId);
// Guest: the host destroyed this instance (so a late update must not bring it back).
bool WasDestroyed(int hostId);

// Guest: world objects created between Begin and End (e.g. by SpawnLand) are removed at End — the host
// sends its own.
void BeginSuppressSpawns();
void EndSuppressSpawns(yyc::CInstance* self);

// The id the host uses for a local instance: itself on the host, the bound host id (or -1) on the guest.
int HostIdOf(int localId);
// The local instance a peer message refers to: by host id when known, else the `object` standing at (x, y).
// 0 if there is none.
int Resolve(yyc::CInstance* self, int hostId, int object, double x, double y);

}  // namespace coop::sync
