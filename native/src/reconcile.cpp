#include "reconcile.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "chests.h"
#include "interiors.h"
#include "log.h"
#include "puppet.h"
#include "sync.h"

namespace coop::reconcile {

namespace {

constexpr uint8_t kReconcile = 21;  // host -> guest: Area, then Entry[]
constexpr uint8_t kWorldCheck = 22;  // host -> guest: CheckEntry[], the whole world (only compared and logged)
constexpr unsigned kWorldCheckEverySteps = 1800;  // 30 s
constexpr size_t kWorldCheckShown = 5;
constexpr unsigned kEverySteps = 300;
// World objects that stay put (decompiled/data/objects.tsv): parObject (rocks, trees, structures, special
// objects), parHerb (bushes, crops), objBridge (no parent).
constexpr int kKinds[] = {80, 81, 115};
constexpr int kMoving[] = {158, 201};  // objLocomotive, objMinecart: their positions differ between the sides
constexpr double kMargin = 32;         // the area keeps this far inside the guest's view
constexpr double kSlack = 16;          // the host lists this much more: objects on the area's edge
constexpr double kMatchDistance = 4;   // pixels, as sync matches objects
constexpr int kNoOne = yyc::kNoOne;
// Globals with the camera's bounds (decompiled/code/var_slots.tsv), as mobs.cpp reads them.
constexpr int kSlotCameraBottom = yyc::slot::cameraBottom;
constexpr int kSlotCameraLeft = yyc::slot::cameraLeft;
constexpr int kSlotCameraRight = yyc::slot::cameraRight;
constexpr int kSlotCameraTop = yyc::slot::cameraTop;

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
struct CheckEntry {
    int16_t object;
    int16_t x, y;
};
#pragma pack(pop)

struct Local {
    int id;
    int object;
    double x, y;
};

unsigned g_steps = 0;
size_t g_nextGuest = 0;  // host: whose surroundings the next check covers
unsigned g_checkSteps = 0;
using Spot = std::tuple<int, int, int>;  // object, x, y
std::set<Spot> g_lastHostOnly, g_lastHereOnly;  // guest: the last world check's differences
std::set<int> g_lastExtra;    // guest: local ids the host did not have at the last check
std::set<int> g_lastMissing;  // guest: host ids missing here at the last check

using yyc::CallReal;

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
        const int count = static_cast<int>(CallReal(self, "instance_number", {double(kind)}));
        for (int i = 0; i < count; ++i) {
            const int id = static_cast<int>(CallReal(self, "instance_find", {double(kind), double(i)}));
            yyc::CInstance* instance = id == kNoOne ? nullptr : yyc::FindInstance(id);
            if (instance == nullptr || interiors::IsInterior(id)) continue;  // only here, past the world's edge
            const int object = static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index));
            const double x = yyc::GetBuiltinReal(instance, yyc::va::YYVar_x);
            const double y = yyc::GetBuiltinReal(instance, yyc::va::YYVar_y);
            if (Moving(object) || chests::IsTreasureChest(object) || !Inside(area, x, y, grow)) continue;
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
    // Each of the host's objects stands for one here: a second copy on the same spot is extra too (a doubled
    // furnace or fish trap, PLAYTEST3.md #1-#3). The copy known as its replica is kept, else the first one.
    std::set<int> claimed, extra, missing;
    for (const Entry& e : host) {
        int match = 0;
        for (const Local& local : locals) {
            if (claimed.count(local.id) > 0 || !Same(local.object, local.x, local.y, e.object, e.x, e.y)) continue;
            if (match == 0 || sync::ReplicaOf(e.id) == local.id) match = local.id;
        }
        if (match != 0) claimed.insert(match);
        if (match != 0) {
            if (sync::ReplicaOf(e.id) == 0 && !sync::IsReplica(match)) sync::AdoptReplica(e.id, match);
        } else if (Inside(area, e.x, e.y, 0)) {
            missing.insert(e.id);
        }
    }
    for (const Local& local : locals) {
        if (claimed.count(local.id) == 0 && Inside(area, local.x, local.y, 0)) extra.insert(local.id);
    }
    size_t removed = 0, added = 0;
    for (const int id : extra) {
        if (g_lastExtra.count(id) == 0) continue;
        sync::RemoveLocal(self, id);
        ++removed;
    }
    for (const Entry& e : host) {
        // A destroy that came after the host's list was taken must not be undone.
        if (missing.count(e.id) == 0 || g_lastMissing.count(e.id) == 0 || sync::WasDestroyed(e.id)) continue;
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

std::string Describe(const std::set<Spot>& spots) {
    std::string text;
    size_t shown = 0;
    for (const auto& [object, x, y] : spots) {
        if (shown++ == kWorldCheckShown) break;
        text += " " + std::to_string(object) + "@(" + std::to_string(x) + "," + std::to_string(y) + ")";
    }
    return text;
}

// Guest: the host's whole world against ours. Only logged - reconcile repairs around the guest; this shows when and
// where the worlds part anywhere (a playtest's log is read afterwards). Differences seen in two checks in a row are
// the real ones: a spawn or destroy may be on its way.
// Objects bucketed by kind and a grid cell at least kMatchDistance wide: a match is in the same or a next cell. (Each
// host object against every one here was ~50 million comparisons on a big world, in one frame.)
int64_t BucketKey(int object, int cx, int cy) {
    return (static_cast<int64_t>(object) << 40) ^ (static_cast<int64_t>(cx & 0xFFFFF) << 20) ^ (cy & 0xFFFFF);
}
constexpr double kCell = 2 * kMatchDistance;

void CompareWorld(yyc::CInstance* self, const std::vector<CheckEntry>& host) {
    const std::vector<WorldObject> here = WorldObjects(self);
    std::vector<bool> used(here.size(), false);
    std::unordered_map<int64_t, std::vector<size_t>> buckets;
    for (size_t i = 0; i < here.size(); ++i) {
        buckets[BucketKey(here[i].object, static_cast<int>(std::floor(here[i].x / kCell)),
                          static_cast<int>(std::floor(here[i].y / kCell)))]
            .push_back(i);
    }
    std::set<Spot> hostOnly, hereOnly;
    for (const CheckEntry& e : host) {
        bool found = false;
        const int cx = static_cast<int>(std::floor(e.x / kCell)), cy = static_cast<int>(std::floor(e.y / kCell));
        for (int dx = -1; dx <= 1 && !found; ++dx) {
            for (int dy = -1; dy <= 1 && !found; ++dy) {
                const auto bucket = buckets.find(BucketKey(e.object, cx + dx, cy + dy));
                if (bucket == buckets.end()) continue;
                for (const size_t i : bucket->second) {
                    if (used[i] || std::abs(here[i].x - e.x) > kMatchDistance ||
                        std::abs(here[i].y - e.y) > kMatchDistance) {
                        continue;
                    }
                    used[i] = found = true;
                    break;
                }
            }
        }
        if (!found) hostOnly.insert({e.object, e.x, e.y});
    }
    for (size_t i = 0; i < here.size(); ++i) {
        if (!used[i]) {
            hereOnly.insert({here[i].object, static_cast<int>(std::lround(here[i].x)),
                             static_cast<int>(std::lround(here[i].y))});
        }
    }
    std::set<Spot> lastingHost, lastingHere;
    for (const Spot& s : hostOnly) {
        if (g_lastHostOnly.count(s) > 0) lastingHost.insert(s);
    }
    for (const Spot& s : hereOnly) {
        if (g_lastHereOnly.count(s) > 0) lastingHere.insert(s);
    }
    if (!lastingHost.empty() || !lastingHere.empty()) {
        Log("worldcheck: host %zu objects, here %zu; lasting: host only %zu%s, here only %zu%s", host.size(),
            here.size(), lastingHost.size(), Describe(lastingHost).c_str(), lastingHere.size(),
            Describe(lastingHere).c_str());
    } else {
        Log("worldcheck: host %zu objects, here %zu - the same (%zu/%zu in flight)", host.size(), here.size(),
            hostOnly.size(), hereOnly.size());
    }
    g_lastHostOnly = std::move(hostOnly);
    g_lastHereOnly = std::move(hereOnly);
}

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

std::vector<WorldObject> WorldObjects(yyc::CInstance* self) {
    // Every instance by id - FindInstance finds asleep ones too, whatever the cull lists say (an instance being woken
    // is on none of them and not active yet for a step). Ids count up from 100000; a long run of none is the end -
    // but never before the highest id created while we watched (a long session's burst of short-lived instances
    // was such a run, and the walk stopped short of the newest objects).
    constexpr int kFirstId = 100000, kEndGap = 20000;
    const int highest = sync::HighestCreatedId();
    std::vector<int8_t> kept(1024, 0);  // object index -> 0 unknown, 1 listed kind, -1 not
    std::vector<WorldObject> objects;
    for (int id = kFirstId, gap = 0; gap < kEndGap || id <= highest; ++id) {
        yyc::CInstance* instance = yyc::FindInstance(id);
        gap = instance == nullptr ? gap + 1 : 0;
        if (instance == nullptr || interiors::IsInterior(id)) continue;
        const int object = static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index));
        if (object < 0 || object >= static_cast<int>(kept.size())) continue;
        if (kept[object] == 0) {
            bool isKind = false;
            for (const int kind : kKinds) {
                isKind = isKind || object == kind ||
                         CallReal(self, "object_is_ancestor", {double(object), double(kind)}) != 0;
            }
            kept[object] = isKind && !Moving(object) && !chests::IsTreasureChest(object) ? 1 : -1;
        }
        if (kept[object] != 1) continue;
        objects.push_back({object, yyc::GetBuiltinReal(instance, yyc::va::YYVar_x),
                           yyc::GetBuiltinReal(instance, yyc::va::YYVar_y)});
    }
    return objects;
}

