#include "storage.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "mutant.h"
#include "net.h"
#include "session.h"
#include "stations.h"
#include "sync.h"

namespace coop::storage {

namespace {

constexpr uint8_t kStorage = 40;      // contents on close; also releases the chest
constexpr uint8_t kStorageLock = 41;  // the sender opened this chest
constexpr uint8_t kStorageDenied = 42;  // host -> guest: someone else had that chest open first; close it
constexpr uint8_t kVaultSlots = 43;     // u8: guest -> host its own vault cells, host -> guest everyone's most
constexpr uint32_t kVaultInteract = 0x00BC88D0;  // objVault Other_11: opens the storage menu
constexpr uint32_t kStorageMenuDestroy = 0x00E51ED0;  // objStorageInventory Destroy_0
constexpr int kSlotOwner = 0x6CA;                     // the menu's chest
constexpr int kSlotContents = 0x7C4;                  // the chest's `slot` (2D array)
constexpr int kObjVault = 114;
constexpr int kObjStorageMenu = 234;
constexpr unsigned kWatchEverySteps = 60;
constexpr size_t kMaxRows = 4096;
constexpr size_t kMaxColumns = 255;

enum CellKind : uint8_t { kCellReal = 0, kCellString = 1, kCellUndefined = 5 };

#pragma pack(push, 1)
struct Header {
    int32_t hostId;
    int32_t object;
    float x, y;
    uint16_t rows;
};
#pragma pack(pop)

yyc::EventFn g_origMenuDestroy = nullptr;
yyc::EventFn g_origVaultInteract = nullptr;
std::vector<int> g_lockedByPeer;  // local chest ids another player has open
std::unordered_map<int, net::PeerId> g_lockOwners;  // host: which guest has each of those open
int g_ownOpen = 0;          // the chest this side has open (0 = none)
bool g_closingDenied = false;  // closing a chest the host refused us: its contents are not news
std::unordered_map<int, size_t> g_known;  // local chest id -> hash of the contents the peer has too
unsigned g_steps = 0;

bool LockedByPeer(int chestId) {
    return std::find(g_lockedByPeer.begin(), g_lockedByPeer.end(), chestId) != g_lockedByPeer.end();
}

void Release(int chestId) {
    g_lockedByPeer.erase(std::remove(g_lockedByPeer.begin(), g_lockedByPeer.end(), chestId), g_lockedByPeer.end());
    g_lockOwners.erase(chestId);
}

Header Describe(int chestId, yyc::CInstance* chest, uint16_t rows) {
    return {sync::HostIdOf(chestId), static_cast<int32_t>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_object_index)),
            static_cast<float>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_x)),
            static_cast<float>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_y)), rows};
}

// Testing aid: COOP_DEBUG_IGNORE_BUSY=1 opens chests and stations other players have open, as a player who
// clicked at the same moment as another would (the host then refuses).
bool IgnoreBusy() {
    static const bool ignore = GetEnvironmentVariableA("COOP_DEBUG_IGNORE_BUSY", nullptr, 0) > 0;
    return ignore;
}

// Only one player at a time may have a chest open. Two guests may still click the same chest at once: the host
// decides (Handle) and the later one closes it again.
void __cdecl VaultInteractHook(yyc::CInstance* self, yyc::CInstance* other) {
    const int chestId = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    if (net::IsConnected() && LockedByPeer(chestId) && !IgnoreBusy()) {
        Log("storage: chest %d is open by the other player", chestId);
        stations::SayBusy(other);
        return;
    }
    g_origVaultInteract(self, other);
    g_ownOpen = chestId;
    if (!net::IsConnected()) return;
    const Header header = Describe(chestId, self, 0);
    net::Send(kStorageLock, &header, sizeof(header));
}

