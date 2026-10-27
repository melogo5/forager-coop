// Startup splashes: rmHumbleLogo (objHumbleBundleLogo) and rmHopFrogLogo (objHopFrogLogo). With the option on,
// each logo's Create sends the game straight on to rmInputSelect - where objHopFrogLogo goes itself on Windows
// once it is done. The logos only draw, play their jingle and prefetch their textures; their Room End / CleanUp
// still run, so the jingle is stopped and the textures flushed as usual.
#include "intro.h"

#include <windows.h>

#include <cwchar>

#include "MinHook.h"
#include "log.h"
#include "yyc.h"

namespace coop {

namespace {

constexpr uint32_t kHumbleLogoCreate = 0x00EBBF90;  // gml_Object_objHumbleBundleLogo_Create_0
constexpr uint32_t kHopFrogLogoCreate = 0x00EB3BB0;  // gml_Object_objHopFrogLogo_Create_0
constexpr int kRoomInputSelect = 5;                  // rmInputSelect, the room after the logos
constexpr wchar_t kOptionFile[] = L"coop_skip_intro.txt";

yyc::EventFn g_origHumbleCreate = nullptr;
yyc::EventFn g_origHopFrogCreate = nullptr;

bool OptionFileExists() {
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (len == 0 || len >= MAX_PATH || slash == nullptr) return false;
    if ((slash - path) + 1 + wcslen(kOptionFile) >= MAX_PATH) return false;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), kOptionFile);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

void SkipLogo(yyc::CInstance* self, const char* which) {
    yyc::RValue target = yyc::Real(kRoomInputSelect);
    yyc::RValue result{};
    yyc::CallBuiltin("room_goto", self, 1, &target, &result);
    yyc::Free(&result);
    Log("intro: %s logo skipped", which);
}

void __cdecl HumbleCreateHook(yyc::CInstance* self, yyc::CInstance* other) {
    g_origHumbleCreate(self, other);
    SkipLogo(self, "Humble Bundle");
}

void __cdecl HopFrogCreateHook(yyc::CInstance* self, yyc::CInstance* other) {
    g_origHopFrogCreate(self, other);
    SkipLogo(self, "HopFrog");
}

bool Hook(uint32_t va, void* detour, void* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original));
    if (status != MH_OK) Log("intro: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

}  // namespace

bool IntroInstall() {
    if (!OptionFileExists() && GetEnvironmentVariableA("COOP_SKIP_INTRO", nullptr, 0) == 0) return true;
    Log("intro: the startup logos are skipped");
    bool ok = Hook(kHumbleLogoCreate, reinterpret_cast<void*>(&HumbleCreateHook), &g_origHumbleCreate);
    ok = Hook(kHopFrogLogoCreate, reinterpret_cast<void*>(&HopFrogCreateHook), &g_origHopFrogCreate) && ok;
    return ok;
}

}  // namespace coop
