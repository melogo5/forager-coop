#include "emotes.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "MinHook.h"
#include "assets.h"
#include "bombs.h"
#include "bonk_wav.h"
#include "dance_wav.h"
#include "commands.h"
#include "coop_fx_png.h"
#include "gui.h"
#include "log.h"
#include "puppet.h"

namespace coop::emotes {

namespace {

constexpr uint8_t kPlayerFx = 114;  // PlayerFxMsg: guest -> host (an act), host -> each player (what it does to them)
constexpr uint8_t kFxBonk = 1;      // a pickaxe on the head
constexpr uint8_t kFxHurt = 2;      // a blast of the actor's bomb: `amount` is DmgPlayer's argument
constexpr uint8_t kYou = 0xFE;      // host -> guest: the victim is the one receiving it
constexpr uint8_t kFlagDancing = 1;  // PlayerStateMsg.flags
constexpr float kMaxHurt = 5;        // hearts: a bomb's blast is 1; a broken or forged message takes no more

#pragma pack(push, 1)
struct PlayerFxMsg {
    uint8_t kind;
    uint8_t victim;  // net::PeerId, or kYou
    float amount;
};
#pragma pack(pop)

// decompiled/code/index.tsv
constexpr uint32_t kScriptUsePickaxe = 0x006DAE40;  // the local player's pickaxe swing (UseItem -> ScriptCall)
constexpr uint32_t kScriptDmgPlayer = 0x00727030;   // DmgPlayer(amount): hurts PlayerFind()
// /dance borrows imguigml_new_line: the GML library imguigml ships unused (commands.h). Its place in the SCPT list.
constexpr uint32_t kDanceScript = 0x010B7650;
constexpr double kDanceIndex = 960;
constexpr unsigned kRegisterEverySteps = 120;
// objPlayer's own squash (its Draw_0 scales the body by it; a hit's swing sets it, Step_2 eases it back to 1).
constexpr int kSlotSquashX = 0x913, kSlotSquashY = 0x91C;  // decompiled/code/var_slots.tsv

// The bonk: where the pickaxe has to land, and how long it lasts.
constexpr double kBonkReach = 40;      // room px from the swinging player to the other one
constexpr double kBonkAim = 12;        // room px from the mouse to the other player's body
constexpr double kBodyHeight = 18;     // the player's sprite stands on its origin (its feet), ~20 px tall
constexpr DWORD kBonkCooldownMs = 350;  // one bonk per swing, even if UsePickaxe runs for a few steps
constexpr DWORD kSquashInMs = 60, kSquashHoldMs = 450, kSquashOutMs = 800, kStarsMs = 1300;
constexpr double kFlatX = 1.45, kFlatY = 0.45;
constexpr int kStars = 3;
constexpr double kSoundPriority = 10;
constexpr double kMusicGain = 0.6;  // the dance's music loop (native/assets/dance.wav): square waves sound loud
// The player's volume settings: global.options (a ds_list; DeserializeConfigMap gives audiogroup_default its [3]
// and audiogroup_music its [4]). Our sounds are made from buffers, in no audio group: their gain is set from these.
constexpr int kSlotOptions = 0x6BD;
constexpr double kOptionSounds = 3, kOptionMusic = 4;
constexpr unsigned kMusicGainEverySteps = 30;  // the settings changed while the music plays

// The dance: a hop per beat - stretched in the air, squashed on landing - turning and swaying to the other side
// each beat, a note rising from every beat.
constexpr DWORD kBeatMs = 350;
constexpr double kHopHeight = 7;     // room px at the top of a hop
constexpr double kSway = 3;          // room px sideways at the top of a hop
constexpr double kStretch = 0.25;    // taller and narrower in the air
constexpr double kLandSquash = 0.35;  // flatter and wider just after landing
constexpr double kLandPart = 0.2;     // of a beat: the landing squash fades over it
constexpr int kNotesShown = 4;
constexpr double kNoteRise = 0.015;  // room px per ms
// The dance ends when the player means to walk: a movement key or the left stick (moved by something else - a
// knockback, a push - it dances on).
constexpr int kWalkKeys[] = {'W', 'A', 'S', 'D', 0x25, 0x26, 0x27, 0x28};  // and vk_left, vk_up, vk_right, vk_down
constexpr double kGpAxisLh = 32785, kGpAxisLv = 32786;                     // gp_axislh, gp_axislv
constexpr double kStickToWalk = 0.5;

constexpr int kFrameStar = 0, kFrameNote = 1;
constexpr double kFxScale = 0.8;
constexpr double kFxOrigin = 4.5;  // the frames are 9x9, drawn from their centre

bool (*g_isHost)() = nullptr;
yyc::ScriptFn g_origUsePickaxe = nullptr;
yyc::ScriptFn g_origDance = nullptr;

int g_fxSprite = -1;   // -1 not loaded yet, -2 could not be
int g_bonkSound = -1;  // the same
int g_danceSound = -1;  // the same
double g_musicVoice = -1;  // the dance music playing (audio_play_sound's instance), -1 none
unsigned g_steps = 0;

// Per player shown (puppet::kLocalPlayer or a net::PeerId): since when.
std::map<int, DWORD> g_bonkedAt;
std::map<int, DWORD> g_dancingSince;
std::map<int, DWORD> g_lastBonked;   // the local player's last bonk on that player (the cooldown)
std::set<net::PeerId> g_knownPeers;  // the players we have puppets of

// What the draw callbacks changed on an objPlayer, to put back after its Draw_0.
struct Saved {
    double squashX, squashY, x, y;
};
std::map<int, Saved> g_saved;

double ReadReal(int id, int slot) {
    yyc::RValue value{};
    yyc::GetVar(id, slot, &value);
    const double result = yyc::ToReal(value);
    yyc::Free(&value);
    return result;
}

void WriteReal(int id, int slot, double value) {
    yyc::RValue v = yyc::Real(value);
    yyc::SetVar(id, slot, &v);
}

int IdOf(yyc::CInstance* instance) { return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)); }

