// World progress kept in global ds_lists is shared (SPEC: museum, global progress): each side writes the list
// out (ds_list_write) once a second and sends it when it changed; the peer reads it in (ds_list_read).
// The museum's donations (global.museumBundles) for now; donating there takes two of an item.
#pragma once

#include "net.h"
#include "yyc.h"

namespace coop::progress {

// Creates the hooks (enabled by the caller): the museum's double price.
bool InstallHooks();

// Game thread, every local player step while connected (both sides).
void Tick(yyc::CInstance* localPlayer);

// Game thread: applies a progress message; false if `message` is not one.
bool Handle(const net::Message& message, yyc::CInstance* localPlayer);

// Forgets what was last seen (the session is over).
void Reset();

}  // namespace coop::progress
