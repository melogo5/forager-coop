#include "skills.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "MinHook.h"
#include "character.h"
#include "log.h"
#include "mutant.h"
#include "skilltab.h"

namespace coop::skills {

namespace {

// Payload: u8 kind, then
//   kNothingStored: nothing;
//   kStored, kUpload: State - u32 earned, u32 learned, u16 count, i8 skills[count];
//   kTree: u16 count, i8 skills[count], u16 effects, then each: u8 script (kItemEdit/kStructureEdit), u8 argc,
//          f64 args[argc].
constexpr uint8_t kSkills = 71;
constexpr uint8_t kNothingStored = 0;  // host -> guest: a new guest
constexpr uint8_t kStored = 1;         // host -> guest: the guest's points as last uploaded
constexpr uint8_t kUpload = 2;         // guest -> host: keep these
constexpr uint8_t kTree = 3;           // either way: the tree as the sender has it, and what its purchases did
constexpr unsigned kEverySteps = 30;
constexpr unsigned kUploadEverySteps = 300;
constexpr int kSlotSkillUnlocked = 0x7BF;  // global.skillUnlocked[skill] (var_slots.tsv)
constexpr int kSlotLearnedSkills = 0x601;  // global.learnedSkills: skills bought here (objSkill Step_0 adds 1)
constexpr int kSlotSkillPoints = 0x7BD;    // objPlayer.skills: the points to spend (the tree shows and spends these;
                                           // WorldControl.availableSkillPoints is set once and never used)
constexpr int kSlotLevel = 0x603;          // objPlayer.level
constexpr int kMaxSkills = 512;
// global.skillUnlocked values: -1 locked, 1 available, 2 learned (what HasSkill checks). Merging two trees keeps the
// larger value of each skill: learned anywhere is learned, available anywhere is available.
constexpr int8_t kAvailable = 1;
constexpr int8_t kLearned = 2;
constexpr uint32_t kScriptItemEdit = 0x006C5990;       // decompiled/code/index.tsv
constexpr uint32_t kScriptStructureEdit = 0x0070CA50;
constexpr uint8_t kItemEdit = 0;
constexpr uint8_t kStructureEdit = 1;
constexpr int kMaxEffectArgs = 4;
constexpr size_t kMaxEffects = 256;  // pending, between two sends

struct State {
    uint32_t earned = 0;   // skill points ever got: one per shared level-up
    uint32_t learned = 0;  // skills bought with them
    std::vector<int8_t> skills;  // global.skillUnlocked as it is, one value per skill
    bool operator==(const State& o) const { return earned == o.earned && learned == o.learned && skills == o.skills; }
};

// A call a purchase made: ItemEdit or StructureEdit with number arguments.
struct Effect {
    uint8_t script = kItemEdit;
    std::vector<double> args;
};

yyc::ScriptFn g_origItemEdit = nullptr;
yyc::ScriptFn g_origStructureEdit = nullptr;
bool g_recording = false;
std::vector<Effect> g_effects;  // made by purchases here since the tree was last sent

// Both sides.
std::vector<int8_t> g_tree;  // the tree both this side and the others know of: what to send is what differs
unsigned g_steps = 0;

// Guest.
bool g_answered = false;
bool g_applied = false;
bool g_newGuest = false;
State g_stored;
State g_lastSent;
uint32_t g_earned = 0;
std::vector<std::vector<uint8_t>> g_early;  // trees that came before the guest was in the world

double ReadGlobal(int slot) {
    const yyc::RValue* value = yyc::GlobalVar(slot);
    return value != nullptr && yyc::IsNumber(*value) ? yyc::ToReal(*value) : 0;
}

void WriteGlobal(int slot, double number) {
    yyc::RValue* value = yyc::GlobalVar(slot);
    if (value != nullptr && yyc::IsNumber(*value)) *value = yyc::Real(number);
}

double ReadNumber(int id, int slot) {
    yyc::RValue value{};
    const bool ok = yyc::GetVar(id, slot, &value) && yyc::IsNumber(value);
    const double number = ok ? yyc::ToReal(value) : 0;
    yyc::Free(&value);
    return number;
}

void WriteNumber(int id, int slot, double number) {
    yyc::RValue value = yyc::Real(number);
    yyc::SetVar(id, slot, &value);
}

// global.skillUnlocked, one value per skill (empty if it is not an array yet).
std::vector<int8_t> ReadSkills(yyc::CInstance* self) {
    yyc::RValue skills = *yyc::GlobalVar(kSlotSkillUnlocked);  // a plain copy: no reference taken or freed
    std::vector<int8_t> values;
    if ((skills.kind & yyc::kKindMask) != yyc::kArray) return values;
    yyc::RValue length{};
    yyc::CallBuiltin("array_length_1d", self, 1, &skills, &length);
    const int count = static_cast<int>(yyc::ToReal(length));
    yyc::Free(&length);
    for (int i = 0; i < count && i < kMaxSkills; ++i) {
        values.push_back(static_cast<int8_t>(static_cast<int>(yyc::ToReal(yyc::ArrayElement(&skills, i)))));
    }
    return values;
}

// Sets the existing entries of global.skillUnlocked in place (they are numbers; never past its end).
void WriteSkills(const std::vector<int8_t>& values) {
    using ArrayIndexFn = yyc::RValue*(__thiscall*)(yyc::RValue* array, int index);
    yyc::RValue* skills = yyc::GlobalVar(kSlotSkillUnlocked);
    if (skills == nullptr || (skills->kind & yyc::kKindMask) != yyc::kArray) return;
    for (size_t i = 0; i < values.size(); ++i) {
        yyc::RValue* element = yyc::At<ArrayIndexFn>(yyc::va::ArrayIndex)(skills, static_cast<int>(i));
        if (element != nullptr && yyc::IsNumber(*element)) *element = yyc::Real(values[i]);
    }
}

int Learned(const std::vector<int8_t>& values) {
    return static_cast<int>(std::count(values.begin(), values.end(), kLearned));
}

// The larger value of each skill (both the same size).
std::vector<int8_t> Merge(std::vector<int8_t> a, const std::vector<int8_t>& b) {
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) a[i] = (std::max)(a[i], b[i]);
    return a;
}

// The learned skills of `now` that `before` did not have, e.g. "16 17 ".
std::string NewlyLearned(const std::vector<int8_t>& before, const std::vector<int8_t>& now) {
    std::string text;
    for (size_t i = 0; i < now.size(); ++i) {
        if (now[i] == kLearned && (i >= before.size() || before[i] != kLearned)) text += std::to_string(i) + " ";
    }
    return text;
}

void Put(std::vector<uint8_t>* out, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

std::vector<uint8_t> Encode(uint8_t kind, const State& state) {
    std::vector<uint8_t> out{kind};
    const uint16_t count = static_cast<uint16_t>(state.skills.size());
    Put(&out, &state.earned, 4);
    Put(&out, &state.learned, 4);
    Put(&out, &count, 2);
    Put(&out, state.skills.data(), count);
    return out;
}

bool Decode(const uint8_t* data, size_t size, State* state) {
    if (size < 10) return false;
    uint16_t count = 0;
    memcpy(&state->earned, data, 4);
    memcpy(&state->learned, data + 4, 4);
    memcpy(&count, data + 8, 2);
    if (size != 10 + static_cast<size_t>(count)) return false;
    state->skills.assign(data + 10, data + 10 + count);
    return true;
}

std::vector<uint8_t> EncodeTree(const std::vector<int8_t>& tree, const std::vector<Effect>& effects) {
    std::vector<uint8_t> out{kTree};
    const uint16_t count = static_cast<uint16_t>(tree.size());
    const uint16_t effectCount = static_cast<uint16_t>(effects.size());
    Put(&out, &count, 2);
    Put(&out, tree.data(), count);
    Put(&out, &effectCount, 2);
    for (const Effect& effect : effects) {
        const uint8_t argc = static_cast<uint8_t>(effect.args.size());
        out.push_back(effect.script);
        out.push_back(argc);
        Put(&out, effect.args.data(), argc * sizeof(double));
    }
    return out;
}

// `data`: the payload after its kind byte.
bool DecodeTree(const uint8_t* data, size_t size, std::vector<int8_t>* tree, std::vector<Effect>* effects) {
    size_t at = 0;
    auto take = [&](void* to, size_t n) {
        if (at + n > size) return false;
        memcpy(to, data + at, n);
        at += n;
        return true;
    };
    uint16_t count = 0, effectCount = 0;
    if (!take(&count, 2) || count > kMaxSkills) return false;
    tree->resize(count);
    if (!take(tree->data(), count) || !take(&effectCount, 2)) return false;
    for (uint16_t e = 0; e < effectCount; ++e) {
        Effect effect;
        uint8_t argc = 0;
        if (!take(&effect.script, 1) || !take(&argc, 1) || argc > kMaxEffectArgs) return false;
        effect.args.resize(argc);
        if (!take(effect.args.data(), argc * sizeof(double))) return false;
        effects->push_back(std::move(effect));
    }
    return at == size;
}

void Record(uint8_t script, int argc, yyc::RValue** args) {
    if (!g_recording || g_effects.size() >= kMaxEffects || argc > kMaxEffectArgs) return;
    Effect effect{script, {}};
    for (int i = 0; i < argc; ++i) {
        if (args[i] == nullptr || !yyc::IsNumber(*args[i])) {
            Log("skills: a purchase called %s with a non-number argument %d - not passed on",
                script == kItemEdit ? "ItemEdit" : "StructureEdit", i);
            return;
        }
        effect.args.push_back(yyc::ToReal(*args[i]));
    }
    g_effects.push_back(std::move(effect));
}

yyc::RValue* __cdecl ItemEditHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                  yyc::RValue** args) {
    Record(kItemEdit, argc, args);
    return g_origItemEdit(self, other, result, argc, args);
}

yyc::RValue* __cdecl StructureEditHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                       yyc::RValue** args) {
    Record(kStructureEdit, argc, args);
    return g_origStructureEdit(self, other, result, argc, args);
}

