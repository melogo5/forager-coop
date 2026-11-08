#include "puzzles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

#include "MinHook.h"
#include "character.h"
#include "files.h"
#include "log.h"
#include "puppet.h"
#include "sync.h"

namespace coop::puzzles {

namespace {

constexpr uint8_t kPuzzleState = 45;  // either way: StateEntry list, what pieces are now (see kKinds: who sends)
constexpr uint8_t kPuzzleUse = 46;    // guest -> host: UseMsg, the guest's E on a piece
constexpr uint8_t kPuzzleMirror = 104;  // either way (the host passes it on): UseMsg, an E done by everyone
constexpr uint8_t kPuzzleReset = 105;   // either way (the host passes it on): int32 puzzle number, /puzzlereset
constexpr uint8_t kPuzzleDig = 111;     // either way (the host passes it on): DigMsg, a puzzle dig spot dug here
constexpr unsigned kEverySteps = 6;   // 10 looks a second
constexpr int kSlotX = -1, kSlotY = -2;  // StateEntry.slot for the position
constexpr int kSlotImage = -3;           // ... for image_index (a terminal's symbol)
constexpr int kSlotImageSpeed = -4;      // ... for image_speed (a skull brazier catching fire)
constexpr size_t kMaxEntries = 512;
// global.puzzle: a flag per dungeon puzzle solved (an array of numbers). StateEntry.object kGlobalPuzzle, slot = index.
constexpr int kSlotGlobalPuzzle = 0x736;
constexpr int16_t kGlobalPuzzle = -1;
constexpr int kMaxPuzzleFlags = 64;

// Variables (decompiled/code/var_slots.tsv) of the pieces.
constexpr int kSolved = 0x7D8, kElectrified = 0x1D2, kFacing = 0x409, kAngleTo = 0x58, kReflectLaser = 0x745,
              kDown = 0x1A7, kNumber = 0x6AF, kUsable = 0x8CA, kH = 0x553, kV = 0x8D9;
// objGemSocketPillar.socket: the gem it holds (what its Draw_0 shows; solved is socket == solveableItem).
constexpr int kSocket = 0x7D0;
// Ice (objFrozenChest, frozen doors): a fire hit sets `burn`, `freeze` approaches `meltTo` every step, and at 0 the
// block makes what it holds (the boss key) and goes.
constexpr int kFreeze = 0x443, kMeltTo = 0x63C, kBurn = 0xD7, kFrozen = 0x447;
constexpr int kObjFrozenChest = 178;
constexpr int kObjTreasureChestSmall = 151;
constexpr int kOnFire = 0x6B3;  // a small chest of the fire island burning (var_slots.tsv)
constexpr int kObjEyeStatue = 174;

enum class Use : uint8_t {
    kNone,     // not interacted with (or nothing of ours happens on E)
    kAtHost,   // the guest's E is done by the host, as the guest's puppet
    kOwn,      // the guest's E stays its own (it takes from its inventory); its result goes to the host
    kMirror,   // anyone's E is done by every player on their own copy, as themselves
};

struct Kind {
    int object;
    uint32_t interact;  // Other_11 (E), 0 if not hooked
    Use use;
    bool moves;  // its place is the host's too (pushed cubes)
    std::vector<int> slots;
    bool inWorld = false;  // a kOwn piece shared in the world as well (see SharedHere)
    // In the world, slots of a host-only piece that a guest sends too (and the host takes): what the guest's own player
    // did to it there, which the host's piece then plays out itself (its own solve, its own reward).
    std::vector<int> worldGuestSlots = {};
};

const std::vector<Kind> kKinds = {
    {193, 0x00D08D00, Use::kAtHost, true, {kElectrified}},                      // objElectricCube
    {194, 0, Use::kNone, false, {kElectrified}},                                 // objReceptor
    // The desert's battery (puzzle 4) is one too: charged by a thunder rod's bolt, a thunder potion's buff next to it or
    // a watcher's beam - each at the player who did it, so each player charged their own and got their own chests. In
    // the world a guest's charge goes to the host as well, whose battery then solves and makes the chests (the
    // guest's own are removed: kRewardMakers). `solved` stays the host's: sent ahead it would keep the host's from
    // making them, as with the gems.
    {175, 0, Use::kNone, false, {kElectrified, kSolved}, true, {kElectrified}},  // objReceptorBig
    {200, 0x00D1F4D0, Use::kAtHost, false, {kFacing, kAngleTo, kReflectLaser}},  // objMirror
    {181, 0x00CDAFC0, Use::kAtHost, false, {kSlotImage, kUsable}},  // objRunicPillar: its rune (colour, glow: animated)
    {205, 0x00D2EEF0, Use::kAtHost, false, {kSolved}},                           // objFireTempleBrazier
    {183, 0x00CE3C10, Use::kOwn, false, {kUsable}},  // objBloodAltar: E opens the boon choice - the guest's
    // The skull islands' gem puzzle (and the dungeons'): each player puts in gems of their own inventory, and every
    // socket has to hold its gem. Not shared in the world, each player saw only their own gems and the puzzle never
    // came out for either; `socket` is the gem drawn in it. `solved` is not shared: a socket's Step_0 sets it when its
    // `socket` turns into its `solveableItem`, and at that moment checks the others and makes the reward chest. Sent
    // with the gem, a guest's last `solved` reached the host already set - the host's socket never turned solved
    // itself and made no chest. Each side's Step_0 solves its sockets from the gems; the host's chest is everyone's.
    {188, 0x00CF1090, Use::kOwn, false, {kSocket, kNumber, kUsable}, true},  // objGemSocketPillar
    // The sleeping princess (a stone statue): the same socket as a gem's - a player puts in a flower of their own and
    // her Step_0 wakes her when `socket` is the cinderbloom (item 18) and she is not `solved` yet, then makes the reward
    // chest. Not shared, each player saw only their own flower and got a chest of their own (a guest's was the guest's
    // alone). Only `socket` is shared: sent with it, a guest's `solved` reached the host first and the host's statue
    // never made its chest - nobody got one. Each side's Step_0 wakes its statue from the flower; the host's chest is
    // the one everybody gets.
    {177, 0x00CC3060, Use::kOwn, false, {kSocket}, true},  // objSleepingStatue
    // The eye statue: an arrow from the right side solves it (objArrow's collision with parObject sets `solved` and
    // makes the reward chest right there). Only the shooter's statue was solved: a guest's chest was the guest's
    // alone, and a guest could solve the host's already solved statue again for a second chest. `solved` is shared;
    // a guest's solve makes the host's chest (EyeSolvedAtHost).
    {kObjEyeStatue, 0, Use::kOwn, false, {kSolved}, true},  // objEyeStatue
    {180, 0x00CD1410, Use::kAtHost, false, {kSlotImage, kUsable}},  // objComputerTerminal: E turns the symbol
    // The skull island's three bells (puzzle 10): E rings one (its note, its sway) and the right order makes the chest
    // right there, in the E. Each player rang their own: the one who finished got a chest of their own alone. Anyone's
    // E rings every player's bell, as that player (kMirror): everyone hears the order and the host's chest is
    // everyone's (the guest's own is removed: kRewardMakers).
    {191, 0x00CF8D40, Use::kMirror, false, {}, true},  // objBell
    // The fire island's pillars (lights out, 16 of them): as objPushDownPillar below, `usable` is the animation's
    // (Step_0: usable from image_index), and its Other_7 at the animation's end sets `down`, checks the others and
    // makes the chest. Without the frame the guest's pillars never moved: the guest pressed one again and again.
    {182, 0x00CE0920, Use::kAtHost, false, {kDown, kSolved, kUsable, kSlotImage, kSlotImageSpeed}},  // objVolcanoPushDownPillar
    // objPushDownPillar is down when its animation is (Step_0: usable = image_index == 0): its frame goes along - sent
    // `usable` was undone the next step, the guest never saw its pillar go down and could not do the puzzle.
    {164, 0x00C96CF0, Use::kAtHost, false, {kSlotImage}},                        // objPushDownPillar
    // The skull islands' braziers: E lights one (solved; its image_speed plays the fire, Other_7 holds the last
    // frame); the last one lit makes the reward chest. Each player lit only their own: done at the host.
    {190, 0x00CF73B0, Use::kAtHost, false, {kSolved, kSlotImage, kSlotImageSpeed}},  // objSkullBrazier
    // The boss door opens for a key that follows PlayerFind() - the local player - so everyone's key follows them.
    // Frozen in ice (rmCrystalCave): a guest's fire thawed its own key only, the host could not take it (playtest 9).
    {75, 0x00B3F510, Use::kMirror, false, {kFreeze, kBurn}},  // objBossKey
    // Ice melts at every player from the same fire (a guest's fire arrow melted only its own block: the key was the
    // guest's alone, playtest 9); each block makes its own key (sync.cpp sends no destroy of it). In the world too:
    // the frozen chest island's block (puzzle 15) is this object, and there each player thawed only their own.
    {kObjFrozenChest, 0, Use::kOwn, false, {kFreeze, kMeltTo, kBurn}, true},  // objFrozenChest
    {209, 0, Use::kOwn, false, {kFreeze, kMeltTo, kBurn, kFrozen}},     // objDungeonDoor (a frozen one)
    {153, 0, Use::kOwn, false, {kFreeze, kBurn}},                       // objTreasureChestSmallFrozen
    // The fire island's burning chests (puzzle 18): a small chest can't be opened while `onFire`; water or cold puts it
    // out. Each player put out only their own: the host's chests still burned at the guest, so the host's opening (sent
    // by chests.cpp) could not open the guest's. Put out by anyone, out at everyone. Its `freeze` goes too: an ice
    // rod's hit freezes the chest (drawn over it, melting away in its Step_0), and that ice is what put the fire out -
    // the others saw the fire go with no ice. A water bottle (UseBottledWater) only clears `onFire`: no ice is shown.
    {kObjTreasureChestSmall, 0, Use::kOwn, false, {kOnFire, kFreeze}, true},  // objTreasureChestSmall
};
constexpr size_t kHookCount = 13;  // kinds with an Other_11 hook, in kKinds order

#pragma pack(push, 1)
// A piece is told by the sender's id of it, the host's id of it and its spot (Locate): world pieces have other ids
// at every player.
struct StateEntry {
    int32_t id;
    int32_t hostId;
    int16_t object;
    int16_t slot;
    float value;
    float x, y;
};
struct UseMsg {
    int32_t id;
    int32_t hostId;
    int16_t object;
    int16_t pad;
    float x, y;
};
#pragma pack(pop)

bool (*g_isHost)() = nullptr;
bool (*g_isGuest)() = nullptr;
std::map<std::pair<int, int>, float> g_known;  // (id, slot) -> the value last sent or received
unsigned g_steps = 0;
int g_applying = 0;  // a peer's E being done here: not intercepted again
yyc::EventFn g_orig[kHookCount] = {};

bool IsHost() { return g_isHost != nullptr && g_isHost(); }
bool IsGuest() { return g_isGuest != nullptr && g_isGuest(); }

const Kind* KindOf(int object) {
    for (const Kind& kind : kKinds) {
        if (kind.object == object) return &kind;
    }
    return nullptr;
}

int ObjectOf(yyc::CInstance* instance) {
    return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index));
}
int IdOf(yyc::CInstance* instance) { return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)); }

