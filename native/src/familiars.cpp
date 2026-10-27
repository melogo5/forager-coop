#include "familiars.h"

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

constexpr uint8_t kFamiliars = 34;  // guest -> host: FamiliarEntry per own familiar
constexpr uint8_t kRestore = 106;   // host -> guest: FamiliarEntry per own familiar it had (kept with its character)
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
#pragma pack(pop)

std::vector<int> g_own;                          // guest: its own familiars' ids
std::vector<FamiliarEntry> g_sent;               // guest
unsigned g_steps = 0;                            // guest
std::map<net::PeerId, std::vector<int>> g_copies;  // host: each guest's familiar copies

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

void DestroyCopies(yyc::CInstance* self, std::vector<int>& copies) {
    for (const int id : copies) {
        puppet::SetFamiliarOwner(id, puppet::kNoOwner);
        if (yyc::FindInstance(id) != nullptr) yyc::CallReal(self, "instance_destroy", {double(id)});
    }
    copies.clear();
}

void ApplyList(yyc::CInstance* self, net::PeerId from, const std::vector<FamiliarEntry>& list) {
    yyc::CInstance* puppet = puppet::Instance(from);
    std::vector<int>& copies = g_copies[from];
    DestroyCopies(self, copies);
    if (puppet == nullptr) return;
    const double x = yyc::GetBuiltinReal(puppet, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(puppet, yyc::va::YYVar_y);
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
        yyc::RValue following = yyc::Real(yyc::GetBuiltinReal(puppet, yyc::va::YYVar_id));
        yyc::SetVar(id, kSlotFollowing, &following);
        puppet::SetFamiliarOwner(id, from);
        copies.push_back(id);
    }
    Log("familiars: guest %u has %zu of its own - copies made", from, copies.size());
    // Kept with the guest's character: the guest's world is the host's, its droids were gone after a rejoin.
    const std::string name = character::HostGuestName(from);
    const auto* bytes = reinterpret_cast<const uint8_t*>(list.data());
    if (!coop::files::WriteAtomic(character::GuestDataFile(name, kStoreExtension), bytes,
                                  list.size() * sizeof(FamiliarEntry))) {
        Log("familiars: cannot keep guest '%s' familiars", name.c_str());
    }
}

std::vector<FamiliarEntry> g_restore;  // guest: its droids as the host kept them, made once in the world
bool g_restorePending = false;

// Guest: its droids from the last game, made again as GainItem makes a crafted one.
void Restore(yyc::CInstance* localPlayer) {
    g_restorePending = false;
    if (!g_own.empty()) return;
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
        yyc::RValue following = yyc::Real(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
        yyc::SetVar(id, kSlotFollowing, &following);
        if (droids != nullptr && yyc::IsNumber(*droids)) {
            yyc::CallReal(localPlayer, "ds_list_add", {yyc::ToReal(*droids), double(droidId)});
        }
        puppet::SetFamiliarOwner(id, puppet::kOwnSelf);
        g_own.push_back(id);
    }
    Log("familiars: %zu own droid(s) back from the last game", g_own.size());
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
    std::vector<FamiliarEntry> list;
    std::vector<int> alive;
    for (const int id : g_own) {
        yyc::CInstance* familiar = yyc::FindInstance(id);
        if (familiar == nullptr) {
            puppet::SetFamiliarOwner(id, puppet::kNoOwner);
            continue;
        }
        alive.push_back(id);
        const int object = static_cast<int>(yyc::GetBuiltinReal(familiar, yyc::va::YYVar_object_index));
        const double look = DroidLook(familiar, static_cast<int>(DroidId(id)));
        if (list.size() < kMaxFamiliars) list.push_back({object, static_cast<float>(look)});
    }
    g_own = alive;
    if (list == g_sent && g_steps % kResendSteps != 0) return;
    if (list.empty() && g_sent.empty()) return;
    if (net::Send(kFamiliars, list.data(), list.size() * sizeof(FamiliarEntry))) g_sent = list;
}

void HostTick(yyc::CInstance* localPlayer) {
    for (auto& [peer, copies] : g_copies) {
        if (!copies.empty() && puppet::Instance(peer) == nullptr) DestroyCopies(localPlayer, copies);
    }
}

void HostOnGuest(const std::string& name, net::PeerId peer) {
    std::ifstream in(character::GuestDataFile(name, kStoreExtension), std::ios::binary);
    const std::vector<uint8_t> stored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (stored.empty() || stored.size() % sizeof(FamiliarEntry) != 0) return;
    net::SendTo(peer, kRestore, stored.data(), stored.size());
    Log("familiars: guest '%s' gets back %zu familiar(s)", name.c_str(), stored.size() / sizeof(FamiliarEntry));
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kRestore) {
        const size_t size = message.payload.size();
        if (message.from != net::kHostPeer || size % sizeof(FamiliarEntry) != 0 ||
            size / sizeof(FamiliarEntry) > kMaxFamiliars) {
            return true;
        }
        g_restore.resize(size / sizeof(FamiliarEntry));
        if (size > 0) memcpy(g_restore.data(), message.payload.data(), size);
        g_restorePending = true;
        return true;
    }
    if (message.type != kFamiliars) return false;
    const size_t size = message.payload.size();
    if (message.from == net::kHostPeer || size % sizeof(FamiliarEntry) != 0 || size / sizeof(FamiliarEntry) > kMaxFamiliars) {
        return true;
    }
    std::vector<FamiliarEntry> list(size / sizeof(FamiliarEntry));
    if (size > 0) memcpy(list.data(), message.payload.data(), size);
    ApplyList(localPlayer, message.from, list);
    return true;
}

bool HasDroidLook(yyc::CInstance* self, int droidId) { return DroidLook(self, droidId) >= 0; }

void Reset() {
    g_own.clear();
    g_restore.clear();
    g_restorePending = false;
    g_sent.clear();
    g_steps = 0;
    g_copies.clear();
}

}  // namespace coop::familiars
