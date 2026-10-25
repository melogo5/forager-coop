#include "look.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "log.h"

namespace coop::look {

namespace fs = std::filesystem;

namespace {

constexpr int kSlotCosmetic = 0x135;  // objPlayer.cosmetic (decompiled/code/var_slots.tsv)
constexpr int kRoomGame = yyc::kRoomGame;
constexpr unsigned kCheckEverySteps = 60;
constexpr wchar_t kFile[] = L"coop_look.txt";

Look g_own;               // the costume in coop_look.txt
bool g_ownLoaded = false;
bool g_dressed = false;   // guest: our own costume is on in this world room
Look g_dressedAs;         // guest: what we wore right after dressing (the wardrobe changes it from there)
unsigned g_steps = 0;

// Testing aid: COOP_DEBUG_LOOK_FILE=<name> uses another file (two games on one PC share the save area).
fs::path File() {
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return fs::path();
    wchar_t name[64] = {};
    const DWORD custom = GetEnvironmentVariableW(L"COOP_DEBUG_LOOK_FILE", name, 64);
    return fs::path(local) / L"Forager" / (custom > 0 && custom < 64 ? name : kFile);
}

Look LoadOwn() {
    Look look;
    std::ifstream in(File());
    int value = 0;
    while (look.count < kMaxSlots && in >> value) look.items[look.count++] = static_cast<int16_t>(value);
    return look;
}

void SaveOwn(const Look& look) {
    std::ofstream out(File(), std::ios::trunc);
    for (int i = 0; i < look.count; ++i) out << look.items[i] << (i + 1 < look.count ? " " : "\n");
    g_own = look;
    std::ostringstream text;
    for (int i = 0; i < look.count; ++i) text << " " << look.items[i];
    Log("look: own costume kept:%s", text.str().c_str());
}

// The slots in use; `count` comes off the wire, so never more than the array holds.
int Slots(const Look& look) { return (std::min)(static_cast<int>(look.count), kMaxSlots); }

}  // namespace

bool operator==(const Look& a, const Look& b) {
    return a.count == b.count && std::equal(a.items, a.items + Slots(a), b.items);
}

Look Read(yyc::CInstance* player) {
    Look look;
    if (player == nullptr) return look;
    const int id = static_cast<int>(yyc::GetBuiltinReal(player, yyc::va::YYVar_id));
    yyc::RValue cosmetic{};
    if (!yyc::GetVar(id, kSlotCosmetic, &cosmetic) || (cosmetic.kind & yyc::kKindMask) != yyc::kArray) {
        yyc::Free(&cosmetic);
        return look;
    }
    yyc::RValue length{};
    yyc::CallBuiltin("array_length_1d", player, 1, &cosmetic, &length);
    const int n = (std::min)(static_cast<int>(yyc::ToReal(length)), kMaxSlots);  // only in-bounds reads
    yyc::Free(&length);
    for (int i = 0; i < n; ++i) look.items[i] = static_cast<int16_t>(yyc::ToReal(yyc::ArrayElement(&cosmetic, i)));
    look.count = static_cast<uint8_t>((std::max)(n, 0));
    yyc::Free(&cosmetic);
    return look;
}

void Apply(yyc::CInstance* player, const Look& look) {
    if (player == nullptr || look.count == 0) return;
    const int id = static_cast<int>(yyc::GetBuiltinReal(player, yyc::va::YYVar_id));
    for (int i = 0; i < Slots(look); ++i) {
        yyc::RValue item = yyc::Real(look.items[i]);
        yyc::SetVarAt(id, kSlotCosmetic, i, &item);
    }
}

void Tick(yyc::CInstance* localPlayer, bool guest, bool inWorld) {
    if (!g_ownLoaded) {
        g_own = LoadOwn();
        g_ownLoaded = true;
    }
    if (!inWorld) {
        g_dressed = false;  // back in the world room, the host's save is read again: dress again
        return;
    }
    if (guest && !g_dressed) {
        g_dressed = true;
        if (g_own.count > 0) {
            Apply(localPlayer, g_own);
            Log("look: dressed in our own costume");
        }
        g_dressedAs = Read(localPlayer);
        return;
    }
    if (++g_steps % kCheckEverySteps != 0) return;
    const Look now = Read(localPlayer);
    if (now.count == 0 || now == g_own) return;
    // A guest keeps what it changed in the wardrobe, never the host's costume it may have loaded with the world.
    if (guest && now == g_dressedAs) return;
    if (guest) g_dressedAs = now;
    SaveOwn(now);
}

void Reset() { g_dressed = false; }

}  // namespace coop::look
