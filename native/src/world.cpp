#include "world.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "MinHook.h"
#include "interiors.h"
#include "log.h"

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

yyc::RValue* __cdecl SaveGameHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                  yyc::RValue** args) {
    // Inside an interior built in the world room, the save must not keep it (or the player past the world's edge).
    if (!interiors::IsInside()) return Save(self, other, result, argc, args);
    interiors::BeforeSave();
    yyc::RValue* r = Save(self, other, result, argc, args);
    interiors::AfterSave();
    return r;
}

void BeginReceive() {
    g_file.close();
    g_filesReceived = 0;
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
    g_file.close();
    if (g_targetDir.empty() || payload.size() < 2) return;
    const size_t length = payload[0] | (payload[1] << 8);
    if (payload.size() != 2 + length + 4) return;
    const std::string relative(payload.begin() + 2, payload.begin() + 2 + length);
    g_fileRemaining = Get32(payload.data() + 2 + length);
    if (!IsSafeRelativePath(relative) || g_fileRemaining > kMaxFileSize) {
        Log("world: rejected file '%s' (%u bytes)", relative.c_str(), g_fileRemaining);
        return;
    }
    const fs::path target = g_targetDir / fs::u8path(relative);
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    g_file.open(target, std::ios::binary | std::ios::trunc);
    if (g_file.is_open()) ++g_filesReceived;
}

void WriteChunk(const std::vector<uint8_t>& payload) {
    if (!g_file.is_open() || payload.size() > g_fileRemaining) return;
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
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(yyc::va::Script_SaveGame),
                                           reinterpret_cast<void*>(&SaveGameHook),
                                           reinterpret_cast<void**>(&g_origSaveGame));
    if (status != MH_OK) Log("world: hook SaveGame failed: %s", MH_StatusToString(status));
    return status == MH_OK;
}

bool HandleMessage(const net::Message& message) {
    switch (message.type) {
        case kWorldBegin:
            BeginReceive();
            return true;
        case kWorldFile:
            BeginFile(message.payload);
            return true;
        case kWorldChunk:
            WriteChunk(message.payload);
            return true;
        case kWorldEnd:
            g_file.close();
            g_ready = g_filesReceived > 0;
            Log("world: received %d files into %s", g_filesReceived, g_targetDir.u8string().c_str());
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
