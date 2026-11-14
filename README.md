# OEL for ESPHome

The animations from Pioneer's organic-EL car stereos of the early 2000s (the
dolphins, the colour movies, the level meters) on an ESP32's display, as an
ESPHome external component.

**[Open the builder](https://vortitron.github.io/oel-esphome/)** to browse
all 83 clips (hover one to play it), pick some, see whether they fit, and
copy a config. Or add the component yourself:

```yaml
external_components:
  - source: github://Vortitron/oel-esphome@v1.1.0
    components: [oel]

oel:
  display_id: screen
  clips:
    - movie8_f                   # dolphins underwater
    - alt_diverdolphins          # a diver with dolphins
    - meter_level01              # a level meter
  clip: cycle                    # each in turn; or name one, or "off"

select:
  - platform: oel                # choose the clip from Home Assistant
    name: Dashboard clip
```

A sister project to [After Dark for ESPHome](https://github.com/Vortitron/after-dark-esphome),
whose own `dolphins` saver is an original drawn in this style.

## The clips

The clips are Pioneer's: the `.lkd` files their OEL Screen Saver Studio
loaded into the stereos. [carozerra](https://github.com/youxufkhan/carozerra)
worked out the format and keeps the 83 that survive; this repository holds
none of them. A clip named in the config is fetched from carozerra at a fixed
commit when ESPHome builds the firmware, and kept in ESPHome's cache after
that.

| Kind | Size | What the player does |
|---|---|---|
| Movies (`movie1`–`movie10_f`) | 256×64, 60 frames, 4 cyan shades | loops at the stereo's 60 ms a frame |
| Colour movies (`color_01_…`–`color_07_…`) | 192×48, 150 frames, 16 colours | loops |
| Small-panel movies (`alt_…`) | 128×33, 150 frames, 4 shades | loops |
| Backgrounds (`alt_bgv…`, `color_bgv…`) | as their panel | loops |
| Meters (`meter_level…`, `alt_meter_li…`) | 96×96 or 64×66, two banks of levels | bounces to made-up music, switching bank now and then |
| Stills (`still_bgp…`, `color_still_bgp…`, `alt_still_bgp_e…`) | one frame | shows it |

Your own `.lkd` works too:

```yaml
    clips:
      - file: clips/my_clip.lkd            # beside the config
      - url: https://example.com/other.lkd
        name: other
```

### On the screen

`aspect:` sets how a clip meets the screen, and a select can change it from
Home Assistant:

| `aspect` | |
|---|---|
| `letterbox` | all of the clip, its shape kept, black bars around |
| `crop` | the screen filled, its shape kept; what overflows pans slowly across and back |
| `stretch` | the screen filled edge to edge, its shape not kept |
| `auto` (default) | letterbox, except crop where a clip would have to shrink but the screen is tall enough (a 256×64 movie on a 128×64 OLED): halving a dithered picture loses it |

Scaled up, each dot becomes a block, with a gap after it once it is 3 pixels
or more each way, like the stereo's matrix. Scaled down, a pixel is the
average of the dots it covers. On a 1-bit panel each colour lights a share of
its dots, dithered.

### Real meters

The level meters bounce to made-up music unless `meter:` gives them
something to show: any ESPHome sensor, or a lambda.

```yaml
oel:
  # ...
  meter:
    sensor: house_power      # the sensor's state ...
    min: 0                   # ... at min reads empty,
    max: 5000                # ... at max full
    style: 1                 # each meter has two looks, 1 or 2

sensor:
  - platform: homeassistant  # a Home Assistant entity
    id: house_power
    entity_id: sensor.house_power
```

It rises at once and falls back steadily, like a real meter, so a sensor that
updates every few seconds still moves smoothly. Some ideas:

- A media player's volume: a `homeassistant` sensor with
  `attribute: volume_level`, `min: 0`, `max: 1`.
- Sound in the room: ESPHome's `sound_level` sensor on an I²S microphone,
  in dB, for a meter that really does follow the music.
- Anything worked out: `lambda: 'return id(a).state - id(b).state;'` in
  place of `sensor:`.

A sensor with no value yet (or unavailable) reads as empty.

Only what changed since the last frame is drawn.

### Flash

Dithered pictures don't compress much, but each frame is stored zlib
compressed where that helps (20 to 50%), and unpacked one frame at a time
with the inflate routine in the ESP32's ROM. Roughly:

| Kind | Flash each |
|---|---|
| a movie | 120–250 KB |
| a colour movie | 300–400 KB |
| a small-panel movie | 100–150 KB |
| a meter | 30–60 KB |
| a still | 2–5 KB |

On a 4 MB board the firmware has a 1.8 MB slot (two, for OTA updates), of
which ESPHome with Wi-Fi, the API, OTA and a display takes about 0.75 MB.
The builder adds it up for you.

## Options

```yaml
oel:
  display_id: screen     # a pixel display, colour or 1-bit
  clips: [...]           # catalogue names, or file:/url: (with name:)
  clip: cycle            # the one to start with, "cycle", or "off"
  clip_time: 20s         # each clip's turn when cycling (a movie always
                         # finishes its pass)
  mode: always           # or screensaver
  aspect: auto           # letterbox, crop or stretch
  meter: {...}           # drive the meters (see Real meters)
```

In `mode: always` the display should have no `lambda` or `pages`, and needs
`update_interval: never` and `auto_clear_enabled: false`; the component
pushes the frames. In `mode: screensaver` the display keeps its own drawing,
and the actions hand it over and back:

```yaml
  - oel.start            # play the selected clip (or cycle)
  - oel.stop             # give the display back and redraw it
  - oel.next             # another clip now
```

`oel.is_running` is a condition.

```yaml
select:
  - platform: oel              # the clip, "cycle" or "off"
    name: Dashboard clip
  - platform: oel              # the aspect
    type: aspect
    name: Dashboard clip aspect
```

## Layout

- `components/oel/`: the component. `lkd.py` turns an `.lkd` into the blob
  the player takes (palette, then a compressed frame each; see its
  docstring). `oel_player.*` plays a blob onto a `Canvas` and knows nothing of
  ESPHome; `oel_component.*`, `__init__.py` and `select.py` are the ESPHome
  end.
- `host/`: the player on the desktop (`make -C host`), writing frames as
  PPM: `host/render CLIP.oel 128x64m 200 OUTDIR`.
- `tools/previews.py`: the builder's catalogue (`docs/clips.json`) and its
  previews, each clip played through `host/render`.
- `docs/`: the builder (GitHub Pages).
- `examples/`: whole configs for an OLED and a colour panel.

The clip names in the builder are descriptive, not Pioneer's.