double CallReal(yyc::CInstance* self, const char* builtin, std::vector<yyc::RValue> args) {
    yyc::RValue result{};
    yyc::CallBuiltin(builtin, self, static_cast<int>(args.size()), args.data(), &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

void PutCell(std::vector<uint8_t>& out, const yyc::RValue& cell) {
    const uint32_t kind = cell.kind & yyc::kKindMask;
    if (kind == yyc::kString) {
        const std::string text = yyc::ToString(cell);
        const uint16_t length = static_cast<uint16_t>((std::min)(text.size(), size_t{0xFFFF}));
        out.push_back(kCellString);
        out.push_back(static_cast<uint8_t>(length & 0xFF));
        out.push_back(static_cast<uint8_t>(length >> 8));
        out.insert(out.end(), text.begin(), text.begin() + length);
    } else if (kind == yyc::kUndefined) {
        out.push_back(kCellUndefined);
    } else {
        const double value = yyc::ToReal(cell);
        out.push_back(kCellReal);
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(value));
    }
}

// The chest's message: Header, then per row u8 columns and the cells. Empty if the chest is gone.
std::vector<uint8_t> Encode(yyc::CInstance* self, int chestId) {
    yyc::CInstance* chest = yyc::FindInstance(chestId);
    if (chest == nullptr) return {};
    yyc::RValue contents{};
    yyc::GetVar(chestId, kSlotContents, &contents);
    const bool isArray = (contents.kind & yyc::kKindMask) == yyc::kArray;
    const size_t rows = isArray ? static_cast<size_t>(CallReal(self, "array_height_2d", {contents})) : 0;
    const Header header = Describe(chestId, chest, static_cast<uint16_t>((std::min)(rows, kMaxRows)));
    std::vector<uint8_t> payload(sizeof(header));
    for (size_t i = 0; i < header.rows; ++i) {
        const size_t columns =
            (std::min)(static_cast<size_t>(CallReal(self, "array_length_2d", {contents, yyc::Real(double(i))})),
                       kMaxColumns);
        payload.push_back(static_cast<uint8_t>(columns));
        for (size_t j = 0; j < columns; ++j) {
            yyc::RValue cell{};
            yyc::GetVarAt(chestId, kSlotContents, static_cast<int>(i * yyc::kArray2dStride + j), &cell);
            PutCell(payload, cell);
            yyc::Free(&cell);
        }
    }
    yyc::Free(&contents);
    memcpy(payload.data(), &header, sizeof(header));
    return payload;
}

size_t Hash(const std::vector<uint8_t>& payload) {
    return std::hash<std::string>{}(std::string(payload.begin(), payload.end()));
}

void SendContents(yyc::CInstance* self, int chestId) {
    const std::vector<uint8_t> payload = Encode(self, chestId);
    if (payload.empty()) return;
    net::Send(kStorage, payload.data(), payload.size());
    g_known[chestId] = Hash(payload);
    Header header{};
    memcpy(&header, payload.data(), sizeof(header));
    Log("storage: chest %d (object %d) sent, %u rows, %zu bytes", chestId, header.object, header.rows,
        payload.size());
}

// How many of a vault's 9 cells are used: objVault Create_0 sets maxSlots = 6, +3 with skill 23 (HasSkill:
// skillUnlocked[23] == 2), with whatever skills are in place then (the guest's vaults come from the host's save before
// its own skills are applied). Vaults are shared, so they have 9 cells for everyone once any player has the skill:
// with each player's own count, the host did not see what the guest put in cells 7-9 (playtests 4 and 5).
constexpr int kSlotMaxSlots = 0x639;
constexpr int kSlotSkillUnlocked = 0x7BF;  // global.skillUnlocked
constexpr int kSkillMoreVaultSlots = 23;

double OwnVaultSlots() {
    const yyc::RValue* skills = yyc::GlobalVar(kSlotSkillUnlocked);
    if (skills == nullptr || (skills->kind & yyc::kKindMask) != yyc::kArray) return 0;
    yyc::RValue copy = *skills;  // a plain copy: no reference taken or freed
    return yyc::ToReal(yyc::ArrayElement(&copy, kSkillMoreVaultSlots)) == 2 ? 9 : 6;
}

std::unordered_map<net::PeerId, double> g_peerSlots;  // host: each guest's own count; guest: the host's shared one
double g_sentSlots = 0;

// Every second: guests tell the host their own count, the host tells the guests the most of all.
double SharedVaultSlots() {
    const double own = OwnVaultSlots();
    double shared = own;
    for (const auto& [peer, slots] : g_peerSlots) {
        if (!Mutant("vaultslots")) shared = (std::max)(shared, slots);
    }
    const double told = session::CurrentRole() == session::Role::kHost ? shared : own;
    if (told > 0 && told != g_sentSlots) {
        const uint8_t value = static_cast<uint8_t>(told);
        net::Send(kVaultSlots, &value, 1);
        g_sentSlots = told;
    }
    return shared;
}

void KeepVaultSlots(int chestId, double slots) {
    yyc::RValue current{};
    const bool ok = yyc::GetVar(chestId, kSlotMaxSlots, &current) && yyc::IsNumber(current);
    const double was = ok ? yyc::ToReal(current) : 0;
    yyc::Free(&current);
    if (!ok || was == slots) return;
    yyc::RValue value = yyc::Real(slots);
    yyc::SetVar(chestId, kSlotMaxSlots, &value);
    Log("storage: chest %d has %g cells for this player (was %g)", chestId, slots, was);
}

// Contents also change with the menu closed - crafting takes from vaults (ItemCount/RemoveItem count them) - so
// every second each side sends the vaults whose contents changed since both last agreed (PLAYTEST.md #12, #13).
void WatchVaults(yyc::CInstance* self) {
    const int count = static_cast<int>(CallReal(self, "instance_number", {yyc::Real(kObjVault)}));
    const double slots = SharedVaultSlots();
    for (int i = 0; i < count; ++i) {
        const int chestId = static_cast<int>(CallReal(self, "instance_find", {yyc::Real(kObjVault), yyc::Real(i)}));
        if (yyc::FindInstance(chestId) == nullptr) continue;
        if (slots > 0) KeepVaultSlots(chestId, slots);
        // The chest open here goes out when it closes: sent while open, it released the other side's lock on it
        // (ApplyContents), so both players had it open at once - items doubled or lost (playtest 5).
        if (LockedByPeer(chestId) || (chestId == g_ownOpen && !Mutant("storagelock"))) continue;
        const size_t hash = Hash(Encode(self, chestId));
        const auto known = g_known.find(chestId);
        if (known == g_known.end()) {
            g_known[chestId] = hash;  // both sides start from the same save
        } else if (known->second != hash) {
            SendContents(self, chestId);
        }
    }
}

// After the menu is gone the chest holds its final contents for this visit.
void __cdecl MenuDestroyHook(yyc::CInstance* self, yyc::CInstance* other) {
    yyc::RValue owner{};
    const int menuId = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    yyc::GetVar(menuId, kSlotOwner, &owner);
    const int chestId = static_cast<int>(yyc::ToReal(owner));
    yyc::Free(&owner);
    g_origMenuDestroy(self, other);
    if (chestId == g_ownOpen) g_ownOpen = 0;
    if (!net::IsConnected() || chestId < 100000 || g_closingDenied) return;
    __try {
        SendContents(self, chestId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("storage: sending chest %d crashed (code 0x%08lX)", chestId, GetExceptionCode());
    }
}

bool ReadCell(const std::vector<uint8_t>& p, size_t* at, yyc::RValue* out) {
    if (*at >= p.size()) return false;
    const uint8_t kind = p[(*at)++];
    if (kind == kCellReal) {
        if (*at + sizeof(double) > p.size()) return false;
        double value = 0;
        memcpy(&value, p.data() + *at, sizeof(value));
        *at += sizeof(value);
        *out = yyc::Real(value);
    } else if (kind == kCellString) {
        if (*at + 2 > p.size()) return false;
        const size_t length = p[*at] | (p[*at + 1] << 8);
        *at += 2;
        if (*at + length > p.size()) return false;
        const std::string text(p.begin() + *at, p.begin() + *at + length);
        *at += length;
        *out = yyc::String(text.c_str());
    } else {
        *out = yyc::RValue{};
        out->kind = yyc::kUndefined;
    }
    return true;
}

// `woken`: the second try, after waking the chest's kind up (an off-screen chest is asleep and not found).
void ApplyContents(yyc::CInstance* self, const net::Message& message, bool woken = false) {
    const auto& p = message.payload;
    if (p.size() < sizeof(Header)) return;
    Header header{};
    memcpy(&header, p.data(), sizeof(header));
    if (header.object != kObjVault || header.rows > kMaxRows) return;
    const int chestId = sync::Resolve(self, header.hostId, header.object, header.x, header.y);
    if (chestId == 0 && !woken) {
        sync::WakeThen(self, header.object, [self, message] { ApplyContents(self, message, true); });
        return;
    }
    if (chestId == 0) {
        Log("storage: chest object %d at (%.0f, %.0f) not found", header.object, header.x, header.y);
        return;
    }
    Release(chestId);
    size_t at = sizeof(Header);
    for (size_t i = 0; i < header.rows && at < p.size(); ++i) {
        const size_t columns = p[at++];
        for (size_t j = 0; j < columns; ++j) {
            yyc::RValue cell{};
            if (!ReadCell(p, &at, &cell)) return;
            yyc::SetVarAt(chestId, kSlotContents, static_cast<int>(i * yyc::kArray2dStride + j), &cell);
            yyc::Free(&cell);
        }
    }
    g_known[chestId] = Hash(Encode(self, chestId));  // what the peer has now: not news to send back
    yyc::RValue item{}, amount{};
    yyc::GetVarAt(chestId, kSlotContents, 0, &item);
    yyc::GetVarAt(chestId, kSlotContents, 1, &amount);
    Log("storage: chest %d updated from the peer (%u rows), first cell: item %g x%g", chestId, header.rows,
        yyc::ToReal(item), yyc::ToReal(amount));
    yyc::Free(&item);
    yyc::Free(&amount);
}

// Host: guest `from` opened `chestId`. Fine unless the host or another guest had it open first: then that guest
// is told to close it again.
bool AllowLock(int chestId, net::PeerId from, const Header& header) {
    const auto owner = g_lockOwners.find(chestId);
    const bool takenByGuest = owner != g_lockOwners.end() && owner->second != from;
    if (chestId != g_ownOpen && !takenByGuest) return true;
    net::SendTo(from, kStorageDenied, &header, sizeof(header));
    Log("storage: guest %u opened chest %d, which %s has open - told to close it", from, chestId,
        takenByGuest ? "another guest" : "the host");
    return false;
}

// Guest: the host refused the chest we just opened. Closed without sending its contents: they are the other
// player's to change.
void CloseDenied(yyc::CInstance* localPlayer, int chestId) {
    g_closingDenied = true;
    CallReal(localPlayer, "instance_destroy", {yyc::Real(kObjStorageMenu)});
    g_closingDenied = false;
    g_ownOpen = 0;
    if (!LockedByPeer(chestId)) g_lockedByPeer.push_back(chestId);
    stations::SayBusy(localPlayer);
    Log("storage: chest %d was opened by someone else first; closed", chestId);
}

}  // namespace

void DebugTick(yyc::CInstance* localPlayer) {
    // Testing aid: COOP_DEBUG_STORAGE=1 opens the nearest chest ~7 s into the world and closes it 1 s later,
    // then ~3 s later changes it with no menu open. COOP_DEBUG_STORAGE=2 opens it and keeps it open.
    static const int mode = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_STORAGE", value, sizeof(value)) > 0 ? atoi(value) : 0;
    }();
    const bool enabled = mode > 0;
    static unsigned steps = 0;
    constexpr unsigned kOpenAt = 420;
    constexpr unsigned kCloseAt = 480;
    constexpr unsigned kChangeAt = 660;
    if (++g_steps % kWatchEverySteps == 0 && net::IsConnected()) WatchVaults(localPlayer);
    if (!enabled) return;
    ++steps;
    if (steps == kOpenAt) {
        const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
        const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
        const int chest = static_cast<int>(CallReal(localPlayer, "instance_nearest",
                                                    {yyc::Real(x), yyc::Real(y), yyc::Real(kObjVault)}));
        if (yyc::CInstance* instance = yyc::FindInstance(chest)) {
            yyc::PerformEvent(instance, localPlayer, yyc::kEvOther, yyc::kEvUser0 + 1);
            Log("storage: debug opened chest %d", chest);
        }
    } else if (steps == kCloseAt && mode == 1) {
        // As if the player put 5 of item #1 in the first cell: [item, amount, ...].
        const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
        const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
        const int chest = static_cast<int>(CallReal(localPlayer, "instance_nearest",
                                                    {yyc::Real(x), yyc::Real(y), yyc::Real(kObjVault)}));
        if (yyc::FindInstance(chest) != nullptr) {
            yyc::RValue item = yyc::Real(1);
            yyc::RValue amount = yyc::Real(5);
            yyc::SetVarAt(chest, kSlotContents, 0, &item);
            yyc::SetVarAt(chest, kSlotContents, 1, &amount);
        }
        CallReal(localPlayer, "instance_destroy", {yyc::Real(kObjStorageMenu)});
        Log("storage: debug closed the storage menu");
    } else if (steps == kChangeAt && mode == 1) {
        // As crafting would: 2 of the 5 are taken with no menu open; the watcher must send it.
        const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
        const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
        const int chest = static_cast<int>(CallReal(localPlayer, "instance_nearest",
                                                    {yyc::Real(x), yyc::Real(y), yyc::Real(kObjVault)}));
        if (yyc::FindInstance(chest) != nullptr) {
            yyc::RValue amount = yyc::Real(3);
            yyc::SetVarAt(chest, kSlotContents, 1, &amount);
            Log("storage: debug took 2 from chest %d with no menu open", chest);
        }
    }
}

