#include "progress.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"

namespace coop::progress {

namespace {

constexpr uint8_t kProgressList = 90;  // either way: u8 list index, then the ds_list_write text
constexpr unsigned kEverySteps = 60;

struct SharedList {
    const char* name;
    int slot;  // a global holding a ds_list id (decompiled/code/var_slots.tsv)
};
constexpr SharedList kLists[] = {
    {"museum", 0x68A},  // global.museumBundles: what was donated
};
constexpr size_t kListCount = sizeof(kLists) / sizeof(kLists[0]);

std::string g_last[kListCount];  // what each list looked like when last sent or received
unsigned g_steps = 0;

double ListId(size_t i) {
    const yyc::RValue* id = yyc::GlobalVar(kLists[i].slot);
    return id != nullptr && (id->kind & yyc::kKindMask) == yyc::kReal ? id->real : -1;
}

bool Exists(yyc::CInstance* self, double id) {
    if (id < 0) return false;
    yyc::RValue args[2] = {yyc::Real(id), yyc::Real(2)};  // ds_type_list
    yyc::RValue result{};
    yyc::CallBuiltin("ds_exists", self, 2, args, &result);
    const bool exists = yyc::ToReal(result) != 0;
    yyc::Free(&result);
    return exists;
}

std::string Write(yyc::CInstance* self, double id) {
    yyc::RValue arg = yyc::Real(id);
    yyc::RValue result{};
    yyc::CallBuiltin("ds_list_write", self, 1, &arg, &result);
    std::string text = yyc::ToString(result);
    yyc::Free(&result);
    return text;
}

// Testing aid: COOP_DEBUG_MUSEUM=<item> adds that item to the museum's list ~5 s in, as a donation would.
void DebugDonate(yyc::CInstance* self) {
    static const int item = [] {
        char value[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_MUSEUM", value, sizeof(value)) > 0 ? atoi(value) : 0;
    }();
    if (item <= 0 || g_steps != 300) return;
    const double id = ListId(0);
    if (!Exists(self, id)) return;
    yyc::RValue args[2] = {yyc::Real(id), yyc::Real(item)};
    yyc::RValue result{};
    yyc::CallBuiltin("ds_list_add", self, 2, args, &result);
    yyc::Free(&result);
    Log("progress: debug donated item %d to the museum", item);
}

}  // namespace

void Tick(yyc::CInstance* localPlayer) {
    ++g_steps;
    DebugDonate(localPlayer);
    if (g_steps % kEverySteps != 0) return;
    for (size_t i = 0; i < kListCount; ++i) {
        const double id = ListId(i);
        if (!Exists(localPlayer, id)) continue;
        std::string text = Write(localPlayer, id);
        if (g_last[i].empty()) {  // the first look: both sides start from the same save
            g_last[i] = std::move(text);
            continue;
        }
        if (text == g_last[i]) continue;
        std::vector<uint8_t> payload(1 + text.size());
        payload[0] = static_cast<uint8_t>(i);
        memcpy(payload.data() + 1, text.data(), text.size());
        if (net::Send(kProgressList, payload.data(), payload.size())) {
            Log("progress: %s changed here, sent", kLists[i].name);
            g_last[i] = std::move(text);
        }
    }
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kProgressList) return false;
    if (message.payload.empty() || message.payload[0] >= kListCount) return true;
    const size_t i = message.payload[0];
    const std::string text(message.payload.begin() + 1, message.payload.end());
    const double id = ListId(i);
    if (!Exists(localPlayer, id)) return true;
    yyc::RValue args[2] = {yyc::Real(id), yyc::String(text.c_str())};
    yyc::RValue result{};
    yyc::CallBuiltin("ds_list_read", localPlayer, 2, args, &result);
    yyc::Free(&result);
    yyc::Free(&args[1]);
    g_last[i] = Write(localPlayer, id);
    Log("progress: %s updated from the peer", kLists[i].name);
    return true;
}

void Reset() {
    for (std::string& last : g_last) last.clear();
}

}  // namespace coop::progress