// The host's id of piece `id` here (-1 if unknown) and its spot, for a message about it.
void Locate(int id, int32_t* hostId, float* x, float* y) {
    *hostId = sync::HostIdOf(id);
    yyc::CInstance* piece = yyc::FindInstance(id);
    *x = piece != nullptr ? static_cast<float>(yyc::GetBuiltinReal(piece, yyc::va::YYVar_x)) : 0;
    *y = piece != nullptr ? static_cast<float>(yyc::GetBuiltinReal(piece, yyc::va::YYVar_y)) : 0;
}

StateEntry Entry(int id, int object, int slot, float value) {
    StateEntry entry{id, 0, static_cast<int16_t>(object), static_cast<int16_t>(slot), value, 0, 0};
    Locate(id, &entry.hostId, &entry.x, &entry.y);
    return entry;
}

UseMsg UseOf(int id, int object) {
    UseMsg msg{id, 0, static_cast<int16_t>(object), 0, 0, 0};
    Locate(id, &msg.hostId, &msg.x, &msg.y);
    return msg;
}

// The sender's piece here, 0 if none. A world piece (the desert's pillars to push down) has other ids at every
// player - it came from the host's save or its spawn - so it is found by the host's id or its spot: by the sender's
// id the host found nothing, and the guest's E did nothing (playtest 10). A dungeon room makes its pieces with the
// same ids everywhere: a cube the host pushed or a boss key following its player is not on the sender's spot.
int LocalPiece(yyc::CInstance* self, int senderId, int hostId, int object, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return 0;
    const int found = sync::Resolve(self, hostId, object, x, y);
    if (found != 0) return found;
    yyc::CInstance* same = yyc::FindInstance(senderId);
    return same != nullptr && ObjectOf(same) == object ? senderId : 0;
}

uint32_t Builtin(int slot) {
    return slot == kSlotX           ? yyc::va::YYVar_x
           : slot == kSlotY         ? yyc::va::YYVar_y
           : slot == kSlotImageSpeed ? yyc::va::YYVar_image_speed
                                     : yyc::va::YYVar_image_index;
}

