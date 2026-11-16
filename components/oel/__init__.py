"""Pioneer's OEL car-stereo clips on an ESPHome display.

    oel:
      display_id: screen
      clips:
        - movie8_f                   # one of carozerra's clips, by name
        - alt_diverdolphins
        - file: clips/my_clip.lkd    # or an .lkd of your own
        - url: https://example.com/clip.lkd
          name: mine
      clip: cycle                    # what to start with: a clip, cycle or off
      clip_time: 20s                 # each clip's turn when cycling
      mode: always                   # or screensaver: oel.start / oel.stop
      aspect: auto                   # letterbox, crop (panned) or stretch
      cutoff: 30%                    # 1-bit panels: shades dimmer than this stay dark
      frame_rate_modulation: false   # 1-bit panels: true (2 subframes) or 2-4; wants SPI
      meter:                         # drive the level meters (default: made-up music)
        sensor: house_power          # any sensor (a homeassistant one for an entity)
        min: 0                       # the value that reads as empty
        max: 5000                    # ... and as full
        style: 1                     # which of a meter's two looks, 1 or 2

    select:
      - platform: oel
        name: Dashboard clip

    number:                          # the cutoff as a slider in Home Assistant
      - platform: oel
        name: Dashboard clip cutoff

Clips named from the catalogue are fetched from carozerra
(https://github.com/youxufkhan/carozerra) at a fixed commit when the firmware
is built, and kept in ESPHome's cache after that; if carozerra can't be
reached, from our fork of it (https://github.com/Vortitron/carozerra). Each is turned into
palette-indexed frames compressed one by one (see lkd.py), which the player
unpacks a frame at a time with the ESP32's ROM inflate.

In mode: always the display should have no lambda or pages; in screensaver
mode it keeps them, and oel.start and oel.stop hand the screen over and back.
"""
import hashlib
import inspect
from pathlib import Path

from esphome import automation, external_files
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display, sensor
from esphome.const import CONF_FILE, CONF_ID, CONF_NAME, CONF_URL
from esphome.core import CORE, HexInt

from . import lkd

CODEOWNERS = ["@Vortitron"]
DOMAIN = "oel"

CONF_DISPLAY_ID = "display_id"
CONF_CLIPS = "clips"
CONF_CLIP = "clip"
CONF_CLIP_TIME = "clip_time"
CONF_MODE = "mode"
CONF_OEL_ID = "oel_id"
CONF_RAW_DATA_ID = "raw_data_id"
CONF_METER = "meter"
CONF_SENSOR = "sensor"
CONF_LAMBDA = "lambda"
CONF_ASPECT = "aspect"
CONF_CUTOFF = "cutoff"
CONF_FRM = "frame_rate_modulation"


def _subframes(value):
    """false, true (2 subframes), or 2-4."""
    if isinstance(value, bool) or (isinstance(value, str) and not value.strip().isdigit()):
        return 2 if cv.boolean(value) else 1
    return cv.int_range(min=2, max=4)(value)
# auto: letterbox, but crop where a clip would shrink and the screen is tall
# enough for it (a 256x64 movie on a 128x64 OLED). Order as oelcore::Aspect.
ASPECTS = ["auto", "letterbox", "crop", "stretch"]

oel_ns = cg.esphome_ns.namespace("oel")
OelComponent = oel_ns.class_("OelComponent", cg.Component)
StartAction = oel_ns.class_("StartAction", automation.Action)
StopAction = oel_ns.class_("StopAction", automation.Action)
NextAction = oel_ns.class_("NextAction", automation.Action)
IsRunningCondition = oel_ns.class_("IsRunningCondition", automation.Condition)


def _clip(value):
    """A catalogue name, or {file|url, name}."""
    if isinstance(value, str):
        name = cv.one_of(*lkd.CATALOGUE, lower=True)(value)
        value = {"catalogue": name, CONF_NAME: name}
    else:
        value = cv.Schema(
            {
                cv.Exclusive(CONF_FILE, "source"): cv.file_,
                cv.Exclusive(CONF_URL, "source"): cv.url,
                cv.Optional(CONF_NAME): cv.All(cv.string_strict, cv.Length(min=1, max=40)),
                cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
            }
        )(value)
        if CONF_FILE not in value and CONF_URL not in value:
            raise cv.Invalid("give a clip's name from the catalogue, or a file: or url:")
        if CONF_NAME not in value:
            src = str(value.get(CONF_FILE) or value[CONF_URL])
            value[CONF_NAME] = Path(src.split("?")[0]).stem.lower()
        return value
    return cv.Schema(
        {
            cv.Required("catalogue"): cv.string,
            cv.Required(CONF_NAME): cv.string,
            cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
        }
    )(value)


def _clips(value):
    clips = [_clip(v) for v in cv.ensure_list(cv.Any(cv.string, dict))(value)]
    names = [c[CONF_NAME] for c in clips]
    for n in names:
        if names.count(n) > 1:
            raise cv.Invalid(f"two clips are called '{n}'; give one a name:")
    if not clips:
        raise cv.Invalid("list at least one clip")
    return clips


def clip_names(config):
    return [c[CONF_NAME] for c in config[CONF_CLIPS]]


