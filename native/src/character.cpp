#include "character.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "MinHook.h"
#include "files.h"
#include "log.h"
#include "puppet.h"
#include "world.h"

namespace coop::character {

namespace fs = std::filesystem;

namespace {

enum MessageType : uint8_t {
    kGuestHello = 30,     // guest -> host: name
    kCharacter = 31,      // host -> guest: u8 hasData, stored Data.cin
    kCharacterSave = 32,  // guest -> host: the guest's Data.cin
    kGearState = 33,      // guest -> host: the guest's owned gear, a float per category (gearCurrent)
    kBuffState = 37,      // guest -> host: the guest's buff[type, k] (ReadBuffs)
};

constexpr uint32_t kScriptDeserializeCharacterChunk = 0x005BC8F0;
// GearEdit(category, field, value): field 2 is the gear a player owns in that category - PlayerFind().gearCurrent
// (objItemGet when gear is picked up, DeserializeCharacterChunk on load); other fields are global.gear.
constexpr uint32_t kScriptGearEdit = 0x006CAAC0;
constexpr int kGearFieldOwned = 2;
constexpr int kSlotGearCurrent = 0x535;
constexpr char kPlayerCreate[] = "gml_Object_objPlayer_Create_0";
constexpr unsigned kGearEverySteps = 60;        // 1 s: look for a change
constexpr unsigned kGearResendSteps = 600;      // 10 s: sent anyway (a puppet made again starts with base gear)
constexpr size_t kMaxGearCategories = 128;
constexpr int kSlotBuff = 0xD2;  // objPlayer.buff[type, k]: HasBuff reads buff[type, 0]
constexpr int kMaxBuffRows = 64;
constexpr int kBuffColumns = 4;
constexpr uint32_t kScriptCinderLoad = 0x005CE0D0;    // CinderLoad(buffer) -> cinder (chunk map)
constexpr uint32_t kScriptCinderDelete = 0x005C9BD0;  // CinderDelete(cinder)
constexpr int kSlotSaveFile = yyc::slot::saveFile;
constexpr unsigned kUploadEverySteps = 900;    // 15 s (a save is small: a few KB)
constexpr unsigned kSaveSettleSteps = 120;     // SaveGame is async
constexpr size_t kMaxName = 32;
constexpr char kGuestsDir[] = "coop_guests";
constexpr wchar_t kLocalCharacterFile[] = L"coop_guest_character.cin";  // guest side, in the save area

bool (*g_isGuest)() = nullptr;
yyc::ScriptFn g_origDeserialize = nullptr;
yyc::ScriptFn g_origGearEdit = nullptr;

// Guest state.
std::atomic<bool> g_answered{false};
std::atomic<bool> g_hasStored{false};
bool g_swapDone = false;
unsigned g_steps = 0;
unsigned g_settle = 0;

// Host state: the name each guest introduced itself with.
std::map<net::PeerId, std::string> g_guestNames;

// Guest: our own name (set from the COOP menu before joining).
std::string g_ownName;
constexpr wchar_t kNameFile[] = L"coop_name.txt";

fs::path SaveArea() {
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return length > 0 && length < MAX_PATH ? fs::path(local) / L"Forager" : fs::path();
}

std::string ComputerName() {
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = sizeof(name);
    return GetComputerNameA(name, &size) ? std::string(name, size) : std::string("guest");
}

std::string SafeName(const std::string& name) {
    std::string safe;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                        c == '_';
        if (ok && safe.size() < kMaxName) safe.push_back(c);
    }
    return safe.empty() ? std::string("guest") : safe;
}

// The file a name is kept under. Windows file names ignore case, so Bob and bob are one file: lower case, and
// names compare without case (FreeName). Device names (CON, NUL, COM1...) are no files at all: a guest called so
// was written into the device and came back new every time - they get a prefix.
std::string FileKey(const std::string& name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](char c) { return static_cast<char>(tolower(c)); });
    static const char* const kDevices[] = {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4",
                                           "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3",
                                           "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* device : kDevices) {
        if (key == device) return "_" + key;
    }
    return key;
}

// Host: `name`, or name_2, name_3... if a guest linked now goes by it: two guests never share a character.
std::string FreeName(const std::string& name, net::PeerId peer) {
    const std::vector<net::PeerId> linked = net::Peers();
    const auto taken = [&](const std::string& candidate) {
        for (const net::PeerId other : linked) {
            const auto found = g_guestNames.find(other);
            if (other != peer && found != g_guestNames.end() && FileKey(found->second) == FileKey(candidate)) {
                return true;
            }
        }
        return false;
    };
    std::string candidate = name;
    for (int n = 2; taken(candidate); ++n) candidate = name + "_" + std::to_string(n);
    return candidate;
}