// Makes the calls another player's purchases made, as they made them.
void Replay(yyc::CInstance* self, const std::vector<Effect>& effects) {
    for (const Effect& effect : effects) {
        yyc::RValue values[kMaxEffectArgs] = {};
        yyc::RValue* args[kMaxEffectArgs] = {};
        for (size_t i = 0; i < effect.args.size(); ++i) {
            values[i] = yyc::Real(effect.args[i]);
            args[i] = &values[i];
        }
        const yyc::ScriptFn script = effect.script == kItemEdit ? yyc::At<yyc::ScriptFn>(kScriptItemEdit)
                                                                : yyc::At<yyc::ScriptFn>(kScriptStructureEdit);
        yyc::RValue result{};
        script(self, self, &result, static_cast<int>(effect.args.size()), args);
        yyc::Free(&result);
    }
}

// Another player's tree (and what its purchases did) joins ours.
void TakeTree(const std::vector<uint8_t>& payload, yyc::CInstance* localPlayer) {
    std::vector<int8_t> theirs;
    std::vector<Effect> effects;
    if (!DecodeTree(payload.data() + 1, payload.size() - 1, &theirs, &effects)) {
        Log("skills: a tree that does not decode (%zu bytes)", payload.size());
        return;
    }
    if (Mutant("skills")) return;
    const std::vector<int8_t> here = ReadSkills(localPlayer);
    if (here.size() != theirs.size()) {
        Log("skills: a tree of %zu skills, %zu here - ignored", theirs.size(), here.size());
        return;
    }
    const std::vector<int8_t> merged = Merge(here, theirs);
    if (merged != here) {
        WriteSkills(merged);
        skilltab::RefreshStates(localPlayer);
    }
    Replay(localPlayer, effects);
    g_tree = Merge(g_tree.size() == theirs.size() ? g_tree : here, theirs);  // a purchase here not sent yet still is
    Log("skills: the shared tree: newly learned here %s(%d learned, %zu purchase calls made)",
        NewlyLearned(here, merged).c_str(), Learned(merged), effects.size());
}

