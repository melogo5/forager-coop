#include "puzzles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "puppet.h"

namespace coop::puzzles {

namespace {

constexpr uint8_t kPuzzleState = 45;  // either way: StateEntry list, what pieces are now (see kKinds: who sends)
constexpr uint8_t kPuzzleUse = 46;    // guest -> host: UseMsg, the guest's E on a piece
constexpr unsigned kEverySteps = 6;   // 10 looks a second
constexpr int kSlotX = -1, kSlotY = -2;  // StateEntry.slot for the position
constexpr int kSlotImage = -3;           // ... for image_index (a terminal's symbol)
constexpr size_t kMaxEntries = 512;
// global.puzzle: a flag per dungeon puzzle solved (an array of numbers). StateEntry.object kGlobalPuzzle, slot = index.
constexpr int kSlotGlobalPuzzle = 0x736;
constexpr int16_t kGlobalPuzzle = -1;
constexpr int kMaxPuzzleFlags = 64;

// Variables (decompiled/code/var_slots.tsv) of the pieces.
constexpr int kSolved = 0x7D8, kElectrified = 0x1D2, kFacing = 0x409, kAngleTo = 0x58, kReflectLaser = 0x745,
              kDown = 0x1A7, kNumber = 0x6AF, kUsable = 0x8CA, kH = 0x553, kV = 0x8D9;

enum class Use : uint8_t {
    kNone,     // not interacted with (or nothing of ours happens on E)
    kAtHost,   // the guest's E is done by the host, as the guest's puppet
    kOwn,      // the guest's E stays its own (it takes from its inventory); its result goes to the host
};

struct Kind {
    int object;
    uint32_t interact;  // Other_11 (E), 0 if not hooked
    Use use;
    bool moves;  // its place is the host's too (pushed cubes)
    std::vector<int> slots;
};

const std::vector<Kind> kKinds = {
    {193, 0x00D08D00, Use::kAtHost, true, {kElectrified}},                      // objElectricCube
    {194, 0, Use::kNone, false, {kElectrified}},                                 // objReceptor
    {175, 0, Use::kNone, false, {kElectrified, kSolved}},                        // objReceptorBig
    {200, 0x00D1F4D0, Use::kAtHost, false, {kFacing, kAngleTo, kReflectLaser}},  // objMirror
    {181, 0x00CDAFC0, Use::kAtHost, false, {kSlotImage, kUsable}},  // objRunicPillar: its rune (colour, glow: animated)
    {205, 0x00D2EEF0, Use::kAtHost, false, {kSolved}},                           // objFireTempleBrazier
    {183, 0x00CE3C10, Use::kOwn, false, {kUsable}},  // objBloodAltar: E opens the boon choice - the guest's
    {188, 0x00CF1090, Use::kOwn, false, {kSolved, kNumber, kUsable}},            // objGemSocketPillar
    {180, 0x00CD1410, Use::kAtHost, false, {kSlotImage, kUsable}},  // objComputerTerminal: E turns the symbol
    {182, 0x00CE0920, Use::kAtHost, false, {kDown, kSolved, kUsable}},  // objVolcanoPushDownPillar
    {164, 0x00C96CF0, Use::kAtHost, false, {kUsable}},                           // objPushDownPillar
};
constexpr size_t kHookCount = 9;  // kinds with an Other_11 hook, in kKinds order

#pragma pack(push, 1)
struct StateEntry {
    int32_t id;
    int16_t object;
    int16_t slot;
    float value;
};
struct UseMsg {
    int32_t id;
    int16_t object;
    int16_t pad;
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

uint32_t Builtin(int slot) {
    return slot == kSlotX ? yyc::va::YYVar_x : slot == kSlotY ? yyc::va::YYVar_y : yyc::va::YYVar_image_index;
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

// What this side sends: the host everything; a guest only what its own E did (kOwn pieces).
bool Sends(const Kind& kind) { return IsHost() || kind.use == Use::kOwn; }

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
        changes.push_back({0, kGlobalPuzzle, static_cast<int16_t>(i), value});
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
    for (const Kind& kind : kKinds) {
        if (!Sends(kind)) continue;
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
                changes.push_back({id, static_cast<int16_t>(kind.object), static_cast<int16_t>(slot), value});
            }
        }
    }
    if (!changes.empty()) net::Send(kPuzzleState, changes.data(), changes.size() * sizeof(StateEntry));
}

void ApplyState(const net::Message& message, yyc::CInstance* localPlayer) {
    const size_t count = message.payload.size() / sizeof(StateEntry);
    if (message.payload.size() % sizeof(StateEntry) != 0 || count > kMaxEntries) return;
    for (size_t i = 0; i < count; ++i) {
        StateEntry entry{};
        memcpy(&entry, message.payload.data() + i * sizeof(StateEntry), sizeof(entry));
        if (entry.object == kGlobalPuzzle) {
            if (IsGuest() && std::isfinite(entry.value)) SetPuzzleFlag(localPlayer, entry.slot, entry.value);
            continue;
        }
        yyc::CInstance* piece = yyc::FindInstance(entry.id);
        const Kind* kind = KindOf(entry.object);
        if (piece == nullptr || kind == nullptr || ObjectOf(piece) != entry.object || !std::isfinite(entry.value)) continue;
        if (!IsGuest() && kind->use != Use::kOwn) continue;  // the host takes from a guest only its own results
        WriteNumber(entry.id, entry.slot, entry.value);
        if (kind->moves && entry.slot == kSlotX) {  // the host moves it: no sliding of its own here
            WriteNumber(entry.id, kH, 0);
            WriteNumber(entry.id, kV, 0);
        }
        g_known[std::make_pair(entry.id, static_cast<int>(entry.slot))] = entry.value;  // not news to send back
    }
}

void ApplyUse(yyc::CInstance* localPlayer, net::PeerId from, const UseMsg& msg) {
    yyc::CInstance* piece = yyc::FindInstance(msg.id);
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

// The guest's E on a piece the host does: sent instead of done here.
template <size_t N>
void __cdecl InteractHook(yyc::CInstance* self, yyc::CInstance* other) {
    const Kind* kind = KindOf(ObjectOf(self));
    if (kind != nullptr && kind->use == Use::kAtHost && IsGuest() && net::IsConnected() && g_applying == 0) {
        const UseMsg msg{IdOf(self), static_cast<int16_t>(kind->object), 0};
        net::Send(kPuzzleUse, &msg, sizeof(msg));
        return;
    }
    g_orig[N](self, other);
}

template <size_t... N>
struct HookTable {
    static constexpr yyc::EventFn kHooks[] = {&InteractHook<N>...};
};
using Hooks = HookTable<0, 1, 2, 3, 4, 5, 6, 7, 8>;
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
    if (!net::IsConnected() || ++g_steps % kEverySteps != 0) return;
    SendChanges(localPlayer);
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type == kPuzzleState) {
        ApplyState(message, localPlayer);
        return true;
    }
    if (message.type != kPuzzleUse) return false;
    UseMsg msg{};
    if (message.payload.size() == sizeof(msg) && IsHost()) {
        memcpy(&msg, message.payload.data(), sizeof(msg));
        ApplyUse(localPlayer, message.from, msg);
    }
    return true;
}

void Reset() {
    g_known.clear();
    g_steps = 0;
}

}  // namespace coop::puzzles