void HostTick(yyc::CInstance* localPlayer) {
    if (++g_checkSteps % kWorldCheckEverySteps == 0) {
        std::vector<uint8_t> payload;
        for (const WorldObject& o : WorldObjects(localPlayer)) {
            const CheckEntry e{static_cast<int16_t>(o.object), static_cast<int16_t>(std::lround(o.x)),
                               static_cast<int16_t>(std::lround(o.y))};
            const auto* bytes = reinterpret_cast<const uint8_t*>(&e);
            payload.insert(payload.end(), bytes, bytes + sizeof(e));
        }
        net::Send(kWorldCheck, payload.data(), payload.size());
    }
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
    if (message.type == kWorldCheck) {
        if (message.payload.size() % sizeof(CheckEntry) != 0) return true;
        std::vector<CheckEntry> host(message.payload.size() / sizeof(CheckEntry));
        if (!host.empty()) memcpy(host.data(), message.payload.data(), message.payload.size());
        CompareWorld(localPlayer, host);
        return true;
    }
    if (message.type != kReconcile) return false;
    const auto& p = message.payload;
    if (p.size() < sizeof(Area) || (p.size() - sizeof(Area)) % sizeof(Entry) != 0) return true;
    Area area{};
    memcpy(&area, p.data(), sizeof(area));
    std::vector<Entry> host((p.size() - sizeof(Area)) / sizeof(Entry));
    if (!host.empty()) memcpy(host.data(), p.data() + sizeof(Area), host.size() * sizeof(Entry));
    // What lies off the guest's screen is asleep (culled): wake the area up and look right before the next cull.
    sync::WakeRegion(localPlayer, area.left - kSlack, area.top - kSlack, area.right - area.left + 2 * kSlack,
                     area.bottom - area.top + 2 * kSlack);
    sync::RunBeforeCull([localPlayer, area, host] { Compare(localPlayer, area, host); });
    return true;
}

void Reset() {
    g_steps = 0;
    g_nextGuest = 0;
    g_lastExtra.clear();
    g_lastMissing.clear();
    g_checkSteps = 0;
    g_lastHostOnly.clear();
    g_lastHereOnly.clear();
}

}  // namespace coop::reconcile