// Sends the tree when this side's has changed since the others last knew it: a purchase here, or a debug skill.
void ShareTree(yyc::CInstance* localPlayer) {
    const std::vector<int8_t> now = ReadSkills(localPlayer);
    if (now.empty()) return;
    if (g_tree.size() != now.size()) {  // the first look in this session: what the world has is known to all
        g_tree = now;
        g_effects.clear();
        return;
    }
    if (now == g_tree) return;
    const std::vector<uint8_t> payload = EncodeTree(now, g_effects);
    if (!net::Send(kSkills, payload.data(), payload.size())) return;
    Log("skills: sent the tree: learned here %s(%d learned, %zu purchase calls)", NewlyLearned(g_tree, now).c_str(),
        Learned(now), g_effects.size());
    g_tree = now;
    g_effects.clear();
}

// Guest, once in the host's world: the tree is the host's (it came with the world); the points are the guest's own.
// A new guest has bought nothing and has a point for every level the shared progress has reached.
void Apply(yyc::CInstance* localPlayer) {
    const int player = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    const double level = ReadNumber(player, kSlotLevel);
    State own = g_stored;
    if (g_newGuest) own = State{level > 1 ? static_cast<uint32_t>(level - 1) : 0, 0, {}};
    WriteGlobal(kSlotLearnedSkills, own.learned);
    g_earned = own.earned;
    g_lastSent = {};
    g_tree = ReadSkills(localPlayer);
    g_effects.clear();
    Log("skills: in the shared tree (%d learned); own points: %u earned, %u spent, level %.0f", Learned(g_tree),
        own.earned, own.learned, level);
    for (const std::vector<uint8_t>& payload : g_early) TakeTree(payload, localPlayer);
    g_early.clear();
}

