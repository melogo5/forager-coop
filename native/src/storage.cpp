#include "storage.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "net.h"
#include "sync.h"

namespace coop::storage {

namespace {

constexpr uint8_t kStorage = 40;      // contents on close; also releases the chest
constexpr uint8_t kStorageLock = 41;  // the sender opened this chest
constexpr uint32_t kVaultInteract = 0x00BC88D0;  // objVault Other_11: opens the storage menu
constexpr uint32_t kStorageMenuDestroy = 0x00E51ED0;  // objStorageInventory Destroy_0
constexpr int kSlotOwner = 0x6CA;                     // the menu's chest
constexpr int kSlotContents = 0x7C4;                  // the chest's `slot` (2D array)
constexpr size_t kMaxRows = 4096;
constexpr size_t kMaxColumns = 255;

enum CellKind : uint8_t { kCellReal = 0, kCellString = 1, kCellUndefined = 5 };

#pragma pack(push, 1)
struct Header {
    int32_t hostId;
    int32_t object;
    float x, y;
    uint16_t rows;
};
#pragma pack(pop)

yyc::EventFn g_origMenuDestroy = nullptr;
yyc::EventFn g_origVaultInteract = nullptr;
std::vector<int> g_lockedByPeer;  // local chest ids the peer has open

bool LockedByPeer(int chestId) {
    return std::find(g_lockedByPeer.begin(), g_lockedByPeer.end(), chestId) != g_lockedByPeer.end();
}

void Release(int chestId) {
    g_lockedByPeer.erase(std::remove(g_lockedByPeer.begin(), g_lockedByPeer.end(), chestId), g_lockedByPeer.end());
}

Header Describe(int chestId, yyc::CInstance* chest, uint16_t rows) {
    return {sync::HostIdOf(chestId), static_cast<int32_t>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_object_index)),
            static_cast<float>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_x)),
            static_cast<float>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_y)), rows};
}

// Only one player at a time may have a chest open.
void __cdecl VaultInteractHook(yyc::CInstance* self, yyc::CInstance* other) {
    const int chestId = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    if (net::IsConnected() && LockedByPeer(chestId)) {
        Log("storage: chest %d is open by the other player", chestId);
        return;
    }
    g_origVaultInteract(self, other);
    if (!net::IsConnected()) return;
    const Header header = Describe(chestId, self, 0);
    net::Send(kStorageLock, &header, sizeof(header));
}

double CallReal(yyc::CInstance* self, const char* builtin, std::vector<yyc::RValue> args) {
    yyc::RValue result{};
    yyc::CallBuiltin(builtin, self, static_cast<int>(args.size()), args.data(), &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

void PutCell(std::vector<uint8_t>& out, const yyc::RValue& cell) {
    const uint32_t kind = cell.kind & yyc::kKindMask;
    if (kind == yyc::kString) {
        const std::string text = yyc::ToString(cell);
        const uint16_t length = static_cast<uint16_t>((std::min)(text.size(), size_t{0xFFFF}));
        out.push_back(kCellString);
        out.push_back(static_cast<uint8_t>(length & 0xFF));
        out.push_back(static_cast<uint8_t>(length >> 8));
        out.insert(out.end(), text.begin(), text.begin() + length);
    } else if (kind == yyc::kUndefined) {
        out.push_back(kCellUndefined);
    } else {
        const double value = yyc::ToReal(cell);
        out.push_back(kCellReal);
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(value));
    }
}

void SendContents(yyc::CInstance* self, int chestId) {
    yyc::CInstance* chest = yyc::FindInstance(chestId);
    if (chest == nullptr) return;
    yyc::RValue contents{};
    yyc::GetVar(chestId, kSlotContents, &contents);
    const bool isArray = (contents.kind & yyc::kKindMask) == yyc::kArray;
    const size_t rows = isArray ? static_cast<size_t>(CallReal(self, "array_height_2d", {contents})) : 0;
    const Header header = Describe(chestId, chest, static_cast<uint16_t>((std::min)(rows, kMaxRows)));
    std::vector<uint8_t> payload(sizeof(header));
    for (size_t i = 0; i < header.rows; ++i) {
        const size_t columns =
            (std::min)(static_cast<size_t>(CallReal(self, "array_length_2d", {contents, yyc::Real(double(i))})),
                       kMaxColumns);
        payload.push_back(static_cast<uint8_t>(columns));
        for (size_t j = 0; j < columns; ++j) {
            yyc::RValue cell{};
            yyc::GetVarAt(chestId, kSlotContents, static_cast<int>(i * yyc::kArray2dStride + j), &cell);
            PutCell(payload, cell);
            yyc::Free(&cell);
        }
    }
    yyc::Free(&contents);
    memcpy(payload.data(), &header, sizeof(header));
    net::Send(kStorage, payload.data(), payload.size());
    Log("storage: chest %d (object %d) sent, %u rows, %zu bytes", chestId, header.object, header.rows,
        payload.size());
}

// After the menu is gone the chest holds its final contents for this visit.
void __cdecl MenuDestroyHook(yyc::CInstance* self, yyc::CInstance* other) {
    yyc::RValue owner{};
    const int menuId = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    yyc::GetVar(menuId, kSlotOwner, &owner);
    const int chestId = static_cast<int>(yyc::ToReal(owner));
    yyc::Free(&owner);
    g_origMenuDestroy(self, other);
    if (!net::IsConnected() || chestId < 100000) return;
    __try {
        SendContents(self, chestId);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("storage: sending chest %d crashed (code 0x%08lX)", chestId, GetExceptionCode());
    }
}

bool ReadCell(const std::vector<uint8_t>& p, size_t* at, yyc::RValue* out) {
    if (*at >= p.size()) return false;
    const uint8_t kind = p[(*at)++];
    if (kind == kCellReal) {
        if (*at + sizeof(double) > p.size()) return false;
        double value = 0;
        memcpy(&value, p.data() + *at, sizeof(value));
        *at += sizeof(value);
        *out = yyc::Real(value);
    } else if (kind == kCellString) {
        if (*at + 2 > p.size()) return false;
        const size_t length = p[*at] | (p[*at + 1] << 8);
        *at += 2;
        if (*at + length > p.size()) return false;
        const std::string text(p.begin() + *at, p.begin() + *at + length);
        *at += length;
        *out = yyc::String(text.c_str());
    } else {
        *out = yyc::RValue{};
        out->kind = yyc::kUndefined;
    }
    return true;
}

void ApplyContents(yyc::CInstance* self, const net::Message& message) {
    const auto& p = message.payload;
    if (p.size() < sizeof(Header)) return;
    Header header{};
    memcpy(&header, p.data(), sizeof(header));
    const int chestId = sync::Resolve(self, header.hostId, header.object, header.x, header.y);
    if (chestId == 0) {
        Log("storage: chest object %d at (%.0f, %.0f) not found", header.object, header.x, header.y);
        return;
    }
    Release(chestId);
    size_t at = sizeof(Header);
    for (size_t i = 0; i < header.rows && at < p.size(); ++i) {
        const size_t columns = p[at++];
        for (size_t j = 0; j < columns; ++j) {
            yyc::RValue cell{};
            if (!ReadCell(p, &at, &cell)) return;
            yyc::SetVarAt(chestId, kSlotContents, static_cast<int>(i * yyc::kArray2dStride + j), &cell);
            yyc::Free(&cell);
        }
    }
    yyc::RValue item{}, amount{};
    yyc::GetVarAt(chestId, kSlotContents, 0, &item);
    yyc::GetVarAt(chestId, kSlotContents, 1, &amount);
    Log("storage: chest %d updated from the peer (%u rows), first cell: item %g x%g", chestId, header.rows,
        yyc::ToReal(item), yyc::ToReal(amount));
    yyc::Free(&item);
    yyc::Free(&amount);
}

}  // namespace