double Lerp(double a, double b, double k) { return a + (b - a) * k; }

// The player's volume setting `option` (0..1); 1 if the settings cannot be read.
double Volume(yyc::CInstance* self, double option) {
    const yyc::RValue* list = yyc::GlobalVar(kSlotOptions);
    if (list == nullptr || !yyc::IsNumber(*list)) return 1;
    const double id = yyc::ToReal(*list);
    if (yyc::CallReal(self, "ds_exists", {id, yyc::kDsTypeList}) == 0) return 1;
    if (yyc::CallReal(self, "ds_list_size", {id}) <= option) return 1;
    const double volume = yyc::CallReal(self, "ds_list_find_value", {id, option});
    return std::isfinite(volume) ? (std::min)(1.0, (std::max)(0.0, volume)) : 1;
}

void PlaySound(yyc::CInstance* self, int sound) {
    if (sound < 0) return;
    const double voice = yyc::CallReal(self, "audio_play_sound", {double(sound), kSoundPriority, 0});
    yyc::CallReal(self, "audio_sound_gain", {voice, Volume(self, kOptionSounds), 0});
}

// --- bonk ---------------------------------------------------------------------------------------------------------

// Shows player `who` bonked (here: the stars and the flattened body come with the draw callbacks).
void ShowBonk(yyc::CInstance* self, int who) {
    g_bonkedAt[who] = GetTickCount();
    PlaySound(self, g_bonkSound);
}

// Host: tells each guest what `actor`'s act does to them - the victim gets kYou, the others who it was (a hurt goes
// to the victim only). The actor has shown its own act already; what the host itself sees is up to the caller.
void Route(const PlayerFxMsg& act, net::PeerId actor) {
    for (const net::PeerId peer : net::Peers()) {
        if (peer == actor) continue;
        if (act.kind == kFxHurt && peer != act.victim) continue;
        PlayerFxMsg msg = act;
        if (peer == act.victim) msg.victim = kYou;
        net::SendTo(peer, kPlayerFx, &msg, sizeof(msg));
    }
}

void HurtLocal(yyc::CInstance* localPlayer, float amount) {
    yyc::RValue arg = yyc::Real(amount);
    yyc::RValue* args[1] = {&arg};
    yyc::RValue result{};
    yyc::At<yyc::ScriptFn>(kScriptDmgPlayer)(localPlayer, localPlayer, &result, 1, args);
    yyc::Free(&result);
    Log("emotes: another player's bomb hurt us (%.1f)", amount);
}