// Guest: a point per level above the first, as the game gives the host (the level is shared); the points to spend
// are those minus what was bought. Counted from the level, not added up per level-up: a rejoin restores the stored
// count and then sees the level "rise" from 1 again (PLAYTEST3.md: 20 points at level 11).
void KeepPoints(yyc::CInstance* localPlayer) {
    const int player = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    const double level = ReadNumber(player, kSlotLevel);
    const uint32_t earned = level > 1 ? static_cast<uint32_t>(level - 1) : 0;
    if (earned != g_earned) Log("skills: level %.0f - %u points earned", level, earned);
    g_earned = earned;
    const double learned = ReadGlobal(kSlotLearnedSkills);
    const double points = g_earned > learned ? g_earned - learned : 0;
    if (ReadNumber(player, kSlotSkillPoints) != points) {
        Log("skills: %g points to spend (were %g here)", points, ReadNumber(player, kSlotSkillPoints));
        WriteNumber(player, kSlotSkillPoints, points);
    }
}

void Upload(yyc::CInstance* localPlayer) {
    const State now{g_earned, static_cast<uint32_t>(ReadGlobal(kSlotLearnedSkills)), ReadSkills(localPlayer)};
    if (now == g_lastSent) return;
    const std::vector<uint8_t> payload = Encode(kUpload, now);
    if (net::Send(kSkills, payload.data(), payload.size())) {
        g_lastSent = now;
        Log("skills: sent own points (%u earned, %u spent)", now.earned, now.learned);
    }
}

// Testing aid: COOP_DEBUG_SKILL=<n> learns skill n here ~10 s in, as buying it would (no point is spent on the
// host; the guest's points follow from what it learned). Nothing it would unlock is unlocked. =avail<k>: the k-th
// skill available here (not learned yet), whatever the save has.
void DebugLearn(yyc::CInstance* localPlayer) {
    static const std::string wanted = [] {
        char text[16] = {};
        GetEnvironmentVariableA("COOP_DEBUG_SKILL", text, sizeof(text));
        return std::string(text);
    }();
    static unsigned steps = 0;
    if (wanted.empty() || ++steps != 600) return;
    std::vector<int8_t> values = ReadSkills(localPlayer);
    int skill = -1;
    if (wanted.rfind("avail", 0) == 0) {
        int k = atoi(wanted.c_str() + 5);
        for (size_t i = 0; i < values.size() && skill < 0; ++i) {
            if (values[i] == kAvailable && k-- == 0) skill = static_cast<int>(i);
        }
    } else {
        skill = atoi(wanted.c_str());
    }
    if (skill < 0 || skill >= static_cast<int>(values.size())) return;
    values[skill] = kLearned;
    WriteSkills(values);
    WriteGlobal(kSlotLearnedSkills, ReadGlobal(kSlotLearnedSkills) + 1);
    Log("skills: debug learned skill %d here", skill);
}

