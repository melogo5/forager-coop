#include "clock.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "log.h"

namespace coop::clock {

namespace {

constexpr uint8_t kClock = 60;  // host -> guest
constexpr unsigned kEverySteps = 60;
constexpr unsigned kLogEverySteps = 600;
constexpr int kNoOne = yyc::kNoOne;

constexpr int kObjTimeControl = 14;  // decompiled/data/objects.tsv
constexpr int kObjWeatherControl = 15;

struct Var {
    int object;
    int slot;  // decompiled/code/var_slots.tsv
};
constexpr Var kVars[] = {
    {kObjTimeControl, 0x886},     // timer: time in the current light state (timerMax[lightState] is the length)
    {kObjTimeControl, 0x60A},     // lightState: morning, day, dusk, night
    {kObjTimeControl, 0x7F5},     // speedToDay: sleeping through the night
    {kObjWeatherControl, 0x743},  // rainTimer
    {kObjWeatherControl, 0x7CF},  // snowTimer
    {kObjWeatherControl, 0x7CC},  // smokeTimer
    {kObjWeatherControl, 0x1C7},  // dustTimer
};
constexpr size_t kVarCount = sizeof(kVars) / sizeof(kVars[0]);

struct ClockMsg {
    double values[kVarCount];
};

unsigned g_steps = 0;
double g_lastLightState = -1;

int InstanceOf(yyc::CInstance* self, int object) {
    yyc::RValue args[2] = {yyc::Real(object), yyc::Real(0)};
    yyc::RValue result{};
    yyc::CallBuiltin("instance_find", self, 2, args, &result);
    const int id = static_cast<int>(yyc::ToReal(result));
    yyc::Free(&result);
    return id;
}

// Only plain numbers are overwritten: replacing an array or a handle would break the game's own code.
bool IsNumber(int instance, int slot) {
    yyc::RValue value{};
    if (!yyc::GetVar(instance, slot, &value)) return false;
    const bool number = (value.kind & yyc::kKindMask) == yyc::kReal;
    yyc::Free(&value);
    return number;
}

double Read(int instance, int slot) {
    yyc::RValue value{};
    if (!yyc::GetVar(instance, slot, &value)) return 0;
    const double real = yyc::ToReal(value);
    yyc::Free(&value);
    return real;
}

// Testing aid: COOP_DEBUG_CLOCK=<timer> sets the host's time of day once, on the first send.
void DebugSetTime(int time) {
    static bool done = false;
    char value[16] = {};
    if (done || GetEnvironmentVariableA("COOP_DEBUG_CLOCK", value, sizeof(value)) == 0) return;
    done = true;
    yyc::RValue timer = yyc::Real(atof(value));
    yyc::SetVar(time, kVars[0].slot, &timer);
    Log("clock: debug time set to %s", value);
}

}  // namespace

void HostTick(yyc::CInstance* localPlayer) {
    if (++g_steps % kEverySteps != 0) return;
    const int time = InstanceOf(localPlayer, kObjTimeControl);
    const int weather = InstanceOf(localPlayer, kObjWeatherControl);
    if (time == kNoOne || weather == kNoOne) return;
    DebugSetTime(time);
    ClockMsg msg{};
    for (size_t i = 0; i < kVarCount; ++i) {
        msg.values[i] = Read(kVars[i].object == kObjTimeControl ? time : weather, kVars[i].slot);
    }
    net::Send(kClock, &msg, sizeof(msg));
    if (g_steps % kLogEverySteps == 0) {
        Log("clock: sent time %.0f, light %.0f, rain %.0f snow %.0f smoke %.0f dust %.0f", msg.values[0], msg.values[1],
            msg.values[3], msg.values[4], msg.values[5], msg.values[6]);
    }
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kClock) return false;
    if (message.payload.size() != sizeof(ClockMsg)) return true;
    ClockMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    const int time = InstanceOf(localPlayer, kObjTimeControl);
    const int weather = InstanceOf(localPlayer, kObjWeatherControl);
    if (time == kNoOne || weather == kNoOne) return true;
    const double localTime = Read(time, kVars[0].slot);
    for (size_t i = 0; i < kVarCount; ++i) {
        const int instance = kVars[i].object == kObjTimeControl ? time : weather;
        if (!IsNumber(instance, kVars[i].slot)) continue;
        yyc::RValue value = yyc::Real(msg.values[i]);
        yyc::SetVar(instance, kVars[i].slot, &value);
    }
    if (msg.values[1] != g_lastLightState) {
        Log("clock: light state %.0f -> %.0f (time %.0f, ours was %.0f)", g_lastLightState, msg.values[1],
            msg.values[0], localTime);
        g_lastLightState = msg.values[1];
    }
    return true;
}

}  // namespace coop::clock