// Host: <save area>/saveData<current slot>/coop_guests/<name>.cin
fs::path GuestFile(const std::string& name) {
    const yyc::RValue* slot = yyc::GlobalVar(kSlotSaveFile);
    const fs::path area = SaveArea();
    if (area.empty() || slot == nullptr) return {};
    const int slotNumber = static_cast<int>(yyc::ToReal(*slot));
    return area / (L"saveData" + std::to_wstring(slotNumber)) / kGuestsDir / fs::u8path(FileKey(SafeName(name)) + ".cin");
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool WriteFile(const fs::path& path, const uint8_t* data, size_t size) {
    return coop::files::WriteAtomic(path, data, size);
}

double CallReal(yyc::CInstance* self, const char* builtin, yyc::RValue* args, int argc) {
    yyc::RValue result{};
    yyc::CallBuiltin(builtin, self, argc, args, &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

// Guest: the stored character goes through the game's own loader: buffer -> cinder -> character chunk.
void RestoreStored(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result) {
    yyc::RValue name = yyc::String("coop_guest_character.cin");
    const double buffer = CallReal(self, "buffer_load", &name, 1);
    yyc::Free(&name);
    if (buffer < 0) {
        Log("character: stored character file missing - starting fresh");
        return;
    }
    yyc::RValue bufferArg = yyc::Real(buffer);
    yyc::RValue* loadArgs[1] = {&bufferArg};
    yyc::RValue cinder{};
    yyc::At<yyc::ScriptFn>(kScriptCinderLoad)(self, other, &cinder, 1, loadArgs);
    // Like DeserializeCinder: the chunk map is element 0 of what CinderLoad returns.
    yyc::RValue chunks = yyc::ArrayElement(&cinder, 0);
    yyc::RValue* chunkArgs[1] = {&chunks};
    g_origDeserialize(self, other, result, 1, chunkArgs);
    yyc::RValue* cinderArgs[1] = {&cinder};
    yyc::RValue ignored{};
    yyc::At<yyc::ScriptFn>(kScriptCinderDelete)(self, other, &ignored, 1, cinderArgs);
    yyc::Free(&ignored);
    yyc::Free(&cinder);
    CallReal(self, "buffer_delete", &bufferArg, 1);
    Log("character: own character restored");
}

// Guest: while the host's world loads, its character is replaced by ours (or skipped for a new guest).
yyc::RValue* __cdecl DeserializeHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                     yyc::RValue** args) {
    if (g_isGuest == nullptr || !g_isGuest() || g_swapDone || !g_answered) {
        return g_origDeserialize(self, other, result, argc, args);
    }
    g_swapDone = true;
    if (!g_hasStored) {
        Log("character: new guest - starting with a fresh character");
        return result;
    }
    RestoreStored(self, other, result);
    return result;
}

double OwnedGear(int playerId, int category) {
    yyc::RValue value{};
    const bool ok = yyc::GetVarAt(playerId, kSlotGearCurrent, category, &value);
    const double gear = ok && yyc::IsNumber(value) ? yyc::ToReal(value) : -4;
    yyc::Free(&value);
    return gear;
}

int GearTarget(yyc::CInstance* self, yyc::CInstance* other) {
    yyc::RValue zero = yyc::Real(0);
    yyc::RValue* findArgs[1] = {&zero};
    yyc::RValue found{};
    yyc::At<yyc::ScriptFn>(yyc::va::Script_PlayerFind)(self, other, &found, 1, findArgs);  // as GearEdit will
    const int target = yyc::IsNumber(found) ? static_cast<int>(yyc::ToReal(found)) : -4;
    yyc::Free(&found);
    return target;
}

// Playtest 6: crafted gear went missing (host and guest, over a rejoin or in play) and nothing in the logs said
// when. In a session every change of a player's owned gear is logged with who got it and what changed it.
// Playtest 8 (that log): objPlayer's Create_0 hands the starting pickaxe, wallet and backpack to PlayerFind() - the
// FIRST player. Another objPlayer made later (the skill tree's avatar, the tree opened in the world) reset the real
// player's crafted ones. A new objPlayer only ever gives gear to itself.
yyc::RValue* __cdecl GearEditHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                  yyc::RValue** args) {
    const yyc::StackFrame* caller = yyc::TopFrame();
    const bool byCreate = caller != nullptr && caller->name != nullptr && strcmp(caller->name, kPlayerCreate) == 0;
    if (byCreate && self != nullptr && !puppet::IsCreatingPuppet()) {
        yyc::CInstance* instance = yyc::FindInstance(GearTarget(self, other));
        if (instance != nullptr && instance != self) {
            static int told = 0;
            if (told++ < 5) Log("gear: a new objPlayer would have reset the player's gear - skipped");
            return result;
        }
    }
    const bool traced = net::IsConnected() && argc >= 3 && yyc::IsNumber(*args[0]) && yyc::IsNumber(*args[1]) &&
                        static_cast<int>(yyc::ToReal(*args[1])) == kGearFieldOwned;
    if (!traced) return g_origGearEdit(self, other, result, argc, args);
    const std::string by = caller != nullptr && caller->name != nullptr ? caller->name : "?";
    const int target = GearTarget(self, other);
    yyc::CInstance* instance = yyc::FindInstance(target);
    const int category = static_cast<int>(yyc::ToReal(*args[0]));
    const double before = instance != nullptr ? OwnedGear(target, category) : -4;
    yyc::RValue* r = g_origGearEdit(self, other, result, argc, args);
    const double after = instance != nullptr ? OwnedGear(target, category) : -4;
    if (after != before) {
        Log("gear: %s %d category %d: %.0f -> %.0f (by %s)", puppet::IsPuppet(instance) ? "puppet" : "player", target,
            category, before, after, by.c_str());
    }
    return r;
}

}  // namespace

