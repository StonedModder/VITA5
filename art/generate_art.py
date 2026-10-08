#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
VITA5 — programmatic placeholder art (PIL/numpy).

Does not replace the committed launcher masters. Outputs:
  app/sce_sys/icon0.png   512x512
  app/sce_sys/pic0.png    3840x2160
  app/sce_sys/pic1.png    3840x2160
  app/sce_sys/splash.png  1920x1080
  art/banner.png          1920x480

Run:  python3 art/generate_art.py
"""

import math
import os
import random

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

# ---------------------------------------------------------------- palette ---
BG_DEEP = (6, 9, 26)
BG_MID = (18, 14, 52)
BG_HIGH = (42, 27, 94)
CYAN = (55, 230, 255)
MAGENTA = (255, 61, 203)
VIOLET = (123, 92, 255)
WHITE = (235, 242, 255)

FONT_BOLD = "C:/Windows/Fonts/segoeuib.ttf"
FONT_REG = "C:/Windows/Fonts/segoeui.ttf"

SS = 2  # supersample factor for vector layers

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCE = os.path.join(REPO, "app", "sce_sys")
ART = os.path.join(REPO, "art")


# ------------------------------------------------------------- utilities ----
def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def v_gradient(w, h, stops):
    """Vertical multi-stop gradient. stops: [(pos0..1, rgb), ...]"""
    img = np.zeros((h, w, 3), dtype=np.float32)
    ys = np.linspace(0.0, 1.0, h)
    pos = np.array([s[0] for s in stops], dtype=np.float32)
    cols = np.array([s[1] for s in stops], dtype=np.float32)
    for c in range(3):
        img[:, :, c] = np.interp(ys, pos, cols[:, c])[:, None]
    return img


def diag_blend(base, color, angle_deg, strength=0.5):
    """Blend a diagonal light wash into a float array (h, w, 3)."""
    h, w = base.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    a = math.radians(angle_deg)
    d = (xx / w) * math.cos(a) + (yy / h) * math.sin(a)
    d = (d - d.min()) / max(d.max() - d.min(), 1e-6)
    m = (d ** 1.6)[..., None] * strength
    return base * (1 - m) + np.array(color, dtype=np.float32) * m


def to_pil(arr):
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")


def add_glow(base, layer_rgba, radius, opacity=1.0):
    """Blur an RGBA layer and screen-composite it over PIL image `base`."""
    glow = layer_rgba.split()[3].filter(ImageFilter.GaussianBlur(radius))
    if opacity != 1.0:
        glow = glow.point(lambda v: int(v * opacity))
    tint = Image.new("RGB", layer_rgba.size, layer_rgba.getpixel((0, 0))[:3]
                     if layer_rgba.getpixel((0, 0))[3] else (255, 255, 255))
    # colorize: use the layer's RGB under its blurred alpha
    rgb = layer_rgba.convert("RGB")
    glow_rgb = Image.merge("RGB", [
        Image.fromarray((np.asarray(rgb.split()[i], dtype=np.float32)
                         * np.asarray(glow, dtype=np.float32) / 255.0
                         ).astype(np.uint8))
        for i in range(3)
    ])
    return Image.fromarray(np.clip(
        np.asarray(base, dtype=np.float32)
        + np.asarray(glow_rgb, dtype=np.float32), 0, 255).astype(np.uint8))


def text_layer(size, text, font_path, px, color, xy, anchor="mm",
               spacing=0, glow_color=None, glow_radius=0, glow_passes=1):
    """Render text on a transparent layer; returns RGBA image."""
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    font = ImageFont.truetype(font_path, px)
    if spacing:
        widths = [d.textlength(ch, font=font) for ch in text]
        total = sum(widths) + spacing * (len(text) - 1)
        x, y = xy
        if anchor[0] == "m":
            x -= total / 2
        elif anchor[0] == "r":
            x -= total
        if anchor[1] == "m":
            y -= px * 0.62
        elif anchor[1] == "b":
            y -= px * 1.05
        for ch, cw in zip(text, widths):
            d.text((x, y), ch, font=font, fill=color + (255,))
            x += cw + spacing
    else:
        d.text(xy, text, font=font, fill=color + (255,), anchor=anchor)
    return layer


def wordmark(size, xy, px, spacing=8, scale=1):
    """'VITA5' in white + '5' in cyan, one wordmark layer (RGBA)."""
    layer = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    font = ImageFont.truetype(FONT_BOLD, px)
    parts = [("VITA5", WHITE), ("5", CYAN)]
    widths = [sum(d.textlength(ch, font=font) for ch in t) + spacing * (len(t) - 1)
              for t, _ in parts]
    gap = int(px * 0.03)
    total = sum(widths) + gap
    x = xy[0] - total / 2
    y = xy[1] - px * 0.62
    for (t, col), wdt in zip(parts, widths):
        cx = x
        for ch in t:
            d.text((cx, y), ch, font=font, fill=col + (255,))
            cx += d.textlength(ch, font=font) + spacing
        x += wdt + gap
    return layer


def vignette(img, strength=0.42, power=1.7):
    w, h = img.size
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    cx, cy = w / 2, h / 2
    r = np.sqrt(((xx - cx) / cx) ** 2 + ((yy - cy) / cy) ** 2) / math.sqrt(2)
    m = 1.0 - strength * (r ** power)
    arr = np.asarray(img, dtype=np.float32) * m[..., None]
    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")


def particles(draw, w, h, n, seed, colors, rmin=1, rmax=4):
    rng = random.Random(seed)
    for _ in range(n):
        x, y = rng.uniform(0, w), rng.uniform(0, h * 0.9)
        r = rng.uniform(rmin, rmax)
        c = colors[rng.randrange(len(colors))]
        a = rng.randint(60, 190)
        draw.ellipse([x - r, y - r, x + r, y + r], fill=c + (a,))


def device(d, box, accent_l=CYAN, accent_r=MAGENTA, glass=(16, 22, 58)):
    """Stylized Vita-like handheld: rounded body, screen, sticks, buttons."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    rad = int(min(w, h) * 0.16)
    d.rounded_rectangle(box, radius=rad, fill=glass + (255,),
                        outline=accent_l + (235,), width=max(2, int(w * 0.012)))
    # screen
    sx0, sy0 = x0 + w * 0.17, y0 + h * 0.12
    sx1, sy1 = x1 - w * 0.17, y1 - h * 0.34
    d.rounded_rectangle([sx0, sy0, sx1, sy1], radius=int(rad * 0.5),
                        fill=(46, 60, 130, 255), outline=accent_l + (120,),
                        width=max(1, int(w * 0.006)))
    # analog sticks
    r = w * 0.052
    for cx in (x0 + w * 0.105, x1 - w * 0.105):
        cy = y1 - h * 0.17
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(30, 38, 88, 255),
                  outline=accent_r + (200,), width=max(2, int(w * 0.008)))
    # buttons hint
    rb = w * 0.020
    for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        bx = x1 - w * 0.245 + dx * rb * 2.6
        by = y1 - h * 0.30 + dy * rb * 2.6
        d.ellipse([bx - rb, by - rb, bx + rb, by + rb],
                  fill=accent_l + (170,))


