#include "mobs.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "net.h"
#include "puppet.h"
#include "sync.h"

namespace coop::mobs {

namespace {

constexpr uint8_t kMobs = 20;  // host -> guest
constexpr int kParEnemy = 23;
// Streamed like enemies: parEnemy, and parCritter (fireflies, butterflies, cows...; PLAYTEST.md #15).
constexpr int kStreamedKinds[] = {kParEnemy, 64};
constexpr unsigned kSendEverySteps = 6;  // 10 updates/s
constexpr size_t kMaxMobs = 2000;        // the count is a u16; more than this means something is wrong
constexpr double kFollow = 0.5;          // replica moves this fraction of the way per step
constexpr double kSnapDistance = 64;
constexpr double kAdoptDistance = 48;  // a guest enemy this close to an unknown host enemy is the same one
constexpr int kNoOne = -4;

// Globals with the host camera's bounds, used by CullObject/ProcessCulls (decompiled/code/var_slots.tsv).
constexpr int kSlotCameraBottom = 0xE0;
constexpr int kSlotCameraLeft = 0xE1;
constexpr int kSlotCameraRight = 0xE2;
constexpr int kSlotCameraTop = 0xE3;

constexpr uint32_t kScriptCullObject = 0x004D0DC0;
constexpr uint32_t kScriptProcessCulls = 0x004D21D0;
constexpr uint32_t kWorldControlEndStep = 0x008F4BD0;  // WorldControl Step_2

#pragma pack(push, 1)
struct MobState {
    int32_t id;
    int16_t object;
    int16_t pad;
    float x, y;
    int32_t sprite;
    float imageIndex;
    float xscale;
    float hp;  // so the replica's health bar shows the guest's hits (which land on the host)
};
#pragma pack(pop)
constexpr int kSlotHp = 0x573;

bool (*g_isHost)() = nullptr;
bool (*g_isGuest)() = nullptr;
unsigned g_steps = 0;
std::unordered_map<int, MobState> g_targets;  // guest: host id -> latest state
unsigned g_received = 0;
unsigned g_purgedAt = 0;  // guest: the update the last purge followed
unsigned g_applySteps = 0;
constexpr unsigned kTraceEverySteps = 120;  // log replica positions every ~2 s

yyc::ScriptFn g_origCullObject = nullptr;
yyc::ScriptFn g_origProcessCulls = nullptr;
yyc::EventFn g_origEndStep = nullptr;

double Builtin(yyc::CInstance* self, const char* name, std::initializer_list<double> args) {
    std::vector<yyc::RValue> values;
    for (const double a : args) values.push_back(yyc::Real(a));
    yyc::RValue result{};
    yyc::CallBuiltin(name, self, static_cast<int>(values.size()), values.data(), &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

// ---- host: keep the guest's surroundings simulated ----

struct CameraBounds {
    double left, right, top, bottom;
};

bool ReadCamera(CameraBounds* out) {
    const int slots[4] = {kSlotCameraLeft, kSlotCameraRight, kSlotCameraTop, kSlotCameraBottom};
    double* fields[4] = {&out->left, &out->right, &out->top, &out->bottom};
    for (int i = 0; i < 4; ++i) {
        const yyc::RValue* v = yyc::GlobalVar(slots[i]);
        if (v == nullptr || (v->kind & yyc::kKindMask) != yyc::kReal) return false;
        *fields[i] = v->real;
    }
    return true;
}

void WriteCamera(const CameraBounds& c) {
    const int slots[4] = {kSlotCameraLeft, kSlotCameraRight, kSlotCameraTop, kSlotCameraBottom};
    const double values[4] = {c.left, c.right, c.top, c.bottom};
    for (int i = 0; i < 4; ++i) {
        yyc::RValue* v = yyc::GlobalVar(slots[i]);
        if (v != nullptr) v->real = values[i];
    }
}

// The host keeps awake what any player sees: its own camera and a camera-sized frame around each guest. Not
// one box over everyone - players far apart would keep everything between them awake.
constexpr int kSlotActive = 0x4F;     // an instance's `active` (CullObject clears it when putting it to sleep)
constexpr double kSpareMargin = 64;   // CullObject also keeps an object's light in view: a little slack

std::vector<CameraBounds> GuestFrames(const CameraBounds& camera) {
    std::vector<CameraBounds> frames;
    if (g_isHost == nullptr || !g_isHost() || !net::IsConnected()) return frames;
    const double halfWidth = (camera.right - camera.left) / 2;
    const double halfHeight = (camera.bottom - camera.top) / 2;
    for (yyc::CInstance* guest : puppet::Instances()) {
        const double gx = yyc::GetBuiltinReal(guest, yyc::va::YYVar_x);
        const double gy = yyc::GetBuiltinReal(guest, yyc::va::YYVar_y);
        frames.push_back({gx - halfWidth, gx + halfWidth, gy - halfHeight, gy + halfHeight});
    }
    return frames;
}

// While the host's CullObject runs: the guests' frames, and the instances it would have put to sleep that a guest
// sees. For each such instance CullObject calls instance_deactivate_object, then ds_list_add twice (its cell's
// list, global.deactivatedInstances), then clears `active`: all of it is skipped or undone.
bool g_culling = false;
std::vector<CameraBounds> g_cullFrames;
std::vector<int> g_spared;
yyc::BuiltinFn g_origDeactivate = nullptr;
yyc::BuiltinFn g_origListAdd = nullptr;
bool g_cullBuiltinsTried = false;
size_t g_sparedLastStep = 0;  // instances kept awake for guests since the last LogCulling

bool SeenByGuest(int id) {
    yyc::CInstance* instance = yyc::FindInstance(id);
    if (instance == nullptr) return false;
    const double x = yyc::GetBuiltinReal(instance, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(instance, yyc::va::YYVar_y);
    for (const CameraBounds& f : g_cullFrames) {
        if (x >= f.left - kSpareMargin && x <= f.right + kSpareMargin && y >= f.top - kSpareMargin &&
            y <= f.bottom + kSpareMargin) {
            return true;
        }
    }
    return false;
}

void __cdecl DeactivateHook(yyc::RValue* result, yyc::CInstance* self, yyc::CInstance* other, int argc,
                            yyc::RValue* args) {
    if (g_culling && argc == 1) {
        const int id = static_cast<int>(yyc::ToReal(args[0]));
        if (SeenByGuest(id)) {
            g_spared.push_back(id);
            return;
        }
    }
    g_origDeactivate(result, self, other, argc, args);
}

void __cdecl ListAddHook(yyc::RValue* result, yyc::CInstance* self, yyc::CInstance* other, int argc,
                         yyc::RValue* args) {
    if (g_culling && argc == 2 && !g_spared.empty() && static_cast<int>(yyc::ToReal(args[1])) == g_spared.back()) {
        return;  // a spared instance is not filed as asleep
    }
    g_origListAdd(result, self, other, argc, args);
}

bool HookBuiltin(const char* name, void* detour, yyc::BuiltinFn* original) {
    const yyc::BuiltinFn target = yyc::BuiltinAddress(name);
    const bool ok = target != nullptr &&
                    MH_CreateHook(reinterpret_cast<void*>(target), detour, reinterpret_cast<void**>(original)) ==
                        MH_OK &&
                    MH_EnableHook(reinterpret_cast<void*>(target)) == MH_OK;
    if (!ok) Log("mobs: %s hook FAILED", name);
    return ok;
}

// The builtin table is filled once the game runs: hooked on first need.
bool CullBuiltinsReady() {
    if (!g_cullBuiltinsTried) {
        g_cullBuiltinsTried = true;
        const bool ok = HookBuiltin("instance_deactivate_object", reinterpret_cast<void*>(&DeactivateHook),
                                    &g_origDeactivate) &&
                        HookBuiltin("ds_list_add", reinterpret_cast<void*>(&ListAddHook), &g_origListAdd);
        Log("mobs: cull hooks %s", ok ? "enabled" : "FAILED");
    }
    return g_origDeactivate != nullptr && g_origListAdd != nullptr;
}

yyc::RValue* __cdecl CullObjectHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                    yyc::RValue** args) {
    sync::BeforeCull(self);
    CameraBounds camera{};
    std::vector<CameraBounds> frames = ReadCamera(&camera) ? GuestFrames(camera) : std::vector<CameraBounds>{};
    if (frames.empty() || !CullBuiltinsReady()) return g_origCullObject(self, other, result, argc, args);
    g_cullFrames = std::move(frames);
    g_spared.clear();
    g_culling = true;
    yyc::RValue* r = g_origCullObject(self, other, result, argc, args);
    g_culling = false;
    g_sparedLastStep += g_spared.size();
    for (const int id : g_spared) {
        yyc::RValue active = yyc::Real(1);
        yyc::SetVar(id, kSlotActive, &active);
    }
    return r;
}

// Every ~10 s: how many instances the guests keep awake, and how long the game's list of sleeping instances is
// (it must not grow while the players stand still).
void LogCulling(yyc::CInstance* self, unsigned steps) {
    constexpr unsigned kEvery = 600;
    constexpr int kSlotDeactivatedInstances = 0x16C;  // global.deactivatedInstances (var_slots.tsv)
    if (steps % kEvery != 0) return;
    const yyc::RValue* list = yyc::GlobalVar(kSlotDeactivatedInstances);
    const double asleep = list != nullptr ? Builtin(self, "ds_list_size", {yyc::ToReal(*list)}) : -1;
    Log("mobs: culling: %zu kept awake for guests in the last %u steps, %.0f asleep", g_sparedLastStep, kEvery,
        asleep);
    g_sparedLastStep = 0;
}

// Wakes what the host's camera shows, then what each guest's frame shows.
yyc::RValue* __cdecl ProcessCullsHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                      yyc::RValue** args) {
    yyc::RValue* r = g_origProcessCulls(self, other, result, argc, args);
    CameraBounds camera{};
    if (!ReadCamera(&camera)) return r;
    for (const CameraBounds& frame : GuestFrames(camera)) {
        WriteCamera(frame);
        yyc::RValue ignored{};
        g_origProcessCulls(self, other, &ignored, argc, args);
        yyc::Free(&ignored);
    }
    WriteCamera(camera);
    return r;
}

// ---- host: stream enemies ----

MobState Capture(yyc::CInstance* mob, int id) {
    MobState s{};
    s.id = id;
    s.object = static_cast<int16_t>(yyc::GetBuiltinReal(mob, yyc::va::YYVar_object_index));
    s.x = static_cast<float>(yyc::GetBuiltinReal(mob, yyc::va::YYVar_x));
    s.y = static_cast<float>(yyc::GetBuiltinReal(mob, yyc::va::YYVar_y));
    s.sprite = static_cast<int32_t>(yyc::GetBuiltinReal(mob, yyc::va::YYVar_sprite_index));
    s.imageIndex = static_cast<float>(yyc::GetBuiltinReal(mob, yyc::va::YYVar_image_index));
    s.xscale = static_cast<float>(yyc::GetBuiltinReal(mob, yyc::va::YYVar_image_xscale));
    yyc::RValue hp{};
    yyc::GetVar(id, kSlotHp, &hp);
    s.hp = static_cast<float>(yyc::ToReal(hp));
    yyc::Free(&hp);
    return s;
}

// ---- guest: follow the host ----

int AdoptOrSpawn(yyc::CInstance* self, const MobState& s) {
    const int nearest = static_cast<int>(Builtin(self, "instance_nearest", {s.x, s.y, double(s.object)}));
    if (nearest != kNoOne && !sync::IsReplica(nearest)) {
        if (yyc::CInstance* mob = yyc::FindInstance(nearest)) {
            const double dx = yyc::GetBuiltinReal(mob, yyc::va::YYVar_x) - s.x;
            const double dy = yyc::GetBuiltinReal(mob, yyc::va::YYVar_y) - s.y;
            if (std::sqrt(dx * dx + dy * dy) <= kAdoptDistance) {
                sync::AdoptReplica(s.id, nearest);
                return nearest;
            }
        }
    }
    return sync::SpawnReplica(self, s.id, s.object, s.x, s.y, 0);
}

void Follow(yyc::CInstance* mob, const MobState& s) {
    const double x = yyc::GetBuiltinReal(mob, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(mob, yyc::va::YYVar_y);
    const double dx = s.x - x;
    const double dy = s.y - y;
    const bool snap = std::sqrt(dx * dx + dy * dy) > kSnapDistance;
    yyc::SetBuiltinReal(mob, yyc::va::YYVar_x, snap ? s.x : x + dx * kFollow);
    yyc::SetBuiltinReal(mob, yyc::va::YYVar_y, snap ? s.y : y + dy * kFollow);
    yyc::SetBuiltinReal(mob, yyc::va::YYVar_sprite_index, s.sprite);
    yyc::SetBuiltinReal(mob, yyc::va::YYVar_image_index, s.imageIndex);
    yyc::SetBuiltinReal(mob, yyc::va::YYVar_image_xscale, s.xscale);
    yyc::RValue hp = yyc::Real(s.hp);
    yyc::SetVar(static_cast<int>(yyc::GetBuiltinReal(mob, yyc::va::YYVar_id)), kSlotHp, &hp);
}

// Guest: enemies are exactly what the host streams. Any other active enemy here - one from the loaded save that
// the host does not have (dead or asleep there), or a replica the host stopped sending (it fell asleep there,
// and its own AI would walk it around) - is a ghost the guest could see and hit for nothing: it goes.
void PurgeGhosts(yyc::CInstance* self, const std::vector<int>& followed) {
    std::vector<int> ghosts;
    for (const int kind : kStreamedKinds) {
        const int count = static_cast<int>(Builtin(self, "instance_number", {double(kind)}));
        for (int i = 0; i < count; ++i) {
            const int id = static_cast<int>(Builtin(self, "instance_find", {double(kind), double(i)}));
            if (id != kNoOne && std::find(followed.begin(), followed.end(), id) == followed.end()) ghosts.push_back(id);
        }
    }
    for (const int id : ghosts) sync::RemoveLocal(self, id);
    if (!ghosts.empty()) Log("mobs: removed %zu enemies the host does not have", ghosts.size());
}

// After every Step event: whatever the replicas' own AI did, they stand where the host says.
void ApplyTargets(yyc::CInstance* self) {
    std::vector<int> followed;
    for (auto it = g_targets.begin(); it != g_targets.end();) {
        const MobState& s = it->second;
        int local = sync::ReplicaOf(s.id);
        if (local == 0 && !sync::WasDestroyed(s.id)) local = AdoptOrSpawn(self, s);
        yyc::CInstance* mob = local != 0 ? yyc::FindInstance(local) : nullptr;
        if (mob == nullptr) {
            it = g_targets.erase(it);  // gone here (destroyed by the host); a new update brings it back
            continue;
        }
        Follow(mob, s);
        followed.push_back(local);
        if (g_applySteps % kTraceEverySteps == 0) {
            Log("mobs: host %d at (%.0f, %.0f) -> replica %d at (%.0f, %.0f)", s.id, s.x, s.y, local,
                yyc::GetBuiltinReal(mob, yyc::va::YYVar_x), yyc::GetBuiltinReal(mob, yyc::va::YYVar_y));
        }
        ++it;
    }
    if (g_purgedAt != g_received) {  // once per update, right after it is applied
        g_purgedAt = g_received;
        PurgeGhosts(self, followed);
    }
    ++g_applySteps;
}

void __cdecl EndStepHook(yyc::CInstance* self, yyc::CInstance* other) {
    g_origEndStep(self, other);
    if (g_isGuest == nullptr || !g_isGuest() || (g_targets.empty() && g_purgedAt == g_received)) return;
    __try {
        ApplyTargets(self);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("mobs: apply crashed (code 0x%08lX)", GetExceptionCode());
        g_targets.clear();
    }
}

bool Hook(uint32_t va, void* detour, void* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original));
    if (status != MH_OK) Log("mobs: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

}  // namespace

bool InstallHooks(bool (*isHost)(), bool (*isGuest)()) {
    g_isHost = isHost;
    g_isGuest = isGuest;
    bool ok = Hook(kScriptCullObject, reinterpret_cast<void*>(&CullObjectHook), &g_origCullObject);
    ok = Hook(kScriptProcessCulls, reinterpret_cast<void*>(&ProcessCullsHook), &g_origProcessCulls) && ok;
    ok = Hook(kWorldControlEndStep, reinterpret_cast<void*>(&EndStepHook), &g_origEndStep) && ok;
    return ok;
}

// Testing aid: COOP_DEBUG_ENEMY=<object index> makes the host spawn that enemy next to the guest
// ~10 s after the guest connects.
void DebugSpawnEnemy(yyc::CInstance* localPlayer) {
    char value[16] = {};
    if (GetEnvironmentVariableA("COOP_DEBUG_ENEMY", value, sizeof(value)) == 0) return;
    const std::vector<yyc::CInstance*> guests = puppet::Instances();
    if (guests.empty()) return;
    yyc::CInstance* guest = guests.front();
    const double x = yyc::GetBuiltinReal(guest, yyc::va::YYVar_x) + 64;
    const double y = yyc::GetBuiltinReal(guest, yyc::va::YYVar_y);
    const int id = static_cast<int>(Builtin(localPlayer, "instance_create_depth", {x, y, -y, double(atoi(value))}));
    Log("mobs: debug enemy %s spawned as %d at (%.0f, %.0f)", value, id, x, y);
}

void HostTick(yyc::CInstance* localPlayer) {
    constexpr unsigned kDebugSpawnStep = 600;
    if (++g_steps == kDebugSpawnStep) DebugSpawnEnemy(localPlayer);
    LogCulling(localPlayer, g_steps);
    if (g_steps % kSendEverySteps != 0) return;
    std::vector<uint8_t> payload(2);
    uint16_t sent = 0;
    for (const int kind : kStreamedKinds) {
        const int count = static_cast<int>(Builtin(localPlayer, "instance_number", {double(kind)}));
        for (int i = 0; i < count && sent < kMaxMobs; ++i) {
            const int id = static_cast<int>(Builtin(localPlayer, "instance_find", {double(kind), double(i)}));
            yyc::CInstance* mob = yyc::FindInstance(id);
            if (mob == nullptr) continue;
            const MobState s = Capture(mob, id);
            const auto* bytes = reinterpret_cast<const uint8_t*>(&s);
            payload.insert(payload.end(), bytes, bytes + sizeof(s));
            ++sent;
        }
    }
    payload[0] = static_cast<uint8_t>(sent & 0xFF);
    payload[1] = static_cast<uint8_t>(sent >> 8);
    net::Send(kMobs, payload.data(), payload.size());
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kMobs) return false;
    const auto& p = message.payload;
    if (p.size() < 2 || g_isGuest == nullptr || !g_isGuest()) return true;
    const size_t count = p[0] | (p[1] << 8);
    if (p.size() != 2 + count * sizeof(MobState)) return true;
    g_targets.clear();  // the host sends all its active enemies every time
    for (size_t i = 0; i < count; ++i) {
        MobState s{};
        memcpy(&s, p.data() + 2 + i * sizeof(MobState), sizeof(s));
        g_targets[s.id] = s;
    }
    if (++g_received % 100 == 1) Log("mobs: update #%u with %zu enemies", g_received, count);
    return true;
}

void Reset() {
    g_targets.clear();
    g_received = 0;
    g_purgedAt = 0;
}

}  // namespace coop::mobs
