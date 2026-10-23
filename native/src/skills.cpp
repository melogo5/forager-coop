#include "skills.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "character.h"
#include "log.h"
#include "mutant.h"

namespace coop::skills {

namespace {

// Payload: u8 kind, then State (kind != kNothingStored): u32 earned, u32 learned, u16 count, i8 skills[count].
constexpr uint8_t kSkills = 71;
constexpr uint8_t kNothingStored = 0;  // host -> guest: a new guest
constexpr uint8_t kStored = 1;         // host -> guest: the guest's skills as last uploaded
constexpr uint8_t kUpload = 2;         // guest -> host: keep these
constexpr unsigned kApplyEverySteps = 30;
constexpr unsigned kUploadEverySteps = 300;
constexpr int kSlotSkillUnlocked = 0x7BF;  // global.skillUnlocked[skill] (var_slots.tsv)
constexpr int kSlotLearnedSkills = 0x601;  // global.learnedSkills: skills bought (objSkill Step_0 adds 1)
constexpr int kSlotSkillPoints = 0x7BD;    // objPlayer.skills: the points to spend (the tree shows and spends these;
                                           // WorldControl.availableSkillPoints is set once and never used)
constexpr int kSlotLevel = 0x603;          // objPlayer.level
constexpr int kMaxSkills = 512;
// global.skillUnlocked values: a new world has the first skill of each 16-skill tree available, the rest locked.
constexpr int8_t kLocked = -1;
constexpr int8_t kAvailable = 1;
constexpr int8_t kLearned = 2;  // what HasSkill checks
constexpr int kTreeSize = 16;
constexpr int kNoOne = -4;

struct State {
    uint32_t earned = 0;   // skill points ever got: one per shared level-up
    uint32_t learned = 0;  // skills bought with them
    std::vector<int8_t> skills;  // global.skillUnlocked as it is, one value per skill
    bool operator==(const State& o) const { return earned == o.earned && learned == o.learned && skills == o.skills; }
};

// Guest.
bool g_answered = false;
bool g_applied = false;
bool g_newGuest = false;
State g_stored;
State g_lastSent;
uint32_t g_earned = 0;
double g_lastLevel = 0;
unsigned g_steps = 0;

double CallReal(yyc::CInstance* self, const char* builtin, std::initializer_list<double> values) {
    yyc::RValue args[4] = {};
    int argc = 0;
    for (const double v : values) args[argc++] = yyc::Real(v);
    yyc::RValue result{};
    yyc::CallBuiltin(builtin, self, argc, args, &result);
    const double value = yyc::ToReal(result);
    yyc::Free(&result);
    return value;
}

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
    int learned = 0;
    for (const int8_t v : values) learned += v == kLearned ? 1 : 0;
    return learned;
}


std::vector<uint8_t> Encode(uint8_t kind, const State& state) {
    std::vector<uint8_t> out{kind};
    auto put = [&out](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + size);
    };
    const uint16_t count = static_cast<uint16_t>(state.skills.size());
    put(&state.earned, 4);
    put(&state.learned, 4);
    put(&count, 2);
    put(state.skills.data(), count);
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

// A new world's skills, for a new guest.
std::vector<int8_t> FreshSkills(size_t count) {
    std::vector<int8_t> values(count, kLocked);
    for (size_t i = 0; i < count; i += kTreeSize) values[i] = kAvailable;
    return values;
}

// The skills that are not locked, e.g. "0=2 1=1 16=1".
std::string Describe(const std::vector<int8_t>& values) {
    std::string text;
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i] != kLocked) text += std::to_string(i) + "=" + std::to_string(values[i]) + " ";
    }
    return text;
}

// Guest, once in the host's world: its own skills replace the host's. A new guest starts with none learned and
// a point for every level the shared progress has reached.
void Apply(yyc::CInstance* localPlayer) {
    const int player = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    const double level = ReadNumber(player, kSlotLevel);
    const std::vector<int8_t> host = ReadSkills(localPlayer);
    Log("skills: the host's: %s(%zu skills, %.0f learned)", Describe(host).c_str(), host.size(),
        ReadGlobal(kSlotLearnedSkills));
    State own = g_stored;
    if (g_newGuest) own = State{level > 1 ? static_cast<uint32_t>(level - 1) : 0, 0, FreshSkills(host.size())};
    if (own.skills.size() == host.size()) {
        WriteSkills(own.skills);
    } else {
        Log("skills: %zu stored skills, %zu here - the host's are kept", own.skills.size(), host.size());
        own.skills = host;
    }
    WriteGlobal(kSlotLearnedSkills, own.learned);
    g_earned = own.earned;
    g_lastLevel = level;
    g_lastSent = {};  // the host acts with them (HostSwapIn), so it hears them at the next upload even unchanged
    Log("skills: own skills applied: %s(%d learned, %u points earned, level %.0f)", Describe(own.skills).c_str(),
        Learned(own.skills), own.earned, level);
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
    g_lastLevel = level;
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
        Log("skills: sent own skills (%d learned, %u points earned)", Learned(now.skills), now.earned);
    }
}

