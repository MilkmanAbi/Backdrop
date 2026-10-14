"""Backdrop icons from the Lucide "image" glyph (24x24 viewBox, stroke 2, round caps/joins):
   backdrop.ico       app / Start menu / installer: white glyph on a dark rounded tile
   tray_light.ico     tray on a dark taskbar: white glyph
   tray_dark.ico      tray on a light taskbar: near-black glyph"""
from pathlib import Path
from PIL import Image, ImageDraw

here = Path(__file__).parent
SS = 1024                      # supersampled canvas


def glyph(colour, scale=1.0):
    """Draw the Lucide 'image' icon centred on an SS x SS canvas; scale < 1 leaves margin (for the tile)."""
    im = Image.new("RGBA", (SS, SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    k = SS / 24 * scale
    o = SS * (1 - scale) / 2
    P = lambda x, y: (o + x * k, o + y * k)
    w = 2 * k
    # rect x3 y3 18x18 rx2, stroke centred on the path
    d.rounded_rectangle((*P(3 - 1, 3 - 1), *P(21 + 1, 21 + 1)), radius=3 * k, outline=colour, width=round(w))
    # circle (9,9) r2
    d.ellipse((*P(9 - 3, 9 - 3), *P(9 + 3, 9 + 3)), outline=colour, width=round(w))
    # path: (21,15) -> (17.914,11.914), arc r2 over the top to (15.086,11.914), -> (6,21)
    import math
    pts = [P(21, 15), P(17.914, 11.914)]
    cx, cy = 16.5, 11.914 + math.sqrt(4 - 1.414 ** 2)
    a0, a1 = math.atan2(11.914 - cy, 17.914 - cx), math.atan2(11.914 - cy, 15.086 - cx)
    for i in range(1, 24):
        a = a0 + (a1 - a0) * i / 24
        pts.append(P(cx + 2 * math.cos(a), cy + 2 * math.sin(a)))
    pts += [P(15.086, 11.914), P(6, 21)]
    d.line(pts, fill=colour, width=round(w), joint="curve")
    for x, y in (pts[0], pts[-1]):   # round caps
        d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=colour)
    return im


def tile():
    im = Image.new("RGBA", (SS, SS), (0, 0, 0, 0))
    ImageDraw.Draw(im).rounded_rectangle((SS * .03, SS * .03, SS * .97, SS * .97), radius=SS * .22, fill=(32, 32, 36, 255))
    im.alpha_composite(glyph((255, 255, 255, 255), 0.62))
    return im


def save(img, name, sizes):
    imgs = [img.resize((n, n), Image.LANCZOS) for n in sizes]
    imgs[-1].save(here / name, sizes=[(i.width, i.height) for i in imgs], append_images=imgs[:-1])


save(tile(), "backdrop.ico", (16, 20, 24, 32, 40, 48, 64, 256))
save(glyph((255, 255, 255, 255)), "tray_light.ico", (16, 20, 24, 32, 40, 48, 64))
save(glyph((26, 26, 26, 255)), "tray_dark.ico", (16, 20, 24, 32, 40, 48, 64))
print("ok")
