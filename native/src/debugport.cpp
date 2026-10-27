#include "debugport.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gui.h"
#include "log.h"
#include "mobs.h"
#include "reconcile.h"
#include "sync.h"

#pragma comment(lib, "ws2_32.lib")

namespace coop::debugport {

namespace {

constexpr auto kAnswerTimeout = std::chrono::seconds(5);  // a loading screen or a modal window: no Tick runs
constexpr size_t kMaxLine = 1024;
constexpr size_t kMaxListed = 100;
constexpr double kSameSpot = 4;  // pixels, as sync and reconcile match objects
constexpr int kNoOne = yyc::kNoOne;

struct Request {
    std::string line;
    std::string reply;
    bool done = false;
};

std::mutex g_lock;
std::condition_variable g_answered;
std::deque<std::shared_ptr<Request>> g_queue;

double Number(const std::string& text) { return std::strtod(text.c_str(), nullptr); }
int Slot(const std::string& text) { return static_cast<int>(std::strtol(text.c_str(), nullptr, 0)); }

std::string Format(const yyc::RValue& value) {
    char text[64] = {};
    if (yyc::IsNumber(value)) {
        snprintf(text, sizeof(text), "%.10g", yyc::ToReal(value));
        return text;
    }
    switch (value.kind & yyc::kKindMask) {
        case yyc::kString: return "\"" + yyc::ToString(value) + "\"";
        case yyc::kArray: return "array";
        case yyc::kBool: return yyc::ToReal(value) != 0 ? "true" : "false";
        default: return "undefined";
    }
}

// value, or its element `index` when one is given and the value is an array (in bounds only: reading past the end
// grows a GML array).
std::string FormatAt(yyc::CInstance* self, yyc::RValue value, const std::vector<std::string>& words, size_t indexWord) {
    if (words.size() <= indexWord) return Format(value);
    if ((value.kind & yyc::kKindMask) != yyc::kArray) return "not an array";
    yyc::RValue length{};
    yyc::CallBuiltin("array_length_1d", self, 1, &value, &length);
    const int n = static_cast<int>(yyc::ToReal(length));
    yyc::Free(&length);
    const int index = Slot(words[indexWord]);
    if (index < 0 || index >= n) return "out of bounds (length " + std::to_string(n) + ")";
    return Format(yyc::ArrayElement(&value, index));
}

std::string Player(yyc::CInstance* self) {
    char text[128] = {};
    snprintf(text, sizeof(text), "%.0f %.1f %.1f %.0f", yyc::GetBuiltinReal(self, yyc::va::YYVar_id),
             yyc::GetBuiltinReal(self, yyc::va::YYVar_x), yyc::GetBuiltinReal(self, yyc::va::YYVar_y),
             yyc::GetBuiltinReal(self, yyc::va::YYVar_room));
    return text;
}

struct Found {
    int id;
    double x, y, distance;
};

// Active instances of `object`; with a point, only those within `radius` of it, nearest first.
std::vector<Found> Instances(int object, bool aroundPoint, double px, double py, double radius) {
    std::vector<Found> found;
    const int count = static_cast<int>(gui::Call("instance_number", {double(object)}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(gui::Call("instance_find", {double(object), double(i)}));
        yyc::CInstance* instance = id == kNoOne ? nullptr : yyc::FindInstance(id);
        if (instance == nullptr) continue;
        const double x = yyc::GetBuiltinReal(instance, yyc::va::YYVar_x);
        const double y = yyc::GetBuiltinReal(instance, yyc::va::YYVar_y);
        const double distance = aroundPoint ? std::hypot(x - px, y - py) : 0;
        if (aroundPoint && distance > radius) continue;
        found.push_back({id, x, y, distance});
    }
    if (aroundPoint) {
        std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.distance < b.distance; });
    }
    return found;
}

std::string List(const std::vector<std::string>& w) {
    const bool aroundPoint = w.size() >= 5;
    const std::vector<Found> found =
        Instances(Slot(w[1]), aroundPoint, aroundPoint ? Number(w[2]) : 0, aroundPoint ? Number(w[3]) : 0, aroundPoint ? Number(w[4]) : 0);
    std::ostringstream out;
    out << found.size() << ":";
    for (size_t i = 0; i < found.size() && i < kMaxListed; ++i) {
        char item[96] = {};
        snprintf(item, sizeof(item), " %d %.1f %.1f;", found[i].id, found[i].x, found[i].y);
        out << item;
    }
    return out.str();
}

// "dump": every world object that should be the same at every player (reconcile::WorldObjects), as "object x y"
// sorted, for world-diff.ps1.
std::string Dump(yyc::CInstance* self) {
    std::vector<std::string> lines;
    for (const reconcile::WorldObject& o : reconcile::WorldObjects(self)) {
        char line[64] = {};
        snprintf(line, sizeof(line), "%d %.0f %.0f", o.object, o.x, o.y);
        lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    std::ostringstream out;
    out << lines.size() << ":";
    for (const std::string& line : lines) out << " " << line << ";";
    return out.str();
}

std::string Call(yyc::CInstance* self, const std::vector<std::string>& w) {
    std::vector<yyc::RValue> args;
    for (size_t i = 2; i < w.size(); ++i) args.push_back(yyc::Real(Number(w[i])));
    yyc::RValue result{};
    if (!yyc::CallBuiltin(w[1].c_str(), self, static_cast<int>(args.size()), args.data(), &result)) {
        return "no builtin " + w[1];
    }
    const std::string text = Format(result);
    yyc::Free(&result);
    return text;
}

std::string Answer(yyc::CInstance* self, const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> w;
    for (std::string word; in >> word;) w.push_back(word);
    if (w.empty()) return "empty";
    const std::string& c = w[0];
    gui::Begin(self);
    if (c == "ping") return "pong";
    if (c == "player") return Player(self);
    if (c == "count" && w.size() == 2) {
        char text[32] = {};
        snprintf(text, sizeof(text), "%.0f", gui::Call("instance_number", {double(Slot(w[1]))}));
        return text;
    }
    if (c == "list" && (w.size() == 2 || w.size() == 5)) return List(w);
    if (c == "at" && w.size() == 4) {
        return std::to_string(Instances(Slot(w[1]), true, Number(w[2]), Number(w[3]), kSameSpot).size());
    }
    if (c == "var" && (w.size() == 3 || w.size() == 4)) {
        yyc::RValue value{};
        if (!yyc::GetVar(Slot(w[1]), Slot(w[2]), &value)) return "no such instance";
        const std::string text = FormatAt(self, value, w, 3);
        yyc::Free(&value);
        return text;
    }
    if (c == "global" && (w.size() == 2 || w.size() == 3)) {
        const yyc::RValue* value = yyc::GlobalVar(Slot(w[1]));
        return value != nullptr ? FormatAt(self, *value, w, 2) : "no globals";
    }
    if (c == "set" && (w.size() == 4 || w.size() == 5)) {
        if (yyc::FindInstance(Slot(w[1])) == nullptr) return "no such instance";
        yyc::RValue value = yyc::Real(Number(w.back()));
        if (w.size() == 5) {
            yyc::SetVarAt(Slot(w[1]), Slot(w[2]), Slot(w[3]), &value);
        } else {
            yyc::SetVar(Slot(w[1]), Slot(w[2]), &value);
        }
        return "ok";
    }
    if (c == "sets" && w.size() == 4) {  // a string variable (one word), e.g. an NPC's callbackPending
        if (yyc::FindInstance(Slot(w[1])) == nullptr) return "no such instance";
        yyc::RValue value = yyc::String(w[3].c_str());
        yyc::SetVar(Slot(w[1]), Slot(w[2]), &value);
        yyc::Free(&value);
        return "ok";
    }
    if (c == "event" && w.size() == 4) {
        yyc::CInstance* target = yyc::FindInstance(Slot(w[1]));
        if (target == nullptr) return "no such instance";
        yyc::PerformEvent(target, self, Slot(w[2]), Slot(w[3]));
        return yyc::FindInstance(Slot(w[1])) != nullptr ? "ok" : "ok (it is gone)";
    }
    if (c == "builtins" && w.size() == 2) return yyc::BuiltinsMatching(w[1].c_str());
    if (c == "builtinat" && w.size() == 2) return yyc::BuiltinNameAt(Slot(w[1]));
    if (c == "yyvar" && w.size() == 2) {  // a game variable `<builtin>.id` (decompiled/code/yyvar_map.tsv)
        const int id = yyc::At<yyc::YYVar*>(static_cast<uint32_t>(std::strtoul(w[1].c_str(), nullptr, 16)))->id;
        return std::to_string(id) + " = " + yyc::BuiltinNameAt(id);
    }
    if (c == "script" && w.size() >= 2) {  // a GML script by its address (decompiled/code/index.tsv), self = player
        std::vector<yyc::RValue> values;
        for (size_t i = 2; i < w.size(); ++i) values.push_back(yyc::Real(Number(w[i])));
        std::vector<yyc::RValue*> args;
        for (yyc::RValue& v : values) args.push_back(&v);
        yyc::RValue result{};
        const auto script = yyc::At<yyc::ScriptFn>(static_cast<uint32_t>(std::strtoul(w[1].c_str(), nullptr, 16)));
        script(self, self, &result, static_cast<int>(args.size()), args.empty() ? nullptr : args.data());
        const std::string text = Format(result);
        yyc::Free(&result);
        return text;
    }
    if (c == "call" && w.size() >= 2) return Call(self, w);
    if (c == "dump") return Dump(self);
    if (c == "sleepcheck") {  // world objects asleep but not on global.deactivatedInstances: "N: id; ..."
        std::vector<double> listed;
        if (const yyc::RValue* asleep = yyc::GlobalVar(0x16C); asleep != nullptr && yyc::IsNumber(*asleep)) {
            const double list = yyc::ToReal(*asleep);
            const int n = static_cast<int>(gui::Call("ds_list_size", {list}));
            for (int i = 0; i < n; ++i) listed.push_back(gui::Call("ds_list_find_value", {list, double(i)}));
        }
        std::ostringstream out;
        int count = 0;
        for (int id = 100000, gap = 0; gap < 20000; ++id) {
            yyc::CInstance* instance = yyc::FindInstance(id);
            gap = instance == nullptr ? gap + 1 : 0;
            if (instance == nullptr || !yyc::IsDeactivated(instance)) continue;
            if (std::find(listed.begin(), listed.end(), double(id)) != listed.end()) continue;
            if (count++ < 20) out << " " << id << ";";
        }
        return std::to_string(count) + ":" + out.str();
    }
    if (c == "cellcheck") return mobs::DeadCellEntries(self);
    if (c == "look" && w.size() == 2) {  // how an instance is drawn: sprite, frame, alpha, visible, xscale, depth
        yyc::CInstance* instance = yyc::FindInstance(Slot(w[1]));
        if (instance == nullptr) return "no such instance";
        constexpr uint32_t kImageAlpha = 0x01AD8790, kVisible = 0x01AD8C80;  // decompiled/code/yyvar_map.tsv
        char text[160] = {};
        snprintf(text, sizeof(text), "sprite %.0f image %.2f alpha %.2f visible %.0f xscale %.2f depth %.0f",
                 yyc::GetBuiltinReal(instance, yyc::va::YYVar_sprite_index),
                 yyc::GetBuiltinReal(instance, yyc::va::YYVar_image_index), yyc::GetBuiltinReal(instance, kImageAlpha),
                 yyc::GetBuiltinReal(instance, kVisible), yyc::GetBuiltinReal(instance, yyc::va::YYVar_image_xscale),
                 yyc::GetBuiltinReal(instance, yyc::va::YYVar_depth));
        return text;
    }
    if (c == "obj" && w.size() == 2) {  // what an instance id is: object, name, place, asleep or not
        yyc::CInstance* instance = yyc::FindInstance(Slot(w[1]));
        if (instance == nullptr) return "no such instance";
        const double object = yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index);
        yyc::RValue arg = yyc::Real(object);
        yyc::RValue name{};
        yyc::CallBuiltin("object_get_name", self, 1, &arg, &name);
        char text[160] = {};
        snprintf(text, sizeof(text), "%.0f %s at (%.0f, %.0f)%s", object, yyc::ToString(name).c_str(),
                 yyc::GetBuiltinReal(instance, yyc::va::YYVar_x), yyc::GetBuiltinReal(instance, yyc::va::YYVar_y),
                 yyc::IsDeactivated(instance) ? " asleep" : "");
        yyc::Free(&name);
        return text;
    }
    return "unknown command (ping, player, count, list, at, var, set, event, global, call, dump)";
}

bool LooksLikeHttp(const std::string& line) {
    for (const char* verb : {"GET ", "POST ", "PUT ", "HEAD ", "OPTIONS ", "DELETE ", "PATCH "}) {
        if (line.rfind(verb, 0) == 0) return true;
    }
    return line.find(" HTTP/") != std::string::npos;
}

// One connection at a time: lines in, an answer each, until the client hangs up.
void Serve(SOCKET client) {
    std::string pending;
    char buffer[512];
    for (;;) {
        const int got = recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0) break;
        pending.append(buffer, got);
        if (pending.size() > kMaxLine * 4) break;
        for (size_t end; (end = pending.find('\n')) != std::string::npos;) {
            std::string line = pending.substr(0, end);
            pending.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (LooksLikeHttp(line)) {  // a web page posting to 127.0.0.1 must not drive the game
                closesocket(client);
                return;
            }
            auto request = std::make_shared<Request>();
            request->line = line.substr(0, kMaxLine);
            std::unique_lock<std::mutex> lock(g_lock);
            g_queue.push_back(request);
            const bool done = g_answered.wait_for(lock, kAnswerTimeout, [&request] { return request->done; });
            if (!done) g_queue.erase(std::remove(g_queue.begin(), g_queue.end(), request), g_queue.end());
            const std::string reply = (done ? request->reply : "timeout (not in a world?)") + "\n";
            lock.unlock();
            send(client, reply.data(), static_cast<int>(reply.size()), 0);
        }
    }
    closesocket(client);
}

