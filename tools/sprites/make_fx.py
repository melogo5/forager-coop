# Builds native/assets/coop_fx.png: the players' little effects, frames of 9x9 side by side (origin at the centre):
#   0 a star (circling over a bonked player's head), 1 a music note (rising over a dancing player)
# Drawn from the pixel maps below in the game's own outline colour.
# Usage (from the repository root): python tools/sprites/make_fx.py
from pathlib import Path

from PIL import Image

SIZE = 9
COLORS = {
    'O': (23, 17, 26, 255),    # the game's outline
    'Y': (255, 222, 69, 255),  # star yellow
    'L': (255, 247, 189, 255), # star highlight
    'W': (252, 252, 252, 255), # note white
}

STAR = [
    '....O....',
    '...OYO...',
    '...OYO...',
    '.OOOYOOO.',
    'OYYYLYYYO',
    '.OOOYOOO.',
    '...OYO...',
    '...OYO...',
    '....O....',
]

NOTE = [
    '...OOOOO.',
    '...OWWWWO',
    '...OWOOOO',
    '...OWO...',
    '...OWO...',
    '.OOOWO...',
    'OWWWWO...',
    'OWWWWO...',
    '.OOOO....',
]

FRAMES = [STAR, NOTE]


def main():
    sheet = Image.new('RGBA', (SIZE * len(FRAMES), SIZE), (0, 0, 0, 0))
    px = sheet.load()
    for f, rows in enumerate(FRAMES):
        assert len(rows) == SIZE and all(len(r) == SIZE for r in rows), f'frame {f} is not {SIZE}x{SIZE}'
        for y, row in enumerate(rows):
            for x, c in enumerate(row):
                if c != '.':
                    px[f * SIZE + x, y] = COLORS[c]
    out = Path('native/assets/coop_fx.png')
    sheet.save(out)
    print(f'{out}: {len(FRAMES)} frames of {SIZE}x{SIZE}')


if __name__ == '__main__':
    main()