bool ReadNumber(int id, int slot, float* out) {
    yyc::CInstance* instance = yyc::FindInstance(id);
    if (instance == nullptr) return false;
    if (slot < 0) {
        *out = static_cast<float>(yyc::GetBuiltinReal(instance, Builtin(slot)));
        return true;
    }
    yyc::RValue value{};
    const bool number = yyc::GetVar(id, slot, &value) && yyc::IsNumber(value);
    if (number) *out = static_cast<float>(yyc::ToReal(value));
    yyc::Free(&value);
    return number;
}

// A variable set even if the instance has none yet (a chest just made: its Create may not set it).
void SetNumber(int id, int slot, double value) {
    yyc::RValue v = yyc::Real(value);
    yyc::SetVar(id, slot, &v);
}

void WriteNumber(int id, int slot, float value) {
    yyc::CInstance* instance = yyc::FindInstance(id);
    if (instance == nullptr) return;
    if (slot < 0) {
        yyc::SetBuiltinReal(instance, Builtin(slot), value);
        return;
    }
    float current = 0;
    if (!ReadNumber(id, slot, &current)) return;  // never turn a string (or nothing) into a number
    yyc::RValue v = yyc::Real(value);
    yyc::SetVar(id, slot, &v);
}

// What this side sends: the host everything; a guest what is its own too (kOwn and kMirror pieces).
bool FromAnyone(const Kind& kind) { return kind.use == Use::kOwn || kind.use == Use::kMirror; }
bool Sends(const Kind& kind) { return IsHost() || FromAnyone(kind); }

// In the world only the pieces the host does (the desert's pillars): there ice and chests go with the world's own
// sync (sync.cpp), and a guest's own pieces stay as they were before the world's puzzles were played together.
bool InWorld(yyc::CInstance* self) { return yyc::GetBuiltinReal(self, yyc::va::YYVar_room) == yyc::kRoomGame; }
bool SharedHere(const Kind& kind, bool inWorld) { return !inWorld || kind.use == Use::kAtHost || kind.inWorld; }

// A puzzle's reward chest: made by the host's puzzle only (it reaches the guests as any new world object). A guest's
// copy of the puzzle comes out too, now that it sees every piece: the chest it makes there would be a second one.
constexpr int kObjTreasureChest = 150;
constexpr const char* kRewardMakers[] = {
    "gml_Object_objGemSocketPillar_Step_0", "gml_Object_objSkullBrazier_Other_11", "gml_Object_objSleepingStatue_Step_0",
    // its only chest is the eye statue's (its collision with every other parObject makes none)
    "gml_Object_objArrow_Collision_d53d5ff2_a0e8_46f2_993b_4e04e7d92f52",
    // The frozen chest island's ice (puzzle 15) melts at every player now; each block makes its chest as it goes, and
    // the guest got its own besides the host's (playtest 2026-11-08). In a dungeon the block holds the boss key: no chest.
    "gml_Object_objFrozenChest_Step_2",
    "gml_Object_objReceptorBig_Step_0",  // the battery's four small chests and its big one
    "gml_Object_objVolcanoPushDownPillar_Other_7",
    "gml_Object_objBell_Other_11"};  // the three bells' chest, made by the E that rings the last right note  // the fire island's pillars, as their animation ends at the guest
constexpr const char* kIceMaker = "gml_Object_objFrozenChest_Step_2";  // sets the chest it makes `grounded`
constexpr int kSlotGrounded = 0x54D;  // a chest done falling (var_slots.tsv)
constexpr double kSameSpotIce = 24;
constexpr DWORD kMeltSpotMs = 60000;
struct MeltSpot {
    double x, y;
    DWORD tick;
};
std::vector<MeltSpot> g_meltSpots;  // guest: where its own ice made a chest lately (the host's replica comes there)
std::vector<MeltSpot> g_fireRespawns;  // guest: where the host is making a burning chest again (puzzle 18)

