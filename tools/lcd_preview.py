#!/usr/bin/env python3
"""lcd_preview.py - an experiment: how an OEL clip could look on a character
LCD (the HD44780 1602 sort), simulated, as a GIF.

A 1602 is 16x2 cells of 5x8 dots with a dot's gap between cells, so 80x16
dots on a 95x17 grid, and only eight cells' worth of shapes of its own at a
time (CGRAM); every other cell must be a ROM character (space, the full block
0xFF and a few punctuation marks that help). Each frame here:

  1. the clip is fitted to the 95x17 grid (`full` squashes the whole frame,
     `crop` keeps the aspect and takes the middle band) by area averaging,
     the gaps dropped, and the contrast stretched over the whole clip;
  2. the eight glyphs are chosen greedily to cut the total error over the 32
     cells most, refined Lloyd-style, preferring last frame's glyphs so they
     don't shimmer;
  3. every cell gets its nearest code (or, with --grey, the nearest pair of
     codes shown on alternate passes by frame-rate modulation, so a dot is
     off, half or full);
  4. it is drawn as a blue backlit panel, with the liquid crystal's slow
     response smearing one update into the next.

    python3 tools/lcd_preview.py movie8_f --fit crop --out DIR
    python3 tools/lcd_preview.py alt_diverdolphins --grey --fit full --out DIR

It prints the bytes a frame would take precomputed, and the I2C time to send
it through a PCF8574 backpack (about 0.78 ms a byte).
"""
import argparse
import os
import sys
import urllib.request

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "components", "oel"))
import lkd  # noqa: E402

COLS, ROWS = 16, 2
GW, GH = COLS * 6 - 1, ROWS * 9 - 1  # the dot grid, gaps and all
BYTE_MS = 25.0 / 32  # a byte through the PCF8574 at 400kHz (32 cells in ~25 ms)

# A00 ROM characters worth having as free stand-ins (rows top first, 5 bits).
ROM = {
    0x20: [0, 0, 0, 0, 0, 0, 0, 0],
    0xFF: [0x1F] * 8,
    ord("."): [0, 0, 0, 0, 0, 0x0C, 0x0C, 0],
    ord("-"): [0, 0, 0, 0x1F, 0, 0, 0, 0],
    ord("_"): [0, 0, 0, 0, 0, 0, 0x1F, 0],
    ord("'"): [0x0C, 0x04, 0x08, 0, 0, 0, 0, 0],
    ord("|"): [0x04] * 7 + [0],
    ord("="): [0, 0, 0x1F, 0, 0x1F, 0, 0, 0],
    0xDB: [0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F, 0],  # a box, in A00
}


def bits_to_dots(rows):
    return np.array([[(r >> (4 - x)) & 1 for x in range(5)] for r in rows], dtype=np.float32).reshape(40)


ROM_CODES = list(ROM)
ROM_DOTS = np.stack([bits_to_dots(ROM[c]) for c in ROM_CODES])


def fetch(name, cache):
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, name + ".lkd")
    if not os.path.exists(path):
        for repo in (lkd.CAROZERRA, lkd.CAROZERRA_MIRROR):
            try:
                with urllib.request.urlopen(lkd.catalogue_url(name, repo), timeout=30) as r:
                    data = r.read()
                break
            except OSError:
                continue
        else:
            raise SystemExit(f"couldn't fetch {name}")
        with open(path, "wb") as f:
            f.write(data)
    w, h, frames = lkd.read_lkd(open(path, "rb").read())
    a = np.array(frames, dtype=np.uint32).reshape(len(frames), h, w)
    rgb = np.stack([(a >> 16) & 255, (a >> 8) & 255, a & 255], -1).astype(np.uint8)
    lum = (77 * rgb[..., 0].astype(np.float32) + 150 * rgb[..., 1] + 29 * rgb[..., 2]) / 256
    return rgb, lum / max(lum.max(), 1)


