#include "commands.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "MinHook.h"
#include "log.h"
#include "puzzles.h"

namespace coop::commands {

namespace {

constexpr uint8_t kChestRemove = 108;  // either way (the host passes it on): ChestMsg, a chest removed with /chestremove

// decompiled/code/index.tsv and the script table (index = entry position): CommandCreate(name, cheat, ScriptWrap(script),
// help...) is how ForagerCommandsInit registers /give; ScriptWrap(index) makes the script value it takes.
constexpr uint32_t kScriptCommandCreate = 0x0087BD70;
constexpr uint32_t kScriptScriptWrap = 0x006929B0;
// Scripts no game code calls (only the script table lists them): the commands' scripts.
constexpr uint32_t kPuzzleResetScript = 0x0084B830;  // rousr_dissonance_dummy_function
constexpr double kPuzzleResetIndex = 825;
constexpr uint32_t kChestRemoveScript = 0x010B7990;  // imguigml_dummy
constexpr double kChestRemoveIndex = 3266;
constexpr int kSlotChatCommands = 0xFC;  // global.chatCommands: the ds_map CommandCreate fills

constexpr int kFirstChest = 150, kLastChest = 153;  // objTreasureChest .. objTreasureChestSmallFrozen
constexpr double kChestReach = 160;                  // /chestremove: a chest this close to the player at most
constexpr double kSameSpot = 24;

#pragma pack(push, 1)
struct ChestMsg {
    int32_t object;
    float x, y;
};
#pragma pack(pop)

yyc::ScriptFn g_origPuzzleReset = nullptr;
yyc::ScriptFn g_origChestRemove = nullptr;
bool g_registered = false;

int IdOf(yyc::CInstance* instance) { return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)); }

// A number typed as text ("15"): the whole word has to be one.
bool ParseNumber(const yyc::RValue& value, double* number) {
    if (yyc::IsNumber(value)) {
        *number = yyc::ToReal(value);
        return true;
    }
    const std::string text = yyc::ToString(value);
    char* end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size()) return false;
    *number = parsed;
    return true;
}

// The first number a command was typed with. The chat runs a command's script with the typed words as ONE array
// argument (how __CommandGive reads them: array_length_1d, real(string_digits(...))) - read as a number or a string,
// that array was 0 and "/puzzlereset 15" reset nothing (playtest 2026-11-08). A plain number or string works too.
double Arg(yyc::CInstance* self, int argc, yyc::RValue** args, int i, double otherwise) {
    if (i >= argc || args == nullptr || args[i] == nullptr) return otherwise;
    yyc::RValue& value = *args[i];
    double number = 0;
    if (value.kind != yyc::kArray) return ParseNumber(value, &number) ? number : otherwise;
    yyc::RValue length{};
    if (!yyc::CallBuiltin("array_length_1d", self, 1, &value, &length)) return otherwise;
    const int count = static_cast<int>(yyc::ToReal(length));
    yyc::Free(&length);
    for (int k = 0; k < count; ++k) {
        const yyc::RValue word = yyc::ArrayElement(&value, k);  // a plain copy: read, never freed
        if (ParseNumber(word, &number)) return number;
    }
    return otherwise;
}

void Register(yyc::CInstance* self, const char* name, double scriptIndex, const char* help) {
    yyc::RValue index = yyc::Real(scriptIndex);
    yyc::RValue* wrapArgs[1] = {&index};
    yyc::RValue wrapped{};
    yyc::At<yyc::ScriptFn>(kScriptScriptWrap)(self, self, &wrapped, 1, wrapArgs);
    yyc::RValue command = yyc::String(name), cheat = yyc::Real(0), helpText = yyc::String(help);
    yyc::RValue* args[4] = {&command, &cheat, &wrapped, &helpText};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kScriptCommandCreate)(self, self, &result, 4, args);
    yyc::Free(&result);
    yyc::Free(&command);
    yyc::Free(&helpText);
    yyc::Free(&wrapped);
}

