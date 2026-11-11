#include "assets.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

#include "log.h"

namespace coop::assets {

namespace {

// GML constants (the runner's): buffer_s16, audio_mono, audio_stereo.
constexpr double kBufferS16 = 4;
constexpr double kAudioMono = 0, kAudioStereo = 1;

// Writes `data` as %LOCALAPPDATA%\Forager\<file>; false if it could not.
bool WriteNextToSaves(const unsigned char* data, size_t size, const char* file) {
    wchar_t local[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    const std::wstring path = std::wstring(local) + L"\\Forager\\" + std::wstring(file, file + strlen(file));
    std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!out) Log("assets: cannot write %s", file);
    return static_cast<bool>(out);
}

uint32_t Read32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
uint16_t Read16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

// Where a WAV's samples are, and what they are.
struct WavData {
    uint32_t offset = 0, length = 0;
    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0;
};

// Walks the RIFF chunks ("fmt " and "data"; others - LIST... - are skipped). False if it is not a WAV we can play.
bool ParseWav(const unsigned char* wav, size_t size, WavData* out) {
    if (size < 12 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0) return false;
    bool haveFormat = false;
    for (size_t at = 12; at + 8 <= size;) {
        const uint32_t chunk = Read32(wav + at + 4);
        const size_t body = at + 8;
        if (chunk > size - body) return false;
        if (memcmp(wav + at, "fmt ", 4) == 0 && chunk >= 16) {
            const uint16_t format = Read16(wav + body);
            out->channels = Read16(wav + body + 2);
            out->rate = Read32(wav + body + 4);
            out->bits = Read16(wav + body + 14);
            haveFormat = format == 1;  // PCM
        } else if (memcmp(wav + at, "data", 4) == 0) {
            out->offset = static_cast<uint32_t>(body);
            out->length = chunk;
            return haveFormat && out->bits == 16 && (out->channels == 1 || out->channels == 2) && out->rate > 0;
        }
        at = body + chunk + (chunk & 1);  // chunks are word-aligned
    }
    return false;
}

}  // namespace

int AddSprite(yyc::CInstance* self, const unsigned char* png, size_t size, const char* file, int frames, double ox,
              double oy) {
    if (!WriteNextToSaves(png, size, file)) return -1;
    yyc::RValue args[6] = {yyc::String(file), yyc::Real(frames), yyc::Real(0), yyc::Real(0), yyc::Real(ox),
                           yyc::Real(oy)};
    yyc::RValue result{};
    yyc::CallBuiltin("sprite_add", self, 6, args, &result);
    const int sprite = static_cast<int>(yyc::ToReal(result));
    yyc::Free(&result);
    yyc::Free(&args[0]);
    Log("assets: sprite %s %s (%d)", file, sprite >= 0 ? "loaded" : "NOT loaded", sprite);
    return sprite >= 0 ? sprite : -1;
}

int AddSound(yyc::CInstance* self, const unsigned char* wav, size_t size, const char* file) {
    WavData data;
    if (!ParseWav(wav, size, &data)) {
        Log("assets: %s is not a 16-bit PCM WAV", file);
        return -1;
    }
    if (!WriteNextToSaves(wav, size, file)) return -1;
    // The buffer stays: the sound plays from it (audio_create_buffer_sound does not copy it).
    yyc::RValue name = yyc::String(file);
    yyc::RValue buffer{};
    yyc::CallBuiltin("buffer_load", self, 1, &name, &buffer);
    yyc::Free(&name);
    const double bufferId = yyc::ToReal(buffer);
    yyc::Free(&buffer);
    if (bufferId < 0) {
        Log("assets: buffer_load %s failed", file);
        return -1;
    }
    const double channels = data.channels == 1 ? kAudioMono : kAudioStereo;
    const int sound = static_cast<int>(yyc::CallReal(self, "audio_create_buffer_sound",
                                                     {bufferId, kBufferS16, double(data.rate), double(data.offset),
                                                      double(data.length), channels}));
    Log("assets: sound %s %s (%d; %u Hz, %u ch, %u bytes)", file, sound >= 0 ? "loaded" : "NOT loaded", sound,
        data.rate, data.channels, data.length);
    return sound >= 0 ? sound : -1;
}

}  // namespace coop::assets
