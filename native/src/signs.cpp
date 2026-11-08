#include "signs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "log.h"
#include "sync.h"

namespace coop::signs {

namespace {

constexpr uint8_t kSignText = 109;  // either way (the host passes a guest's on): SignHeader, then the text
constexpr int kObjSignpost = 154;   // decompiled/data/objects.tsv
constexpr int kSlotMsg = 0x679;     // objSignpost.msg: what it says (var_slots.tsv)
constexpr int kSlotEdited = 0x1CD;  // objSignpost.edited: a player's sign written on (E then reads, not writes)
constexpr unsigned kEverySteps = 30;         // looked at twice a second
constexpr unsigned kResendEverySteps = 600;  // the host tells every sign's text again every ~10 s
constexpr size_t kMaxText = 512;

#pragma pack(push, 1)
struct SignHeader {
    int32_t hostId;  // the host's id of the sign (-1 if the sender has none bound yet: then by its spot)
    float x, y;
    uint8_t edited;
    uint8_t number;   // `msg` is a number (the text below is it written out), not a string
    uint16_t length;  // bytes of text following
};
#pragma pack(pop)

std::map<int, std::string> g_said;  // local sign id -> the text last sent or taken
unsigned g_steps = 0;

// `msg` as text; `number` set when the sign holds a number there instead of a string (written out as text).
bool ReadText(int id, std::string* text, bool* number = nullptr) {
    yyc::RValue value{};
    bool ok = yyc::GetVar(id, kSlotMsg, &value);
    if (ok && value.kind == yyc::kString) {
        *text = yyc::ToString(value);
        if (number != nullptr) *number = false;
    } else if (ok && yyc::IsNumber(value)) {
        char written[32] = {};
        snprintf(written, sizeof(written), "%.17g", yyc::ToReal(value));
        *text = written;
        if (number != nullptr) *number = true;
    } else {
        ok = false;
    }
    yyc::Free(&value);
    return ok;
}

double ReadEdited(int id) {
    yyc::RValue value{};
    const double edited = yyc::GetVar(id, kSlotEdited, &value) && yyc::IsNumber(value) ? yyc::ToReal(value) : 0;
    yyc::Free(&value);
    return edited;
}

void SendText(int id, yyc::CInstance* sign, const std::string& text, bool number) {
    std::vector<uint8_t> payload(sizeof(SignHeader) + text.size());
    const SignHeader header{sync::HostIdOf(id), static_cast<float>(yyc::GetBuiltinReal(sign, yyc::va::YYVar_x)),
                            static_cast<float>(yyc::GetBuiltinReal(sign, yyc::va::YYVar_y)),
                            static_cast<uint8_t>(ReadEdited(id) != 0 ? 1 : 0), static_cast<uint8_t>(number ? 1 : 0),
                            static_cast<uint16_t>(text.size())};
    memcpy(payload.data(), &header, sizeof(header));
    memcpy(payload.data() + sizeof(header), text.data(), text.size());
    net::Send(kSignText, payload.data(), payload.size());
}

}  // namespace

void Tick(yyc::CInstance* localPlayer, bool isHost) {
    if (!net::IsConnected() || ++g_steps % kEverySteps != 0) return;
    const bool resend = isHost && g_steps % kResendEverySteps == 0;
    const int count = static_cast<int>(yyc::CallReal(localPlayer, "instance_number", {double(kObjSignpost)}));
    for (int i = 0; i < count; ++i) {
        const int id = static_cast<int>(yyc::CallReal(localPlayer, "instance_find", {double(kObjSignpost), double(i)}));
        yyc::CInstance* sign = yyc::FindInstance(id);
        std::string text;
        bool number = false;
        if (sign == nullptr || !ReadText(id, &text, &number) || text.empty() || text.size() > kMaxText) continue;
        const auto said = g_said.find(id);
        const bool changed = said == g_said.end() || said->second != text;
        if (!changed && !resend) continue;
        g_said[id] = text;
        SendText(id, sign, text, number);
        if (changed) Log("signs: sign %d at (%.0f, %.0f) says \"%s\" - sent", id, yyc::GetBuiltinReal(sign, yyc::va::YYVar_x),
                         yyc::GetBuiltinReal(sign, yyc::va::YYVar_y), text.c_str());
    }
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kSignText) return false;
    SignHeader header{};
    if (message.payload.size() < sizeof(header)) return true;
    memcpy(&header, message.payload.data(), sizeof(header));
    if (!std::isfinite(header.x) || !std::isfinite(header.y) || header.length > kMaxText ||
        message.payload.size() != sizeof(header) + header.length) {
        return true;
    }
    const std::string text(reinterpret_cast<const char*>(message.payload.data()) + sizeof(header), header.length);
    if (text.empty() || text.find('\0') != std::string::npos) return true;
    const int local = sync::Resolve(localPlayer, header.hostId, kObjSignpost, header.x, header.y);
    if (local == 0) return true;  // not here (asleep far away): the host tells it again later
    std::string current;
    if (ReadText(local, &current) && current == text) {
        g_said[local] = text;
        return true;
    }
    g_said[local] = text;  // taken, not news: not sent back
    if (header.number) {
        char* end = nullptr;
        const double n = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || !std::isfinite(n)) return true;
        yyc::RValue value = yyc::Real(n);
        yyc::SetVar(local, kSlotMsg, &value);
    } else {
        yyc::RValue value = yyc::String(text.c_str());
        yyc::SetVar(local, kSlotMsg, &value);
        yyc::Free(&value);
    }
    if (header.edited) {
        yyc::RValue edited = yyc::Real(1);
        yyc::SetVar(local, kSlotEdited, &edited);
    }
    Log("signs: sign %d at (%.0f, %.0f) now says \"%s\" as at the other player", local, header.x, header.y, text.c_str());
    return true;
}

void Reset() {
    g_said.clear();
    g_steps = 0;
}

}  // namespace coop::signs