// What an act does here: to the local player (kYou) or shown on that player's puppet.
void Apply(yyc::CInstance* localPlayer, const PlayerFxMsg& msg) {
    const bool me = msg.victim == kYou;
    if (msg.kind == kFxBonk) {
        ShowBonk(localPlayer, me ? puppet::kLocalPlayer : msg.victim);
        if (me) Log("emotes: bonked by another player");
    } else if (msg.kind == kFxHurt && me && std::isfinite(msg.amount) && msg.amount > 0) {
        HurtLocal(localPlayer, (std::min)(msg.amount, kMaxHurt));
    }
}

// The local player did `act` to `victim`: shown here at once, and sent (a guest's goes through the host).
void Act(uint8_t kind, net::PeerId victim, float amount) {
    const PlayerFxMsg act{kind, victim, amount};
    if (g_isHost != nullptr && g_isHost()) {
        Route(act, net::kHostPeer);
    } else {
        net::Send(kPlayerFx, &act, sizeof(act));
    }
}

// The swing's target: another player whose body is under the mouse, within the pickaxe's reach.
void TryBonk(yyc::CInstance* player) {
    const double px = yyc::GetBuiltinReal(player, yyc::va::YYVar_x);
    const double py = yyc::GetBuiltinReal(player, yyc::va::YYVar_y);
    const double mx = yyc::GetBuiltinReal(player, yyc::va::YYVar_mouse_x);
    const double my = yyc::GetBuiltinReal(player, yyc::va::YYVar_mouse_y);
    const DWORD now = GetTickCount();
    for (const net::PeerId peer : g_knownPeers) {
        yyc::CInstance* other = puppet::Instance(peer);
        if (other == nullptr) continue;
        const double ox = yyc::GetBuiltinReal(other, yyc::va::YYVar_x);
        const double oy = yyc::GetBuiltinReal(other, yyc::va::YYVar_y);
        const double reach = std::hypot(ox - px, oy - py);
        const double aim = std::hypot(mx - ox, my - (oy - kBodyHeight / 2));
        if (reach > kBonkReach || aim > kBonkAim) continue;
        const auto last = g_lastBonked.find(peer);
        if (last != g_lastBonked.end() && now - last->second < kBonkCooldownMs) return;
        g_lastBonked[peer] = now;
        ShowBonk(player, peer);
        Act(kFxBonk, peer, 0);
        Log("emotes: bonk on player %u", peer);
        return;
    }
}

yyc::RValue* __cdecl UsePickaxeHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                                    yyc::RValue** args) {
    yyc::RValue* r = g_origUsePickaxe(self, other, result, argc, args);
    if (self == nullptr || !net::IsConnected() || g_knownPeers.empty() || puppet::IsActing() ||
        puppet::IsPuppet(self) ||
        static_cast<int>(yyc::GetBuiltinReal(self, yyc::va::YYVar_object_index)) != yyc::obj::player) {
        return r;
    }
    TryBonk(self);
    return r;
}

// Friendly fire: a blast of our bomb reached another player's puppet (bombs.cpp says whose blast it is).
void OnPuppetHurt(net::PeerId player, int argc, yyc::RValue** args) {
    if (!bombs::InOwnBlastStep() || !net::IsConnected()) return;
    const float amount = argc > 0 && args != nullptr && args[0] != nullptr && yyc::IsNumber(*args[0])
                             ? static_cast<float>(yyc::ToReal(*args[0]))
                             : 1.f;
    Log("emotes: our bomb hurt player %u (%.1f)", player, amount);
    Act(kFxHurt, player, amount);
}

// --- dance --------------------------------------------------------------------------------------------------------

void SetDancing(yyc::CInstance* player, bool on) {
    if (on == (g_dancingSince.count(puppet::kLocalPlayer) > 0)) return;
    if (on) {
        g_dancingSince[puppet::kLocalPlayer] = GetTickCount();
    } else {
        g_dancingSince.erase(puppet::kLocalPlayer);
    }
    Log("emotes: dance %s", on ? "on" : "off");
}