bool InstallHooks() {
    const bool ok = MH_CreateHook(yyc::At<void*>(kStorageMenuDestroy), reinterpret_cast<void*>(&MenuDestroyHook),
                                  reinterpret_cast<void**>(&g_origMenuDestroy)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kVaultInteract), reinterpret_cast<void*>(&VaultInteractHook),
                                  reinterpret_cast<void**>(&g_origVaultInteract)) == MH_OK;
    if (!ok) Log("storage: hooks failed");
    return ok;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kStorageLock) {
        if (message.payload.size() >= sizeof(Header)) {
            Header header{};
            memcpy(&header, message.payload.data(), sizeof(header));
            if (header.object != kObjVault) return true;
            const int chestId = sync::Resolve(localPlayer, header.hostId, header.object, header.x, header.y);
            if (message.from != net::kHostPeer && chestId != 0 && !AllowLock(chestId, message.from, header)) {
                return true;
            }
            if (chestId != 0 && !LockedByPeer(chestId)) g_lockedByPeer.push_back(chestId);
            if (chestId != 0) g_lockOwners[chestId] = message.from;
            if (message.from != net::kHostPeer) {
                net::SendExcept(message.from, kStorageLock, message.payload.data(), message.payload.size());
            }
        }
        return true;
    }
    if (message.type == kStorageDenied) {
        if (message.payload.size() >= sizeof(Header)) {
            Header header{};
            memcpy(&header, message.payload.data(), sizeof(header));
            const int chestId = sync::Resolve(localPlayer, header.hostId, header.object, header.x, header.y);
            if (chestId != 0 && chestId == g_ownOpen) CloseDenied(localPlayer, chestId);
        }
        return true;
    }
    if (message.type == kVaultSlots) {
        constexpr uint8_t kFewestCells = 6, kMostCells = 9;  // a vault's cells, without and with every upgrade
        if (message.payload.size() == 1 && message.payload[0] >= kFewestCells && message.payload[0] <= kMostCells) {
            g_peerSlots[message.from] = message.payload[0];
        }
        return true;
    }
    if (message.type != kStorage) return false;
    ApplyContents(localPlayer, message);
    return true;
}

void OnPeerJoined(net::PeerId peer) {
    g_sentSlots = 0;  // the newcomer hears the vaults' cells at the next watch
    for (const int chestId : g_lockedByPeer) {
        yyc::CInstance* chest = yyc::FindInstance(chestId);
        if (chest == nullptr) continue;
        const Header header = Describe(chestId, chest, 0);
        net::SendTo(peer, kStorageLock, &header, sizeof(header));
    }
}

void OnPeerLeft(yyc::CInstance* localPlayer, net::PeerId peer) {
    g_peerSlots.erase(peer);
    std::vector<int> freed;
    for (const auto& [chestId, owner] : g_lockOwners) {
        if (owner == peer) freed.push_back(chestId);
    }
    for (const int chestId : freed) {
        Release(chestId);
        Log("storage: chest %d was open by guest %u, who left; free again", chestId, peer);
        SendContents(localPlayer, chestId);  // releases it for the other guests too
    }
}

void Reset() {
    g_lockedByPeer.clear();
    g_lockOwners.clear();
    g_ownOpen = 0;
    g_known.clear();
    g_steps = 0;
    g_peerSlots.clear();
    g_sentSlots = 0;
}

}  // namespace coop::storage
