#include "quests.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"
#include "sync.h"

namespace coop::quests {

namespace {

constexpr uint8_t kQuest = 91;  // either way: object, quest, state, hasSpoken
constexpr unsigned kEverySteps = 60;
constexpr int kParNPC = 47;  // decompiled/data/objects.tsv
constexpr int kNoOne = -4;
constexpr int kSlotQuest = 0x739;  // decompiled/code/var_slots.tsv
constexpr int kSlotState = 0x825;
constexpr int kSlotHasSpoken = 0x55A;
constexpr uint32_t kScriptNPCSetQuest = 0x007157E0;  // NPCSetQuest(name), on the NPC
constexpr uint32_t kScriptNPCSetState = 0x00715ED0;  // NPCSetState(name), on the NPC

// One NPC's quest: quest and state names (empty if not a string) and hasSpoken.
struct Quest {
    std::string quest, state;
    double spoken = 0;
    bool operator==(const Quest& o) const { return quest == o.quest && state == o.state && spoken == o.spoken; }
};

std::unordered_map<int, Quest> g_seen;  // NPC object index -> what it looked like when last sent/received
unsigned g_steps = 0;

double CallReal(yyc::CInstance* self, const char* builtin, std::initializer_list<double> values) {
    yyc::RValue args[4] = {};
    int argc = 0;
    for (const double v : values) args[argc++] = yyc::Real(v);
    yyc::RValue result{};
    yyc::CallBuiltin(builtin, self, argc, args, &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

std::string ReadString(int id, int slot) {
    yyc::RValue value{};
    if (!yyc::GetVar(id, slot, &value)) return {};
    std::string text = yyc::ToString(value);
    yyc::Free(&value);
    return text;
}

Quest Read(int id) {
    Quest q;
    q.quest = ReadString(id, kSlotQuest);
    q.state = ReadString(id, kSlotState);
    yyc::RValue spoken{};
    if (yyc::GetVar(id, kSlotHasSpoken, &spoken)) q.spoken = yyc::ToReal(spoken);
    yyc::Free(&spoken);
    return q;
}

void CallOn(yyc::CInstance* npc, uint32_t script, const std::string& name) {
    yyc::RValue arg = yyc::String(name.c_str());
    yyc::RValue* args[1] = {&arg};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(script)(npc, npc, &result, 1, args);
    yyc::Free(&result);
    yyc::Free(&arg);
}

void Put16(std::vector<uint8_t>& out, const std::string& text) {
    const uint16_t n = static_cast<uint16_t>(text.size());
    out.push_back(static_cast<uint8_t>(n & 0xFF));
    out.push_back(static_cast<uint8_t>(n >> 8));
    out.insert(out.end(), text.begin(), text.end());
}

bool Get16(const std::vector<uint8_t>& in, size_t* at, std::string* text) {
    if (*at + 2 > in.size()) return false;
    const size_t n = in[*at] | (in[*at + 1] << 8);
    *at += 2;
    if (*at + n > in.size()) return false;
    text->assign(in.begin() + *at, in.begin() + *at + n);
    *at += n;
    return true;
}

void Send(int object, const Quest& q) {
    std::vector<uint8_t> payload(4);
    memcpy(payload.data(), &object, 4);
    Put16(payload, q.quest);
    Put16(payload, q.state);
    const auto* spoken = reinterpret_cast<const uint8_t*>(&q.spoken);
    payload.insert(payload.end(), spoken, spoken + sizeof(q.spoken));
    net::Send(kQuest, payload.data(), payload.size());
}

void Apply(yyc::CInstance* localPlayer, int object, const Quest& q) {
    const int id = static_cast<int>(CallReal(localPlayer, "instance_find", {double(object), 0}));
    yyc::CInstance* npc = id == kNoOne ? nullptr : yyc::FindInstance(id);
    if (npc == nullptr) {
        Log("quests: no NPC %d here for the peer's quest", object);
        return;
    }
    const Quest before = Read(id);
    if (!q.quest.empty() && q.quest != before.quest) CallOn(npc, kScriptNPCSetQuest, q.quest);
    if (!q.state.empty() && q.state != Read(id).state) CallOn(npc, kScriptNPCSetState, q.state);
    yyc::RValue spoken = yyc::Real(q.spoken);
    yyc::SetVar(id, kSlotHasSpoken, &spoken);
    g_seen[object] = Read(id);
    Log("quests: NPC %d now '%s' / '%s' (was '%s' / '%s')", object, g_seen[object].quest.c_str(),
        g_seen[object].state.c_str(), before.quest.c_str(), before.state.c_str());
}

// Testing aid: COOP_DEBUG_QUEST=1 marks the first NPC here as spoken to (or not) and sets its current state again
// (NPCSetState) ~8 s in - the path a delivered quest takes; =<seconds> does it that late.
void DebugQuest(yyc::CInstance* localPlayer) {
    static const unsigned atStep = [] {
        char value[16] = {};
        if (GetEnvironmentVariableA("COOP_DEBUG_QUEST", value, sizeof(value)) == 0) return 0u;
        const int seconds = atoi(value);
        return seconds > 1 ? static_cast<unsigned>(seconds) * 60 : 480u;
    }();
    if (atStep == 0 || g_steps != atStep) return;
    const int id = static_cast<int>(CallReal(localPlayer, "instance_find", {kParNPC, 0}));
    yyc::CInstance* npc = id == kNoOne ? nullptr : yyc::FindInstance(id);
    if (npc == nullptr) {
        Log("quests: debug: no NPC here");
        return;
    }
    const Quest q = Read(id);
    if (!q.state.empty()) CallOn(npc, kScriptNPCSetState, q.state);
    yyc::RValue spoken = yyc::Real(q.spoken != 0 ? 0 : 1);
    yyc::SetVar(id, kSlotHasSpoken, &spoken);
    Log("quests: debug touched NPC %d ('%s' / '%s')",
        static_cast<int>(yyc::GetBuiltinReal(npc, yyc::va::YYVar_object_index)), q.quest.c_str(), q.state.c_str());
}

}  // namespace

void Tick(yyc::CInstance* localPlayer) {
    ++g_steps;
    DebugQuest(localPlayer);
    if (g_steps % kEverySteps != 0) return;
    const int count = static_cast<int>(CallReal(localPlayer, "instance_number", {kParNPC}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(CallReal(localPlayer, "instance_find", {kParNPC, double(i)}));
        yyc::CInstance* npc = id == kNoOne ? nullptr : yyc::FindInstance(id);
        if (npc == nullptr) continue;
        const int object = static_cast<int>(yyc::GetBuiltinReal(npc, yyc::va::YYVar_object_index));
        const Quest now = Read(id);
        const auto seen = g_seen.find(object);
        if (seen == g_seen.end()) {  // first look: both sides start from the same save
            g_seen[object] = now;
            continue;
        }
        if (seen->second == now) continue;
        seen->second = now;
        Send(object, now);
        Log("quests: NPC %d changed here ('%s' / '%s'), sent", object, now.quest.c_str(), now.state.c_str());
    }
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kQuest) return false;
    const auto& p = message.payload;
    if (p.size() < 4) return true;
    int object = 0;
    memcpy(&object, p.data(), 4);
    size_t at = 4;
    Quest q;
    if (!Get16(p, &at, &q.quest) || !Get16(p, &at, &q.state) || at + sizeof(q.spoken) != p.size()) return true;
    memcpy(&q.spoken, p.data() + at, sizeof(q.spoken));
    // The NPC may be off-screen here, i.e. deactivated: wake its kind up first.
    sync::WakeThen(localPlayer, object, [localPlayer, object, q] { Apply(localPlayer, object, q); });
    return true;
}

void Reset() { g_seen.clear(); }

}  // namespace coop::quests