yyc::RValue* __cdecl DanceHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                               yyc::RValue** args) {
    yyc::CInstance* player = yyc::FindInstance(
        static_cast<int>(yyc::CallReal(self, "instance_find", {double(yyc::obj::player), 0})));
    if (player != nullptr && puppet::IsPuppet(player)) {
        // instance_find may give a puppet first: the local player is the one that is none.
        const int count = static_cast<int>(yyc::CallReal(self, "instance_number", {double(yyc::obj::player)}));
        player = nullptr;
        for (int i = 0; i < count && player == nullptr; ++i) {
            yyc::CInstance* candidate = yyc::FindInstance(
                static_cast<int>(yyc::CallReal(self, "instance_find", {double(yyc::obj::player), double(i)})));
            if (candidate != nullptr && !puppet::IsPuppet(candidate)) player = candidate;
        }
    }
    if (player != nullptr) SetDancing(player, g_dancingSince.count(puppet::kLocalPlayer) == 0);
    return result;
}

bool WantsToWalk(yyc::CInstance* player) {
    for (const int key : kWalkKeys) {
        if (yyc::CallReal(player, "keyboard_check", {double(key)}) != 0) return true;
    }
    const double h = yyc::CallReal(player, "gamepad_axis_value", {0, kGpAxisLh});
    const double v = yyc::CallReal(player, "gamepad_axis_value", {0, kGpAxisLv});
    return std::hypot(h, v) > kStickToWalk;
}

// --- drawing ------------------------------------------------------------------------------------------------------

// How an objPlayer is drawn: its squash (a negative squashX mirrors it: the body is drawn with it as its xscale)
// and how far from its spot (dy < 0: up).
struct Pose {
    double squashX = 1, squashY = 1;
    double dx = 0, dy = 0;
};

// The bonk's squash at `t` ms: flat at once, held, then springing back.
bool BonkPose(DWORD t, Pose* pose) {
    if (t >= kSquashOutMs) return false;
    if (t < kSquashInMs) {
        const double k = double(t) / kSquashInMs;
        pose->squashX = Lerp(1, kFlatX, k);
        pose->squashY = Lerp(1, kFlatY, k);
    } else if (t < kSquashHoldMs) {
        pose->squashX = kFlatX;
        pose->squashY = kFlatY;
    } else {
        const double k = double(t - kSquashHoldMs) / (kSquashOutMs - kSquashHoldMs);
        const double spring = (1 - k) * (1 - k) * std::cos(3 * 3.14159265 * k);
        pose->squashX = 1 + (kFlatX - 1) * spring;
        pose->squashY = 1 + (kFlatY - 1) * spring;
    }
    return true;
}

// The dance at `t` ms: a hop each beat, facing and swaying to one side, then the other.
Pose DancePose(DWORD t) {
    const double phase = double(t % kBeatMs) / kBeatMs;
    const double side = (t / kBeatMs) % 2 == 0 ? 1 : -1;
    const double air = std::sin(3.14159265 * phase);  // 0 on the ground, 1 at the top
    const double land = phase < kLandPart ? 1 - phase / kLandPart : 0;
    Pose pose;
    pose.squashY = 1 + kStretch * air - kLandSquash * land;
    pose.squashX = side * (1 - kStretch / 2 * air + kLandSquash * land);
    pose.dx = kSway * side * air;
    pose.dy = -kHopHeight * air;
    return pose;
}

bool PoseOf(int who, Pose* pose) {
    const DWORD now = GetTickCount();
    bool posed = false;
    const auto dance = g_dancingSince.find(who);
    if (dance != g_dancingSince.end()) {
        *pose = DancePose(now - dance->second);
        posed = true;
    }
    const auto bonk = g_bonkedAt.find(who);
    if (bonk != g_bonkedAt.end()) {
        Pose flat = *pose;
        if (BonkPose(now - bonk->second, &flat)) {
            pose->squashX = std::copysign(flat.squashX, pose->squashX);  // a dancer stays turned the way it was
            pose->squashY = flat.squashY;
            pose->dy = 0;
            posed = true;
        }
    }
    return posed;
}