def bigscreen(d, box, frame=CYAN, inner_top=(30, 42, 110), inner_bot=(12, 16, 52)):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    rad = int(min(w, h) * 0.045)
    d.rounded_rectangle(box, radius=rad, fill=(14, 18, 48, 255),
                        outline=frame + (240,), width=max(3, int(w * 0.004)))
    ix0, iy0 = x0 + w * 0.014, y0 + h * 0.02
    ix1, iy1 = x1 - w * 0.014, y1 - h * 0.02
    # glass with vertical tint
    glass = Image.new("RGBA", (int(ix1 - ix0), int(iy1 - iy0)), inner_top + (255,))
    gd = ImageDraw.Draw(glass)
    gh = glass.size[1]
    for i in range(gh):
        t = i / max(gh - 1, 1)
        gd.line([(0, i), (glass.size[0], i)], fill=lerp(inner_top, inner_bot, t) + (255,))
    # soft diagonal sheen
    gd = ImageDraw.Draw(glass)
    gd.polygon([(0, 0), (glass.size[0] * 0.5, 0), (0, gh * 0.7)],
               fill=frame + (26,))
    d._image.paste(glass, (int(ix0), int(iy0)))
    return box


# ------------------------------------------------------------- backgrounds --
def make_backdrop(w, h, seed=1):
    arr = v_gradient(w, h, [(0.0, BG_DEEP), (0.45, BG_MID), (1.0, BG_HIGH)])
    arr = diag_blend(arr, (26, 12, 70), 25, 0.55)
    img = to_pil(arr)
    # ambient glows
    for cx, cy, r, col, op in (
        (w * 0.22, h * 0.72, w * 0.42, VIOLET, 0.35),
        (w * 0.82, h * 0.30, w * 0.38, CYAN, 0.22),
        (w * 0.55, h * 0.85, w * 0.30, MAGENTA, 0.18),
    ):
        layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        ImageDraw.Draw(layer).ellipse(
            [cx - r, cy - r * (h / w) * (w / h), cx + r, cy + r],
            fill=col + (255,))
        img = add_glow(img, layer, radius=int(r * 0.45), opacity=op)
    return img


