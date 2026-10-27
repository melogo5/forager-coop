#include "dungeons.h"

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "log.h"

namespace coop::dungeons {

namespace {

constexpr uint8_t kGoRoom = 38;   // host -> guests: RoomMsg, everyone into this room (the host is on its way)
constexpr uint8_t kAskRoom = 39;  // guest -> host: RoomMsg, the guest's way in or out of a dungeon
constexpr int kFirstDungeon = 14, kLastDungeon = 21;  // decompiled/data/rooms.tsv
constexpr int kRoomGame = yyc::kRoomGame;
constexpr int kObjWorldControl = 3;     // holds currentDungeon (the entrances set it there)
constexpr int kObjTransition = 287;     // the fade between rooms: room_goto(target) at its darkest
constexpr int kSlotTarget = 0x852;
constexpr int kSlotCurrentDungeon = 0x15E;
constexpr int kSlotSpawnPlayerX = 0x7E6, kSlotSpawnPlayerY = 0x7E7;  // globals: where the world puts the player
constexpr uint32_t kScriptSaveGame = 0x004FAE80;

#pragma pack(push, 1)
struct RoomMsg {
    int32_t room;
    float currentDungeon;  // WorldControl.currentDungeon, as the entrance set it
    float spawnX, spawnY;  // the way back out: where the world puts the player
};
#pragma pack(pop)

bool (*g_isGuest)() = nullptr;
bool IsGuest() { return g_isGuest != nullptr && g_isGuest(); }

RoomMsg g_lastGo{};         // guest: the host's last "into room N"
unsigned g_waitSteps = 0;   // guest: steps outside the host's room
constexpr unsigned kRetrySteps = 240;  // 4 s: a fade takes about one
double g_entryX = 0, g_entryY = 0;  // where the player came into the current dungeon
int g_entryRoom = -1;
int g_following = -1;      // guest: the room the host told us to go to, allowed through once
int g_guestRoom = kRoomGame;  // guest: the host's room as last told

bool IsDungeon(int room) { return room >= kFirstDungeon && room <= kLastDungeon; }

int CurrentRoom(yyc::CInstance* any) { return static_cast<int>(yyc::GetBuiltinReal(any, yyc::va::YYVar_room)); }

double GlobalReal(int slot) {
    const yyc::RValue* value = yyc::GlobalVar(slot);
    return value != nullptr ? yyc::ToReal(*value) : 0;
}

void SetGlobalReal(int slot, double value) {
    if (yyc::RValue* global = yyc::GlobalVar(slot)) {
        yyc::Free(global);
        *global = yyc::Real(value);
    }
}

int WorldControl(yyc::CInstance* self) {
    const int id = static_cast<int>(yyc::CallReal(self, "instance_find", {double(kObjWorldControl), 0}));
    return yyc::FindInstance(id) != nullptr ? id : 0;
}

RoomMsg Describe(yyc::CInstance* self, int room) {
    RoomMsg msg{room, 0, static_cast<float>(GlobalReal(kSlotSpawnPlayerX)), static_cast<float>(GlobalReal(kSlotSpawnPlayerY))};
    if (const int control = WorldControl(self)) {
        yyc::RValue value{};
        if (yyc::GetVar(control, kSlotCurrentDungeon, &value) && yyc::IsNumber(value)) {
            msg.currentDungeon = static_cast<float>(yyc::ToReal(value));
        }
        yyc::Free(&value);
    }
    return msg;
}

// What an entrance (or the door back) does, from the message: the dungeon's globals, a save of the world when
// leaving it, and the fade that ends in room_goto.
void Go(yyc::CInstance* player, const RoomMsg& msg) {
    if (IsDungeon(msg.room)) {
        SetGlobalReal(kSlotSpawnPlayerX, msg.spawnX);
        SetGlobalReal(kSlotSpawnPlayerY, msg.spawnY);
        if (const int control = WorldControl(player)) {
            yyc::RValue value = yyc::Real(msg.currentDungeon);
            yyc::SetVar(control, kSlotCurrentDungeon, &value);
        }
    }
    if (CurrentRoom(player) == kRoomGame) {
        yyc::RValue result{};
        yyc::At<yyc::ScriptFn>(kScriptSaveGame)(player, player, &result, 0, nullptr);
        yyc::Free(&result);
    }
    const double x = yyc::GetBuiltinReal(player, yyc::va::YYVar_x), y = yyc::GetBuiltinReal(player, yyc::va::YYVar_y);
    const int fade = static_cast<int>(yyc::CallReal(player, "instance_create_depth", {x, y, -10000, double(kObjTransition)}));
    if (yyc::FindInstance(fade) != nullptr) {
        yyc::RValue target = yyc::Real(msg.room);
        yyc::SetVar(fade, kSlotTarget, &target);
    }
}

}  // namespace

void Install(bool (*isGuest)()) { g_isGuest = isGuest; }

bool InterceptRoomGoto(yyc::CInstance* self, int room) {
    if (!net::IsConnected() || self == nullptr) return false;
    const int current = CurrentRoom(self);
    const bool ours = IsDungeon(room) || (IsDungeon(current) && room == kRoomGame);
    if (!ours) return false;
    if (!IsGuest()) {
        const RoomMsg msg = Describe(self, room);
        net::Send(kGoRoom, &msg, sizeof(msg));
        Log("dungeons: the host goes from room %d to %d - the guests follow", current, room);
        return false;
    }
    if (g_following == room) {
        g_following = -1;
        Log("dungeons: following the host into room %d", room);
        return false;
    }
    const RoomMsg msg = Describe(self, room);
    net::Send(kAskRoom, &msg, sizeof(msg));
    Log("dungeons: asked the host to go from room %d to %d", current, room);
    return true;
}

int SharedRoom(yyc::CInstance* localPlayer) {
    if (IsGuest()) return g_guestRoom;
    const int room = CurrentRoom(localPlayer);
    return room == kRoomGame || IsDungeon(room) ? room : kRoomGame;
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer, bool* stop) {
    if (message.type != kGoRoom && message.type != kAskRoom) return false;
    if (message.payload.size() != sizeof(RoomMsg)) return true;
    RoomMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    const bool valid = (IsDungeon(msg.room) || msg.room == kRoomGame) && std::isfinite(msg.spawnX) &&
                       std::isfinite(msg.spawnY) && std::isfinite(msg.currentDungeon);
    if (!valid) return true;
    if (message.type == kAskRoom && !IsGuest()) {
        if (CurrentRoom(localPlayer) == msg.room) return true;
        Log("dungeons: guest %u asks to go to room %d - going", message.from, msg.room);
        Go(localPlayer, msg);
        return true;
    }
    if (message.type == kGoRoom && IsGuest()) {
        g_guestRoom = msg.room;
        g_lastGo = msg;
        g_waitSteps = 0;
        if (CurrentRoom(localPlayer) != msg.room) {
            g_following = msg.room;
            Go(localPlayer, msg);
            Log("dungeons: the host went to room %d - following", msg.room);
        }
        *stop = true;  // what comes after it is news of that room
    }
    return true;
}

void GuestTick(yyc::CInstance* localPlayer) {
    if (CurrentRoom(localPlayer) == g_guestRoom || ++g_waitSteps < kRetrySteps) return;
    g_waitSteps = 0;
    if (yyc::CallReal(localPlayer, "instance_exists", {double(kObjTransition)}) != 0) return;
    Log("dungeons: still not in room %d - going again", g_guestRoom);
    g_following = g_guestRoom;
    Go(localPlayer, g_lastGo.room == g_guestRoom ? g_lastGo : RoomMsg{g_guestRoom, 0, 0, 0});
}

void DebugTick(yyc::CInstance* localPlayer, bool isHost) {
    static const int room = [] {
        char value[32] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_DUNGEON", value, sizeof(value)) > 0 ? atoi(value) : 0;
    }();
    static const unsigned at = [] {
        char value[32] = {};
        if (GetEnvironmentVariableA("COOP_DEBUG_DUNGEON", value, sizeof(value)) == 0) return 0u;
        const char* comma = strchr(value, ',');
        return (comma != nullptr ? static_cast<unsigned>(atoi(comma + 1)) : 8u) * 60u;
    }();
    static unsigned steps = 0;
    if (!isHost || !IsDungeon(room) || !net::IsConnected()) return;
    ++steps;
    if (steps == at) {
        Log("dungeons: debug goes into room %d", room);
        Go(localPlayer, Describe(localPlayer, room));
    } else if (steps == at + 20 * 60) {
        Log("dungeons: debug goes back to the world");
        Go(localPlayer, Describe(localPlayer, kRoomGame));
    }
}

void OnRoomEntered(yyc::CInstance* localPlayer, int room) {
    if (!IsDungeon(room)) return;
    g_entryRoom = room;
    g_entryX = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_x);
    g_entryY = yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_y);
}

bool RespawnPoint(yyc::CInstance* localPlayer, double* x, double* y) {
    if (!IsDungeon(CurrentRoom(localPlayer)) || CurrentRoom(localPlayer) != g_entryRoom) return false;
    *x = g_entryX;
    *y = g_entryY;
    return true;
}

void Reset() {
    g_entryRoom = -1;
    g_following = -1;
    g_waitSteps = 0;
    g_guestRoom = kRoomGame;
}

}  // namespace coop::dungeons
