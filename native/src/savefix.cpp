#include "savefix.h"

#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "MinHook.h"
#include "log.h"
#include "mutant.h"
#include "yyc.h"

namespace coop::savefix {

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

constexpr uint32_t kScriptLoadGame = 0x00525300;
constexpr double kBufferFixed = 0;
constexpr size_t kChunkTableAt = 10, kChunkEntry = 12;  // "cndr", u16 version, u16 count, u16; then tag4 size4 offset4
constexpr uint8_t kTypeList = 17;
yyc::ScriptFn g_origLoadGame = nullptr;

// ---- the zone format (tools/gametest/cinder_fix.py) ----

struct Key {
    std::string name;
    uint8_t type;
};

size_t FixedSize(uint8_t type) {
    switch (type) {
        case 1: case 2: case 10: return 1;
        case 3: case 4: case 7: return 2;
        case 5: case 6: case 8: return 4;
        case 9: case 12: return 8;
        default: return 0;
    }
}

uint16_t U16(const Bytes& b, size_t at) { return static_cast<uint16_t>(b[at] | (b[at + 1] << 8)); }
uint32_t U32(const Bytes& b, size_t at) { return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (uint32_t(b[at + 3]) << 24); }

class Reader {
public:
    Reader(const Bytes& raw, const std::vector<Key>& keys, size_t at = 0) : raw_(raw), keys_(keys), p_(at) {}
    size_t at() const { return p_; }
    void seek(size_t at) { p_ = at; }
    bool done() const { return p_ >= raw_.size(); }

