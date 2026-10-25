#include "unlocks.h"

#include <windows.h>

#include <cstring>
#include <initializer_list>
#include <vector>

#include "log.h"

namespace coop::unlocks {

namespace {

constexpr uint8_t kUnlocks = 70;  // either way: u16 structure indices unlocked on the sender's side
constexpr unsigned kEverySteps = 300;
constexpr int kObjData = 2;             // the Data object (decompiled/data/objects.tsv)
constexpr int kSlotStructures = 0x83E;  // Data.structures[]: one entry per structure
constexpr int kFieldUnlocked = 11;      // StructureGet(s, 11): unlocked (StructureEdit(s, 11, true) in objSkill)
constexpr int kMaxStructures = 1024;
constexpr uint32_t kScriptStructureGet = 0x0070DD60;
constexpr uint32_t kScriptStructureEdit = yyc::script::StructureEdit;
constexpr int kNoOne = yyc::kNoOne;

std::vector<uint16_t> g_lastSent;
unsigned g_steps = 0;

using yyc::CallReal;

int StructureCount(yyc::CInstance* self) {
    const int data = static_cast<int>(CallReal(self, "instance_find", {kObjData, 0}));
    if (data == kNoOne) return 0;
    yyc::RValue structures{};
    if (!yyc::GetVar(data, kSlotStructures, &structures)) return 0;
    int count = 0;
    if ((structures.kind & yyc::kKindMask) == yyc::kArray) {
        yyc::RValue length{};
        yyc::CallBuiltin("array_length_1d", self, 1, &structures, &length);
        count = static_cast<int>(yyc::ToReal(length));
        yyc::Free(&length);
    }
    yyc::Free(&structures);
    return count < kMaxStructures ? count : kMaxStructures;
}

bool IsUnlocked(yyc::CInstance* self, int structure) {
    yyc::RValue values[2] = {yyc::Real(structure), yyc::Real(kFieldUnlocked)};
    yyc::RValue* args[2] = {&values[0], &values[1]};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kScriptStructureGet)(self, self, &result, 2, args);
    const bool unlocked = yyc::ToReal(result) != 0;
    yyc::Free(&result);
    return unlocked;
}

void Unlock(yyc::CInstance* self, int structure) {
    yyc::RValue values[3] = {yyc::Real(structure), yyc::Real(kFieldUnlocked), yyc::Real(1)};
    yyc::RValue* args[3] = {&values[0], &values[1], &values[2]};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kScriptStructureEdit)(self, self, &result, 3, args);
    yyc::Free(&result);
}

}  // namespace

void Tick(yyc::CInstance* localPlayer) {
    if (++g_steps % kEverySteps != 0) return;
    // Testing aid: COOP_DEBUG_UNLOCKS=1 tells the peer every structure is unlocked (this side is unchanged).
    static const bool everything = GetEnvironmentVariableA("COOP_DEBUG_UNLOCKS", nullptr, 0) > 0;
    std::vector<uint16_t> unlocked;
    const int count = StructureCount(localPlayer);
    for (int s = 0; s < count; ++s) {
        if (everything || IsUnlocked(localPlayer, s)) unlocked.push_back(static_cast<uint16_t>(s));
    }
    if (unlocked.empty() || unlocked == g_lastSent) return;
    if (net::Send(kUnlocks, unlocked.data(), unlocked.size() * sizeof(uint16_t))) {
        Log("unlocks: sent %zu unlocked structures of %d", unlocked.size(), count);
        g_lastSent = std::move(unlocked);
    }
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kUnlocks) return false;
    const size_t n = message.payload.size() / sizeof(uint16_t);
    const int count = StructureCount(localPlayer);
    size_t added = 0;
    for (size_t i = 0; i < n; ++i) {
        uint16_t s = 0;
        memcpy(&s, message.payload.data() + i * sizeof(uint16_t), sizeof(s));
        if (static_cast<int>(s) >= count || IsUnlocked(localPlayer, s)) continue;
        Unlock(localPlayer, s);
        ++added;
    }
    if (added > 0) Log("unlocks: the other player has %zu structures unlocked; %zu were new here", n, added);
    return true;
}

void Reset() {
    g_lastSent.clear();
    g_steps = 0;
}

}  // namespace coop::unlocks
