// The guest's own character, kept by the host next to its world (saveDataN/coop_guests/<name>.json).
// On join the guest introduces itself; the host answers with the stored character (or "new") before
// sending the world. While loading the host's world the guest swaps the host's character for its own;
// a new guest keeps the fresh player objPlayer starts with. The guest uploads its character periodically.
#pragma once

#include <string>

#include "net.h"
#include "yyc.h"

namespace coop::character {

// Creates the DeserializeCharacterChunk hook (enabled by the caller).
bool InstallHooks(bool (*isGuest)());

// Guest: the name the host knows this player by (typed in the COOP menu, remembered between games).
std::string SavedGuestName();
// This player's name as the others see it: the one set for this session, else the saved one.
std::string OwnName();
void SetGuestName(const std::string& name);

// Host: the name guest `peer` introduced itself with ("guest" if it has not yet). Kept after the guest leaves:
// its last upload may still be in the inbox.
std::string HostGuestName(net::PeerId peer);

// Host: <save area>/saveData<current slot>/coop_guests/<name><extension>, for more of the guest's data.
std::wstring GuestDataFile(const std::string& name, const char* extension);

// Guest, network thread: right after connecting.
void GuestOnConnect(net::PeerId host);
// Guest, network thread: consumes the host's character answer.
bool GuestHandleReceive(const net::Message& message);
// Guest: the host's answer has arrived (the world must not be loaded before).
bool GuestReady();
// Guest, game thread, every step in the world: periodic upload.
// Guest: uploads the character the game saved last (e.g. by "save and exit") right away.
void GuestUploadNow();
void GuestTick(yyc::CInstance* localPlayer);

// Host, game thread: handles guest introductions and character uploads; false for other messages.
// Sets *guestIntroduced after answering an introduction: that is the moment to send the world.
bool HostHandle(const net::Message& message, yyc::CInstance* localPlayer, bool* guestIntroduced);

// Forgets the last session (guest: stored character, swap done; host: guest names), to start again.
void Reset();

}  // namespace coop::character
