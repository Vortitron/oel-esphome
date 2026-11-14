"""Pioneer OEL animations (.lkd) into the clips the oel component plays.

Plain Python, so it runs in ESPHome's own environment at build time.

An .lkd is a 20-byte header ("zLKD", version, two unknowns, frame count),
then a gzip of a tar holding one 24-bit BMP with the frames stacked top to
bottom. carozerra (https://github.com/youxufkhan/carozerra) worked the format
out and keeps the clips Pioneer shipped; CATALOGUE names them.

A clip goes onto the device as one blob:

    0   "OEL1"
    4   u16 width, u16 height, u16 frames
    10  u8 bits a pixel (2 or 4), u8 colours, u8 kind, u8 ms a frame
    14  u16 0
    16  colours x 3 bytes of RGB, darkest first
        colours x 1 byte: how much of a 1-bit panel's dither each one lights, 0-16
        padding to 4 bytes
        u32 x (frames + 1): where each frame starts, from the first frame
        the frames: packed pixels, leftmost in the top bits, each frame a
        zlib stream unless that came out no smaller (its length then equals
        the packed size)

Kinds: 0 a clip that loops, 1 a level meter (a frame per level, bounced by
the player), 2 a still.
"""
from collections import Counter
import gzip
import io
import struct
import tarfile
import zlib

CAROZERRA = "youxufkhan/carozerra"
# A fork holding the same commit, should the original go.
CAROZERRA_MIRROR = "Vortitron/carozerra"
CAROZERRA_COMMIT = "d9602015196a92c1bfabcd1cd2f13c50e0c9cb9a"

KIND_CLIP, KIND_METER, KIND_STILL = 0, 1, 2
FRAME_MS = 60  # the stereos' rate, as carozerra plays them

# The clips in carozerra's assets/clips at CAROZERRA_COMMIT.
CATALOGUE = [
    "alt_airship", "alt_bgv1", "alt_bgv2", "alt_bgv3", "alt_bgv4", "alt_diverdolphins", "alt_dragonrider",
    "alt_meter_li01", "alt_meter_li02", "alt_meter_li03", "alt_meter_li04", "alt_meter_li05", "alt_meter_li06",
    "alt_meter_li07", "alt_meter_li08", "alt_metropolis", "alt_still_bgp_e1", "alt_still_bgp_e2",
    "alt_still_bgp_e3", "alt_still_bgp_e4", "alt_still_bgp_e5", "alt_waterboard", "alt_wildlife", "alt_wrc",
    "color_01_nightcruising", "color_02_greatbarrierreef", "color_03_redplanet", "color_04_island",
    "color_05_firedragon", "color_06_racingcart", "color_07_motogp", "color_bgv01", "color_bgv02",
    "color_bgv03", "color_bgv04", "color_bgv05", "color_bgv06", "color_bgv07", "color_still_bgp01",
    "color_still_bgp02", "color_still_bgp03", "color_still_bgp04", "color_still_bgp05", "color_still_bgp06",
    "color_still_bgp07", "color_still_bgp08", "color_still_bgp09", "color_still_bgp10", "meter_level01",
    "meter_level02", "meter_level03", "meter_level04", "meter_level05", "meter_level06_f", "meter_level07_f",
    "movie1", "movie2", "movie3", "movie4", "movie5", "movie6", "movie7", "movie8_f", "movie9_f", "movie10_f",
    "still_bgp01", "still_bgp02", "still_bgp03", "still_bgp04", "still_bgp05", "still_bgp06", "still_bgp07",
    "still_bgp08", "still_bgp09", "still_bgp_f_10", "still_bgp_f_11", "still_bgp_f_12", "still_bgp_f_13",
    "still_bgp_f_14", "still_bgp_f_15", "still_bgp_f_16", "still_bgp_f_17", "still_bgp_f_18",
]


def catalogue_url(name, repo=CAROZERRA):
    return f"https://raw.githubusercontent.com/{repo}/{CAROZERRA_COMMIT}/assets/clips/{name}.lkd"


