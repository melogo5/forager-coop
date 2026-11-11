// Pictures and sounds of our own, embedded in the DLL (native/assets, tools/sprites/png2header.py), made into the
// game's sprites and sounds at run time. Game thread only.
#pragma once

#include <cstddef>

#include "yyc.h"

namespace coop::assets {

// Writes an embedded PNG next to the saves (%LOCALAPPDATA%\Forager, where the game's file functions look) as `file`
// and adds it as a sprite of `frames` frames side by side, origin (ox, oy). The sprite index, or -1.
int AddSprite(yyc::CInstance* self, const unsigned char* png, size_t size, const char* file, int frames, double ox,
              double oy);

// Makes an embedded 16-bit PCM WAV (mono or stereo) a sound the game can play (audio_play_sound), through a buffer
// loaded from `file` written next to the saves. The sound index, or -1.
int AddSound(yyc::CInstance* self, const unsigned char* wav, size_t size, const char* file);

}  // namespace coop::assets