// Testing aid: COOP_DEBUG_SKILL=<n> learns skill n here ~10 s in, as buying it would (no point is spent on the
// host; the guest's points follow from what it learned).
void DebugLearn(yyc::CInstance* localPlayer, unsigned steps) {
    static const int skill = [] {
        char text[16] = {};
        return GetEnvironmentVariableA("COOP_DEBUG_SKILL", text, sizeof(text)) > 0 ? atoi(text) : -1;
    }();
    if (skill < 0 || steps != 600) return;
    std::vector<int8_t> values = ReadSkills(localPlayer);
    if (skill >= static_cast<int>(values.size())) return;
    values[skill] = kLearned;
    WriteSkills(values);
    WriteGlobal(kSlotLearnedSkills, ReadGlobal(kSlotLearnedSkills) + 1);
    Log("skills: debug learned skill %d here", skill);
}

// Host.
unsigned g_hostSteps = 0;
std::map<net::PeerId, std::vector<int8_t>> g_guestSkills;  // each guest's skills, as last stored or uploaded
std::vector<int8_t> g_hostOwn;                             // the host's, while a guest's are swapped in
bool g_swapped = false;

}  // namespace

void HostSwapIn(yyc::CInstance* puppet, net::PeerId player) {
    if (Mutant("skills")) return;
    const auto guest = g_guestSkills.find(player);
    if (g_swapped || puppet == nullptr || guest == g_guestSkills.end()) return;
    std::vector<int8_t> own = ReadSkills(puppet);
    if (own.size() != guest->second.size()) return;
    g_hostOwn = std::move(own);
    WriteSkills(guest->second);
    g_swapped = true;
    static std::map<net::PeerId, bool> logged;
    if (!logged[player]) {
        logged[player] = true;
        Log("skills: acting as guest %u with its own skills (%d learned; the host's: %d)", player,
            Learned(guest->second), Learned(g_hostOwn));
    }
}

void HostSwapOut() {
    if (!g_swapped) return;
    WriteSkills(g_hostOwn);
    g_swapped = false;
}

void HostOnGuest(const std::string& name, net::PeerId peer) {
    std::ifstream in(character::GuestDataFile(name, ".skills"), std::ios::binary);
    const std::vector<uint8_t> stored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    State state;
    const bool ok = !stored.empty() && Decode(stored.data(), stored.size(), &state);
    if (ok) g_guestSkills[peer] = state.skills;
    const std::vector<uint8_t> payload = ok ? Encode(kStored, state) : std::vector<uint8_t>{kNothingStored};
    net::SendTo(peer, kSkills, payload.data(), payload.size());
    Log("skills: guest '%s': %s", name.c_str(), ok ? "stored skills sent" : "nothing stored");
}

void Tick(yyc::CInstance* localPlayer) {
    if (g_swapped) {  // acting ends within a step; never let the host keep (or save) a guest's skills
        HostSwapOut();
        Log("skills: the host's skills put back after acting as a guest");
    }
    DebugLearn(localPlayer, g_answered ? g_steps + 1 : ++g_hostSteps);
    if (!g_answered || ++g_steps % kApplyEverySteps != 0) return;
    if (!g_applied) {
        Apply(localPlayer);
        g_applied = true;
    }
    KeepPoints(localPlayer);
    if (g_steps % kUploadEverySteps == 0) Upload(localPlayer);
}

bool Handle(const net::Message& message) {
    if (message.type != kSkills) return false;
    const auto& p = message.payload;
    if (p.empty()) return true;
    if (p[0] == kUpload) {  // host
        State state;
        if (!Decode(p.data() + 1, p.size() - 1, &state)) return true;
        const std::string name = character::HostGuestName(message.from);
        const std::wstring path = character::GuestDataFile(name, ".skills");
        std::ofstream(path, std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(p.data() + 1), static_cast<std::streamsize>(p.size() - 1));
        g_guestSkills[message.from] = state.skills;
        Log("skills: kept guest '%s' skills (%d learned, %u points earned)", name.c_str(),
            Learned(state.skills), state.earned);
        return true;
    }
    g_newGuest = p[0] == kNothingStored;
    if (!g_newGuest && !Decode(p.data() + 1, p.size() - 1, &g_stored)) return true;
    g_answered = true;
    g_applied = false;
    Log("skills: host answered (%s)", g_newGuest ? "new guest" : "stored skills");
    return true;
}

void Reset() {
    HostSwapOut();
    g_guestSkills.clear();
    g_answered = false;
    g_applied = false;
    g_newGuest = false;
    g_stored = {};
    g_lastSent = {};
    g_earned = 0;
    g_lastLevel = 0;
    g_steps = 0;
}

}  // namespace coop::skills