void DebugTick(yyc::CInstance* localPlayer) {
    // Testing aid: COOP_DEBUG_STORAGE=1 opens the nearest chest ~7 s into the world and closes it 1 s later.
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_STORAGE", nullptr, 0) > 0;
    static unsigned steps = 0;
    constexpr unsigned kOpenAt = 420;
    constexpr unsigned kCloseAt = 480;
    constexpr int kObjVault = 114;
    constexpr int kObjStorageMenu = 234;
    if (!enabled) return;
    ++steps;
    if (steps == kOpenAt) {
        const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
        const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
        const int chest = static_cast<int>(CallReal(localPlayer, "instance_nearest",
                                                    {yyc::Real(x), yyc::Real(y), yyc::Real(kObjVault)}));
        if (yyc::CInstance* instance = yyc::FindInstance(chest)) {
            yyc::PerformEvent(instance, localPlayer, yyc::kEvOther, yyc::kEvUser0 + 1);
            Log("storage: debug opened chest %d", chest);
        }
    } else if (steps == kCloseAt) {
        // As if the player put 5 of item #1 in the first cell: [item, amount, ...].
        const double x = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
        const double y = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
        const int chest = static_cast<int>(CallReal(localPlayer, "instance_nearest",
                                                    {yyc::Real(x), yyc::Real(y), yyc::Real(kObjVault)}));
        if (yyc::FindInstance(chest) != nullptr) {
            yyc::RValue item = yyc::Real(1);
            yyc::RValue amount = yyc::Real(5);
            yyc::SetVarAt(chest, kSlotContents, 0, &item);
            yyc::SetVarAt(chest, kSlotContents, 1, &amount);
        }
        CallReal(localPlayer, "instance_destroy", {yyc::Real(kObjStorageMenu)});
        Log("storage: debug closed the storage menu");
    }
}

bool InstallHooks() {
    const bool ok = MH_CreateHook(yyc::At<void*>(kStorageMenuDestroy), reinterpret_cast<void*>(&MenuDestroyHook),
                                  reinterpret_cast<void**>(&g_origMenuDestroy)) == MH_OK &&
                    MH_CreateHook(yyc::At<void*>(kVaultInteract), reinterpret_cast<void*>(&VaultInteractHook),
                                  reinterpret_cast<void**>(&g_origVaultInteract)) == MH_OK;
    if (!ok) Log("storage: hooks failed");
    return ok;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kStorageLock) {
        if (message.payload.size() >= sizeof(Header)) {
            Header header{};
            memcpy(&header, message.payload.data(), sizeof(header));
            const int chestId = sync::Resolve(localPlayer, header.hostId, header.object, header.x, header.y);
            if (chestId != 0 && !LockedByPeer(chestId)) g_lockedByPeer.push_back(chestId);
        }
        return true;
    }
    if (message.type != kStorage) return false;
    ApplyContents(localPlayer, message);
    return true;
}

}  // namespace coop::storage
