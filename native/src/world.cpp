#include "world.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "MinHook.h"
#include "interiors.h"
#include "log.h"
#include "mutant.h"

namespace coop::world {

namespace fs = std::filesystem;

namespace {

enum MessageType : uint8_t {
    kWorldBegin = 3,  // u32 file count
    kWorldFile = 4,   // u16 path length, path (UTF-8, '/'-separated, relative), u32 size
    kWorldChunk = 5,  // raw bytes of the current file
    kWorldEnd = 6,
};

constexpr int kSaveSettleSteps = 120;  // SaveGame is async; ~2 s at 60 fps is plenty for a few-KB slot
constexpr size_t kChunkSize = 32 * 1024;
constexpr size_t kMaxPath = 200;
constexpr uint32_t kMaxFileSize = 64u * 1024 * 1024;
constexpr char kMarkerName[] = "coop_world.txt";

// Host state (game thread).
int g_settleSteps = 0;
int g_pendingSlot = 0;
std::vector<net::PeerId> g_pendingPeers;  // guests waiting for the save in progress

// Guest state (game thread).
bool (*g_isGuest)() = nullptr;
yyc::ScriptFn g_origSaveGame = nullptr;

// Guest state (receive thread only).
fs::path g_targetDir;
std::ofstream g_file;
uint32_t g_fileRemaining = 0;
int g_filesReceived = 0;
uint32_t g_filesExpected = 0;  // as the host announced
int g_filesBroken = 0;         // rejected, cut short or not written: the world here differs from the host's
std::atomic<bool> g_ready{false};  // read by the game thread

fs::path SaveRoot() {
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    return length > 0 && length < MAX_PATH ? fs::path(local) / L"Forager" : fs::path();
}

// Only plain relative paths inside the slot: the peer is not trusted to choose where files go.
bool IsSafeRelativePath(const std::string& path) {
    if (path.empty() || path.size() > kMaxPath || path.front() == '/' || path.find("..") != std::string::npos) {
        return false;
    }
    for (const char c : path) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-' || c == '/' || c == ' ';
        if (!ok) return false;
    }
    return true;
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t Get32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

bool SendFile(net::PeerId peer, const fs::path& root, const fs::path& file) {
    const std::string relative = fs::relative(file, root).generic_u8string();
    std::ifstream in(file, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) return false;

    std::vector<uint8_t> header;
    header.push_back(static_cast<uint8_t>(relative.size() & 0xFF));
    header.push_back(static_cast<uint8_t>(relative.size() >> 8));
    header.insert(header.end(), relative.begin(), relative.end());
    Put32(header, static_cast<uint32_t>(data.size()));
    if (!net::SendTo(peer, kWorldFile, header.data(), header.size())) return false;
    for (size_t offset = 0; offset < data.size(); offset += kChunkSize) {
        const size_t size = (std::min)(kChunkSize, data.size() - offset);
        if (!net::SendTo(peer, kWorldChunk, data.data() + offset, size)) return false;
    }
    return true;
}

void SendSnapshot(int slotNumber, std::vector<net::PeerId> peers) {
    const std::string slot = "saveData" + std::to_string(slotNumber);
    const fs::path root = SaveRoot() / fs::u8path(slot);
    std::error_code error;
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root, error)) {
        // The host's stored guest characters stay on the host.
        const std::string relative = fs::relative(entry.path(), root, error).generic_u8string();
        if (relative.rfind("coop_guests/", 0) == 0) continue;
        if (entry.is_regular_file() && entry.path().filename() != kMarkerName) files.push_back(entry.path());
    }
    if (error || files.empty()) {
        Log("world: cannot read slot %s", slot.c_str());
        return;
    }
    std::vector<uint8_t> begin;
    Put32(begin, static_cast<uint32_t>(files.size()));
    for (const net::PeerId peer : peers) {
        bool ok = net::SendTo(peer, kWorldBegin, begin.data(), begin.size());
        for (const auto& file : files) ok = ok && SendFile(peer, root, file);
        ok = ok && net::SendTo(peer, kWorldEnd, nullptr, 0);
        Log("world: snapshot of %s (%zu files) to guest %u %s", slot.c_str(), files.size(), peer,
            ok ? "sent" : "FAILED");
    }
}

fs::path GuestSlotDir() {
    const fs::path root = SaveRoot();
    return root.empty() ? root : root / (L"saveData" + std::to_wstring(kGuestSlot));
}

