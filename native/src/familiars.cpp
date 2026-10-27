#include "familiars.h"

#include <cstring>
#include <map>
#include <vector>

#include "log.h"
#include "puppet.h"

namespace coop::familiars {

namespace {

constexpr uint8_t kFamiliars = 34;  // guest -> host: FamiliarEntry per own familiar
constexpr int kParFamiliar = 74;    // decompiled/data/objects.tsv: objBossKey 75, lantern 76, halo 77, objDroid 78
constexpr int kObjBossKey = 75;     // a key flying to its chest, not anyone's companion
constexpr int kSlotDroidId = 0x1B7;
constexpr int kSlotDroidNames = 0x1B8;  // global: each droid's look (a sprite) by droidId
constexpr int kMaxDroidLooks = 256;
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

int DroidLookCount(yyc::CInstance* self) {
    const yyc::RValue* looks = yyc::GlobalVar(kSlotDroidNames);
    if (looks == nullptr || (looks->kind & yyc::kKindMask) != yyc::kArray) return 0;
    yyc::RValue copy = *looks;  // a plain copy: no reference taken or freed
    yyc::RValue length{};
    yyc::CallBuiltin("array_length_1d", self, 1, &copy, &length);
    const int count = static_cast<int>(yyc::ToReal(length));
    yyc::Free(&length);
    return count;
}

double DroidLook(yyc::CInstance* self, int droidId) {
    if (droidId < 0 || droidId >= DroidLookCount(self)) return -1;
    const yyc::RValue look = yyc::ArrayElement(yyc::GlobalVar(kSlotDroidNames), droidId);
    return yyc::IsNumber(look) ? yyc::ToReal(look) : -1;
}

// The host's droidId for a look: an entry of its global.droidNames with it, else one added at the end (the
// runtime grows an array accessed one past its end). -1 if neither.
int DroidIdForLook(yyc::CInstance* self, double look) {
    const int count = DroidLookCount(self);
    for (int i = 0; i < count; ++i) {
        if (DroidLook(self, i) == look) return i;
    }
    yyc::RValue* looks = yyc::GlobalVar(kSlotDroidNames);
    if (looks == nullptr || (looks->kind & yyc::kKindMask) != yyc::kArray || count >= kMaxDroidLooks) return -1;
    using ArrayIndexFn = yyc::RValue*(__thiscall*)(yyc::RValue* array, int index);
    yyc::RValue* added = yyc::At<ArrayIndexFn>(yyc::va::ArrayIndex)(looks, count);
    if (added == nullptr) return -1;
    *added = yyc::Real(look);
    return count;
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
        puppet::SetFamiliarOwner(id, from);
        copies.push_back(id);
    }
    Log("familiars: guest %u has %zu of its own - copies made", from, copies.size());
}

}  // namespace

void OnCreated(yyc::CInstance* self, int createdId, int object) {
    if (!IsFamiliar(self, object) || !CreatedByGainItem()) return;
    g_own.push_back(createdId);
    puppet::SetFamiliarOwner(createdId, puppet::kOwnSelf);
    Log("familiars: own familiar %d (object %d) - follows us, sent to the host", createdId, object);
}

void GuestTick() {
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

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
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
    g_sent.clear();
    g_steps = 0;
    g_copies.clear();
}

}  // namespace coop::familiars