def box_mean(x, r):
    k = 2 * r + 1
    c = np.pad(np.pad(x, r, mode="edge").cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def smooth_bright(lum, k):
    """Bright and smooth over brightness alone: the clips' dolphins are even
    tones against water that is a dither of light and dark, which averages to
    the same grey once shrunk. Local mean less k local deviations."""
    out = np.empty_like(lum)
    r = max(1, lum.shape[2] // 80)
    for i, f in enumerate(lum):
        m = box_mean(f, r)
        sd = np.sqrt(np.maximum(box_mean(f * f, r) - m * m, 0))
        out[i] = np.clip(m - k * sd, 0, 1)
    return out


def fit_box(w, h, fit):
    """The part of the frame that goes onto the grid."""
    if fit == "full":
        return 0, 0, w, h
    ch = w * GH / GW  # keep the aspect: the middle band
    if ch <= h:
        y = (h - ch) / 2
        return 0, y, w, y + ch
    cw = h * GW / GH
    x = (w - cw) / 2
    return x, 0, x + cw, h


def follow(lum, zoom):
    """A window 1/zoom of the width, at the panel's aspect, panning slowly
    after the brightest part of the picture (with --smooth, the dolphins)."""
    f, h, w = lum.shape
    bw = w / zoom
    bh = min(h, bw * GH / GW)
    boxes, cx, cy = [], w / 2, h / 2
    # Twice round, so the first frames start where the loop leaves off.
    for i in list(range(f)) * 2:
        v = lum[i]
        m = v > np.percentile(v, 90)
        if m.any():
            ys, xs = np.nonzero(m)
            cx += 0.15 * (xs.mean() - cx)
            cy += 0.15 * (ys.mean() - cy)
        x0 = min(max(cx - bw / 2, 0), w - bw)
        y0 = min(max(cy - bh / 2, 0), h - bh)
        boxes.append((x0, y0, x0 + bw, y0 + bh))
    return boxes[f:]


def cells_of(lum, fit, lo_pct, hi_pct, gamma, zoom=1):
    """(frames, 32, 40) targets in 0-1, and the box used for each frame."""
    f, h, w = lum.shape
    boxes = follow(lum, zoom) if zoom > 1 else [fit_box(w, h, fit)] * f
    grids = []
    for i in range(f):
        im = Image.fromarray(lum[i], "F").resize((GW, GH), Image.BOX, box=boxes[i])
        grids.append(np.asarray(im))
    g = np.stack(grids)
    lo, hi = np.percentile(g, lo_pct), np.percentile(g, hi_pct)
    g = np.clip((g - lo) / max(hi - lo, 1e-6), 0, 1) ** gamma
    out = np.zeros((f, ROWS * COLS, 40), np.float32)
    for r in range(ROWS):
        for c in range(COLS):
            out[:, r * COLS + c] = g[:, r * 9:r * 9 + 8, c * 6:c * 6 + 5].reshape(f, 40)
    return out, boxes


BAYER = (np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) + 0.5) / 16


def dither(t):
    """Ordered dither in panel coordinates (the gaps count, as on the glass)."""
    out = t.copy()
    for r in range(ROWS):
        for c in range(COLS):
            ys, xs = np.mgrid[r * 9:r * 9 + 8, c * 6:c * 6 + 5]
            out[..., r * COLS + c, :] = (t[..., r * COLS + c, :] > BAYER[ys % 4, xs % 4].reshape(40)).astype(np.float32)
    return out


class Solver:
    """Picks a frame's eight glyphs and every cell's code(s)."""

    def __init__(self, grey, glyphs=8, stick=1.5):
        self.grey, self.n, self.stick = grey, glyphs, stick
        self.prev = []  # last frame's glyphs, slot order

    def errors(self, t, codes):
        """err[cell, a, b]: a cell shown as codes a and b alternately (a == b: one code)."""
        V = codes
        gv = t @ V.T
        vv = V @ V.T
        d = np.diag(vv)
        if not self.grey:
            e = (t * t).sum(1)[:, None] - 2 * gv + d[None, :]
            return e[:, :, None] * np.ones((1, 1, 1)), None
        pair = (d[:, None] + 2 * vv + d[None, :]) / 4
        e = (t * t).sum(1)[:, None, None] - gv[:, :, None] - gv[:, None, :] + pair[None]
        return e, pair

    def best(self, t, codes):
        e, _ = self.errors(t, codes)
        if not self.grey:
            e = e[:, :, 0]
            k = e.argmin(1)
            return e[np.arange(len(t)), k].sum(), np.stack([k, k], 1)
        flat = e.reshape(len(t), -1)
        k = flat.argmin(1)
        n = codes.shape[0]
        return flat[np.arange(len(t)), k].sum(), np.stack([k // n, k % n], 1)

    def solve(self, t):
        # Candidates: each cell at a few thresholds, and last frame's glyphs.
        cand = []
        for th in ((0.25, 0.5, 0.75) if self.grey else (0.5,)):
            cand.extend((t > th).astype(np.float32))
        cand.extend(self.prev)
        cand = np.unique(np.array(cand), axis=0) if cand else np.zeros((0, 40), np.float32)
        cand = np.array([c for c in cand if c.sum() > 0 and c.sum() < 40 and
                         not any((c == r).all() for r in ROM_DOTS)])
        prevset = {c.tobytes() for c in self.prev}
        chosen = []
        for _ in range(self.n):
            if len(cand) == 0:
                break
            base = np.concatenate([ROM_DOTS] + ([np.array(chosen)] if chosen else []))
            best_e, best_i = None, None
            for i, c in enumerate(cand):
                e, _ = self.best(t, np.concatenate([base, c[None]]))
                if c.tobytes() not in prevset:
                    e += self.stick
                if best_e is None or e < best_e:
                    best_e, best_i = e, i
            chosen.append(cand[best_i])
            cand = np.delete(cand, best_i, 0)
        glyphs = np.array(chosen) if chosen else np.zeros((0, 40), np.float32)
        # Lloyd: each glyph's dots set to suit the cells using it.
        for _ in range(3):
            codes = np.concatenate([ROM_DOTS, glyphs]) if len(glyphs) else ROM_DOTS
            _, pick = self.best(t, codes)
            for s in range(len(glyphs)):
                k = len(ROM_DOTS) + s
                score = np.zeros((2, 40))
                used = False
                for i, (a, b) in enumerate(pick):
                    if a != k and b != k:
                        continue
                    used = True
                    for bit in (0, 1):
                        va = bit if a == k else codes[a]
                        vb = bit if b == k else codes[b]
                        score[bit] += (t[i] - (va + vb) / 2) ** 2
                if used:
                    glyphs[s] = (score[1] < score[0]).astype(np.float32)
        codes = np.concatenate([ROM_DOTS, glyphs]) if len(glyphs) else ROM_DOTS
        err, pick = self.best(t, codes)
        # Keep a glyph in its slot from last frame, so only new ones are sent.
        slots = [None] * self.n
        rest = []
        for g in glyphs:
            j = next((j for j, p in enumerate(self.prev) if (p == g).all() and slots[j] is None), None)
            if j is None:
                rest.append(g)
            else:
                slots[j] = g
        sent = len(rest)
        for g in rest:
            slots[next(j for j, v in enumerate(slots) if v is None)] = g
        self.prev = [s for s in slots if s is not None]
        shown = (codes[pick[:, 0]] + codes[pick[:, 1]]) / 2
        modulated = int((pick[:, 0] != pick[:, 1]).sum())
        return shown, err, sent, modulated


def unlimited(t, grey):
    """Every cell exact (no eight-glyph limit): what the dots alone allow."""
    if grey:
        return np.round(t * 2) / 2
    return (t > 0.5).astype(np.float32)


# The look of a blue 1602: dots a shade off the backlight, lit ones near white.
BG = np.array([22, 52, 190], np.float32)
OFF = np.array([34, 70, 214], np.float32)
ON = np.array([226, 236, 255], np.float32)
SCALE = 6


def panel(dots):
    """(32, 40) dot levels into an RGB image of the panel."""
    pad = 2 * SCALE
    img = np.zeros((GH * SCALE + 2 * pad, GW * SCALE + 2 * pad, 3), np.float32)
    img[:] = BG
    for r in range(ROWS):
        for c in range(COLS):
            d = dots[r * COLS + c].reshape(8, 5)
            for y in range(8):
                for x in range(5):
                    v = d[y, x]
                    px = pad + (c * 6 + x) * SCALE
                    py = pad + (r * 9 + y) * SCALE
                    img[py:py + SCALE - 1, px:px + SCALE - 1] = OFF + (ON - OFF) * v
    return img.astype(np.uint8)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("clip")
    ap.add_argument("--fit", choices=["full", "crop"], default="crop")
    ap.add_argument("--grey", action="store_true", help="three levels a dot, by frame-rate modulation")
    ap.add_argument("--dither", action="store_true", help="ordered dither before fitting glyphs (1-bit)")
    ap.add_argument("--unlimited", action="store_true", help="no glyph limit (a reference)")
    ap.add_argument("--smooth", type=float, default=0,
                    help="favour smooth bright areas (local mean less this many deviations); 2 suits the dolphins")
    ap.add_argument("--zoom", type=float, default=1, help="zoom in this much, panning after the bright parts")
    ap.add_argument("--lo", type=float, default=8, help="percentile that goes black")
    ap.add_argument("--hi", type=float, default=92, help="percentile that goes white")
    ap.add_argument("--gamma", type=float, default=1.0)
    ap.add_argument("--fps", type=float, default=10, help="LCD updates a second")
    ap.add_argument("--response", type=float, default=0.45, help="share of the last image left after an update")
    ap.add_argument("--cache", default=".")
    ap.add_argument("--out", default=".")
    ap.add_argument("--tag", default="")
    a = ap.parse_args()

    rgb, lum = fetch(a.clip, a.cache)
    if a.smooth:
        lum = smooth_bright(lum, a.smooth)
    targets, boxes = cells_of(lum, a.fit, a.lo, a.hi, a.gamma, a.zoom)
    if a.dither:
        targets = dither(targets)
    solver = Solver(a.grey)
    nf = len(targets)
    step = 1000.0 / a.fps
    ticks = int(nf * lkd.FRAME_MS / step)
    shown_prev = None
    frames, sent_total, mod_total, err_total = [], 0, 0, 0.0
    cache = {}
    for k in range(ticks):
        i = int(k * step / lkd.FRAME_MS) % nf
        if i not in cache:
            if a.unlimited:
                cache[i] = (unlimited(targets[i], a.grey), 0.0, 8, 0)
            else:
                cache[i] = solver.solve(targets[i])
        shown, err, sent, mod = cache[i]
        sent_total += sent
        mod_total += mod
        err_total += float(((targets[i] - shown) ** 2).mean())
        view = shown if shown_prev is None else shown_prev * a.response + shown * (1 - a.response)
        shown_prev = view
        frames.append(panel(view))
    tag = a.tag or f"{a.clip}-{a.fit}-{'grey' if a.grey else 'dither' if a.dither else '1bit'}" + (
        f"-smooth{a.smooth:g}" if a.smooth else "") + (f"-zoom{a.zoom:g}" if a.zoom > 1 else "") + ("-unlimited" if a.unlimited else "")
    os.makedirs(a.out, exist_ok=True)
    gif = os.path.join(a.out, tag + ".gif")
    ims = [Image.fromarray(f) for f in frames]
    ims[0].save(gif, save_all=True, append_images=ims[1:], duration=int(step), loop=0, optimize=True)

    # Source over panel, a few moments, to judge by.
    pick = np.linspace(0, len(frames) - 1, 6).astype(int)
    rows = []
    for k in pick:
        i = int(k * step / lkd.FRAME_MS) % nf
        src = Image.fromarray(rgb[i])
        x0, y0, x1, y1 = boxes[i]
        src = src.crop((int(x0), int(y0), int(round(x1)), int(round(y1))))
        pw = frames[k].shape[1]
        src = np.asarray(src.resize((pw, int(pw * src.height / src.width)), Image.NEAREST))
        rows += [src, frames[k], np.full((6, pw, 3), 255, np.uint8)]
    Image.fromarray(np.concatenate(rows)).save(os.path.join(a.out, tag + "-compare.png"))

    upd = ticks
    per_frame = sent_total / upd * 9 + COLS * ROWS + 2 * ROWS  # glyphs (cmd + 8) + cells + row addresses
    print(f"{tag}: {gif}")
    print(f"  mean error {err_total / upd:.4f}; glyphs re-sent {sent_total / upd:.1f}/update; "
          f"modulated cells {mod_total / upd:.1f}/32")
    print(f"  ~{per_frame:.0f} bytes an update over I2C = {per_frame * BYTE_MS:.0f} ms "
          f"(max ~{1000 / (per_frame * BYTE_MS):.0f} updates/s); precomputed {8 * 8 + 32}{' + 32' if a.grey else ''} "
          f"bytes a frame -> {nf * (96 + (32 if a.grey else 0)) / 1024:.1f} KB for {nf} frames")


if __name__ == "__main__":
    main()