def read_lkd(data):
    """(width, height, frames), each frame a flat list of 0xRRGGBB."""
    if data[:4] != b"zLKD":
        raise ValueError("not an .lkd (no zLKD at the start)")
    count = struct.unpack("<I", data[16:20])[0]
    tar = tarfile.open(fileobj=io.BytesIO(gzip.decompress(data[20:])))
    member = next(m for m in tar.getmembers() if m.isfile())
    bmp = tar.extractfile(member).read()
    if bmp[:2] != b"BM" or struct.unpack("<H", bmp[28:30])[0] != 24:
        raise ValueError("expected a 24-bit BMP inside the .lkd")
    offset = struct.unpack("<I", bmp[10:14])[0]
    w, h = struct.unpack("<ii", bmp[18:26])
    stride = (w * 3 + 3) & ~3
    rows = abs(h)
    if count < 1 or rows % count:
        raise ValueError(f"{rows} rows don't split into {count} frames")
    fh = rows // count
    pixels = []
    for y in range(rows):
        src = rows - 1 - y if h > 0 else y  # bottom-up unless the height is negative
        line = bmp[offset + src * stride: offset + src * stride + w * 3]
        pixels.extend(line[i + 2] << 16 | line[i + 1] << 8 | line[i] for i in range(0, w * 3, 3))
    size = w * fh
    return w, fh, [pixels[i * size:(i + 1) * size] for i in range(count)]


def _lum(c):
    return (77 * (c >> 16) + 150 * (c >> 8 & 255) + 29 * (c & 255)) >> 8


def _median_cut(counts, n):
    """n colours for a histogram {0xRRGGBB: count}, by median cut."""
    boxes = [list(counts.items())]
    while len(boxes) < n:
        best, best_score, best_ch = None, -1, 0
        for i, box in enumerate(boxes):
            if len(box) < 2:
                continue
            for ch in (16, 8, 0):
                vals = [c >> ch & 255 for c, _ in box]
                score = (max(vals) - min(vals)) * sum(k for _, k in box)
                if score > best_score:
                    best, best_score, best_ch = i, score, ch
        if best is None:
            break
        box = sorted(boxes.pop(best), key=lambda e: e[0] >> best_ch & 255)
        total, run = sum(k for _, k in box), 0
        for cut, (_, k) in enumerate(box):
            run += k
            if run * 2 >= total:
                break
        cut = min(max(cut + 1, 1), len(box) - 1)
        boxes += [box[:cut], box[cut:]]
    palette = []
    for box in boxes:
        total = sum(k for _, k in box)
        r = sum((c >> 16) * k for c, k in box) // total
        g = sum((c >> 8 & 255) * k for c, k in box) // total
        b = sum((c & 255) * k for c, k in box) // total
        palette.append(r << 16 | g << 8 | b)
    return palette


def kind_of(name, frames):
    if "meter" in name:
        return KIND_METER
    return KIND_STILL if frames == 1 else KIND_CLIP


def encode(name, data):
    """The blob for an .lkd, and a few facts about it."""
    w, h, frames = read_lkd(data)
    counts = Counter(c for f in frames for c in f)
    if len(counts) <= 16:
        palette = sorted(counts, key=_lum)
        index = {c: i for i, c in enumerate(palette)}
    else:
        # The level meters are drawn with soft glows in thousands of colours.
        palette = sorted(_median_cut(counts, 16), key=_lum)

        def nearest(c):
            r, g, b = c >> 16, c >> 8 & 255, c & 255
            return min(range(len(palette)), key=lambda i: (r - (palette[i] >> 16)) ** 2
                       + (g - (palette[i] >> 8 & 255)) ** 2 + (b - (palette[i] & 255)) ** 2)

        index = {c: nearest(c) for c in counts}
    bits = 2 if len(palette) <= 4 else 4
    top = max(_lum(c) for c in palette) or 1
    mono = [round(16 * (_lum(c) / top) ** 3) for c in palette]

    out = bytearray()
    offsets = [0]
    raw_total = 0
    per = 8 // bits
    for f in frames:
        idx = [index[c] for c in f]
        idx += [0] * (-len(idx) % per)
        packed = bytearray()
        for i in range(0, len(idx), per):
            v = 0
            for k in range(per):
                v = v << bits | idx[i + k]
            packed.append(v)
        z = zlib.compress(bytes(packed), 9)
        out += z if len(z) < len(packed) else packed
        offsets.append(len(out))
        raw_total += len(packed)

    kind = kind_of(name, len(frames))
    head = b"OEL1" + struct.pack("<HHHBBBBH", w, h, len(frames), bits, len(palette), kind, FRAME_MS, 0)
    pal = b"".join(struct.pack("BBB", c >> 16, c >> 8 & 255, c & 255) for c in palette) + bytes(mono)
    pal += bytes(-(len(head) + len(pal)) % 4)
    blob = head + pal + struct.pack(f"<{len(offsets)}I", *offsets) + bytes(out)
    info = {"width": w, "height": h, "frames": len(frames), "colours": len(palette), "kind": kind,
            "bytes": len(blob), "raw": raw_total}
    return blob, info
