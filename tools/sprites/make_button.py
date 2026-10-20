# Builds the COOP main-menu button sprite (native/assets/coop_button.png: 2 frames side by side, like the game's
# sprMainMenu* - frame 0 with the word, frame 1 without) from the game's own MODDING button and player sprites.
# Input: the PNGs COOP_DEBUG_DUMPSPRITES=1 saves into %LOCALAPPDATA%\Forager (coop_sprite_*.png); run from there or
# pass their folder. Then tools/sprites/png2header.py makes native/src/coop_button_png.h.
# Usage: make_button.py [sprite folder] [hue shift, 0 = MODDING purple; 0.12 = the pink in use] [output]
import colorsys, sys
from PIL import Image

SRC = 'coop_sprite_sprMainMenu'
PAL = {'W': (252, 252, 252, 255), 'c': (33, 36, 72, 255), 'O': (74, 54, 146, 255), 'f': (226, 178, 249, 255),
       'S': None}  # S: keep the body underneath
C = ['cOOOOOOOOc', 'cOfWWWWfOc', 'cfWWWWWWfc', 'cWWWccWWWc', 'cWWWOccccc', 'cWWWOSSSSS',
     'cWWWOSSSSS', 'cWWWOOOOOS', 'cfWWWWWWfc', 'ccfWWWWfcc', 'cccccccccc']
O = ['cOOOOOOOOc', 'cOfWWWWfOc', 'cfWWWWWWfc', 'cWWWccWWWc', 'cWWWOcWWWc', 'cWWWOcWWWc',
     'cWWWOcWWWc', 'cWWWOcWWWc', 'cfWWWWWWfc', 'ccfWWWWfcc', 'cccccccccc']
P = ['cOOOOOSSS', 'cfWWWfOSS', 'cWWWWWWOS', 'cWWccfWfc', 'cWWOccWWc', 'cWWOOfWfc',
     'cWWWWWWcc', 'cWWOccccS', 'cWWOSSSSS', 'cfWOSSSSS', 'ccccSSSSS']

def word(letters):
    width = sum(len(l[0]) for l in letters) - (len(letters) - 1)
    grid = [['S'] * width for _ in range(11)]
    x = 0
    for l in letters:
        for r, row in enumerate(l):
            for i, ch in enumerate(row):
                cur = grid[r][x + i]
                if ch != 'S' and not (cur == 'c' and ch != 'W'):
                    grid[r][x + i] = ch
        x += len(l[0]) - 1
    return grid

def recolor(img, hue_shift):
    if hue_shift == 0:
        return img
    out = img.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = px[x, y]
            if a == 0 or (r, g, b) in ((252, 252, 252), (23, 17, 26)):
                continue
            h, l, s = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
            r2, g2, b2 = colorsys.hls_to_rgb((h + hue_shift) % 1, l, s)
            px[x, y] = (round(r2 * 255), round(g2 * 255), round(b2 * 255), a)
    return out

OUTLINE = (23, 17, 26, 255)

def icon(player):
    """Two players, the second in front, with the dark 1 px outline the other buttons' icons have."""
    pair = Image.new('RGBA', (player.width + 10, player.height + 5))
    pair.alpha_composite(player, (1, 1))
    pair.alpha_composite(player, (9, 4))
    out = Image.new('RGBA', pair.size)
    for y in range(pair.height):
        for x in range(pair.width):
            if pair.getpixel((x, y))[3] == 0 and any(
                    0 <= x + dx < pair.width and 0 <= y + dy < pair.height and pair.getpixel((x + dx, y + dy))[3] > 0
                    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                out.putpixel((x, y), OUTLINE)
    out.alpha_composite(pair)
    return out

def build(hue_shift):
    blank = Image.open(SRC + 'Modding_1.png').convert('RGBA')  # the body with no words and no icon
    body0 = Image.new('RGBA', blank.size)
    body0.paste(blank, (1, 0))  # frame 0's body sits 1 px further right
    player = Image.open(SRC.replace('sprMainMenu', 'sprPlayerIdle') + '_0.png').convert('RGBA')
    frames = []
    for i, body in enumerate((body0, blank)):
        f = body.copy()
        f.alpha_composite(icon(player), (0, 2))  # two players: the icon, sticking out on the left like the others
        if i == 0:
            grid = word([C, O, O, P])
            x0 = 54 - len(grid[0]) // 2
            for r, row in enumerate(grid):
                for k, ch in enumerate(row):
                    if PAL.get(ch):
                        f.putpixel((x0 + k, 6 + r), PAL[ch])
        frames.append(recolor(f, hue_shift))
    strip = Image.new('RGBA', (blank.width * 2, blank.height))
    strip.paste(frames[0], (0, 0))
    strip.paste(frames[1], (blank.width, 0))
    return strip

if __name__ == '__main__':
    import os
    os.chdir(sys.argv[1] if len(sys.argv) > 1 else '.')
    shift = float(sys.argv[2]) if len(sys.argv) > 2 else 0.12
    build(shift).save(sys.argv[3] if len(sys.argv) > 3 else 'coop_button.png')