def _check(config):
    start = config.get(CONF_CLIP, "cycle")
    if start not in ("cycle", "off") and start not in clip_names(config):
        raise cv.Invalid(f"'{start}' isn't one of the clips: {clip_names(config)}", [CONF_CLIP])
    config[CONF_CLIP] = start
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OelComponent),
            cv.Required(CONF_DISPLAY_ID): cv.use_id(display.Display),
            cv.Required(CONF_CLIPS): _clips,
            cv.Optional(CONF_CLIP): cv.string,
            cv.Optional(CONF_CLIP_TIME, default="20s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MODE, default="always"): cv.one_of("always", "screensaver", lower=True),
            cv.Optional(CONF_ASPECT, default="auto"): cv.one_of(*ASPECTS, lower=True),
            cv.Optional(CONF_CUTOFF, default="30%"): cv.percentage,
            cv.Optional(CONF_FRM, default=False): _subframes,
            cv.Optional(CONF_METER): cv.All(
                cv.Schema(
                    {
                        cv.Exclusive(CONF_SENSOR, "source"): cv.use_id(sensor.Sensor),
                        cv.Exclusive(CONF_LAMBDA, "source"): cv.returning_lambda,
                        cv.Optional("min", default=0.0): cv.float_,
                        cv.Optional("max", default=100.0): cv.float_,
                        cv.Optional("style", default=1): cv.int_range(min=1, max=2),
                    }
                ),
                cv.has_exactly_one_key(CONF_SENSOR, CONF_LAMBDA),
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _check,
)

OEL_ACTION_SCHEMA = automation.maybe_simple_id({cv.GenerateID(): cv.use_id(OelComponent)})

# ESPHome 2025.10 asks whether an action finishes at once; older ones don't take it.
_SYNC = {"synchronous": True} if "synchronous" in inspect.signature(automation.register_action).parameters else {}


@automation.register_action("oel.start", StartAction, OEL_ACTION_SCHEMA, **_SYNC)
@automation.register_action("oel.stop", StopAction, OEL_ACTION_SCHEMA, **_SYNC)
@automation.register_action("oel.next", NextAction, OEL_ACTION_SCHEMA, **_SYNC)
async def oel_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition("oel.is_running", IsRunningCondition, OEL_ACTION_SCHEMA)
async def oel_condition_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


def _lkd_bytes(clip):
    """The .lkd itself: fetched (and cached) or read from the config's folder."""
    if CONF_FILE in clip:
        return Path(CORE.relative_config_path(clip[CONF_FILE])).read_bytes()
    if CONF_URL in clip:
        return _download(clip[CONF_URL])
    try:
        return _download(lkd.catalogue_url(clip["catalogue"]))
    except cv.Invalid:
        # carozerra gone or unreachable: the fork has the same commit.
        return _download(lkd.catalogue_url(clip["catalogue"], lkd.CAROZERRA_MIRROR))


def _download(url):
    path = external_files.compute_local_file_dir(DOMAIN) / f"{external_files.url_cache_key(url)}.lkd"
    return external_files.download_content(url, path)


def _blob(clip):
    """The clip as the player takes it, cached by the .lkd's hash: the meters'
    colour reduction takes a few seconds."""
    data = _lkd_bytes(clip)
    key = hashlib.sha256(data + b"oel1").hexdigest()[:16]
    cached = external_files.compute_local_file_dir(DOMAIN) / f"{key}.oel"
    if cached.exists():
        return cached.read_bytes()
    try:
        blob, _ = lkd.encode(clip[CONF_NAME], data)
    except Exception as e:  # noqa: BLE001 - any failure is a bad clip
        raise cv.Invalid(f"clip '{clip[CONF_NAME]}': {e}") from e
    cached.write_bytes(blob)
    return blob


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_display(await cg.get_variable(config[CONF_DISPLAY_ID])))
    for clip in config[CONF_CLIPS]:
        blob = _blob(clip)
        arr = cg.progmem_array(clip[CONF_RAW_DATA_ID], [HexInt(b) for b in blob])
        cg.add(var.add_clip(clip[CONF_NAME], arr, len(blob)))
    cg.add(var.set_initial_clip(config[CONF_CLIP]))
    cg.add(var.set_aspect(config[CONF_ASPECT]))
    cg.add(var.set_cutoff(round(config[CONF_CUTOFF] * 100)))
    if config[CONF_FRM] > 1:
        cg.add(var.set_frame_rate_modulation(config[CONF_FRM]))
    cg.add(var.set_clip_time(config[CONF_CLIP_TIME]))
    if config[CONF_MODE] == "screensaver":
        cg.add(var.set_screensaver_mode(True))
    if meter := config.get(CONF_METER):
        if CONF_SENSOR in meter:
            sens = await cg.get_variable(meter[CONF_SENSOR])
            source = cg.RawExpression(f"[]() -> float {{ return {sens}->state; }}")
        else:
            source = await cg.process_lambda(meter[CONF_LAMBDA], [], return_type=cg.float_)
        cg.add(var.set_meter_source(source, meter["min"], meter["max"], meter["style"] - 1))
