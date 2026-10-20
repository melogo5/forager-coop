#include "reconcile.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <set>
#include <utility>
#include <vector>

#include "log.h"
#include "puppet.h"
#include "sync.h"

namespace coop::reconcile {

namespace {

constexpr uint8_t kReconcile = 21;  // host -> guest: Area, then Entry[]
constexpr unsigned kEverySteps = 300;
// World objects that stay put (decompiled/data/objects.tsv): parObject (rocks, trees, structures, special
// objects), parHerb (bushes, crops), objBridge (no parent).
constexpr int kKinds[] = {80, 81, 115};
constexpr int kMoving[] = {158, 201};  // objLocomotive, objMinecart: their positions differ between the sides
constexpr double kMargin = 32;         // the area keeps this far inside the guest's view
constexpr double kSlack = 16;          // the host lists this much more: objects on the area's edge
constexpr double kMatchDistance = 4;   // pixels, as sync matches objects
constexpr int kNoOne = -4;
// Globals with the camera's bounds (decompiled/code/var_slots.tsv), as mobs.cpp reads them.
constexpr int kSlotCameraBottom = 0xE0;
constexpr int kSlotCameraLeft = 0xE1;
constexpr int kSlotCameraRight = 0xE2;
constexpr int kSlotCameraTop = 0xE3;

#pragma pack(push, 1)
struct Area {
    float left, top, right, bottom;
};
struct Entry {
    int32_t id;  // the host's instance
    int32_t object;
    float x, y, depth;
    float sprite, image;  // the look it rolled (ore vs plain rock...)
};
#pragma pack(pop)

struct Local {
    int id;
    int object;
    double x, y;
};

unsigned g_steps = 0;
size_t g_nextGuest = 0;  // host: whose surroundings the next check covers
std::set<int> g_lastExtra;    // guest: local ids the host did not have at the last check
std::set<int> g_lastMissing;  // guest: host ids missing here at the last check

double Builtin(yyc::CInstance* self, const char* name, std::initializer_list<double> args) {
    std::vector<yyc::RValue> values;
    for (const double a : args) values.push_back(yyc::Real(a));
    yyc::RValue result{};
    yyc::CallBuiltin(name, self, static_cast<int>(values.size()), values.data(), &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

double GlobalReal(int slot) {
    const yyc::RValue* value = yyc::GlobalVar(slot);
    return value != nullptr && yyc::IsNumber(*value) ? yyc::ToReal(*value) : 0;
}

bool Inside(const Area& a, double x, double y, double grow) {
    return x >= a.left - grow && x <= a.right + grow && y >= a.top - grow && y <= a.bottom + grow;
}

bool Moving(int object) {
    for (const int m : kMoving) {
        if (object == m) return true;
    }
    return false;
}

// The active world objects inside `area` (grown by `grow`).
template <typename Visit>
void ForEachObject(yyc::CInstance* self, const Area& area, double grow, Visit visit) {
    for (const int kind : kKinds) {
        const int count = static_cast<int>(Builtin(self, "instance_number", {double(kind)}));
        for (int i = 0; i < count; ++i) {
            const int id = static_cast<int>(Builtin(self, "instance_find", {double(kind), double(i)}));
            yyc::CInstance* instance = id == kNoOne ? nullptr : yyc::FindInstance(id);
            if (instance == nullptr) continue;
            const int object = static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index));
            const double x = yyc::GetBuiltinReal(instance, yyc::va::YYVar_x);
            const double y = yyc::GetBuiltinReal(instance, yyc::va::YYVar_y);
            if (Moving(object) || !Inside(area, x, y, grow)) continue;
            visit(instance, id, object, x, y);
        }
    }
}

bool Same(int objectA, double xa, double ya, int objectB, double xb, double yb) {
    return objectA == objectB && std::hypot(xa - xb, ya - yb) <= kMatchDistance;
}

// Guest, right before the game culls (the woken area is active then).
void Compare(yyc::CInstance* self, const Area& area, const std::vector<Entry>& host) {
    std::vector<Local> locals;
    ForEachObject(self, area, kSlack, [&locals](yyc::CInstance*, int id, int object, double x, double y) {
        locals.push_back({id, object, x, y});
    });
    std::set<int> extra, missing;
    for (const Local& local : locals) {
        if (!Inside(area, local.x, local.y, 0)) continue;
        bool known = false;
        for (const Entry& e : host) known = known || Same(local.object, local.x, local.y, e.object, e.x, e.y);
        if (!known) extra.insert(local.id);
    }
    for (const Entry& e : host) {
        int match = 0;
        for (const Local& local : locals) {
            if (match == 0 && Same(local.object, local.x, local.y, e.object, e.x, e.y)) match = local.id;
        }
        if (match != 0) {
            if (sync::ReplicaOf(e.id) == 0 && !sync::IsReplica(match)) sync::AdoptReplica(e.id, match);
        } else if (Inside(area, e.x, e.y, 0)) {
            missing.insert(e.id);
        }
    }
    size_t removed = 0, added = 0;
    for (const int id : extra) {
        if (g_lastExtra.count(id) == 0) continue;
        sync::RemoveLocal(self, id);
        ++removed;
    }
    for (const Entry& e : host) {
        if (missing.count(e.id) == 0 || g_lastMissing.count(e.id) == 0) continue;
        const int id = sync::SpawnReplica(self, e.id, e.object, e.x, e.y, e.depth);
        yyc::CInstance* replica = id != 0 ? yyc::FindInstance(id) : nullptr;
        if (replica != nullptr && e.sprite >= 0) {
            yyc::SetBuiltinReal(replica, yyc::va::YYVar_sprite_index, e.sprite);
            yyc::SetBuiltinReal(replica, yyc::va::YYVar_image_index, e.image);
        }
        if (replica != nullptr) ++added;
    }
    if (!extra.empty() || !missing.empty()) {
        Log("reconcile: %zu objects at the host, %zu here; %zu extra, %zu missing; removed %zu, added %zu",
            host.size(), locals.size(), extra.size(), missing.size(), removed, added);
    }
    g_lastExtra = std::move(extra);
    g_lastMissing = std::move(missing);
}

}  // namespace

void DebugTick(yyc::CInstance* localPlayer) {
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_DESYNC", nullptr, 0) > 0;
    static unsigned steps = 0;
    if (!enabled || ++steps != 480) return;
    const std::vector<yyc::CInstance*> puppets = puppet::Instances();
    yyc::CInstance* guest = !puppets.empty() ? puppets.front() : localPlayer;
    const double gx = yyc::GetBuiltinReal(guest, yyc::va::YYVar_x);
    const double gy = yyc::GetBuiltinReal(guest, yyc::va::YYVar_y);
    const Area around{static_cast<float>(gx - 80), static_cast<float>(gy - 60), static_cast<float>(gx + 80),
                      static_cast<float>(gy + 60)};
    std::vector<std::pair<double, Local>> nearby;
    ForEachObject(localPlayer, around, 0, [&](yyc::CInstance*, int id, int object, double x, double y) {
        nearby.push_back({std::hypot(x - gx, y - gy), Local{id, object, x, y}});
    });
    std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    // The host removes the nearest three, the guest the next three: both directions get repaired.
    const size_t first = !puppets.empty() ? 0 : 3;
    for (size_t i = first; i < first + 3 && i < nearby.size(); ++i) {
        const Local& l = nearby[i].second;
        Log("reconcile: debug removes object %d at (%.0f, %.0f) here only", l.object, l.x, l.y);
        sync::RemoveLocal(localPlayer, l.id);
    }
}

void HostTick(yyc::CInstance* localPlayer) {
    // The guests in turn, each checked every kEverySteps: with more guests the checks come more often, spread out.
    const std::vector<net::PeerId> peers = net::Peers();
    if (peers.empty()) return;
    if (++g_steps % (std::max)(1u, kEverySteps / static_cast<unsigned>(peers.size())) != 0) return;
    const net::PeerId peer = peers[g_nextGuest++ % peers.size()];
    yyc::CInstance* guest = puppet::Instance(peer);
    if (guest == nullptr) return;
    const double halfWidth = (GlobalReal(kSlotCameraRight) - GlobalReal(kSlotCameraLeft)) / 2 - kMargin;
    const double halfHeight = (GlobalReal(kSlotCameraBottom) - GlobalReal(kSlotCameraTop)) / 2 - kMargin;
    if (halfWidth <= 0 || halfHeight <= 0) return;
    const double gx = yyc::GetBuiltinReal(guest, yyc::va::YYVar_x);
    const double gy = yyc::GetBuiltinReal(guest, yyc::va::YYVar_y);
    const Area area{static_cast<float>(gx - halfWidth), static_cast<float>(gy - halfHeight),
                    static_cast<float>(gx + halfWidth), static_cast<float>(gy + halfHeight)};
    std::vector<uint8_t> payload(sizeof(area));
    memcpy(payload.data(), &area, sizeof(area));
    ForEachObject(localPlayer, area, kSlack, [&payload](yyc::CInstance* instance, int id, int object, double x, double y) {
        const Entry e{id,
                      object,
                      static_cast<float>(x),
                      static_cast<float>(y),
                      static_cast<float>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_depth)),
                      static_cast<float>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_sprite_index)),
                      static_cast<float>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_image_index))};
        const auto* bytes = reinterpret_cast<const uint8_t*>(&e);
        payload.insert(payload.end(), bytes, bytes + sizeof(e));
    });
    net::SendTo(peer, kReconcile, payload.data(), payload.size());
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kReconcile) return false;
    const auto& p = message.payload;
    if (p.size() < sizeof(Area) || (p.size() - sizeof(Area)) % sizeof(Entry) != 0) return true;
    Area area{};
    memcpy(&area, p.data(), sizeof(area));
    std::vector<Entry> host((p.size() - sizeof(Area)) / sizeof(Entry));
    if (!host.empty()) memcpy(host.data(), p.data() + sizeof(Area), host.size() * sizeof(Entry));
    // What lies off the guest's screen is asleep (culled): wake the area up and look right before the next cull.
    Builtin(localPlayer, "instance_activate_region",
            {area.left - kSlack, area.top - kSlack, area.right - area.left + 2 * kSlack,
             area.bottom - area.top + 2 * kSlack, 1});
    sync::RunBeforeCull([localPlayer, area, host] { Compare(localPlayer, area, host); });
    return true;
}

void Reset() {
    g_steps = 0;
    g_nextGuest = 0;
    g_lastExtra.clear();
    g_lastMissing.clear();
}

}  // namespace coop::reconcile
