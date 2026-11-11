# Builds native/assets/dance.wav: the dance's music, a chiptune loop of our own (square-wave lead, triangle bass,
# noise hi-hat) at the dance's tempo - one beat per hop (emotes.cpp kBeatMs = 350 ms), 16 beats, looped seamlessly.
# 16-bit mono PCM at 16 kHz (a retro sound, and small).
# Usage (from the repository root): python tools/sprites/make_dance_music.py
import math
import random
import struct
import wave
from pathlib import Path

RATE = 16000
BEAT_S = 0.350
STEPS_PER_BEAT = 2  # eighth notes
STEP = int(RATE * BEAT_S / STEPS_PER_BEAT)

# MIDI notes per eighth (None: rest). A bouncy C-major tune, 32 eighths = 16 beats.
LEAD = [72, None, 76, 79, 81, 79, 76, None,
        74, None, 77, 81, 79, 77, 74, None,
        72, None, 76, 79, 84, 83, 81, 79,
        77, 76, 74, 76, 72, None, 72, None]
BASS = [48, 48, 55, 55, 48, 48, 55, 55,
        50, 50, 57, 57, 50, 50, 57, 57,
        53, 53, 60, 60, 52, 52, 55, 55,
        50, 50, 55, 55, 48, 48, 43, 43]
HAT = [1, 0, 1, 1] * 8


def freq(note: int) -> float:
    return 440.0 * 2 ** ((note - 69) / 12)


def square(phase: float, duty: float = 0.25) -> float:
    return 1.0 if (phase % 1.0) < duty else -1.0


def triangle(phase: float) -> float:
    p = phase % 1.0
    return 4 * p - 1 if p < 0.5 else 3 - 4 * p


def envelope(i: int, length: int) -> float:
    attack = min(1.0, i / 40)
    release = min(1.0, (length - i) / 300)
    return attack * release * (0.65 + 0.35 * math.exp(-i / (length * 0.4)))


def render() -> list[float]:
    rng = random.Random(7)
    samples = []
    lead_phase = bass_phase = 0.0
    for step in range(len(LEAD)):
        lead, bass, hat = LEAD[step], BASS[step], HAT[step]
        for i in range(STEP):
            value = 0.0
            if lead is not None:
                lead_phase += freq(lead) / RATE
                value += 0.30 * square(lead_phase) * envelope(i, STEP)
            bass_phase += freq(bass) / RATE
            value += 0.28 * triangle(bass_phase) * envelope(i, STEP)
            if hat and i < STEP // 6:
                value += 0.10 * (rng.random() * 2 - 1) * (1 - i / (STEP / 6))
            samples.append(value)
    return samples


def main() -> None:
    samples = render()
    peak = max(abs(s) for s in samples)
    pcm = b''.join(struct.pack('<h', int(s / peak * 0.85 * 32767)) for s in samples)
    out = Path('native/assets/dance.wav')
    with wave.open(str(out), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)
    print(f'{out}: {len(samples) / RATE:.2f} s, {len(pcm)} bytes')


if __name__ == '__main__':
    main()