bool InstallHooks(bool (*isGuest)()) {
    g_isGuest = isGuest;
    MH_STATUS status =
        MH_CreateHook(yyc::At<void*>(kScriptDeserializeCharacterChunk), reinterpret_cast<void*>(&DeserializeHook),
                      reinterpret_cast<void**>(&g_origDeserialize));
    if (status == MH_OK) {
        status = MH_CreateHook(yyc::At<void*>(kScriptGearEdit), reinterpret_cast<void*>(&GearEditHook),
                               reinterpret_cast<void**>(&g_origGearEdit));
    }
    if (status != MH_OK) Log("character: hook failed: %s", MH_StatusToString(status));
    return status == MH_OK;
}

std::string HostGuestName(net::PeerId peer) {
    const auto found = g_guestNames.find(peer);
    return found != g_guestNames.end() ? found->second : "guest";
}

std::wstring GuestDataFile(const std::string& name, const char* extension) {
    fs::path path = GuestFile(name);
    return path.empty() ? std::wstring() : path.replace_extension(extension).wstring();
}

std::string SavedGuestName() {
    std::ifstream in(SaveArea() / kNameFile);
    std::string name;
    std::getline(in, name);
    return name.empty() ? SafeName(ComputerName()).substr(0, 16) : SafeName(name);
}

std::string OwnName() { return g_ownName.empty() ? SavedGuestName() : g_ownName; }

void SetGuestName(const std::string& name) {
    g_ownName = SafeName(name);
    std::ofstream(SaveArea() / kNameFile, std::ios::trunc) << g_ownName << "\n";
}

void GuestOnConnect(net::PeerId host) {
    const std::string name = g_ownName.empty() ? SavedGuestName() : g_ownName;
    net::SendTo(host, kGuestHello, name.data(), name.size());
    Log("character: introduced as '%s'", name.c_str());
}

bool GuestHandleReceive(const net::Message& message) {
    if (message.type != kCharacter) return false;
    if (message.payload.empty()) return true;
    const bool stored = message.payload[0] != 0 && message.payload.size() > 1;
    if (stored) {
        WriteFile(SaveArea() / kLocalCharacterFile, message.payload.data() + 1, message.payload.size() - 1);
    }
    g_hasStored = stored;
    g_answered = true;
    Log("character: host answered (%s)", stored ? "stored character" : "new guest");
    return true;
}

bool GuestReady() { return g_answered; }

void GuestUploadNow() {
    const std::vector<uint8_t> data = ReadFile(fs::path(world::GuestSlotPath()) / L"Data.cin");
    if (data.empty()) {
        Log("character: save produced no Data.cin");
    } else if (!net::Send(kCharacterSave, data.data(), data.size())) {
        Log("character: upload of %zu bytes failed", data.size());
    }
}

