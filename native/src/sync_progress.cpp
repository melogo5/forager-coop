// Level and XP are shared and kept by the host (QUESTIONS.md): the guest's XP goes to the host, the shared progress
// comes back. Steam achievements are the host's only.
#include "sync.h"
#include "sync_internal.h"

#include <cstring>

#include "log.h"
#include "puppet.h"

namespace coop::sync {

namespace {

constexpr uint32_t kScriptGainXp = 0x00730600;
constexpr uint32_t kScriptUnlockAchievement = 0x004A6DA0;  // UnlockAchievement(name): Steam achievement

#pragma pack(push, 1)
struct XpMsg {
    float amount;
};

struct ProgressMsg {
    float level, xp, xpCap, xpToGain;
};
#pragma pack(pop)

yyc::ScriptFn g_origGainXp = nullptr;
yyc::ScriptFn g_origUnlockAchievement = nullptr;

// The shared progress lives in the host's player: level, xp, xpCap (XP for the next level), xpToGain (XP still
// being counted up). Skill points are each player's own (skills.h). The host sends it twice a second; the guest's
// player shows it.
constexpr int kProgressSlots[] = {0x603, 0x90E, 0x910, 0x912};  // decompiled/code/var_slots.tsv
constexpr unsigned kProgressEverySteps = 30;

// Host: XP earned while the puppet acts goes to the host's own player. Guest: XP is not added here but sent to the
// host; the shared progress comes back as kProgress.
yyc::RValue* __cdecl GainXpHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                yyc::RValue** args) {
    if (g_dryHit) return result;  // the hit's XP comes from the host
    // The guest's own run of world code (SpawnLand for an island someone bought, dirt growing) earns nothing: it
    // gave ~45 XP per island bought (playtest 4, bug 8).
    if (IsGuest() && g_suppressing > 0) return result;
    if (argc >= 1 && IsGuest() && Connected() && g_applyingRemote == 0) {
        const XpMsg msg{static_cast<float>(yyc::ToReal(*args[0]))};
        net::Send(kXp, &msg, sizeof(msg));
        return result;
    }
    if (!IsHost() || !puppet::IsActing()) return g_origGainXp(self, other, result, argc, args);
    const puppet::Acting acting = puppet::PauseActing();
    yyc::RValue* r = g_origGainXp(self, other, result, argc, args);
    puppet::ResumeActing(acting);
    return r;
}

// Steam achievements are the host's only (docs/coop/SPEC.md): the guest's game does not unlock any.
yyc::RValue* __cdecl UnlockAchievementHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result,
                                           int argc, yyc::RValue** args) {
    if (IsGuest() && Connected()) return result;
    return g_origUnlockAchievement(self, other, result, argc, args);
}

void GiveXp(yyc::CInstance* localPlayer, double amount) {
    yyc::RValue value = yyc::Real(amount);
    yyc::RValue* args[1] = {&value};
    yyc::RValue result{};
    g_origGainXp(localPlayer, localPlayer, &result, 1, args);
    yyc::Free(&result);
}

// Host: XP the guest earned, added to the shared progress.
void ApplyXp(yyc::CInstance* localPlayer, const XpMsg& msg) {
    constexpr float kMaxXpAtOnce = 1e6f;
    if (!(msg.amount >= 0 && msg.amount <= kMaxXpAtOnce)) return;  // NaN fails both
    Log("sync: +%.1f XP from the guest", msg.amount);
    GiveXp(localPlayer, msg.amount);
}

void ApplyProgress(yyc::CInstance* localPlayer, const ProgressMsg& msg) {
    static float shownLevel = -1;
    const int id = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    const float values[4] = {msg.level, msg.xp, msg.xpCap, msg.xpToGain};
    // A loaded character may keep these as integers: any number is overwritten; anything else (not set up yet)
    // is left alone.
    for (int i = 0; i < 4; ++i) {
        yyc::RValue current{};
        const bool number = yyc::GetVar(id, kProgressSlots[i], &current) && yyc::IsNumber(current);
        const uint32_t kind = current.kind & yyc::kKindMask;
        yyc::Free(&current);
        if (!number) {
            static bool told[4] = {};
            if (!told[i]) Log("sync: progress slot 0x%X is kind %u here - not set", kProgressSlots[i], kind);
            told[i] = true;
            continue;
        }
        yyc::RValue value = yyc::Real(values[i]);
        yyc::SetVar(id, kProgressSlots[i], &value);
    }
    if (msg.level != shownLevel) {
        Log("sync: shared progress: level %.0f, xp %.0f/%.0f (level here now %.0f)", msg.level, msg.xp, msg.xpCap,
            ReadVar(id, kProgressSlots[0]));
        shownLevel = msg.level;
    }
}

}  // namespace

namespace internal {

void GiveXp(yyc::CInstance* localPlayer, double amount) { sync::GiveXp(localPlayer, amount); }

}  // namespace internal

bool InstallProgressHooks() {
    bool ok = Hook(kScriptGainXp, reinterpret_cast<void*>(&GainXpHook), &g_origGainXp);
    ok = Hook(kScriptUnlockAchievement, reinterpret_cast<void*>(&UnlockAchievementHook), &g_origUnlockAchievement) &&
         ok;
    return ok;
}

void SendProgress(yyc::CInstance* localPlayer, unsigned steps) {
    if (steps % kProgressEverySteps != 0) return;
    const int id = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    float values[4] = {};
    for (int i = 0; i < 4; ++i) values[i] = ReadVar(id, kProgressSlots[i]);
    const ProgressMsg msg{values[0], values[1], values[2], values[3]};
    net::Send(kProgress, &msg, sizeof(msg));
}

bool HandleProgress(const net::Message& message, yyc::CInstance* localPlayer) {
    const size_t size = message.payload.size();
    if (message.type == kProgress) {
        if (size == sizeof(ProgressMsg) && IsGuest()) {
            ProgressMsg msg{};
            memcpy(&msg, message.payload.data(), sizeof(msg));
            ApplyProgress(localPlayer, msg);
        }
        return true;
    }
    if (message.type != kXp) return false;
    if (size == sizeof(XpMsg) && IsHost()) {
        XpMsg msg{};
        memcpy(&msg, message.payload.data(), sizeof(msg));
        ApplyXp(localPlayer, msg);
    }
    return true;
}

}  // namespace coop::sync
