#include "displays.h"

#include <cmath>
#include <cstring>
#include <map>
#include <utility>

#include "log.h"
#include "sync.h"

namespace coop::displays {

namespace {

constexpr uint8_t kDisplay = 57;          // either way: DisplayMsg
constexpr int kObjDisplayCase = 145;      // decompiled/data/objects.tsv
constexpr int kSlotSocket = 0x7D0;        // objDisplayCase.socket: the item shown, noone when empty (var_slots.tsv)
constexpr double kNoOne = yyc::kNoOne;
constexpr unsigned kScanEverySteps = 30;  // ~0.5 s

#pragma pack(push, 1)
struct DisplayMsg {
    double x, y;    // the case's position (both sides have it at the same spot)
    double socket;  // the item it shows, or noone
};
#pragma pack(pop)

using Spot = std::pair<double, double>;
std::map<Spot, double> g_known;  // what each case held when last seen or set from the network
unsigned g_steps = 0;

double Socket(int id) {
    yyc::RValue value{};
    const bool number = yyc::GetVar(id, kSlotSocket, &value) && yyc::IsNumber(value);
    const double socket = number ? yyc::ToReal(value) : kNoOne;
    yyc::Free(&value);
    return socket;
}

// A case changed here since it was last seen: a player of this side put something in or took it out.
void Scan(yyc::CInstance* localPlayer) {
    const int count = static_cast<int>(yyc::CallReal(localPlayer, "instance_number", {kObjDisplayCase}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(yyc::CallReal(localPlayer, "instance_find", {kObjDisplayCase, double(i)}));
        yyc::CInstance* display = yyc::FindInstance(id);
        if (display == nullptr) continue;
        const DisplayMsg msg{yyc::GetBuiltinReal(display, yyc::va::YYVar_x),
                             yyc::GetBuiltinReal(display, yyc::va::YYVar_y), Socket(id)};
        const auto known = g_known.find({msg.x, msg.y});
        if (known == g_known.end()) {
            g_known[{msg.x, msg.y}] = msg.socket;  // first sight: what came with the world
            continue;
        }
        if (known->second == msg.socket) continue;
        known->second = msg.socket;
        net::Send(kDisplay, &msg, sizeof(msg));
        Log("displays: the case at (%.0f, %.0f) now shows %.0f - sent", msg.x, msg.y, msg.socket);
    }
}

void Apply(yyc::CInstance* localPlayer, const DisplayMsg& msg) {
    const int id = static_cast<int>(yyc::CallReal(localPlayer, "instance_position", {msg.x, msg.y, kObjDisplayCase}));
    yyc::CInstance* display = id == yyc::kNoOne ? nullptr : yyc::FindInstance(id);
    if (display == nullptr || yyc::GetBuiltinReal(display, yyc::va::YYVar_x) != msg.x ||
        yyc::GetBuiltinReal(display, yyc::va::YYVar_y) != msg.y) {
        Log("displays: no case at (%.0f, %.0f) for item %.0f", msg.x, msg.y, msg.socket);
        return;
    }
    yyc::RValue socket = yyc::Real(msg.socket);
    yyc::SetVar(id, kSlotSocket, &socket);
    g_known[{msg.x, msg.y}] = msg.socket;
    Log("displays: the case at (%.0f, %.0f) shows %.0f, as the peer's", msg.x, msg.y, msg.socket);
}

}  // namespace

void Tick(yyc::CInstance* localPlayer) {
    if (localPlayer == nullptr || !net::IsConnected() || ++g_steps % kScanEverySteps != 0) return;
    Scan(localPlayer);
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kDisplay) return false;
    if (message.payload.size() != sizeof(DisplayMsg)) return true;
    DisplayMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (!std::isfinite(msg.x) || !std::isfinite(msg.y) || !std::isfinite(msg.socket)) return true;
    // The case may be off-screen, i.e. deactivated: wake the cases up first.
    sync::WakeThen(localPlayer, kObjDisplayCase, [localPlayer, msg] { Apply(localPlayer, msg); });
    return true;
}

void Reset() {
    g_known.clear();
    g_steps = 0;
}

}  // namespace coop::displays