int GearCount(yyc::CInstance* player, int playerId) {
    yyc::RValue gear{};
    if (!yyc::GetVar(playerId, kSlotGearCurrent, &gear)) return 0;
    int count = 0;
    if ((gear.kind & yyc::kKindMask) == yyc::kArray) {
        yyc::RValue length{};
        yyc::CallBuiltin("array_length_1d", player, 1, &gear, &length);
        count = static_cast<int>(yyc::ToReal(length));
        yyc::Free(&length);
    }
    yyc::Free(&gear);
    return (std::min)(count, static_cast<int>(kMaxGearCategories));
}

// Playtest 8 #15: amulets did nothing for the guest. Its drops are made by the host, which asks the gear of
// PlayerFind() - the guest's puppet, that had only the starting gear. The puppet carries the guest's gear.
void SendOwnGear(yyc::CInstance* localPlayer) {
    static std::vector<float> sent;
    static unsigned steps = 0;
    if (++steps % kGearEverySteps != 0) return;
    const int id = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    std::vector<float> gear(static_cast<size_t>(GearCount(localPlayer, id)));
    for (size_t i = 0; i < gear.size(); ++i) gear[i] = static_cast<float>(OwnedGear(id, static_cast<int>(i)));
    if (gear.empty() || (gear == sent && steps % kGearResendSteps != 0)) return;
    if (net::Send(kGearState, gear.data(), gear.size() * sizeof(float))) sent = gear;
}

void ApplyGuestGear(const net::Message& message) {
    yyc::CInstance* puppet = puppet::Instance(message.from);
    if (puppet == nullptr || message.payload.size() % sizeof(float) != 0) return;
    const int id = static_cast<int>(yyc::GetBuiltinReal(puppet, yyc::va::YYVar_id));
    const size_t count = (std::min)(message.payload.size() / sizeof(float), static_cast<size_t>(GearCount(puppet, id)));
    int changed = 0;
    for (size_t i = 0; i < count; ++i) {
        float value = 0;
        memcpy(&value, message.payload.data() + i * sizeof(float), sizeof(value));
        if (!std::isfinite(value) || value == OwnedGear(id, static_cast<int>(i))) continue;
        yyc::RValue item = yyc::Real(value);
        yyc::SetVarAt(id, kSlotGearCurrent, static_cast<int>(i), &item);
        ++changed;
    }
    if (changed > 0) Log("character: guest %u's puppet now has its gear (%d categories changed)", message.from, changed);
}

// The player's buff[type, k] (shrines, potions, boons): rows, then each row's values; absent ones are NaN.
std::vector<float> ReadBuffs(yyc::CInstance* player, int id) {
    yyc::RValue buff{};
    std::vector<float> out;
    if (!yyc::GetVar(id, kSlotBuff, &buff) || (buff.kind & yyc::kKindMask) != yyc::kArray) {
        yyc::Free(&buff);
        return out;
    }
    yyc::RValue height{};
    yyc::CallBuiltin("array_height_2d", player, 1, &buff, &height);
    const int rows = (std::min)(static_cast<int>(yyc::ToReal(height)), kMaxBuffRows);
    yyc::Free(&height);
    out.push_back(static_cast<float>(rows));
    for (int r = 0; r < rows; ++r) {
        yyc::RValue args[2] = {buff, yyc::Real(r)};
        yyc::RValue width{};
        yyc::CallBuiltin("array_length_2d", player, 2, args, &width);
        const int cols = static_cast<int>(yyc::ToReal(width));
        yyc::Free(&width);
        for (int c = 0; c < kBuffColumns; ++c) {
            yyc::RValue value{};
            const bool known = c < cols && yyc::GetVarAt(id, kSlotBuff, r * yyc::kArray2dStride + c, &value) &&
                               yyc::IsNumber(value);
            out.push_back(known ? static_cast<float>(yyc::ToReal(value)) : NAN);
            yyc::Free(&value);
        }
    }
    yyc::Free(&buff);
    return out;
}

// Playtest 8 #10: HasBuff asks PlayerFind().buff - for the guest's drops (made by the host) its puppet's.
void SendOwnBuffs(yyc::CInstance* localPlayer) {
    static std::vector<float> sent;
    static unsigned steps = 0;
    if (++steps % kGearEverySteps != 0) return;
    const int id = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    const std::vector<float> buffs = ReadBuffs(localPlayer, id);
    const auto same = [](const std::vector<float>& a, const std::vector<float>& b) {
        return a.size() == b.size() && memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;  // NaN-safe
    };
    if (buffs.empty() || (same(buffs, sent) && steps % kGearResendSteps != 0)) return;
    if (net::Send(kBuffState, buffs.data(), buffs.size() * sizeof(float))) sent = buffs;
}