def perspective_grid(size, horizon_y, color=CYAN, alpha=42, spacing=None):
    w, h = size
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    vp = (w * 0.5, horizon_y)
    n = 26
    for i in range(-n, n + 1):
        x = w * 0.5 + i * (w / n) * 1.4
        d.line([vp, (x, h)], fill=color + (alpha,), width=2)
    ys = [0.06, 0.12, 0.22, 0.38, 0.62, 1.0]
    for t in ys:
        y = horizon_y + (h - horizon_y) * (t ** 2.1)
        d.line([(0, y), (w, y)], fill=color + (int(alpha * 0.8),), width=2)
    return layer


def light_streaks(size, n, seed, colors):
    w, h = size
    layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    rng = random.Random(seed)
    for _ in range(n):
        y = rng.uniform(-h * 0.1, h * 1.1)
        thick = rng.uniform(h * 0.002, h * 0.012)
        x0 = rng.uniform(-w * 0.3, w * 0.7)
        ln = rng.uniform(w * 0.25, w * 0.9)
        col = colors[rng.randrange(len(colors))]
        a = rng.randint(14, 46)
        d.line([(x0, y), (x0 + ln, y - h * rng.uniform(0.02, 0.10))],
               fill=col + (a,), width=int(thick))
    return layer


# ------------------------------------------------------------------ icon ----
def gen_icon():
    size = 512
    img = make_backdrop(size, size, seed=7)
    base = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    d._image = base

    # big screen (upper), device docked in front (lower)
    bigscreen(d, [70, 84, 442, 300], frame=CYAN)
    # beam between screen and device
    beam = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beam)
    bd.polygon([(212, 292), (300, 292), (330, 362), (182, 362)], fill=CYAN + (200,))
    img = add_glow(img, beam, radius=18, opacity=0.85)
    img = add_glow(img, beam, radius=46, opacity=0.45)

    device(d, [162, 330, 350, 470])
    img = add_glow(img, base, radius=3, opacity=1.0)

    # crisp shapes on top
    crisp = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    cd = ImageDraw.Draw(crisp)
    cd._image = crisp
    bigscreen(cd, [70, 84, 442, 300], frame=CYAN)
    device(cd, [162, 330, 350, 470])
    img = Image.alpha_composite(img.convert("RGBA"), crisp).convert("RGB")

    img = vignette(img, 0.32)
    img.save(os.path.join(SCE, "icon0.png"))


# ------------------------------------------------------- shared scene parts -
def scene_wordmark(img, xy, px, spacing=10, glow=1.0):
    wm = wordmark(img.size, xy, px, spacing=spacing)
    img = add_glow(img, wm, radius=int(px * 0.30), opacity=0.85 * glow)
    img = add_glow(img, wm, radius=int(px * 0.9), opacity=0.35 * glow)
    return Image.alpha_composite(img.convert("RGBA"), wm).convert("RGB")


