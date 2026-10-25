#include "unlocks.h"

#include <windows.h>

#include <cstring>
#include <initializer_list>
#include <iterator>
#include <vector>

#include "log.h"

namespace coop::unlocks {

namespace {

// What a kind of unlock is: the Data array with one entry per thing, and the field the game keeps "unlocked" in
// (objSkill sets it with StructureEdit(s, 11, true) / ItemEdit(i, 12, 1)).
struct Kind {
    uint8_t message;  // either way: u16 indices unlocked on the sender's side
    const char* name;
    int dataSlot;
    int field;
    uint32_t get;
    uint32_t edit;
};

// Items: the recipes skills open (Industry: glass and steel). A save keeps them in the character chunk
// (ItemUnlocks), which a guest takes from its own character, not from the host's world: without this a guest
// never has what the host's or its own earlier purchases opened.
constexpr Kind kKinds[] = {
    {70, "structures", 0x83E, 11, 0x0070DD60, yyc::script::StructureEdit},
    {72, "items", 0x5CC, 12, 0x006C5F10, 0x006C5990},  // Data.items, ItemGet, ItemEdit (decompiled/code)
};
constexpr unsigned kEverySteps = 300;
constexpr int kObjData = 2;  // the Data object (decompiled/data/objects.tsv)
constexpr int kMaxEntries = 1024;
constexpr int kNoOne = yyc::kNoOne;

std::vector<uint16_t> g_lastSent[std::size(kKinds)];
unsigned g_steps = 0;

using yyc::CallReal;

int EntryCount(yyc::CInstance* self, const Kind& kind) {
    const int data = static_cast<int>(CallReal(self, "instance_find", {kObjData, 0}));
    if (data == kNoOne) return 0;
    yyc::RValue entries{};
    if (!yyc::GetVar(data, kind.dataSlot, &entries)) return 0;
    int count = 0;
    if ((entries.kind & yyc::kKindMask) == yyc::kArray) {
        yyc::RValue length{};
        yyc::CallBuiltin("array_length_1d", self, 1, &entries, &length);
        count = static_cast<int>(yyc::ToReal(length));
        yyc::Free(&length);
    }
    yyc::Free(&entries);
    return count < kMaxEntries ? count : kMaxEntries;
}

bool IsUnlocked(yyc::CInstance* self, const Kind& kind, int entry) {
    yyc::RValue values[2] = {yyc::Real(entry), yyc::Real(kind.field)};
    yyc::RValue* args[2] = {&values[0], &values[1]};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kind.get)(self, self, &result, 2, args);
    const bool unlocked = yyc::ToReal(result) != 0;
    yyc::Free(&result);
    return unlocked;
}

void Unlock(yyc::CInstance* self, const Kind& kind, int entry) {
    yyc::RValue values[3] = {yyc::Real(entry), yyc::Real(kind.field), yyc::Real(1)};
    yyc::RValue* args[3] = {&values[0], &values[1], &values[2]};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kind.edit)(self, self, &result, 3, args);
    yyc::Free(&result);
}

void Share(yyc::CInstance* localPlayer, const Kind& kind, std::vector<uint16_t>* lastSent) {
    // Testing aid: COOP_DEBUG_UNLOCKS=1 tells the peer every structure is unlocked (this side is unchanged).
    static const bool everything = GetEnvironmentVariableA("COOP_DEBUG_UNLOCKS", nullptr, 0) > 0;
    std::vector<uint16_t> unlocked;
    const int count = EntryCount(localPlayer, kind);
    for (int e = 0; e < count; ++e) {
        if ((everything && kind.message == kKinds[0].message) || IsUnlocked(localPlayer, kind, e)) {
            unlocked.push_back(static_cast<uint16_t>(e));
        }
    }
    if (unlocked.empty() || unlocked == *lastSent) return;
    if (net::Send(kind.message, unlocked.data(), unlocked.size() * sizeof(uint16_t))) {
        Log("unlocks: sent %zu unlocked %s of %d", unlocked.size(), kind.name, count);
        *lastSent = std::move(unlocked);
    }
}

}  // namespace

void Tick(yyc::CInstance* localPlayer) {
    if (++g_steps % kEverySteps != 0) return;
    for (size_t k = 0; k < std::size(kKinds); ++k) Share(localPlayer, kKinds[k], &g_lastSent[k]);
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    const Kind* kind = nullptr;
    for (const Kind& candidate : kKinds) {
        if (candidate.message == message.type) kind = &candidate;
    }
    if (kind == nullptr) return false;
    const size_t n = message.payload.size() / sizeof(uint16_t);
    const int count = EntryCount(localPlayer, *kind);
    size_t added = 0;
    for (size_t i = 0; i < n; ++i) {
        uint16_t e = 0;
        memcpy(&e, message.payload.data() + i * sizeof(uint16_t), sizeof(e));
        if (static_cast<int>(e) >= count || IsUnlocked(localPlayer, *kind, e)) continue;
        Unlock(localPlayer, *kind, e);
        ++added;
    }
    if (added > 0) Log("unlocks: the other player has %zu %s unlocked; %zu were new here", n, kind->name, added);
    return true;
}

void Reset() {
    for (std::vector<uint16_t>& sent : g_lastSent) sent.clear();
    g_steps = 0;
}

}  // namespace coop::unlocks