void RemoveGuestSlot() {
    const fs::path dir = GuestSlotDir();
    std::error_code error;
    if (dir.empty() || !fs::exists(dir / kMarkerName, error)) return;  // only ever delete our own slot
    fs::remove_all(dir, error);
    Log("world: temporary slot %d %s", kGuestSlot, error ? "could not be removed" : "removed");
}

// The guest's game saves into slot 99 like any game into its slot: leaving the world (skill tree, other
// rooms) saves, and coming back reloads the world from that save. Marked so a later start cleans it up.
yyc::RValue* Save(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc, yyc::RValue** args) {
    if (g_isGuest != nullptr && g_isGuest()) {
        std::error_code error;
        fs::create_directories(GuestSlotDir(), error);
        std::ofstream(GuestSlotDir() / kMarkerName) << "Forager coop: the guest's working copy. Removed automatically.\n";
    }
    return g_origSaveGame(self, other, result, argc, args);
}

double ListCall(yyc::CInstance* self, const char* name, double list, double arg) {
    yyc::RValue args[2] = {yyc::Real(list), yyc::Real(arg)};
    yyc::RValue result{};
    yyc::CallBuiltin(name, self, 2, args, &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

double Call1(yyc::CInstance* self, const char* name, double arg) {
    yyc::RValue value = yyc::Real(arg);
    yyc::RValue result{};
    yyc::CallBuiltin(name, self, 1, &value, &result);
    const double number = yyc::ToReal(result);
    yyc::Free(&result);
    return number;
}

std::vector<int> ListIds(yyc::CInstance* self, double list) {
    std::vector<int> ids;
    const int count = static_cast<int>(Call1(self, "ds_list_size", list));
    for (int i = 0; i < count; ++i) ids.push_back(static_cast<int>(ListCall(self, "ds_list_find_value", list, i)));
    return ids;
}

double GlobalNumber(int slot) {
    const yyc::RValue* value = yyc::GlobalVar(slot);
    return value != nullptr && yyc::IsNumber(*value) ? yyc::ToReal(*value) : -1;
}

// SaveGame writes a record for every instance in its lists - global.herbs and global.trueInstanceCache (object ->
// ds_list of ids) - from inside `with (id)`. An instance that is asleep is woken for that only if it is listed in
// global.deactivatedInstances (TrueInstanceExists -> TemporaryActivate); one gone, or asleep and unlisted, is
// written as an empty record, and LoadZone then fails on it (asset_get_index(undefined)): the world never loads
// again (playtest 4, bug 10; seen on herbs). Before every save: the gone ones leave the lists, the unlisted
// sleepers are listed. Which coop path leaves them so is not known yet - the log says which ones.
void PrepareSaveLists(yyc::CInstance* self) {
    if (Mutant("savelists")) return;
    constexpr int kSlotHerbs = 0x55F, kSlotTrueInstanceCache = 0x8BD, kSlotDeactivated = 0x16C;  // var_slots.tsv
    const double deactivated = GlobalNumber(kSlotDeactivated);
    if (deactivated < 0) return;
    const std::vector<int> listedIds = ListIds(self, deactivated);
    std::set<int> listed(listedIds.begin(), listedIds.end());
    std::vector<double> lists;
    if (GlobalNumber(kSlotHerbs) >= 0) lists.push_back(GlobalNumber(kSlotHerbs));
    const double cache = GlobalNumber(kSlotTrueInstanceCache);
    if (cache >= 0) {
        const int keys = static_cast<int>(Call1(self, "ds_map_size", cache));
        yyc::RValue key{};
        yyc::RValue mapArg = yyc::Real(cache);
        yyc::CallBuiltin("ds_map_find_first", self, 1, &mapArg, &key);
        for (int k = 0; k < keys; ++k) {
            yyc::RValue args[2] = {yyc::Real(cache), key};
            yyc::RValue value{};
            yyc::CallBuiltin("ds_map_find_value", self, 2, args, &value);
            if (yyc::IsNumber(value)) lists.push_back(yyc::ToReal(value));
            yyc::Free(&value);
            yyc::RValue next{};
            yyc::CallBuiltin("ds_map_find_next", self, 2, args, &next);
            yyc::Free(&key);
            key = next;
        }
        yyc::Free(&key);
    }
    int gone = 0, unlisted = 0;
    for (const double list : lists) {
        const std::vector<int> ids = ListIds(self, list);
        for (int i = static_cast<int>(ids.size()) - 1; i >= 0; --i) {
            const yyc::CInstance* instance = yyc::FindInstance(ids[i]);
            if (instance == nullptr) {
                ListCall(self, "ds_list_delete", list, i);
                if (++gone <= 10) Log("world: instance %d is gone but still listed - left out of the save", ids[i]);
            } else if (yyc::IsDeactivated(instance) && listed.insert(ids[i]).second) {
                ListCall(self, "ds_list_add", deactivated, ids[i]);
                if (++unlisted <= 10) {
                    Log("world: instance %d (object %.0f at %.0f, %.0f) sleeps unlisted - listed so the save wakes it",
                        ids[i], yyc::GetBuiltinReal(const_cast<yyc::CInstance*>(instance), yyc::va::YYVar_object_index),
                        yyc::GetBuiltinReal(const_cast<yyc::CInstance*>(instance), yyc::va::YYVar_x),
                        yyc::GetBuiltinReal(const_cast<yyc::CInstance*>(instance), yyc::va::YYVar_y));
                }
            }
        }
    }
    if (gone + unlisted > 0) Log("world: before the save: %d gone, %d unlisted sleepers", gone, unlisted);
}

// A herb on global.herbs that is gone stops the game: HerbProcess (WorldControl's Step_1) walks the list and
// IsDeactivated fails on it ("bool argument is unset", the "UH OH" window) - the same step the id goes bad. So the
// list is cleaned right before every HerbProcess. SaveGame fails the same way on a gone id in trueInstanceCache
// (PrepareSaveLists).
constexpr uint32_t kScriptHerbProcess = 0x004BD0A0;
yyc::ScriptFn g_origHerbProcess = nullptr;
int g_herbsLogged = 0;

yyc::RValue* __cdecl HerbProcessHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                     yyc::RValue** args) {
    constexpr int kSlotHerbs = 0x55F;
    const double herbs = Mutant("herbs") ? -1 : GlobalNumber(kSlotHerbs);
    if (herbs >= 0) {
        const std::vector<int> ids = ListIds(self, herbs);
        for (int i = static_cast<int>(ids.size()) - 1; i >= 0; --i) {
            if (yyc::FindInstance(ids[i]) != nullptr) continue;
            ListCall(self, "ds_list_delete", herbs, i);
            if (++g_herbsLogged <= 20) {
                Log("world: herb %d is gone but still listed - taken off (HerbProcess would stop the game)", ids[i]);
            }
        }
    }
    return g_origHerbProcess(self, other, result, argc, args);
}

yyc::RValue* __cdecl SaveGameHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                  yyc::RValue** args) {
    PrepareSaveLists(self);
    // Inside an interior built in the world room, the save must not keep it (or the player past the world's edge).
    if (!interiors::IsInside()) return Save(self, other, result, argc, args);
    interiors::BeforeSave();
    yyc::RValue* r = Save(self, other, result, argc, args);
    interiors::AfterSave();
    return r;
}