// Host: a guest's arrow solved eye statue `id`: the chest the guest's arrow made there is made here, as objArrow's
// collision does - instance_create_depth(x, y + 32, depth, objTreasureChest) - and reaches every player.
void EyeSolvedAtHost(yyc::CInstance* self, int id) {
    yyc::CInstance* statue = yyc::FindInstance(id);
    if (statue == nullptr) return;
    constexpr double kChestBelow = 32;
    const double x = yyc::GetBuiltinReal(statue, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(statue, yyc::va::YYVar_y) + kChestBelow;
    const double depth = yyc::GetBuiltinReal(statue, yyc::va::YYVar_depth);
    yyc::CallReal(self, "instance_create_depth", {x, y, depth, double(kObjTreasureChest)});
    Log("puzzles: a guest's arrow solved eye statue %d - its chest made here", id);
}
std::vector<int> g_guestRewards;  // guest: such chests, removed at the next step (the code making one still uses it)

void RemoveGuestRewards(yyc::CInstance* self) {
    for (const int id : g_guestRewards) {
        if (yyc::FindInstance(id) == nullptr) continue;
        yyc::CallReal(self, "instance_destroy", {double(id), 0});  // a treasure chest's destroy is never sent
        Log("puzzles: the puzzle's reward chest %d made here removed - the host's comes", id);
    }
    g_guestRewards.clear();
}

int PuzzleFlagCount(yyc::CInstance* self) {
    const yyc::RValue* flags = yyc::GlobalVar(kSlotGlobalPuzzle);
    if (flags == nullptr || (flags->kind & yyc::kKindMask) != yyc::kArray) return 0;
    yyc::RValue copy = *flags;  // a plain copy: no reference taken or freed
    yyc::RValue length{};
    yyc::CallBuiltin("array_length_1d", self, 1, &copy, &length);
    const int count = static_cast<int>(yyc::ToReal(length));
    yyc::Free(&length);
    return (std::min)(count, kMaxPuzzleFlags);
}

// The host's solved flags, when they change.
void AddPuzzleFlags(yyc::CInstance* self, std::vector<StateEntry>& changes) {
    const int count = PuzzleFlagCount(self);
    for (int i = 0; i < count; ++i) {
        const yyc::RValue flag = yyc::ArrayElement(yyc::GlobalVar(kSlotGlobalPuzzle), i);
        if (!yyc::IsNumber(flag)) continue;
        const float value = static_cast<float>(yyc::ToReal(flag));
        const auto key = std::make_pair(-1, i);
        const auto known = g_known.find(key);
        if (known != g_known.end() && known->second == value) continue;
        g_known[key] = value;
        changes.push_back({0, 0, kGlobalPuzzle, static_cast<int16_t>(i), value, 0, 0});
    }
}

void SetPuzzleFlag(yyc::CInstance* self, int index, float value) {
    using ArrayIndexFn = yyc::RValue*(__thiscall*)(yyc::RValue* array, int index);
    yyc::RValue* flags = yyc::GlobalVar(kSlotGlobalPuzzle);
    if (index < 0 || index >= PuzzleFlagCount(self) || flags == nullptr) return;  // never past its end
    yyc::RValue* element = yyc::At<ArrayIndexFn>(yyc::va::ArrayIndex)(flags, index);
    if (element != nullptr && yyc::IsNumber(*element)) *element = yyc::Real(value);
    g_known[std::make_pair(-1, index)] = value;
}

void SendChanges(yyc::CInstance* self) {
    std::vector<StateEntry> changes;
    if (IsHost()) AddPuzzleFlags(self, changes);
    const bool inWorld = InWorld(self);
    for (const Kind& kind : kKinds) {
        const bool guestInWorld = !Sends(kind) && IsGuest() && inWorld && !kind.worldGuestSlots.empty();
        if ((!Sends(kind) && !guestInWorld) || !SharedHere(kind, inWorld)) continue;
        const int count = static_cast<int>(yyc::CallReal(self, "instance_number", {double(kind.object)}));
        for (int i = 0; i < count && changes.size() < kMaxEntries; ++i) {
            const int id = static_cast<int>(yyc::CallReal(self, "instance_find", {double(kind.object), double(i)}));
            std::vector<int> slots = guestInWorld ? kind.worldGuestSlots : kind.slots;
            if (kind.moves && IsHost()) {
                slots.push_back(kSlotX);
                slots.push_back(kSlotY);
            }
            for (const int slot : slots) {
                float value = 0;
                if (!ReadNumber(id, slot, &value)) continue;
                const auto key = std::make_pair(id, slot);
                const auto known = g_known.find(key);
                if (known != g_known.end() && known->second == value) continue;
                g_known[key] = value;
                changes.push_back(Entry(id, kind.object, slot, value));
            }
        }
    }
    if (!changes.empty()) net::Send(kPuzzleState, changes.data(), changes.size() * sizeof(StateEntry));
}

void ApplyState(const net::Message& message, yyc::CInstance* localPlayer) {
    const size_t count = message.payload.size() / sizeof(StateEntry);
    if (message.payload.size() % sizeof(StateEntry) != 0 || count > kMaxEntries) return;
    const bool inWorld = InWorld(localPlayer);
    for (size_t i = 0; i < count; ++i) {
        StateEntry entry{};
        memcpy(&entry, message.payload.data() + i * sizeof(StateEntry), sizeof(entry));
        if (entry.object == kGlobalPuzzle) {
            if (IsGuest() && std::isfinite(entry.value)) SetPuzzleFlag(localPlayer, entry.slot, entry.value);
            continue;
        }
        const Kind* kind = KindOf(entry.object);
        if (kind == nullptr || !std::isfinite(entry.value) || !SharedHere(*kind, inWorld)) continue;
        const bool guestSlot = inWorld && std::find(kind->worldGuestSlots.begin(), kind->worldGuestSlots.end(),
                                                    entry.slot) != kind->worldGuestSlots.end();
        if (!IsGuest() && !FromAnyone(*kind) && !guestSlot) continue;  // the host takes from a guest only what is its own too
        const int piece = LocalPiece(localPlayer, entry.id, entry.hostId, entry.object, entry.x, entry.y);
        if (piece == 0) continue;
        float before = 0;
        const bool solvedNow = IsHost() && entry.object == kObjEyeStatue && entry.slot == kSolved && entry.value != 0 &&
                               ReadNumber(piece, kSolved, &before) && before == 0;
        WriteNumber(piece, entry.slot, entry.value);
        if (solvedNow) EyeSolvedAtHost(localPlayer, piece);
        if (kind->moves && entry.slot == kSlotX) {  // the host moves it: no sliding of its own here
            WriteNumber(piece, kH, 0);
            WriteNumber(piece, kV, 0);
        }
        g_known[std::make_pair(piece, static_cast<int>(entry.slot))] = entry.value;  // not news to send back
    }
}

void ApplyUse(yyc::CInstance* localPlayer, net::PeerId from, const UseMsg& msg) {
    yyc::CInstance* piece = yyc::FindInstance(LocalPiece(localPlayer, msg.id, msg.hostId, msg.object, msg.x, msg.y));
    yyc::CInstance* puppet = puppet::Instance(from);
    const Kind* kind = KindOf(msg.object);
    if (piece == nullptr || puppet == nullptr || kind == nullptr || ObjectOf(piece) != msg.object) {
        Log("puzzles: guest %u used piece %d (object %d) - not here", from, msg.id, msg.object);
        return;
    }
    ++g_applying;
    puppet::BeginActing(from);
    yyc::PerformEvent(piece, puppet, yyc::kEvOther, yyc::kEvUser0 + 1);  // Other_11: E
    puppet::EndActing();
    --g_applying;
    Log("puzzles: guest %u used piece %d (object %d)", from, msg.id, msg.object);
}

// Another player's E on a kMirror piece: ours does the same, as the local player.
void ApplyMirror(yyc::CInstance* localPlayer, net::PeerId from, const UseMsg& msg) {
    yyc::CInstance* piece = yyc::FindInstance(LocalPiece(localPlayer, msg.id, msg.hostId, msg.object, msg.x, msg.y));
    const Kind* kind = KindOf(msg.object);
    if (piece == nullptr || kind == nullptr || kind->use != Use::kMirror || ObjectOf(piece) != msg.object) {
        Log("puzzles: player %u used piece %d (object %d) - not here", from, msg.id, msg.object);
        return;
    }
    ++g_applying;
    yyc::PerformEvent(piece, localPlayer, yyc::kEvOther, yyc::kEvUser0 + 1);  // Other_11: E
    --g_applying;
    Log("puzzles: player %u used piece %d (object %d) - done here too", from, msg.id, msg.object);
}

// The guest's E on a piece the host does: sent instead of done here. An E on a kMirror piece: done here and sent.
template <size_t N>
void __cdecl InteractHook(yyc::CInstance* self, yyc::CInstance* other) {
    const Kind* kind = KindOf(ObjectOf(self));
    const bool ours = kind != nullptr && net::IsConnected() && g_applying == 0;
    const UseMsg msg = UseOf(IdOf(self), kind != nullptr ? kind->object : 0);
    if (ours && kind->use == Use::kAtHost && IsGuest()) {
        net::Send(kPuzzleUse, &msg, sizeof(msg));
        return;
    }
    g_orig[N](self, other);
    if (ours && kind->use == Use::kMirror) net::Send(kPuzzleMirror, &msg, sizeof(msg));
}

template <size_t... N>
struct HookTable {
    static constexpr yyc::EventFn kHooks[] = {&InteractHook<N>...};
};
using Hooks = HookTable<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12>;
static_assert(sizeof(Hooks::kHooks) / sizeof(Hooks::kHooks[0]) == kHookCount, "one hook per hooked kind");

}  // namespace