void BeforeDraw(yyc::CInstance* player, int who) {
    Pose pose;
    if (!PoseOf(who, &pose)) return;
    const int id = IdOf(player);
    const double x = yyc::GetBuiltinReal(player, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(player, yyc::va::YYVar_y);
    g_saved[who] = Saved{ReadReal(id, kSlotSquashX), ReadReal(id, kSlotSquashY), x, y};
    WriteReal(id, kSlotSquashX, pose.squashX);
    WriteReal(id, kSlotSquashY, pose.squashY);
    // Only for its draw: back in AfterDraw, before anything moves it (its step, the network).
    if (pose.dx != 0) yyc::SetBuiltinReal(player, yyc::va::YYVar_x, x + pose.dx);
    if (pose.dy != 0) yyc::SetBuiltinReal(player, yyc::va::YYVar_y, y + pose.dy);
}

void AfterDraw(yyc::CInstance* player, int who) {
    const auto saved = g_saved.find(who);
    if (saved == g_saved.end()) return;
    const int id = IdOf(player);
    WriteReal(id, kSlotSquashX, saved->second.squashX);
    WriteReal(id, kSlotSquashY, saved->second.squashY);
    yyc::SetBuiltinReal(player, yyc::va::YYVar_x, saved->second.x);
    yyc::SetBuiltinReal(player, yyc::va::YYVar_y, saved->second.y);
    g_saved.erase(saved);
}

void DrawFx(int frame, double x, double y, double alpha) {
    gui::Call("draw_sprite_ext", {double(g_fxSprite), double(frame), x, y, kFxScale, kFxScale, 0, gui::kWhite, alpha});
}

void Overlay(yyc::CInstance* player, int who) {
    if (g_fxSprite < 0) return;
    const DWORD now = GetTickCount();
    const double x = yyc::GetBuiltinReal(player, yyc::va::YYVar_x);
    const double y = yyc::GetBuiltinReal(player, yyc::va::YYVar_y);
    const auto bonk = g_bonkedAt.find(who);
    if (bonk != g_bonkedAt.end()) {
        const DWORD t = now - bonk->second;
        if (t >= kStarsMs) {
            g_bonkedAt.erase(bonk);
        } else {
            Pose flat;
            const double squashY = BonkPose(t, &flat) ? flat.squashY : 1;
            const double alpha = t + 300 > kStarsMs ? double(kStarsMs - t) / 300 : 1;
            for (int i = 0; i < kStars; ++i) {
                const double angle = t * 0.008 + i * 2 * 3.14159265 / kStars;
                DrawFx(kFrameStar, x + 9 * std::cos(angle) - kFxOrigin * kFxScale,
                       y - kBodyHeight * squashY - 4 + 3 * std::sin(angle) - kFxOrigin * kFxScale, alpha);
            }
        }
    }
    const auto dance = g_dancingSince.find(who);
    if (dance == g_dancingSince.end()) return;
    const DWORD t = now - dance->second;
    const DWORD beat = t / kBeatMs;
    for (DWORD b = beat >= kNotesShown - 1 ? beat - (kNotesShown - 1) : 0; b <= beat; ++b) {
        const double age = double(t - b * kBeatMs);
        const double life = double(kNotesShown) * kBeatMs;
        const double side = b % 2 == 0 ? 7 : -7;
        DrawFx(kFrameNote, x + side + 2 * std::sin(age * 0.01) - kFxOrigin * kFxScale,
               y - kBodyHeight - age * kNoteRise - kFxOrigin * kFxScale, 1 - age / life);
    }
}

bool Hook(uint32_t va, void* detour, void* original) {
    const MH_STATUS status = MH_CreateHook(yyc::At<void*>(va), detour, static_cast<void**>(original));
    if (status != MH_OK) Log("emotes: hook @ VA %08X failed: %s", va, MH_StatusToString(status));
    return status == MH_OK;
}

}  // namespace

bool InstallHooks(bool (*isHost)()) {
    g_isHost = isHost;
    const bool ok = Hook(kScriptUsePickaxe, reinterpret_cast<void*>(&UsePickaxeHook), &g_origUsePickaxe) &&
                    Hook(kDanceScript, reinterpret_cast<void*>(&DanceHook), &g_origDance);
    puppet::DrawCallbacks callbacks;
    callbacks.before = &BeforeDraw;
    callbacks.after = &AfterDraw;
    callbacks.overlay = &Overlay;
    puppet::SetDrawCallbacks(callbacks);
    puppet::SetPuppetHurtHandler(&OnPuppetHurt);
    return ok;
}