// Closes the file being written; one that did not get all its bytes counts as broken.
void EndFile() {
    if (!g_file.is_open()) return;
    g_file.close();
    if (g_fileRemaining != 0 || g_file.fail()) ++g_filesBroken;
    g_fileRemaining = 0;
}

void BeginReceive(const std::vector<uint8_t>& payload) {
    g_file.close();
    g_filesReceived = 0;
    g_filesBroken = 0;
    g_filesExpected = payload.size() == 4 ? Get32(payload.data()) : 0;
    g_ready = false;
    const fs::path root = SaveRoot();
    if (root.empty()) {
        Log("world: LOCALAPPDATA is not set");
        return;
    }
    // A fixed high slot: the guest's working copy, replaced on every join, removed back in the main menu.
    g_targetDir = GuestSlotDir();
    std::error_code error;
    fs::remove_all(g_targetDir, error);
    fs::create_directories(g_targetDir, error);
    std::ofstream(g_targetDir / kMarkerName) << "Forager coop: world received from the host. Overwritten on join.\n";
}

void BeginFile(const std::vector<uint8_t>& payload) {
    EndFile();
    if (g_targetDir.empty() || payload.size() < 2) return;
    const size_t length = payload[0] | (payload[1] << 8);
    if (payload.size() != 2 + length + 4) return;
    const std::string relative(payload.begin() + 2, payload.begin() + 2 + length);
    g_fileRemaining = Get32(payload.data() + 2 + length);
    if (!IsSafeRelativePath(relative) || g_fileRemaining > kMaxFileSize) {
        Log("world: rejected file '%s' (%u bytes)", relative.c_str(), g_fileRemaining);
        ++g_filesBroken;
        return;
    }
    const fs::path target = g_targetDir / fs::u8path(relative);
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    g_file.open(target, std::ios::binary | std::ios::trunc);
    if (g_file.is_open()) {
        ++g_filesReceived;
    } else {
        Log("world: cannot write '%s'", relative.c_str());
        ++g_filesBroken;
    }
}