namespace {

// Puzzle 6, "where flowers don't live": four objDigSpot with `puzzle` set; digging one destroys it, and the last one's
// Destroy_0 makes the chest. Dig spots are no world objects (no parent): each player's stayed theirs - the digger's
// were gone and only the digger got a chest. A puzzle spot dug here is told to the others: the host's goes through its
// own Destroy_0 (the last one makes the chest, which reaches everyone), a guest's quietly (its chest would be a second).
constexpr uint32_t kDigSpotDestroy = 0x00C8EA90;  // gml_Object_objDigSpot_Destroy_0
constexpr int kObjDigSpot = 162;
constexpr int kSlotPuzzleSpot = 0x736;  // objDigSpot.puzzle (the same slot number as global.puzzle)
constexpr double kDigSameSpot = 8;
#pragma pack(push, 1)
struct DigMsg {
    float x, y;
};
#pragma pack(pop)
yyc::EventFn g_origDigDestroy = nullptr;
int g_applyingDig = 0;        // destroying a spot another player dug: not news to send back
bool g_puzzleDigHere = false;  // inside a puzzle spot's Destroy_0 (a guest's chest made there is not the reward)

bool IsPuzzleSpot(int id) {
    float puzzle = 0;
    return ReadNumber(id, kSlotPuzzleSpot, &puzzle) && puzzle != 0;
}

void __cdecl DigDestroyHook(yyc::CInstance* self, yyc::CInstance* other) {
    const int id = static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_id));
    const bool puzzleSpot = IsPuzzleSpot(id);
    if (puzzleSpot && g_applyingDig == 0 && net::IsConnected()) {
        const DigMsg msg{static_cast<float>(yyc::GetBuiltinReal(self, yyc::va::YYVar_x)),
                         static_cast<float>(yyc::GetBuiltinReal(self, yyc::va::YYVar_y))};
        net::Send(kPuzzleDig, &msg, sizeof(msg));
        Log("puzzles: puzzle dig spot dug here at (%.0f, %.0f) - told the others", msg.x, msg.y);
    }
    const bool before = g_puzzleDigHere;
    g_puzzleDigHere = puzzleSpot;
    g_origDigDestroy(self, other);
    g_puzzleDigHere = before;
}

// Another player dug the puzzle spot at (x, y): ours there goes too (asleep ones woken first).
void ApplyDig(yyc::CInstance* self, float x, float y) {
    sync::WakeThen(self, kObjDigSpot, [self, x, y] {
        const int count = static_cast<int>(yyc::CallReal(self, "instance_number", {double(kObjDigSpot)}));
        for (int i = 0; i < count; ++i) {
            const int id = static_cast<int>(yyc::CallReal(self, "instance_find", {double(kObjDigSpot), double(i)}));
            yyc::CInstance* spot = yyc::FindInstance(id);
            if (spot == nullptr || !IsPuzzleSpot(id) ||
                std::hypot(yyc::GetBuiltinReal(spot, yyc::va::YYVar_x) - x, yyc::GetBuiltinReal(spot, yyc::va::YYVar_y) - y) >
                    kDigSameSpot) {
                continue;
            }
            ++g_applyingDig;
            if (IsHost()) {
                yyc::CallReal(self, "instance_destroy", {double(id)});  // its Destroy_0: the last one makes the chest
            } else {
                yyc::CallReal(self, "instance_destroy", {double(id), 0});
            }
            --g_applyingDig;
            Log("puzzles: the other player dug the puzzle spot at (%.0f, %.0f) - gone here too", x, y);
            return;
        }
        Log("puzzles: the other player dug a puzzle spot at (%.0f, %.0f) - none here", x, y);
    });
}

}  // namespace

bool InstallHooks(bool (*isHost)(), bool (*isGuest)()) {
    g_isHost = isHost;
    g_isGuest = isGuest;
    size_t n = 0;
    bool ok = true;
    for (const Kind& kind : kKinds) {
        if (kind.interact == 0) continue;
        ok = MH_CreateHook(yyc::At<void*>(kind.interact), reinterpret_cast<void*>(Hooks::kHooks[n]),
                           reinterpret_cast<void**>(&g_orig[n])) == MH_OK && ok;
        ++n;
    }
    ok = MH_CreateHook(yyc::At<void*>(kDigSpotDestroy), reinterpret_cast<void*>(&DigDestroyHook),
                       reinterpret_cast<void**>(&g_origDigDestroy)) == MH_OK && ok;
    if (!ok || n != kHookCount) Log("puzzles: hooks failed");
    return ok && n == kHookCount;
}