# ------------------------------------------------------------------- pic0 ---
def gen_pic0():
    w, h = 3840, 2160
    img = make_backdrop(w, h, seed=11)

    grid = perspective_grid((w, h), h * 0.52, color=CYAN, alpha=34)
    img = add_glow(img, grid, radius=2, opacity=0.9)
    img = add_glow(img, light_streaks((w, h), 16, 3, [CYAN, VIOLET, MAGENTA]),
                   radius=8, opacity=0.8)

    base = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    d._image = base
    # TV at right, docked Vita in foreground left
    bigscreen(d, [w * 0.30, h * 0.14, w * 0.92, h * 0.62], frame=CYAN)
    beam = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beam)
    bd.polygon([(w * 0.42, h * 0.615), (w * 0.52, h * 0.615),
                (w * 0.40, h * 0.72), (w * 0.335, h * 0.72)],
               fill=CYAN + (190,))
    img = add_glow(img, beam, radius=40, opacity=0.8)
    img = add_glow(img, beam, radius=120, opacity=0.4)

    device(d, [w * 0.115, h * 0.575, w * 0.375, h * 0.905])
    img = add_glow(img, base, radius=6, opacity=1.0)

    crisp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    cd = ImageDraw.Draw(crisp)
    cd._image = crisp
    bigscreen(cd, [w * 0.30, h * 0.14, w * 0.92, h * 0.62], frame=CYAN)
    device(cd, [w * 0.115, h * 0.575, w * 0.375, h * 0.905])
    img = Image.alpha_composite(img.convert("RGBA"), crisp).convert("RGB")

    # sparkle field
    sp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    particles(ImageDraw.Draw(sp), w, h, 220, 21, [CYAN, VIOLET, MAGENTA, WHITE],
              rmin=1, rmax=5)
    img = add_glow(img, sp, radius=6, opacity=0.7)

    img = scene_wordmark(img, (w * 0.62, h * 0.845), int(h * 0.088), spacing=12)

    # thin accent rule under the wordmark
    rule = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(rule).line(
        [(w * 0.475, h * 0.905), (w * 0.765, h * 0.905)],
        fill=CYAN + (170,), width=6)
    img = add_glow(img, rule, radius=10, opacity=0.8)
    img = Image.alpha_composite(img.convert("RGBA"), rule).convert("RGB")

    img = vignette(img, 0.38)
    img.save(os.path.join(SCE, "pic0.png"))


# ------------------------------------------------------------------- pic1 ---
def gen_pic1():
    w, h = 3840, 2160
    img = make_backdrop(w, h, seed=23)
    img = add_glow(img, light_streaks((w, h), 12, 8, [CYAN, VIOLET, MAGENTA]),
                   radius=10, opacity=0.7)

    cx, cy = w * 0.5, h * 0.44
    # concentric rings
    rings = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    rd = ImageDraw.Draw(rings)
    for i, (r, col, a) in enumerate((
            (h * 0.20, CYAN, 150), (h * 0.285, VIOLET, 110),
            (h * 0.375, MAGENTA, 85), (h * 0.47, CYAN, 55))):
        rd.ellipse([cx - r * 1.35, cy - r, cx + r * 1.35, cy + r],
                   outline=col + (a,), width=max(3, int(h * 0.0022)))
    img = add_glow(img, rings, radius=14, opacity=0.9)
    img = add_glow(img, rings, radius=60, opacity=0.5)

    # centered emblem: screen + device + beam, small and clean
    base = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    d._image = base
    ew, eh = w * 0.16, h * 0.24
    bigscreen(d, [cx - ew, cy - eh * 1.02, cx + ew, cy - eh * 0.10], frame=CYAN)
    beam = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beam)
    bd.polygon([(cx - ew * 0.34, cy - eh * 0.12), (cx + ew * 0.34, cy - eh * 0.12),
                (cx + ew * 0.55, cy + eh * 0.28), (cx - ew * 0.55, cy + eh * 0.28)],
               fill=CYAN + (190,))
    img = add_glow(img, beam, radius=30, opacity=0.8)
    img = add_glow(img, beam, radius=90, opacity=0.35)
    device(d, [cx - ew * 0.62, cy + eh * 0.16, cx + ew * 0.62, cy + eh * 1.02])
    img = add_glow(img, base, radius=5, opacity=1.0)

    crisp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    cd = ImageDraw.Draw(crisp)
    cd._image = crisp
    bigscreen(cd, [cx - ew, cy - eh * 1.02, cx + ew, cy - eh * 0.10], frame=CYAN)
    device(cd, [cx - ew * 0.62, cy + eh * 0.16, cx + ew * 0.62, cy + eh * 1.02])
    img = Image.alpha_composite(img.convert("RGBA"), crisp).convert("RGB")

    sp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    particles(ImageDraw.Draw(sp), w, h, 160, 42, [CYAN, VIOLET, MAGENTA, WHITE],
              rmin=1, rmax=4)
    img = add_glow(img, sp, radius=5, opacity=0.6)

    img = scene_wordmark(img, (cx, h * 0.875), int(h * 0.062), spacing=10)
    img = vignette(img, 0.40)
    img.save(os.path.join(SCE, "pic1.png"))


