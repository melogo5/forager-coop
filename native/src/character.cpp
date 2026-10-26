#include "character.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
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
};

constexpr uint32_t kScriptDeserializeCharacterChunk = 0x005BC8F0;
// GearEdit(category, field, value): field 2 is the gear a player owns in that category - PlayerFind().gearCurrent
// (objItemGet when gear is picked up, DeserializeCharacterChunk on load); other fields are global.gear.
constexpr uint32_t kScriptGearEdit = 0x006CAAC0;
constexpr int kGearFieldOwned = 2;
constexpr int kSlotGearCurrent = 0x535;
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

// Playtest 6: crafted gear went missing (host and guest, over a rejoin or in play) and nothing in the logs said
// when. In a session every change of a player's owned gear is logged with who got it and what changed it.
yyc::RValue* __cdecl GearEditHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                  yyc::RValue** args) {
    const bool traced = net::IsConnected() && argc >= 3 && yyc::IsNumber(*args[0]) && yyc::IsNumber(*args[1]) &&
                        static_cast<int>(yyc::ToReal(*args[1])) == kGearFieldOwned;
    if (!traced) return g_origGearEdit(self, other, result, argc, args);
    const yyc::StackFrame* caller = yyc::TopFrame();
    const std::string by = caller != nullptr && caller->name != nullptr ? caller->name : "?";
    yyc::RValue zero = yyc::Real(0);
    yyc::RValue* findArgs[1] = {&zero};
    yyc::RValue found{};
    yyc::At<yyc::ScriptFn>(yyc::va::Script_PlayerFind)(self, other, &found, 1, findArgs);  // as GearEdit will
    const int target = static_cast<int>(yyc::ToReal(found));
    yyc::Free(&found);
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

// Every 15 s: save into the temporary slot, then upload its Data.cin.
void GuestTick(yyc::CInstance* localPlayer) {
    if (g_settle > 0 && --g_settle == 0) {
        GuestUploadNow();
        return;
    }
    if (++g_steps % kUploadEverySteps != 0 || !net::IsConnected()) return;
    world::GuestSaveCharacter(localPlayer);
    g_settle = kSaveSettleSteps;
}

bool HostHandle(const net::Message& message, yyc::CInstance* localPlayer, bool* guestIntroduced) {
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