namespace {

// Puzzles whose solving takes their piece out of the world: the ice melts into its chest (15, frozen chest) and the
// giant crystal is mined (20: not one of the 19 puzzles, its island gives a feat). A reset makes the piece again where
// it stood. Only the host makes it; sync.cpp sends a new world object to the guests.
struct Respawn {
    int number;
    int object;
};
constexpr int kObjGiantCrystal = 179;
constexpr Respawn kRespawns[] = {{15, kObjFrozenChest}, {20, kObjGiantCrystal}};
constexpr char kSpotsFile[] = "coop_puzzle_spots.txt";  // "<object> <x> <y>" lines, in the save's coop_guests folder
constexpr unsigned kSpotEverySteps = 60;
constexpr int kLastChest = 153;       // objTreasureChest .. objTreasureChestSmallFrozen
constexpr double kRewardReach = 160;  // no spot noted: the reward chest this close to the player marks the spot
constexpr double kChestClear = 40;    // the reward chests this close to the spot go with a reset (they bounce off it)
constexpr double kTile = 16;

#pragma pack(push, 1)
struct ResetMsg {
    int32_t number;
    float x, y;        // to the host: where the player who typed it stands; from the host: the piece's spot
    uint8_t fromHost;  // the host passes a guest's message on to the other guests: only the host's own one counts
};
#pragma pack(pop)

std::map<int, std::pair<double, double>> g_spots;  // host: object -> where its piece stands in this world
// Puzzle 18, the fire island's burning chests: plain small chests, told from the rest by `onFire` (only these start
// burning). Noted while burning; a reset makes the four again where they stood, on fire.
constexpr int kFireChests = 18;
constexpr double kNearSpot = 8;
std::vector<std::pair<double, double>> g_fireSpots;  // host: where the fire island's chests stand
bool g_spotsRead = false;
unsigned g_spotSteps = 0;

const Respawn* FindRespawn(int number) {
    for (const Respawn& r : kRespawns) {
        if (r.number == number) return &r;
    }
    return nullptr;
}

void ReadSpots() {
    g_spotsRead = true;
    g_spots.clear();
    const std::wstring path = character::WorldNoteFile(kSpotsFile);
    if (path.empty()) return;
    std::ifstream in{std::filesystem::path(path)};
    int object = 0;
    double x = 0, y = 0;
    g_fireSpots.clear();
    while (in >> object >> x >> y) {
        if (!std::isfinite(x) || !std::isfinite(y)) continue;
        if (object == kObjTreasureChestSmall) {
            g_fireSpots.push_back({x, y});
        } else {
            g_spots[object] = {x, y};
        }
    }
}

void WriteSpots() {
    const std::wstring path = character::WorldNoteFile(kSpotsFile);
    if (path.empty()) return;
    std::ostringstream out;
    for (const auto& [object, spot] : g_spots) out << object << ' ' << spot.first << ' ' << spot.second << '\n';
    for (const auto& spot : g_fireSpots) out << kObjTreasureChestSmall << ' ' << spot.first << ' ' << spot.second << '\n';
    const std::string text = out.str();
    files::WriteAtomic(std::filesystem::path(path), reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

// Host (or alone), in the world: where each such piece stands, noted while it is there - a save solved before this
// build has nothing to note (then the reset goes by the reward chest or the player).
void NoteSpots(yyc::CInstance* localPlayer) {
    // InWorld: rmGame only - a dungeon's objFrozenChest (the Crystal Cave's boss key) is not one of these.
    if (IsGuest() || ++g_spotSteps % kSpotEverySteps != 0 || !InWorld(localPlayer)) return;
    if (!g_spotsRead) ReadSpots();
    bool changed = false;
    for (const Respawn& r : kRespawns) {
        if (yyc::CallReal(localPlayer, "instance_number", {double(r.object)}) < 1) continue;
        const int id = static_cast<int>(yyc::CallReal(localPlayer, "instance_find", {double(r.object), 0}));
        yyc::CInstance* piece = yyc::FindInstance(id);
        if (piece == nullptr) continue;
        const std::pair<double, double> spot{yyc::GetBuiltinReal(piece, yyc::va::YYVar_x),
                                             yyc::GetBuiltinReal(piece, yyc::va::YYVar_y)};
        const auto known = g_spots.find(r.object);
        if (known != g_spots.end() && known->second == spot) continue;
        g_spots[r.object] = spot;
        changed = true;
        Log("puzzles: puzzle %d's piece (object %d) noted at (%.0f, %.0f)", r.number, r.object, spot.first, spot.second);
    }
    // The fire island's chests, while they burn.
    const int smalls = static_cast<int>(yyc::CallReal(localPlayer, "instance_number", {double(kObjTreasureChestSmall)}));
    for (int i = 0; i < smalls; ++i) {
        const int id = static_cast<int>(yyc::CallReal(localPlayer, "instance_find", {double(kObjTreasureChestSmall), double(i)}));
        yyc::CInstance* chest = yyc::FindInstance(id);
        float burning = 0;
        if (chest == nullptr || !ReadNumber(id, kOnFire, &burning) || !(burning > 0)) continue;
        const double x = yyc::GetBuiltinReal(chest, yyc::va::YYVar_x), y = yyc::GetBuiltinReal(chest, yyc::va::YYVar_y);
        const bool known = std::any_of(g_fireSpots.begin(), g_fireSpots.end(), [x, y](const auto& f) {
            return std::hypot(f.first - x, f.second - y) <= kNearSpot;
        });
        if (known) continue;
        g_fireSpots.push_back({x, y});
        changed = true;
        Log("puzzles: puzzle 18's burning chest noted at (%.0f, %.0f)", x, y);
    }
    if (changed) WriteSpots();
}

// Treasure chests within `reach` of (x, y): ids, nearest first.
std::vector<int> ChestsNear(yyc::CInstance* self, double x, double y, double reach) {
    std::vector<std::pair<double, int>> found;
    for (int kind = kObjTreasureChest; kind <= kLastChest; ++kind) {
        const int count = static_cast<int>(yyc::CallReal(self, "instance_number", {double(kind)}));
        for (int i = 0; i < count; ++i) {
            const int id = static_cast<int>(yyc::CallReal(self, "instance_find", {double(kind), double(i)}));
            yyc::CInstance* chest = yyc::FindInstance(id);
            if (chest == nullptr) continue;
            const double d = std::hypot(yyc::GetBuiltinReal(chest, yyc::va::YYVar_x) - x,
                                        yyc::GetBuiltinReal(chest, yyc::va::YYVar_y) - y);
            if (d <= reach) found.push_back({d, id});
        }
    }
    std::sort(found.begin(), found.end());
    std::vector<int> ids;
    for (const auto& f : found) ids.push_back(f.second);
    return ids;
}

// The reward chests around a reset piece: each player removes its own, quietly (no loot; never sent, sync.cpp).
int ClearChests(yyc::CInstance* self, double x, double y) {
    const std::vector<int> ids = ChestsNear(self, x, y, kChestClear);
    for (const int id : ids) yyc::CallReal(self, "instance_destroy", {double(id), 0});
    return static_cast<int>(ids.size());
}

// Host (or alone): puzzle `r`'s piece made again - at its noted spot; else (a save solved before the spots were
// noted) where the ice's reward chest lies near (fromX, fromY), the player who typed the reset; else on that player's
// tile. Then the reward chests around it go, here and (told the spot) at the guests.
void RespawnPiece(yyc::CInstance* self, const Respawn& r, double fromX, double fromY) {
    sync::WakeThen(self, r.object, [self, r, fromX, fromY] {
        if (!InWorld(self)) {
            Log("puzzles: reset %d - the host is not in the world (a dungeon, a building): nothing made", r.number);
            return;
        }
        if (yyc::CallReal(self, "instance_number", {double(r.object)}) > 0) {
            Log("puzzles: reset %d - its piece (object %d) is still there: nothing to make", r.number, r.object);
            return;
        }
        if (!g_spotsRead) ReadSpots();
        double x = 0, y = 0;
        const char* how = "its noted spot";
        if (const auto known = g_spots.find(r.object); known != g_spots.end()) {
            x = known->second.first;
            y = known->second.second;
        } else if (std::vector<int> chests; r.object == kObjFrozenChest && std::isfinite(fromX) &&
                   !(chests = ChestsNear(self, fromX, fromY, kRewardReach)).empty()) {
            yyc::CInstance* chest = yyc::FindInstance(chests.front());
            x = yyc::GetBuiltinReal(chest, yyc::va::YYVar_x);
            y = yyc::GetBuiltinReal(chest, yyc::va::YYVar_y);
            how = "its reward chest";
        } else if (std::isfinite(fromX) && std::isfinite(fromY)) {
            x = std::round(fromX / kTile) * kTile;
            y = std::round(fromY / kTile) * kTile;
            how = "the player's tile (no spot noted)";
        } else {
            Log("puzzles: reset %d - no spot known for object %d", r.number, r.object);
            return;
        }
        const int id = static_cast<int>(yyc::CallReal(self, "instance_create_depth", {x, y, -y, double(r.object)}));
        const int cleared = ClearChests(self, x, y);
        g_spots[r.object] = {x, y};
        WriteSpots();
        if (net::IsConnected()) {
            const ResetMsg msg{r.number, static_cast<float>(x), static_cast<float>(y), 1};
            net::Send(kPuzzleReset, &msg, sizeof(msg));
        }
        Log("puzzles: puzzle %d reset - object %d made again (id %d) at (%.0f, %.0f), %s; %d chest(s) cleared", r.number,
            r.object, id, x, y, how, cleared);
    });
}

// Host (or alone): puzzle 18 - the small chests at its noted spots (else the ones near (fromX, fromY), the player who
// typed it) are made again there, grounded and burning: made new, as the island made them (a chest whose fire was put
// out keeps no flames going). The guests are told each spot first: their own chests there go, and the replicas that
// come there are made grounded and burning (OnReplicaCreated).
void ResetFireChests(yyc::CInstance* self, double fromX, double fromY) {
    sync::WakeThen(self, kObjTreasureChestSmall, [self, fromX, fromY] {
        if (!InWorld(self)) {
            Log("puzzles: reset 18 - the host is not in the world: nothing made");
            return;
        }
        if (!g_spotsRead) ReadSpots();
        std::vector<std::pair<double, double>> spots = g_fireSpots;
        const char* how = "the noted spots";
        if (spots.empty() && std::isfinite(fromX) && std::isfinite(fromY)) {
            for (const int id : ChestsNear(self, fromX, fromY, kRewardReach)) {
                yyc::CInstance* chest = yyc::FindInstance(id);
                if (chest == nullptr || static_cast<int>(yyc::GetBuiltinReal(chest, yyc::va::YYVar_object_index)) !=
                                            kObjTreasureChestSmall) continue;
                spots.push_back({yyc::GetBuiltinReal(chest, yyc::va::YYVar_x), yyc::GetBuiltinReal(chest, yyc::va::YYVar_y)});
            }
            how = "the small chests near the player (no spots noted)";
        }
        if (spots.empty()) {
            Log("puzzles: reset 18 - no spot known for the burning chests");
            return;
        }
        for (const auto& [x, y] : spots) {
            if (net::IsConnected()) {
                const ResetMsg msg{kFireChests, static_cast<float>(x), static_cast<float>(y), 1};
                net::Send(kPuzzleReset, &msg, sizeof(msg));
            }
            for (const int id : ChestsNear(self, x, y, kSameSpotIce)) yyc::CallReal(self, "instance_destroy", {double(id), 0});
            const int id = static_cast<int>(
                yyc::CallReal(self, "instance_create_depth", {x, y, -y, double(kObjTreasureChestSmall)}));
            if (yyc::FindInstance(id) == nullptr) continue;
            SetNumber(id, kSlotGrounded, 1);
            SetNumber(id, kOnFire, 1);
        }
        g_fireSpots = spots;
        WriteSpots();
        Log("puzzles: puzzle 18 reset - %zu burning chests made again at %s", spots.size(), how);
    });
}

}  // namespace

void Tick(yyc::CInstance* localPlayer) {
    if (!g_guestRewards.empty()) RemoveGuestRewards(localPlayer);
    NoteSpots(localPlayer);
    if (!net::IsConnected() || ++g_steps % kEverySteps != 0) return;
    SendChanges(localPlayer);
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kPuzzleReset) {
        int32_t number = 0;
        ResetMsg msg{};
        if (message.payload.size() == sizeof(number)) {
            memcpy(&number, message.payload.data(), sizeof(number));
            ResetPuzzle(localPlayer, number, false);
        } else if (message.payload.size() == sizeof(msg)) {
            memcpy(&msg, message.payload.data(), sizeof(msg));
            if (msg.number == kFireChests) {
                if (IsHost() && !msg.fromHost) {
                    ResetFireChests(localPlayer, msg.x, msg.y);
                } else if (IsGuest() && msg.fromHost && std::isfinite(msg.x) && std::isfinite(msg.y)) {
                    int gone = 0;
                    for (const int id : ChestsNear(localPlayer, msg.x, msg.y, kSameSpotIce)) {
                        yyc::CallReal(localPlayer, "instance_destroy", {double(id), 0});
                        ++gone;
                    }
                    g_fireRespawns.push_back({msg.x, msg.y, GetTickCount()});
                    Log("puzzles: the host remade a burning chest at (%.0f, %.0f); %d chest(s) gone here", msg.x, msg.y, gone);
                }
                return true;
            }
            const Respawn* r = FindRespawn(msg.number);
            if (r != nullptr && IsHost() && !msg.fromHost) {
                RespawnPiece(localPlayer, *r, msg.x, msg.y);  // a guest's /puzzlereset: the host makes the piece
            } else if (r != nullptr && IsGuest() && msg.fromHost && std::isfinite(msg.x) && std::isfinite(msg.y)) {
                const int cleared = ClearChests(localPlayer, msg.x, msg.y);  // the piece itself comes as a new object
                Log("puzzles: the host reset puzzle %d at (%.0f, %.0f); %d chest(s) cleared here", msg.number, msg.x,
                    msg.y, cleared);
            }
        }
        return true;
    }
    if (message.type == kPuzzleState) {
        ApplyState(message, localPlayer);
        return true;
    }
    if (message.type == kPuzzleDig) {
        DigMsg msg{};
        if (message.payload.size() != sizeof(msg)) return true;
        memcpy(&msg, message.payload.data(), sizeof(msg));
        if (std::isfinite(msg.x) && std::isfinite(msg.y)) ApplyDig(localPlayer, msg.x, msg.y);
        return true;
    }
    if (message.type != kPuzzleUse && message.type != kPuzzleMirror) return false;
    UseMsg msg{};
    if (message.payload.size() != sizeof(msg)) return true;
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (message.type == kPuzzleMirror) {
        ApplyMirror(localPlayer, message.from, msg);
    } else if (IsHost()) {
        ApplyUse(localPlayer, message.from, msg);
    }
    return true;
}

bool MeltsAtEachSide(int object) { return object == kObjFrozenChest; }

namespace {

// What each resettable puzzle's pieces go back to (the puzzle list's numbers: the wiki's order of the puzzles).
struct ResetSlot {
    int slot;
    double value;
};
struct PuzzleReset {
    int number;
    int object;
    std::vector<ResetSlot> slots;
};
const std::vector<PuzzleReset> kResets = {
    {5, kObjEyeStatue, {{kSolved, 0}}},                                                  // eye statue
    {9, 190, {{kSolved, 0}, {kSlotImage, 0}, {kSlotImageSpeed, 0}}},                      // skull braziers: unlit
    {11, 188, {{kSocket, -4}, {kSolved, 0}, {kUsable, 1}}},                               // colored pedestals: no gem
    {14, 177, {{kSocket, -4}, {kSolved, 0}, {kUsable, 1}}},                               // stone princess: no flower
};

// Every piece of the puzzle (asleep ones too: woken first, done right before the game culls again).
void ResetPieces(yyc::CInstance* self, const PuzzleReset& reset) {
    sync::WakeThen(self, reset.object, [self, reset] {
        const int count = static_cast<int>(yyc::CallReal(self, "instance_number", {double(reset.object)}));
        for (int i = 0; i < count; ++i) {
            const int id = static_cast<int>(yyc::CallReal(self, "instance_find", {double(reset.object), double(i)}));
            for (const ResetSlot& s : reset.slots) {
                WriteNumber(id, s.slot, static_cast<float>(s.value));
                g_known[std::make_pair(id, s.slot)] = static_cast<float>(s.value);  // not news: everyone resets
            }
        }
        Log("puzzles: puzzle %d reset (%d pieces of object %d)", reset.number, count, reset.object);
    });
}

}  // namespace

bool ResetPuzzle(yyc::CInstance* self, int number, bool share) {
    if ((FindRespawn(number) != nullptr || number == kFireChests) && share) {  // typed here (a received one is Handle's)
        const int playerId = static_cast<int>(yyc::CallReal(self, "instance_find", {double(yyc::obj::player), 0}));
        yyc::CInstance* player = yyc::FindInstance(playerId);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double px = player != nullptr ? yyc::GetBuiltinReal(player, yyc::va::YYVar_x) : nan;
        const double py = player != nullptr ? yyc::GetBuiltinReal(player, yyc::va::YYVar_y) : nan;
        if (IsGuest()) {
            if (net::IsConnected()) {
                const ResetMsg msg{number, static_cast<float>(px), static_cast<float>(py), 0};
                net::Send(kPuzzleReset, &msg, sizeof(msg));
            }
            Log("puzzles: /puzzlereset %d asked of the host", number);
        } else if (number == kFireChests) {
            ResetFireChests(self, px, py);
        } else {
            RespawnPiece(self, *FindRespawn(number), px, py);
        }
        return true;
    }
    for (const PuzzleReset& reset : kResets) {
        if (reset.number != number) continue;
        ResetPieces(self, reset);
        if (share && net::IsConnected()) {
            const int32_t msg = number;
            net::Send(kPuzzleReset, &msg, sizeof(msg));
        }
        return true;
    }
    return false;
}

void OnCreated(int createdId, int object) {
    if (!IsGuest() || !net::IsConnected() || (object != kObjTreasureChest && object != kObjTreasureChestSmall)) return;
    const yyc::StackFrame* top = yyc::TopFrame();
    if (top == nullptr || top->name == nullptr) return;
    for (const char* maker : kRewardMakers) {
        if (strcmp(top->name, maker) == 0) g_guestRewards.push_back(createdId);
    }
    if (g_puzzleDigHere && g_applyingDig == 0) g_guestRewards.push_back(createdId);  // the host's last spot makes it
    if (strcmp(top->name, kIceMaker) == 0) {  // where the host's replica of this chest comes: on the ground there
        if (yyc::CInstance* chest = yyc::FindInstance(createdId)) {
            g_meltSpots.push_back({yyc::GetBuiltinReal(chest, yyc::va::YYVar_x), yyc::GetBuiltinReal(chest, yyc::va::YYVar_y),
                                   GetTickCount()});
        }
    }
}

void OnReplicaCreated(yyc::CInstance* self, int id, int object) {
    if (!IsGuest() || object < kObjTreasureChest || object > kLastChest) return;
    yyc::CInstance* chest = yyc::FindInstance(id);
    if (chest == nullptr) return;
    const double x = yyc::GetBuiltinReal(chest, yyc::va::YYVar_x), y = yyc::GetBuiltinReal(chest, yyc::va::YYVar_y);
    const DWORD now = GetTickCount();
    g_meltSpots.erase(std::remove_if(g_meltSpots.begin(), g_meltSpots.end(),
                                     [now](const MeltSpot& m) { return now - m.tick > kMeltSpotMs; }),
                      g_meltSpots.end());
    // Here the ice either melted already (its chest noted above) or is still melting (the host's went first).
    bool fromIce = std::any_of(g_meltSpots.begin(), g_meltSpots.end(),
                               [x, y](const MeltSpot& m) { return std::hypot(m.x - x, m.y - y) <= kSameSpotIce; });
    const int blocks = static_cast<int>(yyc::CallReal(self, "instance_number", {double(kObjFrozenChest)}));
    for (int i = 0; i < blocks && !fromIce; ++i) {
        const int block = static_cast<int>(yyc::CallReal(self, "instance_find", {double(kObjFrozenChest), double(i)}));
        if (yyc::CInstance* ice = yyc::FindInstance(block)) {
            fromIce = std::hypot(yyc::GetBuiltinReal(ice, yyc::va::YYVar_x) - x,
                                 yyc::GetBuiltinReal(ice, yyc::va::YYVar_y) - y) <= kSameSpotIce;
        }
    }
    const auto fire = std::find_if(g_fireRespawns.begin(), g_fireRespawns.end(), [x, y, now](const MeltSpot& m) {
        return now - m.tick <= kMeltSpotMs && std::hypot(m.x - x, m.y - y) <= kSameSpotIce;
    });
    if (fire != g_fireRespawns.end() && object == kObjTreasureChestSmall) {
        g_fireRespawns.erase(fire);
        SetNumber(id, kSlotGrounded, 1);
        SetNumber(id, kOnFire, 1);
        SetNumber(id, kFreeze, 0);
        Log("puzzles: the host's remade burning chest %d at (%.0f, %.0f) made on the ground, burning", id, x, y);
        return;
    }
    if (!fromIce) return;
    yyc::RValue grounded = yyc::Real(1);
    yyc::SetVar(id, kSlotGrounded, &grounded);
    Log("puzzles: the host's frozen chest reward %d at (%.0f, %.0f) made on the ground here", id, x, y);
}

void OnDestroyed(int id, int object) {
    if (!MeltsAtEachSide(object) || !net::IsConnected()) return;
    // Its last `freeze` may not have gone out: the others' blocks melt now (Approach would raise a lone freeze of 0).
    const StateEntry melted[2] = {Entry(id, object, kFreeze, 0), Entry(id, object, kMeltTo, 0)};
    net::Send(kPuzzleState, melted, sizeof(melted));
    Log("puzzles: ice block %d melted here - melts at the others too", id);
}

void Reset() {
    g_guestRewards.clear();
    g_known.clear();
    g_steps = 0;
    g_spotsRead = false;  // another world maybe: its own spots file
    g_spotSteps = 0;
}

}  // namespace coop::puzzles
