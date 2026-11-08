#include "signs.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
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
// Localize(key): the game's text for `key` in the language being played (decompiled/code/index.tsv).
constexpr uint32_t kScriptLocalize = 0x006A7D60;

#pragma pack(push, 1)
struct SignHeader {
    int32_t hostId;  // the host's id of the sign (-1 if the sender has none bound yet: then by its spot)
    float x, y;
    uint8_t edited;
    uint8_t kind;     // SignKind: what the text below is
    uint16_t length;  // bytes of text following
};
#pragma pack(pop)
enum SignKind : uint8_t {
    kText = 0,    // the words themselves (a message a player wrote)
    kNumber = 1,  // `msg` is a number, written out
    kKey = 2,     // a key of the game's texts: each player says it in their own language
};

std::map<int, std::string> g_said;  // local sign id -> the text last sent or taken
unsigned g_steps = 0;

// ---- The game's texts in every language (local/<language>.json beside Forager.exe) ----
// A sign made by the game says Localize(key) in the language of the player who made it (the island's buyer), and that
// text is what it keeps (in the save too): another player in another language read the maker's. A sign's text that
// is one of the game's, in any language, is told by its key and said in each player's own.

// The same text whatever its line breaks: the json's '*' is a new line in the game; spaces runs count as one.
std::string Normalized(const std::string& text) {
    std::string out;
    bool space = false;
    for (const char c : text) {
        const bool blank = c == '*' || c == '\n' || c == '\r' || c == '\t' || c == ' ';
        if (blank) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return out;
}

void AppendUtf8(std::string* out, unsigned code) {
    if (code < 0x80) {
        *out += static_cast<char>(code);
    } else if (code < 0x800) {
        *out += static_cast<char>(0xC0 | (code >> 6));
        *out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        *out += static_cast<char>(0xE0 | (code >> 12));
        *out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// A json string starting at text[*at] (the opening quote); false if it is not one.
bool ReadJsonString(const std::string& text, size_t* at, std::string* out) {
    if (*at >= text.size() || text[*at] != '"') return false;
    out->clear();
    for (size_t i = *at + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"') {
            *at = i + 1;
            return true;
        }
        if (c != '\\' || i + 1 >= text.size()) {
            *out += c;
            continue;
        }
        const char e = text[++i];
        if (e == 'n') *out += '\n';
        else if (e == 't') *out += '\t';
        else if (e == 'r') *out += '\r';
        else if (e == 'u' && i + 4 < text.size()) {
            AppendUtf8(out, static_cast<unsigned>(std::strtoul(text.substr(i + 1, 4).c_str(), nullptr, 16)));
            i += 4;
        } else {
            *out += e;  // \" \\ \/
        }
    }
    return false;
}

// Normalized text -> key, from every language's file; read once.
const std::unordered_map<std::string, std::string>& TextKeys() {
    static std::unordered_map<std::string, std::string> keys;
    static bool read = false;
    if (read) return keys;
    read = true;
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0) return keys;
    std::error_code error;
    const std::filesystem::path folder = std::filesystem::path(exe).parent_path() / L"local";
    for (const auto& entry : std::filesystem::directory_iterator(folder, error)) {
        if (entry.path().extension() != L".json") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        size_t at = 0;
        std::string key, value;
        while ((at = text.find('"', at)) != std::string::npos) {
            if (!ReadJsonString(text, &at, &key)) break;
            const size_t colon = text.find_first_not_of(" \t\r\n", at);
            if (colon == std::string::npos || text[colon] != ':') continue;  // a value, not a key: go on
            at = text.find('"', colon);
            if (at == std::string::npos || !ReadJsonString(text, &at, &value)) break;
            const std::string normal = Normalized(value);
            if (normal.empty()) continue;
            // Signposts' keys first: a sign's text that is also some other key's is the sign's.
            const auto known = keys.find(normal);
            if (known == keys.end() || (key.rfind("signpost", 0) == 0 && known->second.rfind("signpost", 0) != 0)) {
                keys[normal] = key;
            }
        }
    }
    Log("signs: %zu of the game's texts known (every language)", keys.size());
    return keys;
}

std::string KeyOf(const std::string& text) {
    const auto& keys = TextKeys();
    const auto found = keys.find(Normalized(text));
    return found != keys.end() ? found->second : std::string();
}

// The game's text for `key` in the language played here; empty if it has none.
std::string Localize(yyc::CInstance* self, const std::string& key) {
    yyc::RValue arg = yyc::String(key.c_str());
    yyc::RValue* args[1] = {&arg};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kScriptLocalize)(self, self, &result, 1, args);
    std::string text = result.kind == yyc::kString ? yyc::ToString(result) : std::string();
    yyc::Free(&result);
    yyc::Free(&arg);
    return text;
}

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