bool Hook(uint32_t va, void* detour, void* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original));
    if (status != MH_OK) Log("skills: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

}  // namespace

bool InstallHooks() {
    bool ok = Hook(kScriptItemEdit, reinterpret_cast<void*>(&ItemEditHook), &g_origItemEdit);
    ok = Hook(kScriptStructureEdit, reinterpret_cast<void*>(&StructureEditHook), &g_origStructureEdit) && ok;
    return ok;
}

void RecordPurchases(bool on) { g_recording = on; }

void HostOnGuest(const std::string& name, net::PeerId peer, yyc::CInstance* localPlayer) {
    std::ifstream in(character::GuestDataFile(name, ".skills"), std::ios::binary);
    const std::vector<uint8_t> stored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    State state;
    const bool ok = !stored.empty() && Decode(stored.data(), stored.size(), &state);
    // A guest from before the tree was shared kept a tree of its own: what it learned there joins the shared tree
    // (the snapshot sent next carries it), without what those skills unlocked on the guest's side back then.
    const std::vector<int8_t> here = ok ? ReadSkills(localPlayer) : std::vector<int8_t>{};
    if (ok && !here.empty() && here.size() == state.skills.size()) {
        const std::vector<int8_t> merged = Merge(here, state.skills);
        if (merged != here) {
            WriteSkills(merged);
            Log("skills: guest '%s' had learned on its own %s- now in the shared tree", name.c_str(),
                NewlyLearned(here, merged).c_str());
        }
    }
    const std::vector<uint8_t> payload = ok ? Encode(kStored, state) : std::vector<uint8_t>{kNothingStored};
    net::SendTo(peer, kSkills, payload.data(), payload.size());
    Log("skills: guest '%s': %s", name.c_str(), ok ? "stored points sent" : "nothing stored");
}

void Tick(yyc::CInstance* localPlayer, bool host) {
    if (host || g_applied) DebugLearn(localPlayer);
    if (++g_steps % kEverySteps != 0) return;
    if (!host) {
        if (!g_answered) return;
        if (!g_applied) {
            Apply(localPlayer);
            g_applied = true;
        }
        KeepPoints(localPlayer);
        if (g_steps % kUploadEverySteps == 0) Upload(localPlayer);
    }
    ShareTree(localPlayer);
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kSkills) return false;
    const auto& p = message.payload;
    if (p.empty()) return true;
    if (p[0] == kTree) {
        // Messages from the host come only to guests: one not in the world yet takes the tree once it is.
        if (message.from == net::kHostPeer && !g_applied) {
            g_early.push_back(p);
            return true;
        }
        TakeTree(p, localPlayer);
        if (message.from != net::kHostPeer) net::SendExcept(message.from, kSkills, p.data(), p.size());
        return true;
    }
    if (p[0] == kUpload) {  // host
        State state;
        if (!Decode(p.data() + 1, p.size() - 1, &state)) return true;
        const std::string name = character::HostGuestName(message.from);
        const std::wstring path = character::GuestDataFile(name, ".skills");
        std::ofstream(path, std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(p.data() + 1), static_cast<std::streamsize>(p.size() - 1));
        Log("skills: kept guest '%s' points (%u earned, %u spent)", name.c_str(), state.earned, state.learned);
        return true;
    }
    g_newGuest = p[0] == kNothingStored;
    if (!g_newGuest && !Decode(p.data() + 1, p.size() - 1, &g_stored)) return true;
    g_answered = true;
    g_applied = false;
    Log("skills: host answered (%s)", g_newGuest ? "new guest" : "stored points");
    return true;
}

void Reset() {
    g_recording = false;
    g_effects.clear();
    g_tree.clear();
    g_steps = 0;
    g_answered = false;
    g_applied = false;
    g_newGuest = false;
    g_stored = {};
    g_lastSent = {};
    g_earned = 0;
    g_early.clear();
}

}  // namespace coop::skills
