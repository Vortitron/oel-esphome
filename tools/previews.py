#!/usr/bin/env python3
"""
previews.py - the builder's catalogue (docs/clips.json) and previews
(docs/previews/), made by playing each clip through host/render.

    make -C host
    python3 tools/previews.py [--lkd DIR]

For each clip in the catalogue:

    previews/<name>.png        a still, at the clip's own size
    previews/<name>.gif        the clip playing at its own size, a pass of it
                               (a meter: six seconds of its made-up music)

and in clips.json its palette and each colour's share of lit dots on a 1-bit
panel, from which the builder draws its OLED preview live off the GIF.

--lkd is a folder of .lkd files already downloaded; otherwise they are
fetched from carozerra into tools/.cache/.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'components', 'oel'))
import lkd  # noqa: E402

RENDER = os.path.join(ROOT, 'host', 'render')
OUT = os.path.join(ROOT, 'docs')
FPS = 1000 / lkd.FRAME_MS

# Names for the clips, where the file name doesn't say. Seen, not official.
TITLES = {
    'movie1': 'Mountain Flight', 'movie2': 'City Drive', 'movie3': 'Flowers', 'movie4': 'Circuit',
    'movie5': 'Carrozzeria', 'movie6': 'Jungle', 'movie7': 'Fight Movie', 'movie8_f': 'Dolphins',
    'movie9_f': 'Tunnel', 'movie10_f': 'Machine',
    'alt_airship': 'Airship', 'alt_diverdolphins': 'Diver and Dolphins', 'alt_dragonrider': 'Dragon Rider',
    'alt_metropolis': 'Metropolis', 'alt_waterboard': 'Waterboard', 'alt_wildlife': 'Wildlife',
    'alt_wrc': 'Rally',
    'color_01_nightcruising': 'Night Cruising', 'color_02_greatbarrierreef': 'Great Barrier Reef',
    'color_03_redplanet': 'Red Planet', 'color_04_island': 'Island', 'color_05_firedragon': 'Fire Dragon',
    'color_06_racingcart': 'Racing Kart', 'color_07_motogp': 'MotoGP',
}


def category(name):
    if 'meter' in name:
        return 'Meters'
    if 'still' in name:
        return 'Stills'
    if 'bgv' in name:
        return 'Backgrounds'
    if name.startswith('color_'):
        return 'Colour movies'
    if name.startswith('alt_'):
        return 'Small-panel movies'
    return 'Movies'


def title(name):
    if name in TITLES:
        return TITLES[name]
    digits = ''.join(ch for ch in name if ch.isdigit()).lstrip('0') or '1'
    if 'meter' in name:
        return ('Small meter ' if name.startswith('alt_') else 'Level meter ') + digits
    if 'still' in name:
        prefix = 'Colour still ' if name.startswith('color_') else 'Small still ' if name.startswith('alt_') else 'Still '
        return prefix + (name.rsplit('_', 1)[-1].upper() if name.startswith('alt_') else digits)
    if 'bgv' in name:
        return ('Colour background ' if name.startswith('color_') else 'Background ') + digits
    return name


def lkd_bytes(name, folder):
    if folder:
        return open(os.path.join(folder, name + '.lkd'), 'rb').read()
    cache = os.path.join(HERE, '.cache')
    os.makedirs(cache, exist_ok=True)
    path = os.path.join(cache, name + '.lkd')
    if not os.path.exists(path):
        with urllib.request.urlopen(lkd.catalogue_url(name)) as r, open(path, 'wb') as f:
            shutil.copyfileobj(r, f)
    return open(path, 'rb').read()


def gif(frames_dir, out, colours):
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-framerate', '%.4f' % FPS, '-i',
                    os.path.join(frames_dir, '%04d.ppm'), '-vf',
                    'split[a][b];[a]palettegen=max_colors=%d:reserve_transparent=0[p];[b][p]paletteuse=dither=none'
                    % max(colours, 4), out], check=True)


def render(blob_path, screen, frames, out_dir):
    for f in os.listdir(out_dir):
        os.remove(os.path.join(out_dir, f))
    subprocess.run([RENDER, blob_path, screen, str(frames), out_dir, '%.4f' % FPS, '7'], check=True,
                   capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--lkd', help='folder of .lkd files already downloaded')
    args = ap.parse_args()
    os.makedirs(os.path.join(OUT, 'previews'), exist_ok=True)
    clips = {}
    with tempfile.TemporaryDirectory() as tmp:
        blob_path = os.path.join(tmp, 'clip.oel')
        frames = os.path.join(tmp, 'f')
        os.makedirs(frames)
        for name in lkd.CATALOGUE:
            blob, info = lkd.encode(name, lkd_bytes(name, args.lkd))
            open(blob_path, 'wb').write(blob)
            kind = info['kind']
            n = 1 if kind == lkd.KIND_STILL else int(6 * FPS) if kind == lkd.KIND_METER else info['frames']
            render(blob_path, '%dx%d' % (info['width'], info['height']), n, frames)
            poster = os.path.join(frames, '%04d.ppm' % (n // 3))
            # No date chunks, so a rerun only changes the stills that changed.
            subprocess.run(['convert', poster, '-define', 'png:exclude-chunks=date,time',
                            os.path.join(OUT, 'previews', name + '.png')], check=True)
            if kind != lkd.KIND_STILL:
                gif(frames, os.path.join(OUT, 'previews', name + '.gif'), info['colours'])
            cols = blob[11]
            palette = blob[16:16 + 3 * cols]
            clips[name] = {
                'title': title(name), 'category': category(name), 'width': info['width'],
                'height': info['height'], 'frames': info['frames'], 'colours': info['colours'],
                'kind': ['clip', 'meter', 'still'][kind], 'bytes': info['bytes'],
                'palette': [palette[i:i + 3].hex() for i in range(0, len(palette), 3)],
                'levels': list(blob[16 + 3 * cols:16 + 4 * cols]),
            }
            print('%-28s %-24s %7d bytes' % (name, clips[name]['title'], info['bytes']))
    # Firmware without clips, from real builds (ESPHome 2026.10.1, ESP-IDF, Wi-Fi + API + OTA, the
    # examples' screens): the C3 + SH1106 and S3 + ILI9341 examples, less their clips. The other two
    # pairings are moved by how after-dark-esphome's measured bases differ between colour and 1-bit.
    # The SSD1322 (grey) from a build of examples/oel-c3-ssd1322.yaml less its clips and the player,
    # moved onto the S3 as the 1-bit screen is.
    # A 1-bit screen's config also has the frame-rate modulation switch: 1680 bytes.
    base = {'esp32c3': {'mono': 779257 + 1680, 'colour': 779257 + 51642, 'grey': 782414},
            'esp32s3': {'colour': 763114, 'mono': 763114 - 47578 + 1680, 'grey': 763114 - 47578 + (782414 - 779257)}}
    app_slot = {'4MB': 0x1C0000, '8MB': 0x3C0000, '16MB': 0x7C0000}
    report = {'commit': lkd.CAROZERRA_COMMIT, 'base': base, 'app_slot': app_slot, 'clips': clips}
    json.dump(report, open(os.path.join(OUT, 'clips.json'), 'w'), indent=1)
    print(len(clips), 'clips')


if __name__ == '__main__':
    main()