void WriteText(int id, const std::string& text) {
    yyc::RValue value = yyc::String(text.c_str());
    yyc::SetVar(id, kSlotMsg, &value);
    yyc::Free(&value);
}

double ReadEdited(int id) {
    yyc::RValue value{};
    const double edited = yyc::GetVar(id, kSlotEdited, &value) && yyc::IsNumber(value) ? yyc::ToReal(value) : 0;
    yyc::Free(&value);
    return edited;
}

void SendText(int id, yyc::CInstance* sign, const std::string& text, SignKind kind) {
    std::vector<uint8_t> payload(sizeof(SignHeader) + text.size());
    const SignHeader header{sync::HostIdOf(id), static_cast<float>(yyc::GetBuiltinReal(sign, yyc::va::YYVar_x)),
                            static_cast<float>(yyc::GetBuiltinReal(sign, yyc::va::YYVar_y)),
                            static_cast<uint8_t>(ReadEdited(id) != 0 ? 1 : 0), kind, static_cast<uint16_t>(text.size())};
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
        const std::string key = number ? std::string() : KeyOf(text);
        if (!key.empty()) {
            // One of the game's texts, maybe in another player's language (from the save, or made by them): ours.
            const std::string ours = Localize(localPlayer, key);
            if (!ours.empty() && ours != text) {
                WriteText(id, ours);
                Log("signs: sign %d said the game's \"%s\" in another language - now in ours", id, key.c_str());
                text = ours;
            }
        }
        const auto said = g_said.find(id);
        const bool changed = said == g_said.end() || said->second != text;
        if (!changed && !resend) continue;
        g_said[id] = text;
        if (!key.empty()) {
            SendText(id, sign, key, kKey);
        } else {
            SendText(id, sign, text, number ? kNumber : kText);
        }
        if (changed) Log("signs: sign %d at (%.0f, %.0f) says \"%s\" - sent", id, yyc::GetBuiltinReal(sign, yyc::va::YYVar_x),
                         yyc::GetBuiltinReal(sign, yyc::va::YYVar_y), key.empty() ? text.c_str() : key.c_str());
    }
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kSignText) return false;
    SignHeader header{};
    if (message.payload.size() < sizeof(header)) return true;
    memcpy(&header, message.payload.data(), sizeof(header));
    if (!std::isfinite(header.x) || !std::isfinite(header.y) || header.length > kMaxText ||
        message.payload.size() != sizeof(header) + header.length || header.kind > kKey) {
        return true;
    }
    std::string text(reinterpret_cast<const char*>(message.payload.data()) + sizeof(header), header.length);
    if (text.empty() || text.find('\0') != std::string::npos) return true;
    if (header.kind == kKey) {
        text = Localize(localPlayer, text);  // in our own language
        if (text.empty()) return true;
    }
    const int local = sync::Resolve(localPlayer, header.hostId, kObjSignpost, header.x, header.y);
    if (local == 0) return true;  // not here (asleep far away): the host tells it again later
    std::string current;
    if (header.kind != kNumber && ReadText(local, &current) && current == text) {
        g_said[local] = text;
        return true;
    }
    g_said[local] = text;  // taken, not news: not sent back
    if (header.kind == kNumber) {
        char* end = nullptr;
        const double n = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || !std::isfinite(n)) return true;
        yyc::RValue value = yyc::Real(n);
        yyc::SetVar(local, kSlotMsg, &value);
    } else {
        WriteText(local, text);
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