void Listen(SOCKET server) {
    for (;;) {
        const SOCKET client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET) break;
        Serve(client);
    }
}

}  // namespace

void Start() {
    static bool started = false;
    if (started || GetEnvironmentVariableA("COOP_DEBUG_PORT", nullptr, 0) == 0) return;
    started = true;
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
    const SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this PC only
    address.sin_port = 0;                              // any free port: two games run side by side in tests
    int size = sizeof(address);
    if (server == INVALID_SOCKET || bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(server, 1) != 0 || getsockname(server, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        Log("debugport: could not listen (error %d)", WSAGetLastError());
        if (server != INVALID_SOCKET) closesocket(server);
        return;
    }
    Log("debugport: listening on 127.0.0.1:%u", ntohs(address.sin_port));
    std::thread(Listen, server).detach();
}

void Finish(const std::shared_ptr<Request>& request, const std::string& reply) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        request->reply = reply;
        request->done = true;
    }
    g_answered.notify_all();
}

void Tick(yyc::CInstance* localPlayer) {
    std::deque<std::shared_ptr<Request>> requests;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_queue.empty()) return;
        requests.swap(g_queue);
    }
    for (const auto& request : requests) {
        // "woken <object> <command...>": the command runs right before the next cull, with that kind awake (objects
        // away from the player sleep, and the game culls them again before our step).
        std::istringstream in(request->line);
        std::string first, object;
        in >> first >> object;
        if (first == "woken" && !object.empty()) {
            std::string rest;
            std::getline(in, rest);
            sync::WakeThen(localPlayer, Slot(object),
                           [localPlayer, request, rest] { Finish(request, Answer(localPlayer, rest)); });
            continue;
        }
        Finish(request, Answer(localPlayer, request->line));
    }
}

}  // namespace coop::debugport
