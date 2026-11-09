#include "puzzles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "puppet.h"
#include "sync.h"

namespace coop::puzzles {

namespace {

constexpr uint8_t kPuzzleState = 45;  // either way: StateEntry list, what pieces are now (see kKinds: who sends)
constexpr uint8_t kPuzzleUse = 46;    // guest -> host: UseMsg, the guest's E on a piece
constexpr uint8_t kPuzzleMirror = 104;  // either way (the host passes it on): UseMsg, an E done by everyone
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
};

const std::vector<Kind> kKinds = {
    {193, 0x00D08D00, Use::kAtHost, true, {kElectrified}},                      // objElectricCube
    {194, 0, Use::kNone, false, {kElectrified}},                                 // objReceptor
    {175, 0, Use::kNone, false, {kElectrified, kSolved}},                        // objReceptorBig
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
    {182, 0x00CE0920, Use::kAtHost, false, {kDown, kSolved, kUsable}},  // objVolcanoPushDownPillar
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
    // guest's alone, playtest 9); each block makes its own key (sync.cpp sends no destroy of it).
    {kObjFrozenChest, 0, Use::kOwn, false, {kFreeze, kMeltTo, kBurn}},  // objFrozenChest
    {209, 0, Use::kOwn, false, {kFreeze, kMeltTo, kBurn, kFrozen}},     // objDungeonDoor (a frozen one)
    {153, 0, Use::kOwn, false, {kFreeze, kBurn}},                       // objTreasureChestSmallFrozen
};
constexpr size_t kHookCount = 12;  // kinds with an Other_11 hook, in kKinds order

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
constexpr int kObjTreasureChest = 150, kObjTreasureChestSmall = 151;
constexpr const char* kRewardMakers[] = {
    "gml_Object_objGemSocketPillar_Step_0", "gml_Object_objSkullBrazier_Other_11", "gml_Object_objSleepingStatue_Step_0",
    // its only chest is the eye statue's (its collision with every other parObject makes none)
    "gml_Object_objArrow_Collision_d53d5ff2_a0e8_46f2_993b_4e04e7d92f52"};

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
        if (!Sends(kind) || !SharedHere(kind, inWorld)) continue;
        const int count = static_cast<int>(yyc::CallReal(self, "instance_number", {double(kind.object)}));
        for (int i = 0; i < count && changes.size() < kMaxEntries; ++i) {
            const int id = static_cast<int>(yyc::CallReal(self, "instance_find", {double(kind.object), double(i)}));
            std::vector<int> slots = kind.slots;
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
        if (!IsGuest() && !FromAnyone(*kind)) continue;  // the host takes from a guest only what is its own too
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
using Hooks = HookTable<0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11>;
static_assert(sizeof(Hooks::kHooks) / sizeof(Hooks::kHooks[0]) == kHookCount, "one hook per hooked kind");

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
    if (!ok || n != kHookCount) Log("puzzles: hooks failed");
    return ok && n == kHookCount;
}

void Tick(yyc::CInstance* localPlayer) {
    if (!g_guestRewards.empty()) RemoveGuestRewards(localPlayer);
    if (!net::IsConnected() || ++g_steps % kEverySteps != 0) return;
    SendChanges(localPlayer);
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kPuzzleState) {
        ApplyState(message, localPlayer);
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

void OnCreated(int createdId, int object) {
    if (!IsGuest() || !net::IsConnected() || (object != kObjTreasureChest && object != kObjTreasureChestSmall)) return;
    const yyc::StackFrame* top = yyc::TopFrame();
    if (top == nullptr || top->name == nullptr) return;
    for (const char* maker : kRewardMakers) {
        if (strcmp(top->name, maker) == 0) g_guestRewards.push_back(createdId);
    }
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
}

}  // namespace coop::puzzles
