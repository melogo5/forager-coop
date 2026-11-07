#include "familiars.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#include "character.h"
#include "files.h"
#include "log.h"
#include "puppet.h"

namespace coop::familiars {

namespace {

constexpr uint8_t kFamiliars = 34;     // guest -> host: FamiliarEntry per own familiar
constexpr uint8_t kRestore = 106;      // host -> guest: FamiliarEntry per own familiar it had (kept with its character)
constexpr uint8_t kFamiliarsOf = 107;  // host -> guests: OwnerHeader, then FamiliarEntry per familiar of that player
constexpr int kParFamiliar = 74;    // decompiled/data/objects.tsv: objBossKey 75, lantern 76, halo 77, objDroid 78
constexpr int kObjBossKey = 75;     // a key flying to its chest, not anyone's companion
constexpr int kObjDroid = 78;
constexpr int kSlotDroidId = 0x1B7;
// global.droidNames: a ds_grid with a row per droidId - name, look (a sprite), colour. Read as an array, no droid had a
// look and none was drawn, the guest's own crafted one included (playtest 9).
constexpr int kSlotDroidNames = 0x1B8;
constexpr double kLookColumn = 1;
constexpr double kDsTypeGrid = 5;
constexpr int kSlotDroids = 0x1BA;  // global: the droidIds of the world's droids (ds_list); GainItem adds to it
constexpr int kSlotFollowing = 0x435;
constexpr int kSlotFacing = 0x409;
constexpr char kStoreExtension[] = ".familiars";
constexpr unsigned kCheckEverySteps = 60;
constexpr unsigned kResendSteps = 600;
constexpr size_t kMaxFamiliars = 32;
constexpr char kOwnCreator[] = "gml_Script_GainItem";  // a crafted droid / artifact is made there

#pragma pack(push, 1)
struct FamiliarEntry {
    int32_t object;
    float look;  // a droid's global.droidNames[droidId]; -1 for other familiars
    bool operator==(const FamiliarEntry& o) const { return object == o.object && look == o.look; }
};
struct OwnerHeader {
    uint8_t owner;
};
#pragma pack(pop)

using List = std::vector<FamiliarEntry>;

// Another player's familiars as shown here: copies next to its puppet. They do no work - they are not in
// global.droids, which HandleDroids walks.
struct Shown {
    List has;                 // what that player has
    List copied;              // what the copies show (at a guest, of the host's: only those the room has not)
    std::vector<int> copies;  // their ids
    int puppetId = 0;         // whom they follow
};

std::vector<int> g_own;  // guest: its own familiars' ids
// Guest: the looks of its own droids. A droid is never lost, but the game makes every droid again at each room
// start (WorldControl Other_4, from global.droids) - with no owner here, so it went after the host's puppet.
std::vector<float> g_ownDroids;
List g_sent;  // what we last told the others about our own
unsigned g_steps = 0;
std::map<net::PeerId, Shown> g_shown;  // every other player's familiars

bool CreatedByGainItem() {
    for (const yyc::StackFrame* frame = yyc::TopFrame(); frame != nullptr; frame = frame->prev) {
        if (frame->name != nullptr && strcmp(frame->name, kOwnCreator) == 0) return true;
    }
    return false;
}

bool IsFamiliar(yyc::CInstance* self, int object) {
    if (object == kObjBossKey) return false;
    return object == kParFamiliar || yyc::CallReal(self, "object_is_ancestor", {double(object), double(kParFamiliar)}) != 0;
}

bool Contains(const std::vector<int>& ids, int id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }

bool IsCopy(int id) {
    for (const auto& [owner, shown] : g_shown) {
        if (Contains(shown.copies, id)) return true;
    }
    return false;
}

double DroidId(int id) {
    yyc::RValue value{};
    const bool known = yyc::GetVar(id, kSlotDroidId, &value) && yyc::IsNumber(value);
    const double droidId = known ? yyc::ToReal(value) : -1;
    yyc::Free(&value);
    return droidId;
}

double DroidGrid(yyc::CInstance* self) {
    const yyc::RValue* grid = yyc::GlobalVar(kSlotDroidNames);
    if (grid == nullptr || !yyc::IsNumber(*grid)) return -1;
    const double id = yyc::ToReal(*grid);
    return yyc::CallReal(self, "ds_exists", {id, kDsTypeGrid}) != 0 ? id : -1;
}

int DroidLookCount(yyc::CInstance* self) {
    const double grid = DroidGrid(self);
    return grid >= 0 ? static_cast<int>(yyc::CallReal(self, "ds_grid_height", {grid})) : 0;
}

double DroidLook(yyc::CInstance* self, int droidId) {
    if (droidId < 0 || droidId >= DroidLookCount(self)) return -1;
    return yyc::CallReal(self, "ds_grid_get", {DroidGrid(self), kLookColumn, double(droidId)});
}

// The droidId with this look here (the grid is the game's own list, the same for everyone), -1 if none.
int DroidIdForLook(yyc::CInstance* self, double look) {
    const int count = DroidLookCount(self);
    for (int i = 0; i < count; ++i) {
        if (DroidLook(self, i) == look) return i;
    }
    return -1;
}

FamiliarEntry EntryOf(yyc::CInstance* familiar, int id) {
    const int object = static_cast<int>(yyc::GetBuiltinReal(familiar, yyc::va::YYVar_object_index));
    return {object, static_cast<float>(DroidLook(familiar, static_cast<int>(DroidId(id))))};
}

void SetFollowing(int id, double whom) {
    yyc::RValue following = yyc::Real(whom);
    yyc::SetVar(id, kSlotFollowing, &following);
}

// The familiars in the room that are not copies shown here (the boss key is no one's).
std::vector<int> RoomFamiliars(yyc::CInstance* self) {
    std::vector<int> ids;
    const int count = static_cast<int>(yyc::CallReal(self, "instance_number", {double(kParFamiliar)}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(yyc::CallReal(self, "instance_find", {double(kParFamiliar), double(i)}));
        yyc::CInstance* familiar = yyc::FindInstance(id);
        if (familiar == nullptr || IsCopy(id)) continue;
        if (static_cast<int>(yyc::GetBuiltinReal(familiar, yyc::va::YYVar_object_index)) == kObjBossKey) continue;
        ids.push_back(id);
    }
    return ids;
}

void DestroyCopies(yyc::CInstance* self, Shown& shown) {
    for (const int id : shown.copies) {
        puppet::SetFamiliarOwner(id, puppet::kNoOwner);
        if (yyc::FindInstance(id) != nullptr) yyc::CallReal(self, "instance_destroy", {double(id)});
    }
    shown.copies.clear();
    shown.copied.clear();
    shown.puppetId = 0;
}

void MakeCopies(yyc::CInstance* self, net::PeerId owner, yyc::CInstance* puppet, const List& list, Shown& shown) {
    DestroyCopies(self, shown);
    const double x = yyc::GetBuiltinReal(puppet, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(puppet, yyc::va::YYVar_y);
    shown.puppetId = static_cast<int>(yyc::GetBuiltinReal(puppet, yyc::va::YYVar_id));
    for (const FamiliarEntry& entry : list) {
        if (!IsFamiliar(self, entry.object)) continue;
        const int id = static_cast<int>(yyc::CallReal(self, "instance_create_depth", {x, y, -y, double(entry.object)}));
        if (yyc::FindInstance(id) == nullptr) continue;
        if (entry.look >= 0) {
            yyc::RValue droidId = yyc::Real(DroidIdForLook(self, entry.look));
            yyc::SetVar(id, kSlotDroidId, &droidId);
        }
        // Its End Step takes `facing` from whom it follows: with no one, none, and Draw_0 failed on it (line 25).
        yyc::RValue facing = yyc::Real(1);
        yyc::SetVar(id, kSlotFacing, &facing);
        SetFollowing(id, shown.puppetId);
        puppet::SetFamiliarOwner(id, owner);
        shown.copies.push_back(id);
    }
    shown.copied = list;
    Log("familiars: player %u - %zu copies shown", owner, shown.copies.size());
}

// Guest: the host's familiars the room has not got by itself. Those that came with the host's world (made at the
// room start from global.droids) follow the host's puppet; the rest - made after the world was sent - are copied.
List HostSurplus(const List& hostHas, const std::vector<int>& room, int hostPuppetId) {
    std::vector<int> unowned;
    for (const int id : room) {
        if (!Contains(g_own, id)) unowned.push_back(id);
    }
    List surplus;
    for (const FamiliarEntry& entry : hostHas) {
        const auto match = std::find_if(unowned.begin(), unowned.end(), [&entry](int id) {
            yyc::CInstance* familiar = yyc::FindInstance(id);
            return familiar != nullptr && EntryOf(familiar, id) == entry;
        });
        if (match == unowned.end()) {
            surplus.push_back(entry);
            continue;
        }
        SetFollowing(*match, hostPuppetId);  // made at the room start after the guest itself (PlayerFind there)
        unowned.erase(match);
    }
    return surplus;
}

// Every side, every kCheckEverySteps: each other player's copies are there and follow its puppet. A room start
// destroys them (familiars are not persistent) and a puppet made again has a new id.
void Maintain(yyc::CInstance* self, bool guest) {
    const std::vector<int> room = guest ? RoomFamiliars(self) : std::vector<int>{};
    for (auto& [owner, shown] : g_shown) {
        yyc::CInstance* puppet = puppet::Instance(owner);
        if (puppet == nullptr) {
            if (!shown.copies.empty()) DestroyCopies(self, shown);
            continue;
        }
        const int puppetId = static_cast<int>(yyc::GetBuiltinReal(puppet, yyc::va::YYVar_id));
        const List wanted = guest && owner == net::kHostPeer ? HostSurplus(shown.has, room, puppetId) : shown.has;
        const bool intact = std::all_of(shown.copies.begin(), shown.copies.end(),
                                        [](int id) { return yyc::FindInstance(id) != nullptr; });
        if (wanted == shown.copied && puppetId == shown.puppetId && intact) continue;
        MakeCopies(self, owner, puppet, wanted, shown);
    }
}

// Guest: its droids the room start made again (with no owner) are its own once more.
void AdoptOwnDroids(yyc::CInstance* localPlayer) {
    std::vector<float> missing = g_ownDroids;
    for (const int id : g_own) {
        yyc::CInstance* familiar = yyc::FindInstance(id);
        if (familiar == nullptr) continue;
        const auto it = std::find(missing.begin(), missing.end(), EntryOf(familiar, id).look);
        if (it != missing.end()) missing.erase(it);
    }
    if (missing.empty()) return;
    const double self = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id);
    for (const int id : RoomFamiliars(localPlayer)) {
        yyc::CInstance* familiar = yyc::FindInstance(id);
        if (familiar == nullptr || Contains(g_own, id)) continue;
        const FamiliarEntry entry = EntryOf(familiar, id);
        const auto it = std::find(missing.begin(), missing.end(), entry.look);
        if (entry.object != kObjDroid || it == missing.end()) continue;
        missing.erase(it);
        g_own.push_back(id);
        puppet::SetFamiliarOwner(id, puppet::kOwnSelf);
        SetFollowing(id, self);
        Log("familiars: droid %d (made at the room start) is our own again", id);
    }
}

// Guest: its own familiars - those alive here, and its droids even where the room has none (in a building).
List OwnList() {
    List list;
    std::vector<int> alive;
    std::vector<float> droids;
    for (const int id : g_own) {
        yyc::CInstance* familiar = yyc::FindInstance(id);
        if (familiar == nullptr) {
            puppet::SetFamiliarOwner(id, puppet::kNoOwner);
            continue;
        }
        alive.push_back(id);
        const FamiliarEntry entry = EntryOf(familiar, id);
        if (entry.object == kObjDroid) {
            droids.push_back(entry.look);
        } else if (list.size() < kMaxFamiliars) {
            list.push_back(entry);
        }
    }
    g_own = alive;
    // Droids only come (crafted, or back from the last game): more alive of a look than known is a new one.
    for (const float look : droids) {
        if (std::count(droids.begin(), droids.end(), look) > std::count(g_ownDroids.begin(), g_ownDroids.end(), look)) {
            g_ownDroids.push_back(look);
        }
    }
    for (const float look : g_ownDroids) {
        if (list.size() < kMaxFamiliars) list.push_back({kObjDroid, look});
    }
    return list;
}

// Host: its own familiars - every one in the room but the copies.
List HostOwnList(yyc::CInstance* localPlayer) {
    List list;
    for (const int id : RoomFamiliars(localPlayer)) {
        yyc::CInstance* familiar = yyc::FindInstance(id);
        if (familiar != nullptr && list.size() < kMaxFamiliars) list.push_back(EntryOf(familiar, id));
    }
    return list;
}

std::vector<uint8_t> OwnersMessage(net::PeerId owner, const List& list) {
    std::vector<uint8_t> bytes(sizeof(OwnerHeader) + list.size() * sizeof(FamiliarEntry));
    bytes[0] = owner;
    if (!list.empty()) memcpy(bytes.data() + sizeof(OwnerHeader), list.data(), list.size() * sizeof(FamiliarEntry));
    return bytes;
}

bool ParseList(const uint8_t* data, size_t size, List* out) {
    if (size % sizeof(FamiliarEntry) != 0 || size / sizeof(FamiliarEntry) > kMaxFamiliars) return false;
    out->resize(size / sizeof(FamiliarEntry));
    if (size > 0) memcpy(out->data(), data, size);
    return true;
}

// Host: guest `from`'s own familiars - copied here (Maintain), passed on to the other guests, kept with its
// character (the guest's world is the host's: its droids were gone after a rejoin).
void HostOnGuestList(net::PeerId from, const List& list) {
    g_shown[from].has = list;
    const std::vector<uint8_t> forward = OwnersMessage(from, list);
    net::SendExcept(from, kFamiliarsOf, forward.data(), forward.size());
    Log("familiars: guest %u has %zu of its own", from, list.size());
    const std::string name = character::HostGuestName(from);
    const auto* bytes = reinterpret_cast<const uint8_t*>(list.data());
    if (!coop::files::WriteAtomic(character::GuestDataFile(name, kStoreExtension), bytes,
                                  list.size() * sizeof(FamiliarEntry))) {
        Log("familiars: cannot keep guest '%s' familiars", name.c_str());
    }
}

List g_restore;  // guest: its droids as the host kept them, made once in the world
bool g_restorePending = false;

// Guest: its droids from the last game, made again as GainItem makes a crafted one.
void Restore(yyc::CInstance* localPlayer) {
    g_restorePending = false;
    if (!g_own.empty() || !g_ownDroids.empty()) return;
    const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
    const yyc::RValue* droids = yyc::GlobalVar(kSlotDroids);
    for (const FamiliarEntry& entry : g_restore) {
        if (entry.object != kObjDroid) continue;  // the lantern and the halo come back with the gear
        const int droidId = DroidIdForLook(localPlayer, entry.look);
        const int id =
            static_cast<int>(yyc::CallReal(localPlayer, "instance_create_depth", {x, y, -y, double(kObjDroid)}));
        if (yyc::FindInstance(id) == nullptr) continue;
        yyc::RValue value = yyc::Real(droidId);
        yyc::SetVar(id, kSlotDroidId, &value);
        SetFollowing(id, yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
        if (droids != nullptr && yyc::IsNumber(*droids)) {
            yyc::CallReal(localPlayer, "ds_list_add", {yyc::ToReal(*droids), double(droidId)});
        }
        puppet::SetFamiliarOwner(id, puppet::kOwnSelf);
        g_own.push_back(id);
    }
    Log("familiars: %zu own droid(s) back from the last game", g_own.size());
}

bool WorthSending(const List& list) {
    if (list.empty() && g_sent.empty()) return false;
    return !(list == g_sent) || g_steps % kResendSteps == 0;
}

}  // namespace

void OnCreated(yyc::CInstance* self, int createdId, int object) {
    if (!IsFamiliar(self, object) || !CreatedByGainItem()) return;
    g_own.push_back(createdId);
    puppet::SetFamiliarOwner(createdId, puppet::kOwnSelf);
    Log("familiars: own familiar %d (object %d) - follows us, sent to the host", createdId, object);
}

void GuestTick(yyc::CInstance* localPlayer) {
    if (g_restorePending) Restore(localPlayer);
    if (++g_steps % kCheckEverySteps != 0) return;
    AdoptOwnDroids(localPlayer);
    Maintain(localPlayer, true);
    const List list = OwnList();
    if (!WorthSending(list)) return;
    if (net::Send(kFamiliars, list.data(), list.size() * sizeof(FamiliarEntry))) g_sent = list;
}

void HostTick(yyc::CInstance* localPlayer) {
    if (++g_steps % kCheckEverySteps != 0) return;
    Maintain(localPlayer, false);
    const List list = HostOwnList(localPlayer);
    if (!WorthSending(list)) return;
    const std::vector<uint8_t> message = OwnersMessage(net::kHostPeer, list);
    if (net::Send(kFamiliarsOf, message.data(), message.size())) g_sent = list;
}

void HostOnGuest(const std::string& name, net::PeerId peer) {
    // What the others have (its puppets for them come a little later: the copies wait for them).
    for (const auto& [owner, shown] : g_shown) {
        if (owner == peer) continue;
        const std::vector<uint8_t> message = OwnersMessage(owner, shown.has);
        net::SendTo(peer, kFamiliarsOf, message.data(), message.size());
    }
    const std::vector<uint8_t> own = OwnersMessage(net::kHostPeer, g_sent);
    net::SendTo(peer, kFamiliarsOf, own.data(), own.size());

    std::ifstream in(character::GuestDataFile(name, kStoreExtension).c_str(), std::ios::binary);
    const std::vector<uint8_t> stored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (stored.empty() || stored.size() % sizeof(FamiliarEntry) != 0) return;
    net::SendTo(peer, kRestore, stored.data(), stored.size());
    Log("familiars: guest '%s' gets back %zu familiar(s)", name.c_str(), stored.size() / sizeof(FamiliarEntry));
}

void OnPeerLeft(yyc::CInstance* localPlayer, net::PeerId peer, bool host) {
    const auto it = g_shown.find(peer);
    if (it == g_shown.end()) return;
    DestroyCopies(localPlayer, it->second);
    g_shown.erase(it);
    if (!host) return;
    const std::vector<uint8_t> none = OwnersMessage(peer, {});  // a guest taking its number later starts with none
    net::Send(kFamiliarsOf, none.data(), none.size());
}

bool Handle(const net::Message& message) {
    const uint8_t* data = message.payload.data();
    const size_t size = message.payload.size();
    if (message.type == kRestore) {
        if (message.from == net::kHostPeer && ParseList(data, size, &g_restore)) g_restorePending = true;
        return true;
    }
    if (message.type == kFamiliarsOf) {
        List list;
        if (message.from == net::kHostPeer && size >= sizeof(OwnerHeader) &&
            ParseList(data + sizeof(OwnerHeader), size - sizeof(OwnerHeader), &list)) {
            g_shown[data[0]].has = list;  // shown at the next check
        }
        return true;
    }
    if (message.type != kFamiliars) return false;
    List list;
    if (message.from != net::kHostPeer && ParseList(data, size, &list)) HostOnGuestList(message.from, list);
    return true;
}

bool HasDroidLook(yyc::CInstance* self, int droidId) { return DroidLook(self, droidId) >= 0; }

void Reset() {
    g_own.clear();
    g_ownDroids.clear();
    g_restore.clear();
    g_restorePending = false;
    g_sent.clear();
    g_steps = 0;
    g_shown.clear();
}

}  // namespace coop::familiars