# ----------------------------------------------------------------- splash ---
def gen_splash():
    w, h = 1920, 1080
    img = make_backdrop(w, h, seed=31)

    cx, cy = w * 0.5, h * 0.385
    base = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    d._image = base
    ew, eh = w * 0.075, h * 0.115
    bigscreen(d, [cx - ew, cy - eh * 1.05, cx + ew, cy - eh * 0.12], frame=CYAN)
    beam = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beam)
    bd.polygon([(cx - ew * 0.35, cy - eh * 0.14), (cx + ew * 0.35, cy - eh * 0.14),
                (cx + ew * 0.58, cy + eh * 0.26), (cx - ew * 0.58, cy + eh * 0.26)],
               fill=CYAN + (190,))
    img = add_glow(img, beam, radius=22, opacity=0.85)
    img = add_glow(img, beam, radius=70, opacity=0.4)
    device(d, [cx - ew * 0.64, cy + eh * 0.14, cx + ew * 0.64, cy + eh * 1.02])
    img = add_glow(img, base, radius=4, opacity=1.0)

    crisp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    cd = ImageDraw.Draw(crisp)
    cd._image = crisp
    bigscreen(cd, [cx - ew, cy - eh * 1.05, cx + ew, cy - eh * 0.12], frame=CYAN)
    device(cd, [cx - ew * 0.64, cy + eh * 0.14, cx + ew * 0.64, cy + eh * 1.02])
    img = Image.alpha_composite(img.convert("RGBA"), crisp).convert("RGB")

    img = scene_wordmark(img, (cx, h * 0.70), int(h * 0.085), spacing=6)

    # loading dots
    dots = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dots)
    r = h * 0.009
    for i, a in enumerate((70, 130, 230)):
        x = cx + (i - 1) * h * 0.045
        y = h * 0.80
        dd.ellipse([x - r, y - r, x + r, y + r], fill=CYAN + (a,))
    img = add_glow(img, dots, radius=10, opacity=0.9)
    img = Image.alpha_composite(img.convert("RGBA"), dots).convert("RGB")

    sp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    particles(ImageDraw.Draw(sp), w, h, 110, 55, [CYAN, VIOLET, MAGENTA, WHITE],
              rmin=1, rmax=3)
    img = add_glow(img, sp, radius=5, opacity=0.55)

    img = vignette(img, 0.42)
    img.save(os.path.join(SCE, "splash.png"))


# ----------------------------------------------------------------- banner ---
def gen_banner():
    w, h = 1920, 480
    img = make_backdrop(w, h, seed=63)
    img = add_glow(img, light_streaks((w, h), 10, 7, [CYAN, VIOLET, MAGENTA]),
                   radius=8, opacity=0.75)

    # emblem at left
    base = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    d._image = base
    cx, cy = w * 0.175, h * 0.5
    ew, eh = h * 0.22, h * 0.16
    bigscreen(d, [cx - ew, cy - eh * 1.55, cx + ew, cy - eh * 0.28], frame=CYAN)
    beam = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bd = ImageDraw.Draw(beam)
    bd.polygon([(cx - ew * 0.35, cy - eh * 0.30), (cx + ew * 0.35, cy - eh * 0.30),
                (cx + ew * 0.55, cy + eh * 0.10), (cx - ew * 0.55, cy + eh * 0.10)],
               fill=CYAN + (185,))
    img = add_glow(img, beam, radius=16, opacity=0.8)
    device(d, [cx - ew * 0.62, cy + eh * 0.05, cx + ew * 0.62, cy + eh * 1.05])
    img = add_glow(img, base, radius=4, opacity=1.0)

    crisp = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    cd = ImageDraw.Draw(crisp)
    cd._image = crisp
    bigscreen(cd, [cx - ew, cy - eh * 1.55, cx + ew, cy - eh * 0.28], frame=CYAN)
    device(cd, [cx - ew * 0.62, cy + eh * 0.05, cx + ew * 0.62, cy + eh * 1.05])
    img = Image.alpha_composite(img.convert("RGBA"), crisp).convert("RGB")

    img = scene_wordmark(img, (w * 0.55, h * 0.46), int(h * 0.30), spacing=6)

    rule = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(rule).line([(w * 0.36, h * 0.70), (w * 0.74, h * 0.70)],
                              fill=MAGENTA + (150,), width=4)
    img = add_glow(img, rule, radius=8, opacity=0.8)
    img = Image.alpha_composite(img.convert("RGBA"), rule).convert("RGB")

    img = vignette(img, 0.30)
    img.save(os.path.join(ART, "banner.png"))


if __name__ == "__main__":
    os.makedirs(SCE, exist_ok=True)
    os.makedirs(ART, exist_ok=True)
    gen_icon()
    print("icon0.png done")
    gen_pic0()
    print("pic0.png done")
    gen_pic1()
    print("pic1.png done")
    gen_splash()
    print("splash.png done")
    gen_banner()
    print("banner.png done")