void WriteChunk(const std::vector<uint8_t>& payload) {
    if (!g_file.is_open()) return;
    if (payload.size() > g_fileRemaining) {
        g_file.setstate(std::ios::failbit);  // more than announced: the file is not what the host has
        return;
    }
    g_file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    g_fileRemaining -= static_cast<uint32_t>(payload.size());
}

}  // namespace

void HostRequestSnapshot(yyc::CInstance* localPlayer, net::PeerId peer) {
    g_pendingPeers.push_back(peer);
    if (g_settleSteps > 0) {  // a save is under way: it serves this guest too (reconcile fixes the moment's lag)
        Log("world: guest %u waits for the save in progress", peer);
        return;
    }
    const yyc::RValue* saveFile = yyc::GlobalVar(yyc::slot::saveFile);
    if (saveFile == nullptr || (saveFile->kind & yyc::kKindMask) != yyc::kReal) {
        Log("world: global.saveFile is not a slot number; no snapshot");
        g_pendingPeers.clear();
        return;
    }
    g_pendingSlot = static_cast<int>(saveFile->real);
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(yyc::va::Script_SaveGame)(localPlayer, localPlayer, &result, 0, nullptr);
    yyc::Free(&result);
    g_settleSteps = kSaveSettleSteps;
    Log("world: saving slot %d for guest %u", g_pendingSlot, peer);
}

void HostTick() {
    if (g_settleSteps == 0 || --g_settleSteps != 0) return;
    std::thread(SendSnapshot, g_pendingSlot, std::move(g_pendingPeers)).detach();
    g_pendingPeers.clear();
}

bool GuestWorldReady() { return g_ready; }

void GuestSaveCharacter(yyc::CInstance* localPlayer) {
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(yyc::va::Script_SaveGame)(localPlayer, localPlayer, &result, 0, nullptr);
    yyc::Free(&result);
}

std::wstring GuestSlotPath() { return GuestSlotDir().wstring(); }

void RemoveGuestSaveSlot() {
    // Our reserved slot: SaveGame recreates it without the marker, so delete by name.
    std::error_code error;
    const fs::path dir = GuestSlotDir();
    if (!dir.empty()) fs::remove_all(dir, error);
}

bool InstallHooks(bool (*isGuest)()) {
    g_isGuest = isGuest;
    RemoveGuestSlot();
    if (MH_CreateHook(yyc::At<void*>(kScriptHerbProcess), reinterpret_cast<void*>(&HerbProcessHook),
                      reinterpret_cast<void**>(&g_origHerbProcess)) != MH_OK) {
        Log("world: hook HerbProcess failed");
    }
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(yyc::va::Script_SaveGame),
                                           reinterpret_cast<void*>(&SaveGameHook),
                                           reinterpret_cast<void**>(&g_origSaveGame));
    if (status != MH_OK) Log("world: hook SaveGame failed: %s", MH_StatusToString(status));
    return status == MH_OK;
}

bool HandleMessage(const net::Message& message) {
    switch (message.type) {
        case kWorldBegin:
            BeginReceive(message.payload);
            return true;
        case kWorldFile:
            BeginFile(message.payload);
            return true;
        case kWorldChunk:
            WriteChunk(message.payload);
            return true;
        case kWorldEnd:
            EndFile();
            g_ready = g_filesReceived > 0;
            Log("world: received %d files into %s", g_filesReceived, g_targetDir.u8string().c_str());
            if (g_filesBroken > 0 || g_filesReceived != static_cast<int>(g_filesExpected)) {
                Log("world: WARNING the world came incomplete: %d of %u files, %d broken - it may differ from the "
                    "host's", g_filesReceived, g_filesExpected, g_filesBroken);
            }
            return true;
        default:
            return false;
    }
}

void Reset() {
    g_file.close();
    g_fileRemaining = 0;
    g_filesReceived = 0;
    g_ready = false;
    g_settleSteps = 0;
    g_pendingPeers.clear();
}

}  // namespace coop::world