void ApplyGuestBuffs(const net::Message& message) {
    yyc::CInstance* puppet = puppet::Instance(message.from);
    const size_t count = message.payload.size() / sizeof(float);
    if (puppet == nullptr || count == 0 || message.payload.size() % sizeof(float) != 0) return;
    std::vector<float> values(count);
    memcpy(values.data(), message.payload.data(), message.payload.size());
    const int rows = static_cast<int>(values[0]);
    if (rows < 0 || rows > kMaxBuffRows || count != 1 + static_cast<size_t>(rows) * kBuffColumns) return;
    const int id = static_cast<int>(yyc::GetBuiltinReal(puppet, yyc::va::YYVar_id));
    int active = 0;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < kBuffColumns; ++c) {
            const float value = values[1 + r * kBuffColumns + c];
            if (!std::isfinite(value)) continue;
            yyc::RValue v = yyc::Real(value);
            yyc::SetVarAt(id, kSlotBuff, r * yyc::kArray2dStride + c, &v);
            if (c == 0 && value != 0) ++active;
        }
    }
    static int lastActive = -1;
    if (active != lastActive) Log("character: guest %u's puppet carries its buffs (%d active)", message.from, active);
    lastActive = active;
}

// Every 15 s: save into the temporary slot, then upload its Data.cin.
void GuestTick(yyc::CInstance* localPlayer) {
    if (net::IsConnected()) {
        SendOwnGear(localPlayer);
        SendOwnBuffs(localPlayer);
    }
    if (g_settle > 0 && --g_settle == 0) {
        GuestUploadNow();
        return;
    }
    if (++g_steps % kUploadEverySteps != 0 || !net::IsConnected()) return;
    // SaveGame in a dungeon would write the dungeon into the guest's copy of the world (back out, it loads that).
    if (static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_room)) != yyc::kRoomGame) return;
    world::GuestSaveCharacter(localPlayer);
    g_settle = kSaveSettleSteps;
}

bool HostHandle(const net::Message& message, yyc::CInstance* localPlayer, bool* guestIntroduced) {
    if (message.type == kGearState) {
        ApplyGuestGear(message);
        return true;
    }
    if (message.type == kBuffState) {
        ApplyGuestBuffs(message);
        return true;
    }
    if (message.type == kGuestHello) {
        const std::string asked = SafeName(std::string(message.payload.begin(), message.payload.end()));
        const std::string name = FreeName(asked, message.from);
        if (name != asked) {
            Log("character: '%s' is playing already; guest %u plays as '%s'", asked.c_str(), message.from,
                name.c_str());
        }
        g_guestNames[message.from] = name;
        const std::vector<uint8_t> stored = ReadFile(GuestFile(name));
        std::vector<uint8_t> payload{static_cast<uint8_t>(stored.empty() ? 0 : 1)};
        payload.insert(payload.end(), stored.begin(), stored.end());
        net::SendTo(message.from, kCharacter, payload.data(), payload.size());
        Log("character: guest %u is '%s' (%s)", message.from, name.c_str(),
            stored.empty() ? "new" : "stored character sent");
        *guestIntroduced = true;
        return true;
    }
    if (message.type == kCharacterSave) {
        const std::string name = HostGuestName(message.from);
        const std::vector<uint8_t>& data = message.payload;
        // An empty or foreign file would replace the stored character and make this guest a new one next time.
        if (data.size() < 4 || memcmp(data.data(), "cndr", 4) != 0) {
            Log("character: guest '%s' sent a broken save (%zu bytes) - the stored one is kept", name.c_str(),
                data.size());
            return true;
        }
        if (!WriteFile(GuestFile(name), data.data(), data.size())) {
            Log("character: cannot save guest '%s'", name.c_str());
            return true;
        }
        Log("character: saved guest '%s' (%zu bytes)", name.c_str(), data.size());
        return true;
    }
    return false;
}

void Reset() {
    g_answered = false;
    g_hasStored = false;
    g_swapDone = false;
    g_settle = 0;
    g_guestNames.clear();
}

}  // namespace coop::character