    bool Key16(uint16_t* key) {
        if (p_ + 2 > raw_.size()) return false;
        *key = U16(raw_, p_);
        p_ += 2;
        return *key < keys_.size();
    }
    bool SkipValue(uint8_t type) {
        if (const size_t n = FixedSize(type)) return Advance(n);
        if (type == 11 || type == 18) {
            while (p_ < raw_.size() && raw_[p_] != 0) ++p_;
            return Advance(1);
        }
        if (type == 16) {
            if (p_ + 3 > raw_.size()) return false;
            const uint8_t element = raw_[p_];
            const uint16_t count = U16(raw_, p_ + 1);
            p_ += 3;
            for (uint16_t i = 0; i < count; ++i) {
                if (!SkipValue(element)) return false;
            }
            return true;
        }
        if (type == kTypeList) {
            if (p_ + 2 > raw_.size()) return false;
            const uint16_t count = U16(raw_, p_);
            p_ += 2;
            for (uint16_t i = 0; i < count; ++i) {
                if (!SkipMap()) return false;
            }
            return true;
        }
        if (type == 19) {
            if (p_ + 2 > raw_.size()) return false;
            const uint16_t length = U16(raw_, p_);
            return Advance(2 + size_t(length));
        }
        return false;
    }
    bool SkipMap() {
        if (p_ >= raw_.size()) return false;
        const uint8_t fields = raw_[p_++];
        for (uint8_t i = 0; i < fields; ++i) {
            uint16_t key = 0;
            if (!Key16(&key) || !SkipValue(keys_[key].type)) return false;
        }
        return true;
    }

private:
    bool Advance(size_t n) {
        if (p_ + n > raw_.size()) return false;
        p_ += n;
        return true;
    }
    const Bytes& raw_;
    const std::vector<Key>& keys_;
    size_t p_;
};

std::vector<Key> ParseKeys(const Bytes& raw) {
    std::vector<Key> keys;
    if (raw.size() < 4) return keys;
    const uint32_t count = U32(raw, 0);
    size_t p = 4;
    for (uint32_t i = 0; i < count && p < raw.size(); ++i) {
        size_t end = p;
        while (end < raw.size() && raw[end] != 0) ++end;
        if (end + 1 >= raw.size()) break;
        keys.push_back({std::string(raw.begin() + p, raw.begin() + end), raw[end + 1]});
        p = end + 2;
    }
    return keys;
}

// True when raw[at..] reads as the zone's next top-level fields, right to its end.
bool RestParses(const Bytes& raw, const std::vector<Key>& keys, size_t at) {
    Reader r(raw, keys, at);
    while (!r.done()) {
        uint16_t key = 0;
        if (!r.Key16(&key) || !r.SkipValue(keys[key].type)) return false;
    }
    return r.at() == raw.size();
}

// The zone record with its lists repaired; `bad` counts dropped and missing records. False if it cannot be read.
bool RepairRecord(const Bytes& raw, const std::vector<Key>& keys, Bytes* out, int* bad, std::string* what) {
    Reader r(raw, keys);
    while (!r.done()) {
        const size_t start = r.at();
        uint16_t key = 0;
        if (!r.Key16(&key)) return false;
        if (keys[key].type != kTypeList) {
            if (!r.SkipValue(keys[key].type)) return false;
            out->insert(out->end(), raw.begin() + start, raw.begin() + r.at());
            continue;
        }
        if (r.at() + 2 > raw.size()) return false;
        const uint16_t count = U16(raw, r.at());
        r.seek(r.at() + 2);
        std::vector<std::pair<size_t, size_t>> kept;
        for (uint16_t i = 0; i < count; ++i) {
            const size_t at = r.at();
            if (!r.SkipMap()) {
                if (!RestParses(raw, keys, at)) return false;
                r.seek(at);
                *bad += count - i;
                *what += " " + keys[key].name + " " + std::to_string(i) + "/" + std::to_string(count);
                break;
            }
            if (raw[at] == 0) {
                ++*bad;
            } else {
                kept.push_back({at, r.at()});
            }
        }
        if (kept.size() != count) *what += " " + keys[key].name + " kept " + std::to_string(kept.size());
        out->push_back(raw[start]);
        out->push_back(raw[start + 1]);
        out->push_back(static_cast<uint8_t>(kept.size() & 0xFF));
        out->push_back(static_cast<uint8_t>(kept.size() >> 8));
        for (const auto& [from, to] : kept) out->insert(out->end(), raw.begin() + from, raw.begin() + to);
    }
    return true;
}

// ---- zlib through the game's buffers ----

yyc::RValue Builtin(yyc::CInstance* self, const char* name, std::vector<yyc::RValue> args) {
    yyc::RValue result{};
    yyc::CallBuiltin(name, self, static_cast<int>(args.size()), args.data(), &result);
    return result;
}

double BuiltinReal(yyc::CInstance* self, const char* name, std::vector<yyc::RValue> args) {
    yyc::RValue r = Builtin(self, name, std::move(args));
    const double value = yyc::ToReal(r);
    yyc::Free(&r);
    return value;
}

uint8_t* Address(yyc::CInstance* self, double buffer) {
    yyc::RValue r = Builtin(self, "buffer_get_address", {yyc::Real(buffer)});
    uint8_t* p = (r.kind & yyc::kKindMask) == yyc::kPtr ? static_cast<uint8_t*>(r.ptr) : nullptr;
    static int told = 0;
    if (p == nullptr && told++ < 3) Log("savefix: buffer_get_address(%g) gave kind %u", buffer, r.kind & yyc::kKindMask);
    yyc::Free(&r);
    return p;
}

// A buffer holding `data`. Bytes put straight into its memory do not count as written (buffer_decompress then sees an
// empty buffer), so they go into a scratch buffer and are buffer_copy'd over, as the game's ChunkLoad does.
double ToBuffer(yyc::CInstance* self, const uint8_t* data, size_t size) {
    const double scratch = BuiltinReal(self, "buffer_create", {yyc::Real(double(size)), yyc::Real(kBufferFixed), yyc::Real(1)});
    uint8_t* p = Address(self, scratch);
    if (p == nullptr) {
        BuiltinReal(self, "buffer_delete", {yyc::Real(scratch)});
        return -1;
    }
    memcpy(p, data, size);
    const double buffer = BuiltinReal(self, "buffer_create", {yyc::Real(double(size)), yyc::Real(kBufferFixed), yyc::Real(1)});
    BuiltinReal(self, "buffer_copy", {yyc::Real(scratch), yyc::Real(0), yyc::Real(double(size)), yyc::Real(buffer), yyc::Real(0)});
    BuiltinReal(self, "buffer_delete", {yyc::Real(scratch)});
    return buffer;
}

bool FromBuffer(yyc::CInstance* self, double buffer, Bytes* out) {
    if (buffer < 0) return false;
    const size_t size = static_cast<size_t>(BuiltinReal(self, "buffer_get_size", {yyc::Real(buffer)}));
    uint8_t* p = Address(self, buffer);
    if (p != nullptr) out->assign(p, p + size);
    BuiltinReal(self, "buffer_delete", {yyc::Real(buffer)});
    return p != nullptr;
}

bool Inflate(yyc::CInstance* self, const uint8_t* data, size_t size, Bytes* out) {
    const double in = ToBuffer(self, data, size);
    if (in < 0) return false;
    const double result = BuiltinReal(self, "buffer_decompress", {yyc::Real(in)});
    if (result < 0) Log("savefix: buffer_decompress of %zu bytes failed (buffer %g)", size, in);
    BuiltinReal(self, "buffer_delete", {yyc::Real(in)});
    return FromBuffer(self, result, out);
}

bool Deflate(yyc::CInstance* self, const Bytes& raw, Bytes* out) {
    const double in = ToBuffer(self, raw.data(), raw.size());
    if (in < 0) return false;
    const double result = BuiltinReal(self, "buffer_compress", {yyc::Real(in), yyc::Real(0), yyc::Real(double(raw.size()))});
    BuiltinReal(self, "buffer_delete", {yyc::Real(in)});
    return FromBuffer(self, result, out);
}

// ---- one zone file ----

void RepairZone(yyc::CInstance* self, const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const Bytes file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (file.size() < kChunkTableAt || memcmp(file.data(), "cndr", 4) != 0) return;
    const uint16_t count = U16(file, 6);
    if (count < 2 || file.size() < kChunkTableAt + kChunkEntry * count) return;
    std::vector<std::pair<std::string, Bytes>> chunks;  // tag, raw
    for (uint16_t i = 0; i < count; ++i) {
        const size_t entry = kChunkTableAt + kChunkEntry * i;
        const uint32_t size = U32(file, entry + 4), offset = U32(file, entry + 8);
        Bytes raw;
        if (size_t(offset) + size > file.size() || !Inflate(self, file.data() + offset, size, &raw)) {
            Log("savefix: %s - chunk %u cannot be read; left as it is", path.filename().u8string().c_str(), i);
            return;
        }
        chunks.push_back({std::string(file.begin() + entry, file.begin() + entry + 4), std::move(raw)});
    }
    const std::vector<Key> keys = ParseKeys(chunks[0].second);
    int bad = 0;
    std::string what;
    for (size_t i = 1; i < chunks.size(); ++i) {
        Bytes fixed;
        if (!RepairRecord(chunks[i].second, keys, &fixed, &bad, &what)) {
            Log("savefix: %s - its record does not read even after repair; left as it is",
                path.filename().u8string().c_str());
            return;
        }
        chunks[i].second = std::move(fixed);
    }
    if (bad == 0) return;
    Bytes out(file.begin(), file.begin() + kChunkTableAt);
    std::vector<Bytes> packed;
    for (const auto& [tag, raw] : chunks) {
        Bytes comp;
        if (!Deflate(self, raw, &comp)) {
            Log("savefix: %s - cannot compress the repair; left as it is", path.filename().u8string().c_str());
            return;
        }
        packed.push_back(std::move(comp));
    }
    uint32_t offset = static_cast<uint32_t>(kChunkTableAt + kChunkEntry * chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        out.insert(out.end(), chunks[i].first.begin(), chunks[i].first.end());
        const uint32_t size = static_cast<uint32_t>(packed[i].size());
        for (int s = 0; s < 4; ++s) out.push_back(static_cast<uint8_t>(size >> (8 * s)));
        for (int s = 0; s < 4; ++s) out.push_back(static_cast<uint8_t>(offset >> (8 * s)));
        offset += size;
    }
    for (const Bytes& comp : packed) out.insert(out.end(), comp.begin(), comp.end());
    std::error_code error;
    fs::copy_file(path, fs::path(path).concat(L".broken"), fs::copy_options::overwrite_existing, error);
    std::ofstream(path, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(out.data()),
                                                                 static_cast<std::streamsize>(out.size()));
    Log("savefix: %s had %d bad records (%s) - repaired before loading; the broken one is kept as .broken",
        path.filename().u8string().c_str(), bad, what.c_str());
}

void RepairSlot(yyc::CInstance* self) {
    const yyc::RValue* slot = yyc::GlobalVar(yyc::slot::saveFile);
    if (slot == nullptr || !yyc::IsNumber(*slot)) return;
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    const fs::path zones = fs::path(local) / L"Forager" / (L"saveData" + std::to_wstring(int(yyc::ToReal(*slot)))) / L"Zones";
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(zones, error)) {
        if (entry.path().extension() == L".cin") RepairZone(self, entry.path());
    }
}

yyc::RValue* __cdecl LoadGameHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                  yyc::RValue** args) {
    if (!Mutant("savefix")) {
        __try {
            RepairSlot(self);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("savefix: checking the save crashed (code 0x%08lX) - loading it as it is", GetExceptionCode());
        }
    }
    return g_origLoadGame(self, other, result, argc, args);
}

}  // namespace

bool InstallHooks() {
    const bool ok = MH_CreateHook(yyc::At<void*>(kScriptLoadGame), reinterpret_cast<void*>(&LoadGameHook),
                                  reinterpret_cast<void**>(&g_origLoadGame)) == MH_OK;
    if (!ok) Log("savefix: hook LoadGame failed");
    return ok;
}

}  // namespace coop::savefix
