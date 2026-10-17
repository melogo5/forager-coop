#include "character.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "MinHook.h"
#include "log.h"
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
constexpr uint32_t kScriptCinderLoad = 0x005CE0D0;    // CinderLoad(buffer) -> cinder (chunk map)
constexpr uint32_t kScriptCinderDelete = 0x005C9BD0;  // CinderDelete(cinder)
constexpr int kSlotSaveFile = 0x76F;
constexpr unsigned kUploadEverySteps = 900;    // 15 s (a save is small: a few KB)
constexpr unsigned kSaveSettleSteps = 120;     // SaveGame is async
constexpr size_t kMaxName = 32;
constexpr char kGuestsDir[] = "coop_guests";
constexpr wchar_t kLocalCharacterFile[] = L"coop_guest_character.cin";  // guest side, in the save area

bool (*g_isGuest)() = nullptr;
yyc::ScriptFn g_origDeserialize = nullptr;

// Guest state.
std::atomic<bool> g_answered{false};
std::atomic<bool> g_hasStored{false};
bool g_swapDone = false;
unsigned g_steps = 0;
unsigned g_settle = 0;

// Host state.
std::string g_guestName;

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

// Host: <save area>/saveData<current slot>/coop_guests/<name>.cin
fs::path GuestFile(const std::string& name) {
    const yyc::RValue* slot = yyc::GlobalVar(kSlotSaveFile);
    const fs::path area = SaveArea();
    if (area.empty() || slot == nullptr) return {};
    const int slotNumber = static_cast<int>(yyc::ToReal(*slot));
    return area / (L"saveData" + std::to_wstring(slotNumber)) / kGuestsDir / fs::u8path(SafeName(name) + ".cin");
}

std::vector<uint8_t> ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteFile(const fs::path& path, const uint8_t* data, size_t size) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    std::ofstream(path, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
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

}  // namespace

bool InstallHooks(bool (*isGuest)()) {
    g_isGuest = isGuest;
    const MH_STATUS status =
        MH_CreateHook(yyc::At<void*>(kScriptDeserializeCharacterChunk), reinterpret_cast<void*>(&DeserializeHook),
                      reinterpret_cast<void**>(&g_origDeserialize));
    if (status != MH_OK) Log("character: hook failed: %s", MH_StatusToString(status));
    return status == MH_OK;
}

std::string SavedGuestName() {
    std::ifstream in(SaveArea() / kNameFile);
    std::string name;
    std::getline(in, name);
    return name.empty() ? SafeName(ComputerName()).substr(0, 16) : SafeName(name);
}

void SetGuestName(const std::string& name) {
    g_ownName = SafeName(name);
    std::ofstream(SaveArea() / kNameFile, std::ios::trunc) << g_ownName << "\n";
}

void GuestOnConnect() {
    const std::string name = g_ownName.empty() ? SavedGuestName() : g_ownName;
    net::Send(kGuestHello, name.data(), name.size());
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

// Every minute: save into the temporary slot, then upload its Data.cin and remove the slot again.
void GuestTick(yyc::CInstance* localPlayer) {
    if (g_settle > 0 && --g_settle == 0) {
        const std::vector<uint8_t> data = ReadFile(fs::path(world::GuestSlotPath()) / L"Data.cin");
        if (data.empty()) {
            Log("character: save produced no Data.cin");
        } else if (!net::Send(kCharacterSave, data.data(), data.size())) {
            Log("character: upload of %zu bytes failed", data.size());
        }
        return;
    }
    if (++g_steps % kUploadEverySteps != 0 || !net::IsConnected()) return;
    world::GuestSaveCharacter(localPlayer);
    g_settle = kSaveSettleSteps;
}

bool HostHandle(const net::Message& message, yyc::CInstance* localPlayer, bool* guestIntroduced) {
    if (message.type == kGuestHello) {
        g_guestName = SafeName(std::string(message.payload.begin(), message.payload.end()));
        const std::vector<uint8_t> stored = ReadFile(GuestFile(g_guestName));
        std::vector<uint8_t> payload{static_cast<uint8_t>(stored.empty() ? 0 : 1)};
        payload.insert(payload.end(), stored.begin(), stored.end());
        net::Send(kCharacter, payload.data(), payload.size());
        Log("character: guest '%s' (%s)", g_guestName.c_str(), stored.empty() ? "new" : "stored character sent");
        *guestIntroduced = true;
        return true;
    }
    if (message.type == kCharacterSave) {
        WriteFile(GuestFile(g_guestName.empty() ? "guest" : g_guestName), message.payload.data(),
                  message.payload.size());
        Log("character: saved guest '%s' (%zu bytes)", g_guestName.c_str(), message.payload.size());
        return true;
    }
    return false;
}

}  // namespace coop::character