// The treasure chest of kind `object` (any kind if -1) nearest to (x, y), within `reach`; 0 if none.
int NearestChest(yyc::CInstance* self, int object, double x, double y, double reach) {
    int best = 0;
    double bestDistance = reach * reach;
    for (int kind = kFirstChest; kind <= kLastChest; ++kind) {
        if (object >= 0 && kind != object) continue;
        const int count = static_cast<int>(yyc::CallReal(self, "instance_number", {double(kind)}));
        for (int i = 0; i < count; ++i) {
            const int id = static_cast<int>(yyc::CallReal(self, "instance_find", {double(kind), double(i)}));
            yyc::CInstance* chest = yyc::FindInstance(id);
            if (chest == nullptr) continue;
            const double dx = yyc::GetBuiltinReal(chest, yyc::va::YYVar_x) - x;
            const double dy = yyc::GetBuiltinReal(chest, yyc::va::YYVar_y) - y;
            if (dx * dx + dy * dy > bestDistance) continue;
            bestDistance = dx * dx + dy * dy;
            best = id;
        }
    }
    return best;
}

// A treasure chest's destroy is never sent (sync.cpp): each player removes its own, quietly (no loot).
void RemoveChest(yyc::CInstance* self, int id) { yyc::CallReal(self, "instance_destroy", {double(id), 0}); }

yyc::RValue* __cdecl PuzzleResetHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                     yyc::RValue** args) {
    const int number = static_cast<int>(Arg(self, argc, args, 0, 0));
    if (!puzzles::ResetPuzzle(self, number, true)) {
        Log("commands: /puzzlereset %d - no such puzzle, or it cannot be reset yet", number);
    }
    return result;
}

yyc::RValue* __cdecl ChestRemoveHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                     yyc::RValue** args) {
    yyc::CInstance* player = yyc::FindInstance(static_cast<int>(yyc::CallReal(self, "instance_find", {double(yyc::obj::player), 0})));
    if (player == nullptr) return result;
    const double px = yyc::GetBuiltinReal(player, yyc::va::YYVar_x);
    const double py = yyc::GetBuiltinReal(player, yyc::va::YYVar_y);
    const int id = NearestChest(self, -1, px, py, kChestReach);
    yyc::CInstance* chest = id != 0 ? yyc::FindInstance(id) : nullptr;
    if (chest == nullptr) {
        Log("commands: /chestremove - no treasure chest within %.0f px", kChestReach);
        return result;
    }
    const ChestMsg msg{static_cast<int32_t>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_object_index)),
                       static_cast<float>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_x)),
                       static_cast<float>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_y))};
    RemoveChest(self, id);
    if (net::IsConnected()) net::Send(kChestRemove, &msg, sizeof(msg));
    Log("commands: /chestremove - chest %d (object %d) at (%.0f, %.0f) removed", id, msg.object, msg.x, msg.y);
    return result;
}

bool Hook(uint32_t va, void* detour, void* original) {
    return MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original)) == MH_OK;
}

}  // namespace

bool InstallHooks() {
    const bool ok = Hook(kPuzzleResetScript, reinterpret_cast<void*>(&PuzzleResetHook), &g_origPuzzleReset) &&
                    Hook(kChestRemoveScript, reinterpret_cast<void*>(&ChestRemoveHook), &g_origChestRemove);
    if (!ok) Log("commands: hooks failed");
    return ok;
}

void Tick(yyc::CInstance* localPlayer) {
    if (g_registered) return;
    const yyc::RValue* map = yyc::GlobalVar(kSlotChatCommands);
    if (map == nullptr || !yyc::IsNumber(*map)) return;  // the chat is not set up yet
    if (yyc::CallReal(localPlayer, "ds_exists", {yyc::ToReal(*map), yyc::kDsTypeMap}) == 0) return;
    Register(localPlayer, "puzzlereset", kPuzzleResetIndex, "number");
    Register(localPlayer, "chestremove", kChestRemoveIndex, "");
    g_registered = true;
    Log("commands: /puzzlereset <n> and /chestremove registered in the chat");
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kChestRemove) return false;
    if (message.payload.size() != sizeof(ChestMsg)) return true;
    ChestMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (!std::isfinite(msg.x) || !std::isfinite(msg.y) || msg.object < kFirstChest || msg.object > kLastChest) return true;
    const int id = NearestChest(localPlayer, msg.object, msg.x, msg.y, kSameSpot);
    if (id != 0) RemoveChest(localPlayer, id);
    Log("commands: the other player's /chestremove at (%.0f, %.0f): %s", msg.x, msg.y, id != 0 ? "removed here" : "no chest here");
    return true;
}

void Reset() { g_registered = false; }

}  // namespace coop::commands
