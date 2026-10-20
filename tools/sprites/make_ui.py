# Builds native/assets/coop_ui.png: blank menu-button bodies for the COOP screen, side by side (frames of equal
# size, origin top-left), made from the game's blank MODDING button by recolouring it:
#   0 green (create/join/connect), 1 purple (back), 2 grey (not available), 3 pink (as the COOP button)
# The screen stretches them sideways: the end caps stay, the middle is repeated (gui::Bar3).
# Input: the PNGs COOP_DEBUG_DUMPSPRITES=1 saves into %LOCALAPPDATA%\Forager (coop_sprite_*.png).
# Usage: make_ui.py [sprite folder] [output]
import colorsys
import os
import sys

from PIL import Image

FRAMES = [('hue', -0.46), ('hue', 0.0), ('grey', 0.0), ('hue', 0.12)]
KEEP = ((252, 252, 252), (23, 17, 26))  # white and the outline stay as they are


def recolor(img, kind, amount):
    out = img.copy()
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b, a = px[x, y]
            if a == 0 or (r, g, b) in KEEP:
                continue
            h, l, s = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
            if kind == 'grey':
                s *= 0.15
            else:
                h = (h + amount) % 1
            r2, g2, b2 = colorsys.hls_to_rgb(h, l, s)
            px[x, y] = (round(r2 * 255), round(g2 * 255), round(b2 * 255), a)
    return out


def main():
    os.chdir(sys.argv[1] if len(sys.argv) > 1 else '.')
    body = Image.open('coop_sprite_sprMainMenuModding_1.png').convert('RGBA')
    body = body.crop(body.getbbox())  # just the body: no empty margin around it
    strip = Image.new('RGBA', (body.width * len(FRAMES), body.height))
    for i, (kind, amount) in enumerate(FRAMES):
        strip.paste(recolor(body, kind, amount), (i * body.width, 0))
    strip.save(sys.argv[2] if len(sys.argv) > 2 else 'coop_ui.png')
    print(f'{len(FRAMES)} frames of {body.width}x{body.height}')


if __name__ == '__main__':
    main()
