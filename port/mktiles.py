#!/usr/bin/env python3
"""port/tiles.rgba = gfx/primetiles.png (32x32, key colour = pixel 0,0 made
transparent) + extra rows of RLTiles sprites (gfx/rltiles.png) for things
the PRIME sheet lacks. EXTRA: (row, col) in rltiles, index = order here;
XUI.cpp addresses them as row kRowExtra (= sheet rows) + i/41."""
from PIL import Image
import struct

EXTRA = [
    None,      # column 0 means "no tile" to the game
    (11, 0),   # ghoul (hunched grey figure; PRIME's ghoul is an irradiated human)
    (11, 32),  # alien embryo (pale larva)
]

def rgba(path):
    im = Image.open(path).convert('RGBA')
    key = im.getpixel((0, 0))
    px = im.load()
    for y in range(im.height):
        for x in range(im.width):
            if px[x, y][:3] == key[:3]:
                px[x, y] = (0, 0, 0, 0)
    return im

prime = rgba('gfx/primetiles.png')
rl = rgba('gfx/rltiles.png')
cols = prime.width // 32
rows = prime.height // 32 + (len(EXTRA) + cols - 1) // cols
out = Image.new('RGBA', (prime.width, rows * 32))
out.paste(prime, (0, 0))
for i, rc in enumerate(EXTRA):
    if not rc:
        continue
    r, c = rc
    x, y = i % cols, prime.height // 32 + i // cols
    out.paste(rl.crop((c * 32, r * 32, c * 32 + 32, r * 32 + 32)), (x * 32, y * 32))
with open('port/tiles.rgba', 'wb') as f:
    f.write(struct.pack('<II', out.width, out.height))
    f.write(out.tobytes())
out.save('port/tiles.png')
print(out.size)