// The dance's music plays while anyone here dances (one loop for all of them). A dancer whose puppet is gone (it left,
// or is in another room) is forgotten.
static void PlayDanceMusic(yyc::CInstance* self) {
    for (auto it = g_dancingSince.begin(); it != g_dancingSince.end();) {
        const bool gone = it->first != puppet::kLocalPlayer &&
                          puppet::Instance(static_cast<net::PeerId>(it->first)) == nullptr;
        it = gone ? g_dancingSince.erase(it) : std::next(it);
    }
    const bool anyone = !g_dancingSince.empty();
    if (anyone && g_musicVoice < 0 && g_danceSound >= 0) {
        g_musicVoice = yyc::CallReal(self, "audio_play_sound", {double(g_danceSound), kSoundPriority, 1});
        yyc::CallReal(self, "audio_sound_gain", {g_musicVoice, kMusicGain * Volume(self, kOptionMusic), 0});
    } else if (anyone && g_musicVoice >= 0 && g_steps % kMusicGainEverySteps == 0) {
        yyc::CallReal(self, "audio_sound_gain", {g_musicVoice, kMusicGain * Volume(self, kOptionMusic), 0});
    } else if (!anyone && g_musicVoice >= 0) {
        yyc::CallReal(self, "audio_stop_sound", {g_musicVoice});
        g_musicVoice = -1;
    }
}

void LocalTick(yyc::CInstance* localPlayer) {
    if (g_fxSprite == -1) {
        const int sprite = assets::AddSprite(localPlayer, kCoopFxPng, sizeof(kCoopFxPng), "coop_fx.png", 2, 0, 0);
        g_fxSprite = sprite >= 0 ? sprite : -2;
    }
    if (g_bonkSound == -1) {
        const int sound = assets::AddSound(localPlayer, kBonkWav, sizeof(kBonkWav), "coop_bonk.wav");
        g_bonkSound = sound >= 0 ? sound : -2;
    }
    if (g_danceSound == -1) {
        const int sound = assets::AddSound(localPlayer, kDanceWav, sizeof(kDanceWav), "coop_dance.wav");
        g_danceSound = sound >= 0 ? sound : -2;
    }
    if (g_steps++ % kRegisterEverySteps == 0) {
        commands::RegisterIfMissing(localPlayer, "dance", kDanceIndex, "");
    }
    if (g_dancingSince.count(puppet::kLocalPlayer) > 0 && WantsToWalk(localPlayer)) SetDancing(localPlayer, false);
    PlayDanceMusic(localPlayer);
}

uint8_t StateFlags() { return g_dancingSince.count(puppet::kLocalPlayer) > 0 ? kFlagDancing : 0; }

void OnRemoteState(net::PeerId player, uint8_t flags) {
    g_knownPeers.insert(player);
    const bool dancing = (flags & kFlagDancing) != 0;
    if (!dancing) {
        g_dancingSince.erase(player);
    } else if (g_dancingSince.count(player) == 0) {
        g_dancingSince[player] = GetTickCount();
    }
}

bool Handle(const net::Message& message, yyc::CInstance* localPlayer) {
    if (message.type != kPlayerFx) return false;
    if (message.payload.size() != sizeof(PlayerFxMsg)) return true;
    PlayerFxMsg msg{};
    memcpy(&msg, message.payload.data(), sizeof(msg));
    if (msg.kind != kFxBonk && msg.kind != kFxHurt) return true;
    if (g_isHost != nullptr && g_isHost()) {
        if (message.from == net::kHostPeer || msg.victim == kYou || msg.victim == message.from) return true;
        Route(msg, message.from);
        PlayerFxMsg here = msg;
        if (here.victim == net::kHostPeer) here.victim = kYou;
        Apply(localPlayer, here);
        return true;
    }
    Apply(localPlayer, msg);
    return true;
}

void Reset() {
    g_knownPeers.clear();
    g_lastBonked.clear();
    g_saved.clear();
    for (auto it = g_bonkedAt.begin(); it != g_bonkedAt.end();) {
        it = it->first == puppet::kLocalPlayer ? std::next(it) : g_bonkedAt.erase(it);
    }
    for (auto it = g_dancingSince.begin(); it != g_dancingSince.end();) {
        it = it->first == puppet::kLocalPlayer ? std::next(it) : g_dancingSince.erase(it);
    }
}

}  // namespace coop::emotes
