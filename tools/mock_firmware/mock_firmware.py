"""Mock of the laser firmware's web protocol, for UI tests.

Serves src/web/index.html at /, src/web/admin.html at /admin and
src/web/patterns.json at /patterns.json (read from the repo on every request) and
speaks the WebSocket protocol from docs/PROTOCOL.md on port 81, including the
preset/show library, show playback on the mock's tick and the library message.

It covers per-client admin flags, the PIN lockout, calibration (selectable look,
marks only in a worst-case look, pre-marked full-width horizontal limit, an
optional check round, save from adjust or check), the raw debug session (rule 9),
the 3 s hb rule, rules 7 and 8 (docs/SAFETY.md), and per-client state JSON. Where the
docs leave a detail open, the mock follows lib/laser_core.

The patch (lib/laser_core/src/fixtures.*, controller.cpp), up to 4 fixtures on one DMX line:
- Slots 1-4, each 'none', 'laser' (10 channels, every laser rule) or 'head' (19 channels,
  no limits at all). Default: slot 1 = laser at address 1. At most one laser; footprints
  must fit 1..512 and must not overlap.
- 'f<1-4>' in front of '<ch> <value>', '0' or 'off' picks the slot; on any other command it is
  bad-command. Without it, '<ch> <value>' is for slot 1, except during a calibration/debug
  session, where it is for the laser's slot. '0' and 'off' without it act on every fixture.
- 'off': laser CH1 to 0; head CH7-19 (light and macro) to 0, CH1-6 (pan/tilt) kept.
- 'patch <1-4> none|laser <addr>|head <addr>' (admin): saved like other settings, then every
  fixture goes dark, sessions and shows end, and 'patch-changed' is broadcast. Moving or
  removing the laser makes it uncalibrated; changing only a head keeps the limits.
- Sessions need a patched laser (no-fixture) and black out every fixture when they start or end.
- The state has 'fixtures': [{slot, type, addr, ch, preset}] for the patched slots only.

Library details, mirroring lib/laser_core/src/controller.cpp and src/library_service.cpp:
- Presets are typed ('laser' or 'head'), 48 slots. '[f<n>] preset save|load' acts on fixture n
  (save: slot 1 without a prefix; load: the first fixture of the preset's type); wrong-type if they differ.
- Shows are steps of cues ('<patch slot>=<preset>'), a hold or a nested show, a pause (':10');
  they are unfolded at 'show play'. A cue reaches a fixture only if the types match; the show drives
  the fixtures its cues reach, and a manual change or preset load on one of those stops it.
- play.step is 0-based (like cal.step); play.steps counts the unfolded show.
- 'preset load' sets that fixture's preset and stops a show driving it; a show step sets it to the
  step's preset; a manual channel command, 'off', '0' or any blackout clears it.
- 'preset save' (presetSaved): a playing show that uses the slot stops (the look stays).
  The saved fixture's preset = slot when its look equals the saved one (unless a show drives it);
  any other fixture that had that slot loses it.
- 'preset delete' clears every fixture's preset that was that slot.
- 'show play' refuses (not-calibrated) if a step would switch the laser on while uncalibrated,
  and no-fixture if it would drive nothing. 'show stop' with nothing playing is a harmless no-op.
- 'show save' over the playing show, or 'show delete' of it, stops playback (the look stays).
- Step timing keeps its schedule: a late tick shortens the next hold by the lateness.
- Library changes broadcast the library and the state; no 'saved' event is sent for them.

Run:
    uv run --no-project --with websockets python tools/mock_firmware/mock_firmware.py

Test hooks (HTTP GET on the page port; not part of the firmware):
    /__mock/state                         internal state as JSON ('channels'/'frame'/'preset' are the
                                          laser's; 'patch', 'fixtures' and 'universe' cover every slot)
    /__mock/stop-button?held=1|0          press or release IO0
    /__mock/config?hb=&checkStep=&adminTimeout=&failSave=0|1
    /__mock/reset?settingsReset=0|1       factory settings (uncalibrated, factory PIN)
    /__mock/debug-raw?ch=&value=          change one raw debug channel (as if another source did)
    /__mock/library?reset=0|1             empty the library; reset=1 also sets libraryReset (corrupt file)
    /__mock/mute?on=1|0                   a dead link: sockets stay open, but nothing is handled or sent
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import re
import sys
import time
from dataclasses import asdict, dataclass, field, replace
from pathlib import Path
from typing import Callable
from urllib.parse import parse_qs, urlsplit

from websockets.asyncio.server import ServerConnection, serve
from websockets.exceptions import ConnectionClosed

ROOT = Path(__file__).resolve().parents[2]
PAGES = {
    "/": (ROOT / "src" / "web" / "index.html", "text/html; charset=utf-8"),
    "/admin": (ROOT / "src" / "web" / "admin.html", "text/html; charset=utf-8"),
    "/patterns.json": (ROOT / "src" / "web" / "patterns.json", "application/json; charset=utf-8"),
}

CHANNEL_COUNT = 10  # the laser
POSITION_MAX = 127
RAW_MAX = 255
MODE_ON_MIN = 64
MODE_AUTO_MIN = 128  # auto/sound: permanently unavailable outside debug (rule 7)

# The patch (lib/laser_core/src/fixtures.h, moving_head.h)
MAX_FIXTURES = 4
MAX_FIXTURE_CHANNELS = 19  # the moving head
UNIVERSE_SIZE = 512
FIXTURE_CHANNELS = {"none": 0, "laser": CHANNEL_COUNT, "head": MAX_FIXTURE_CHANNELS}
HEAD_LIGHT_FIRST = 7  # head 'off': CH7-19 (dimmer, strobe, colours, lasers, macro) to 0; CH1-6 stay
PREFIX_PATTERN = re.compile(r"f([1-4])")
NUMBER_PATTERN = re.compile(r"[0-9]{1,3}")  # firmware parseNumber(token, 3)

MAX_CLIENTS = 8  # WEBSOCKETS_SERVER_CLIENT_MAX
MAX_LINE_LENGTH = 1023  # "show save" with 24 steps that cue 4 fixtures is ~650 bytes
MAX_WRONG_TRIES = 5
LOCK_MAX_SECONDS = 3600
PIN_PATTERN = re.compile(r"[0-9]{6,8}")
TICK_SECONDS = 0.05
HTTP_HEADER_TIMEOUT_S = 5

# Calibration look (lib/laser_core/src/calibration.h)
CAL_MODE_VALUE = 96
CAL_PATTERN_NUMBER = 1
CAL_SPIN_VALUE = 140
CAL_START_CH7 = 64  # "up high" on the user's laser
CAL_START_CH6 = 0   # no horizontal offset
LOOK_ROTATION = {"square": 0, "diamond": 64, "spin": CAL_SPIN_VALUE}
DEFAULT_LOOK = "diamond"
WORST_CASE_LOOKS = ("diamond", "spin")
SIZE_MAX_FIXED = 0
SIZE_GROW = 100
SIZE_PULSE = 220
VERTICAL_FLIPPING = 200
PATTERN_COUNT = 51
MARK_NAMES = ("low7", "high7", "left6", "right6")

# Library (docs/PROTOCOL.md, "Presets and shows")
PRESET_SLOTS = 48
SHOW_SLOTS = 16
MAX_STEPS = 24
HOLD_MIN, HOLD_MAX = 1, 6000  # tenths of a second
NAME_MAX_BYTES = 32
SHOW_MAX_DEPTH = 3         # a show in a show in a show
SHOW_MAX_FLAT_STEPS = 64   # one pass with every nested show unfolded
SHOW_MAX_LOOKS = 24        # different presets one show may use, nested shows included
CUE_PATTERN = re.compile(r"([1-4])=([0-9]{1,3})")
NESTED_PATTERN = re.compile(r"s([0-9]{1,3})")
HOLD_PATTERN = re.compile(r"[0-9]{1,4}")

def format_steps(show) -> str:
    """Firmware formatSteps(): e.g. "1=3+2=7:50,s4,1=5+s2,:10"."""
    parts = []
    for step in show.steps:
        cues = "+".join(f"{k + 1}={p}" for k, p in enumerate(step.cues) if p)
        if step.sub:
            parts.append(f"{cues}+s{step.sub}" if cues else f"s{step.sub}")
        else:
            parts.append(f"{cues}:{step.hold}")
    return ",".join(parts)


# (name shown on the admin page, pattern number, CH8 size, CH5 vertical flip)
CHECK_STEPS = (
    ("Vierkant", 1, SIZE_MAX_FIXED, 0),
    ("Propeller, 4 bladen", 50, SIZE_MAX_FIXED, 0),
    ("Molentje (5 streepjes)", 35, SIZE_MAX_FIXED, 0),
    ("Omlijnde pijlpunt", 46, SIZE_MAX_FIXED, 0),
    ("Klavertje vier", 39, SIZE_MAX_FIXED, 0),
    ("Knoop", 45, SIZE_MAX_FIXED, 0),
    ("Cijfer 3", 19, SIZE_MAX_FIXED, 0),
    ("Muzieknoten", 16, SIZE_MAX_FIXED, 0),
    ("Hart", 25, SIZE_MAX_FIXED, 0),
    ("Zigzag", 49, SIZE_MAX_FIXED, 0),
    ("Zoomcirkel (groeit)", 17, SIZE_GROW, 0),
    ("Twee zoomcirkels (groeit)", 29, SIZE_GROW, 0),
    ("Zoomcirkel in cirkel (groeit)", 30, SIZE_GROW, 0),
    ("Vierkant, verticaal spiegelen", 1, SIZE_MAX_FIXED, VERTICAL_FLIPPING),
    ("Vierkant, pulseren", 1, SIZE_PULSE, 0),
)


def pattern_value(number: int) -> int:
    """CH2 value in the middle of pattern n's band (same formula as the pages)."""
    return (2 * number - 1) * 128 // PATTERN_COUNT


def now() -> float:
    return time.monotonic()


@dataclass(frozen=True)
class PatchSlot:
    type: str = "none"  # 'none', 'laser' or 'head'
    addr: int = 1       # the fixture's CH1 on the DMX line (1 for an empty slot, like the firmware)

    @property
    def last(self) -> int:
        return self.addr + FIXTURE_CHANNELS[self.type] - 1


DEFAULT_PATCH = (PatchSlot("laser", 1), PatchSlot(), PatchSlot(), PatchSlot())


def patch_problem(patch: tuple[PatchSlot, ...]) -> str:
    """'' if the patch is fine (firmware checkPatch), else why not."""
    lasers = 0
    for i, a in enumerate(patch):
        if a.type == "none":
            continue
        if a.addr < 1 or a.last > UNIVERSE_SIZE:
            return "bad-address"
        lasers += a.type == "laser"
        if any(b.type != "none" and a.addr <= b.last and b.addr <= a.last for b in patch[:i]):
            return "overlap"
    return "two-lasers" if lasers > 1 else ""


def laser_slot_index(patch: tuple[PatchSlot, ...]) -> int | None:
    return next((i for i, s in enumerate(patch) if s.type == "laser"), None)


def is_laser_moved(before: tuple[PatchSlot, ...], after: tuple[PatchSlot, ...]) -> bool:
    """The limit is only valid for the slot and address it was calibrated on (firmware isLaserMoved)."""
    was, is_ = laser_slot_index(before), laser_slot_index(after)
    return was != is_ or (is_ is not None and before[was].addr != after[is_].addr)


def off_channels(kind: str, channels: list[int]) -> list[int]:
    """The type's 'off': the laser's CH1 to 0; the head's light channels and macro to 0."""
    if kind == "laser":
        return [0, *channels[1:]]
    if kind == "head":
        return channels[:HEAD_LIGHT_FIRST - 1] + [0] * (MAX_FIXTURE_CHANNELS - HEAD_LIGHT_FIRST + 1)
    return [0] * MAX_FIXTURE_CHANNELS


@dataclass(frozen=True)
class Limits:
    ch7_low: int = 0
    ch7_high: int = POSITION_MAX
    ch6_enabled: bool = True  # full width by default: blocks the CH6 128-255 sweep
    ch6_left: int = 0
    ch6_right: int = POSITION_MAX
    calibrated: bool = False


@dataclass
class Timing:
    hb_timeout: float
    admin_timeout: float
    check_step: float
    lock_base: float
    fail_save: bool = False


@dataclass
class Session:
    """A calibration (phase adjust/check) or a raw debug session (phase debug)."""

    owner: int
    phase: str
    last_hb: float
    ch7: int = CAL_START_CH7
    ch6: int = CAL_START_CH6
    look: str = DEFAULT_LOOK
    h_on: bool = True
    marks: dict = field(default_factory=lambda: dict.fromkeys(MARK_NAMES))
    step: int = 0
    step_started: float = 0.0
    check_complete: bool = False  # every step shown since the last 'lim check' (informational)
    raw: list = field(default_factory=lambda: [0] * CHANNEL_COUNT)

    @property
    def is_debug(self) -> bool:
        return self.phase == "debug"


@dataclass(frozen=True)
class Preset:
    type: str  # "laser" or "head": the fixture type it is a look for
    name: str
    ch: tuple[int, ...]


@dataclass(frozen=True)
class Step:
    cues: tuple[int, ...]  # preset slot per patch slot, 0 = leave that fixture alone
    sub: int               # nested show played once through, 0 = none
    hold: int              # tenths of a second; 0 when sub is set


@dataclass(frozen=True)
class Show:
    name: str
    steps: tuple[Step, ...]


@dataclass(frozen=True)
class FlatStep:
    looks: tuple[int, ...]  # preset slot per patch slot, 0 = unchanged
    hold: int


@dataclass
class Playback:
    """A show unfolded at "show play" (firmware ActiveShow): it plays from copies, never the library."""
    show: int
    step: int  # 0-based, in the unfolded show
    started: float
    flat: tuple[FlatStep, ...]
    looks: dict[int, Preset]  # the presets it was built from, copied
    shows: frozenset[int]     # the shows it was built from, nested ones included
    driven: int               # bit k: the show drives the fixture in patch slot k


@dataclass
class Client:
    cid: int
    ws: ServerConnection
    ip: str
    admin: bool = False
    last_admin_action: float = 0.0


STATE = object()  # placeholder: "the per-client state message"
LIBRARY = object()  # placeholder: "the library message"


class Out:
    """Outgoing messages, delivered in order after a command is handled."""

    def __init__(self) -> None:
        self.items: list[tuple[int | None, object]] = []

    def to(self, cid: int, obj: object) -> None:
        self.items.append((cid, obj))

    def all(self, obj: object) -> None:
        self.items.append((None, obj))

    def state(self, cid: int | None = None) -> None:
        self.items.append((cid, STATE))

    def library(self, cid: int | None = None) -> None:
        self.items.append((cid, LIBRARY))


class MockFirmware:
    def __init__(self, factory_pin: str, timing: Timing, log_path: Path | None, settings_reset: bool) -> None:
        self.factory_pin = factory_pin
        self.timing = timing
        self.log_file = log_path.open("a", encoding="utf-8") if log_path else None
        self.clients: dict[int, Client] = {}
        self.next_cid = 1
        self.muted = False
        self.stop_held = False
        self.presets: dict[int, Preset] = {}
        self.shows: dict[int, Show] = {}
        self.library_reset = False
        self.play: Playback | None = None
        self.reset_settings(settings_reset)

    # --- settings ----------------------------------------------------------------
    def reset_settings(self, settings_reset: bool) -> None:
        self.limits = Limits()
        self.patch = DEFAULT_PATCH
        self.pin = self.factory_pin
        self.settings_reset = settings_reset
        self.cal: Session | None = None
        self.darken_all()
        self.wrong_tries = 0
        self.lockouts = 0
        self.lock_until = 0.0

    # --- fixtures ------------------------------------------------------------------
    def darken_all(self) -> None:
        """Every fixture to 0 with no show running: boot, sessions, patch changes."""
        self.fixtures = [[0] * MAX_FIXTURE_CHANNELS for _ in range(MAX_FIXTURES)]
        self.loaded: list[int | None] = [None] * MAX_FIXTURES  # last loaded preset per slot
        self.play = None

    def laser_index(self) -> int | None:
        return laser_slot_index(self.patch)

    @property
    def channels(self) -> list[int]:
        """The laser's requested channels (all 0 when no laser is patched)."""
        i = self.laser_index()
        return [0] * CHANNEL_COUNT if i is None else self.fixtures[i][:CHANNEL_COUNT]

    @property
    def preset(self) -> int | None:
        """The laser's last loaded preset."""
        i = self.laser_index()
        return None if i is None else self.loaded[i]

    def set_laser_preset(self, preset: int | None) -> None:
        i = self.laser_index()
        if i is not None:
            self.loaded[i] = preset

    # --- logging -----------------------------------------------------------------
    def log(self, cid: int, **entry: object) -> None:
        if self.log_file is None:
            return
        self.log_file.write(json.dumps({"t": round(time.time(), 3), "client": cid, **entry}) + "\n")
        self.log_file.flush()

    # --- output ------------------------------------------------------------------
    def state_for(self, client: Client) -> dict:
        lim = self.limits
        return {
            "type": "state",
            "fixtures": self.fixtures_json(),
            "limits": {
                "ch7": [lim.ch7_low, lim.ch7_high],
                "ch6": {"on": lim.ch6_enabled, "range": [lim.ch6_left, lim.ch6_right]},
                "calibrated": lim.calibrated,
                "allowAuto": False,  # kept for old pages; auto/sound no longer exists
            },
            "stopHeld": self.stop_held,
            "admin": client.admin,
            "cal": self.cal_json(client),
            "play": self.play_json(),
            "settingsReset": self.settings_reset,
            "libraryReset": self.library_reset,
        }

    def fixtures_json(self) -> list[dict]:
        """The patched slots in slot order: the requested channels, not the frame (web_ui addFixtures)."""
        return [
            {"slot": i + 1, "type": s.type, "addr": s.addr, "ch": self.fixtures[i][:FIXTURE_CHANNELS[s.type]],
             "preset": self.loaded[i]}
            for i, s in enumerate(self.patch) if s.type != "none"
        ]

    def play_json(self) -> dict | None:
        p = self.play
        if p is None:
            return None
        hold = p.flat[p.step].hold / 10
        left = max(0, round((hold - (now() - p.started)) * 1000))
        return {"show": p.show, "step": p.step, "steps": len(p.flat), "holdLeftMs": left}

    def library_json(self) -> dict:
        return {
            "type": "library",
            "presets": [{"slot": s, "type": p.type, "name": p.name, "ch": list(p.ch[:FIXTURE_CHANNELS[p.type]])}
                        for s, p in sorted(self.presets.items())],
            "shows": [{"slot": s, "name": sh.name, "steps": format_steps(sh)} for s, sh in sorted(self.shows.items())],
        }

    def cal_json(self, client: Client) -> dict | None:
        c = self.cal
        if c is None:
            return None
        return {
            "phase": c.phase,
            "owner": c.owner == client.cid,
            "look": c.look,
            "ch7": c.ch7,
            "ch6": c.ch6,
            "marks": dict(c.marks),
            "hOn": c.h_on,
            "step": c.step,
            "steps": len(CHECK_STEPS),
            "stepName": CHECK_STEPS[c.step][0],
            "checkDone": c.check_complete,
            "raw": self.frame() if not c.is_debug else list(c.raw),
        }

    def frame(self) -> list[int]:
        """The laser's DMX frame (all 0 without a laser), exposed through /__mock/state."""
        i = self.laser_index()
        return [0] * CHANNEL_COUNT if i is None else self.fixture_frame(i)

    def fixture_frame(self, i: int) -> list[int]:
        """What patch slot i puts on the wire: its type's rules applied, all 0 while IO0 is held."""
        kind = self.patch[i].type
        count = FIXTURE_CHANNELS[kind]
        if self.stop_held or kind == "none":
            return [0] * count
        if kind == "head":
            return self.fixtures[i][:count]  # no limits (user decision)
        if self.cal is not None:
            return list(self.cal.raw) if self.cal.is_debug else self.calibration_frame(self.cal)
        return self.reclamped(self.fixtures[i][:count])

    def reclamped(self, look: list[int]) -> list[int]:
        """Firmware reclamp(): CH6/CH7 clamps, and CH1 to 0 if its mode is not allowed."""
        out = [self.clamp_channel(i + 1, v) for i, v in enumerate(look)]
        if not self.is_mode_allowed(out[0]):
            out[0] = 0
        return out

    def universe(self) -> dict[str, int]:
        """The non-zero DMX slots of the whole universe (firmware buildUniverse)."""
        dmx: dict[str, int] = {}
        for i, s in enumerate(self.patch):
            for k, v in enumerate(self.fixture_frame(i)):
                if v:
                    dmx[str(s.addr + k)] = v
        return dmx

    @staticmethod
    def calibration_frame(c: Session) -> list[int]:
        frame = [0] * CHANNEL_COUNT  # CH9 stays 0: the laser can't select a single colour
        frame[0] = CAL_MODE_VALUE
        if c.phase == "adjust":
            frame[1] = pattern_value(CAL_PATTERN_NUMBER)
            frame[2] = LOOK_ROTATION[c.look]
            frame[5], frame[6] = c.ch6, c.ch7
            return frame
        _, pattern, size, flip = CHECK_STEPS[c.step]
        frame[1], frame[2], frame[4], frame[7] = pattern_value(pattern), CAL_SPIN_VALUE, flip, size
        m = c.marks
        if c.h_on:  # no offset, unless the candidate range excludes it
            lo, hi = sorted((m["left6"], m["right6"]))
            frame[5] = max(lo, min(hi, CAL_START_CH6))
        frame[6] = m["low7"]
        return frame

    async def deliver(self, out: Out) -> None:
        if self.muted:
            return
        for cid, obj in out.items:
            if cid is None:
                targets = list(self.clients.values())
            else:
                targets = [self.clients[cid]] if cid in self.clients else []
            for client in targets:
                if obj is STATE:
                    payload = self.state_for(client)
                elif obj is LIBRARY:
                    payload = self.library_json()
                else:
                    payload = obj
                try:
                    await client.ws.send(json.dumps(payload))
                except ConnectionClosed:
                    pass

    @staticmethod
    def error(client: Client, code: str, out: Out) -> None:
        out.to(client.cid, {"type": "error", "code": code})

    # --- limits ------------------------------------------------------------------
    def is_mode_allowed(self, value: int) -> bool:
        if value >= MODE_AUTO_MIN:
            return False
        return value < MODE_ON_MIN or self.limits.calibrated

    def clamp_channel(self, ch: int, value: int) -> int:
        lim = self.limits
        if ch == 7:
            lo, hi = sorted((lim.ch7_low, lim.ch7_high))
            return max(lo, min(hi, value, POSITION_MAX))
        if ch == 6 and lim.ch6_enabled:
            lo, hi = sorted((lim.ch6_left, lim.ch6_right))
            return max(lo, min(hi, value))
        return value

    def reclamp(self) -> None:
        """Firmware commitSettings: the laser's look re-clamped to the (new) limits."""
        i = self.laser_index()
        if i is not None:
            self.fixtures[i][:CHANNEL_COUNT] = self.reclamped(self.fixtures[i][:CHANNEL_COUNT])

    # --- stopping ----------------------------------------------------------------
    def blackout(self, reason: str, out: Out) -> None:
        """Firmware endCalibration + a stop event: every fixture dark, no session, no show."""
        self.cal = None
        self.darken_all()  # a stop also cancels playback, and nothing resumes it
        out.all({"type": "stop", "reason": reason})
        out.state()

    def set_stop_button(self, held: bool, out: Out) -> None:
        if held and not self.stop_held:
            self.stop_held = True
            self.blackout("button", out)
        elif not held and self.stop_held:
            self.stop_held = False  # releasing restores nothing
            out.state()

    # --- commands ----------------------------------------------------------------
    def handle(self, client: Client, text: str, out: Out) -> None:
        line = text.strip()
        if not line or len(line) > MAX_LINE_LENGTH:
            self.error(client, "bad-command", out)
            return
        kind = line.split()[0].lower()
        if kind in ("preset", "show"):  # parsed apart: names keep their case and may contain spaces
            self.library_command(client, kind, line, out)
            return
        prefix = PREFIX_PATTERN.fullmatch(kind)
        rest = line.split(None, 1)[1] if len(line.split()) > 1 else ""
        if prefix and rest.lower().split()[:1] == ["preset"]:  # "f2 preset save|load ..." (a bare "f2" is bad-command below)
            self.library_command(client, "preset", rest, out, fixture=int(prefix[1]))
            return
        words = line.lower().split()  # the firmware lower-cases every other command
        fixture = 0
        prefix = PREFIX_PATTERN.fullmatch(words[0])
        if len(words) >= 2 and prefix:
            fixture = int(prefix[1])
            words = words[1:]
        if fixture or words == ["off"] or words[0].isdigit():
            self.fixture_command(client, fixture, words, out)
            return
        if words == ["hb"]:
            if self.cal is not None and self.cal.owner == client.cid:
                t = now()
                self.cal.last_hb = t
                if client.admin:
                    client.last_admin_action = t  # an owner still on the page stays logged in
            return
        if words == ["?"]:
            out.state(client.cid)
            return
        handlers: dict[str, Callable[[Client, list[str], Out], None]] = {
            "auth": self.cmd_auth,
            "logout": self.cmd_logout,
            "pin": self.cmd_pin,
            "patch": self.cmd_patch,
            "cal": self.cmd_cal,
            "dbg": self.cmd_dbg,
            "lim": self.cmd_lim,
            "lib": self.cmd_lib,
        }
        handler = handlers.get(words[0])
        if handler is None:
            self.error(client, "bad-command", out)
            return
        handler(client, words[1:], out)

    def fixture_command(self, client: Client, fixture: int, words: list[str], out: Out) -> None:
        """The commands that take an 'f<1-4>' prefix: '<ch> <value>', '0' and 'off' (0 = no prefix)."""
        if words == ["off"] or (len(words) == 1 and NUMBER_PATTERN.fullmatch(words[0]) and int(words[0]) == 0):
            self.stop_command(fixture, words[0] == "off", out)
        elif words[0].isdigit():
            self.channel_command(client, fixture, words, out)
        else:
            self.error(client, "bad-command", out)  # a prefix on any other command

    def stop_command(self, fixture: int, is_off: bool, out: Out) -> None:
        """'off' and '0': accepted from anyone, always; they end calibration and debug.
        Without a prefix they act on every fixture, with one only on that slot."""
        reason = "off" if is_off else "blackout"
        if self.cal is not None:
            self.blackout(reason, out)
            return
        laser = self.laser_index()
        for i, slot in enumerate(self.patch):
            if fixture and i != fixture - 1:
                continue
            if self.is_show_driving(i):
                self.play = None  # nothing resumes it
            self.fixtures[i] = off_channels(slot.type, self.fixtures[i]) if is_off else [0] * MAX_FIXTURE_CHANNELS
            self.loaded[i] = None
        if not fixture:  # only a stop of every fixture is announced; "f2 off" is a normal change
            out.all({"type": "stop", "reason": reason})
        out.state()

    def channel_command(self, client: Client, fixture: int, words: list[str], out: Out) -> None:
        if len(words) != 2 or not all(NUMBER_PATTERN.fullmatch(w) for w in words):
            self.error(client, "bad-command", out)
            return
        ch, value = int(words[0]), int(words[1])
        if not (1 <= ch <= MAX_FIXTURE_CHANNELS and value <= RAW_MAX):
            self.error(client, "bad-command", out)
            return
        if self.stop_held:
            self.error(client, "stop-held", out)
            return
        laser = self.laser_index()
        # Without a prefix: slot 1, except during a session, which is about the laser.
        index = fixture - 1 if fixture else (laser if self.cal is not None else 0)
        kind = "none" if index is None else self.patch[index].type
        if kind == "none":
            self.error(client, "no-fixture", out)
            return
        if ch > FIXTURE_CHANNELS[kind]:
            self.error(client, "bad-command", out)
            return
        c = self.cal
        if c is not None:
            self.session_channel(client, c, index == laser, ch, value, out)
            return
        if kind == "laser":
            if ch == 1 and not self.is_mode_allowed(value):
                self.error(client, "auto-blocked" if value >= MODE_AUTO_MIN else "not-calibrated", out)
                return
            self.fixtures[index][ch - 1] = self.clamp_channel(ch, value)
        else:
            self.fixtures[index][ch - 1] = value  # the head has no limits (user decision)
        if self.is_show_driving(index):
            self.play = None  # a manual change takes over from the show
        self.loaded[index] = None
        out.state()

    def session_channel(self, client: Client, c: Session, is_laser: bool, ch: int, value: int, out: Out) -> None:
        """During a session only its owner may move the laser, within what the session allows."""
        if c.owner != client.cid or not is_laser:
            self.error(client, "calibration-busy", out)
            return
        if c.is_debug and ch <= CHANNEL_COUNT:
            c.raw[ch - 1] = value  # rule 9: raw, no limits at all
        elif ch in (6, 7) and c.phase == "adjust":
            if ch == 7:
                c.ch7 = min(value, POSITION_MAX)
            else:
                c.ch6 = min(value, POSITION_MAX)
        else:  # calibration pins every other channel, and CH6/CH7 during the check
            self.error(client, "calibration-busy", out)
            return
        self.touch(client)
        out.state()

    def require_admin(self, client: Client, out: Out) -> bool:
        if not client.admin:
            self.error(client, "not-admin", out)
            return False
        client.last_admin_action = now()
        return True

    def check_pin(self, client: Client, attempt: str) -> dict:
        """Returns the auth event. Mirrors laser::checkPin (5 tries, then 60 s doubling to 1 h)."""
        t = now()
        if t < self.lock_until:
            return {"type": "auth", "ok": False, "lockedSeconds": math.ceil(self.lock_until - t), "attemptsLeft": 0}
        if PIN_PATTERN.fullmatch(attempt) and attempt == self.pin:
            self.wrong_tries = 0
            self.lockouts = 0
            return {"type": "auth", "ok": True, "lockedSeconds": 0, "attemptsLeft": MAX_WRONG_TRIES}
        self.wrong_tries += 1
        print(f"admin: wrong PIN from client {client.cid} ({client.ip})", flush=True)
        if self.wrong_tries < MAX_WRONG_TRIES:
            return {"type": "auth", "ok": False, "lockedSeconds": 0, "attemptsLeft": MAX_WRONG_TRIES - self.wrong_tries}
        duration = min(self.timing.lock_base * (2 ** min(self.lockouts, 6)), LOCK_MAX_SECONDS)
        self.wrong_tries = 0
        self.lockouts += 1
        self.lock_until = t + duration
        print(f"admin: locked for {duration:g} s", flush=True)
        return {"type": "auth", "ok": False, "lockedSeconds": math.ceil(duration), "attemptsLeft": 0}

    def cmd_auth(self, client: Client, args: list[str], out: Out) -> None:
        if len(args) != 1:
            self.error(client, "bad-command", out)
            return
        event = self.check_pin(client, args[0])
        if event["ok"]:
            client.admin = True
            client.last_admin_action = now()
        out.to(client.cid, event)
        out.state(client.cid)

    def cmd_logout(self, client: Client, args: list[str], out: Out) -> None:
        if args:
            self.error(client, "bad-command", out)
            return
        if self.cal is not None and self.cal.owner == client.cid:
            self.blackout("calibration-owner-left", out)
        client.admin = False
        out.state(client.cid)

    def cmd_pin(self, client: Client, args: list[str], out: Out) -> None:
        if args == ["reset"]:
            if self.require_admin(client, out):
                self.error(client, "serial-only", out)
            return
        if len(args) != 2 or not PIN_PATTERN.fullmatch(args[1]):
            self.error(client, "bad-command", out)
            return
        if not self.require_admin(client, out):
            return
        event = self.check_pin(client, args[0])
        if not event["ok"]:
            out.to(client.cid, event)  # wrong current PIN: same answer as a wrong login
            return
        if not self.save(client, out):
            return
        self.pin = args[1]
        self.reclamp()  # firmware commitSettings re-clamps on every commit
        out.state()

    def save(self, client: Client, out: Out, quiet: bool = False) -> bool:
        if self.timing.fail_save:
            self.error(client, "save-failed", out)
            return False
        if not quiet:
            out.to(client.cid, {"type": "saved"})
        return True

    @staticmethod
    def parse_patch_slot(kind: str, address_text: str) -> PatchSlot | None:
        if kind not in ("laser", "head") or not NUMBER_PATTERN.fullmatch(address_text):
            return None
        address = int(address_text)
        return PatchSlot(kind, address) if 1 <= address <= UNIVERSE_SIZE else None

    @staticmethod
    def parse_patch_entry(entry: str) -> PatchSlot | None:
        """'none' or 'laser:<address>' / 'head:<address>' (firmware parsePatchEntry)."""
        if entry == "none":
            return PatchSlot()
        kind, colon, address = entry.partition(":")
        return MockFirmware.parse_patch_slot(kind, address) if colon else None

    @staticmethod
    def parse_patch(args: list[str]) -> tuple[PatchSlot, ...] | tuple[int, PatchSlot] | None:
        """'<1-4> none', '<1-4> laser|head <1-512>' or 'set <entry>,<entry>,<entry>,<entry>' (firmware parsePatch).
        Returns the whole new patch for 'set', else (slot, entry)."""
        if len(args) == 2 and args[0] == "set":
            entries = [MockFirmware.parse_patch_entry(e) for e in args[1].split(",")]
            return tuple(entries) if len(entries) == len(DEFAULT_PATCH) and None not in entries else None
        if len(args) < 2 or not re.fullmatch(r"[1-4]", args[0]):
            return None
        if args[1:] == ["none"]:
            return int(args[0]), PatchSlot()
        entry = MockFirmware.parse_patch_slot(args[1], args[2]) if len(args) == 3 else None
        return (int(args[0]), entry) if entry else None

    def cmd_patch(self, client: Client, args: list[str], out: Out) -> None:
        parsed = self.parse_patch(args)
        if parsed is None:
            self.error(client, "bad-command", out)
            return
        if not self.require_admin(client, out):
            return
        if self.cal is not None:
            self.error(client, "calibration-busy", out)
            return
        if len(parsed) == len(DEFAULT_PATCH):
            patch = parsed  # "patch set": every slot at once
        else:
            slot, entry = parsed
            patch = tuple(entry if i == slot - 1 else s for i, s in enumerate(self.patch))
        if patch_problem(patch):
            self.error(client, "bad-patch", out)
            return
        if patch == self.patch:
            out.state(client.cid)  # nothing moved: no save, no blackout (firmware handleSetPatch)
            return
        if not self.save(client, out):
            return
        if is_laser_moved(self.patch, patch):
            self.limits = replace(self.limits, calibrated=False)  # the clamp must be re-verified on the new slots
        self.patch = patch
        self.blackout("patch-changed", out)  # fixtures moved: every one starts dark
        self.reclamp()  # firmware commitSettings re-clamps after the blackout

    def owned_session(self, client: Client, out: Out) -> Session | None:
        if self.cal is None:
            self.error(client, "not-calibrating", out)
            return None
        if self.cal.owner != client.cid:
            self.error(client, "not-owner", out)
            return None
        return self.cal

    def touch(self, client: Client) -> None:
        if self.cal is not None:
            self.cal.last_hb = now()
        client.last_admin_action = now()

    def start_allowed(self, client: Client, out: Out) -> bool:
        if self.stop_held:
            self.error(client, "stop-held", out)
            return False
        if self.cal is not None:
            self.error(client, "calibration-busy", out)
            return False
        if self.laser_index() is None:
            self.error(client, "no-fixture", out)  # sessions are for the laser
            return False
        return True

    def cmd_cal(self, client: Client, args: list[str], out: Out) -> None:
        sub = args[0] if args else ""
        valid = (sub in ("start", "stop", "alive") and len(args) == 1) or (
            sub == "look" and len(args) == 2 and args[1] in LOOK_ROTATION
        )
        if not valid:
            self.error(client, "bad-command", out)
            return
        if not self.require_admin(client, out):
            return
        if sub == "start":
            if self.start_allowed(client, out):
                self.cal_start(client, out)
            return
        c = self.owned_session(client, out)
        if c is None:
            return
        if sub == "stop":
            self.blackout("debug-stopped" if c.is_debug else "calibration-stopped", out)
            return
        if sub == "look":
            if c.phase != "adjust":
                self.error(client, "wrong-phase", out)
                return
            c.look = args[1]
        self.touch(client)  # 'cal alive' is still accepted; it only counts as activity
        out.state(client.cid if sub == "alive" else None)

    def cal_start(self, client: Client, out: Out) -> None:
        lim = self.limits
        self.cal = Session(
            owner=client.cid,
            phase="adjust",
            last_hb=now(),
            h_on=lim.ch6_enabled,
            marks={"low7": None, "high7": None, "left6": lim.ch6_left, "right6": lim.ch6_right},
        )
        self.darken_all()  # every fixture: nothing resumes after calibration ends
        out.state()

    def cmd_dbg(self, client: Client, args: list[str], out: Out) -> None:
        if args not in (["start"], ["stop"]):
            self.error(client, "bad-command", out)
            return
        if not self.require_admin(client, out):
            return
        if args[0] == "start":
            if self.start_allowed(client, out):
                self.cal = Session(owner=client.cid, phase="debug", last_hb=now())  # every channel starts at 0
                self.darken_all()
                out.state()
            return
        c = self.owned_session(client, out)
        if c is None:
            return
        if not c.is_debug:
            self.error(client, "wrong-phase", out)
            return
        self.blackout("debug-stopped", out)

    def cmd_lim(self, client: Client, args: list[str], out: Out) -> None:
        sub = args[0] if args else ""
        valid = (
            (sub == "mark" and len(args) == 2 and args[1] in MARK_NAMES)
            or (sub == "h" and args[1:] in (["on"], ["off"]))
            or (sub in ("check", "save", "redo") and len(args) == 1)
        )
        if not valid:
            self.error(client, "bad-command", out)
            return
        if not self.require_admin(client, out):
            return
        c = self.owned_session(client, out)
        if c is None:
            return
        if c.is_debug:
            self.error(client, "wrong-phase", out)
            return
        if sub == "save":
            self.lim_save(client, c, out)
            return
        if sub == "redo":
            if c.phase != "check":
                self.error(client, "wrong-phase", out)
                return
            c.phase = "adjust"
        elif c.phase != "adjust":
            self.error(client, "wrong-phase", out)
            return
        elif sub == "mark":
            if c.look not in WORST_CASE_LOOKS:
                self.error(client, "need-worst-case-look", out)
                return
            c.marks[args[1]] = c.ch7 if args[1].endswith("7") else c.ch6
        elif sub == "h":
            c.h_on = args[1] == "on"
        elif sub == "check":
            problem = self.marks_problem(c)
            if problem:
                self.error(client, problem, out)
                return
            c.phase = "check"
            c.step = 0
            c.step_started = now()
            c.check_complete = False
        self.touch(client)
        out.state()

    @staticmethod
    def marks_problem(c: Session) -> str:
        m = c.marks
        if m["low7"] is None or m["high7"] is None:
            return "need-both-marks"
        if m["low7"] == m["high7"]:
            return "equal-ends"
        if c.h_on and (m["left6"] is None or m["right6"] is None):
            return "need-horizontal-marks"
        if c.h_on and m["left6"] == m["right6"]:
            return "equal-ends"
        return ""

    def lim_save(self, client: Client, c: Session, out: Out) -> None:
        """Works in adjust once the marks are valid, and in check at any time (the check is optional)."""
        problem = self.marks_problem(c)
        if problem:
            self.error(client, problem, out)
            return
        self.touch(client)
        if not self.save(client, out):
            return
        m = c.marks
        self.limits = Limits(
            ch7_low=m["low7"],
            ch7_high=m["high7"],
            ch6_enabled=c.h_on,
            ch6_left=m["left6"] if c.h_on else 0,
            ch6_right=m["right6"] if c.h_on else POSITION_MAX,
            calibrated=True,
        )
        self.settings_reset = False
        self.blackout("calibration-saved", out)
        self.reclamp()

    # --- library: presets and shows ------------------------------------------------
    @staticmethod
    def is_valid_name(name: str) -> bool:
        size = len(name.encode("utf-8"))
        return 1 <= size <= NAME_MAX_BYTES and not any(ord(c) < 32 or ord(c) == 127 or c in '"\\' for c in name)

    @staticmethod
    def parse_slot(text: str, count: int) -> int | None | str:
        """The slot number, or 'bad-command' (not a number) / 'bad-slot' (out of range)."""
        if not text.isdigit():
            return "bad-command"
        slot = int(text)
        return slot if 1 <= slot <= count else "bad-slot"

    def library_command(self, client: Client, kind: str, line: str, out: Out, fixture: int = 0) -> None:
        words = line.split(" ")
        sub = words[1].lower() if len(words) > 1 else ""
        if fixture and not (kind == "preset" and sub in ("save", "load")):
            self.error(client, "bad-command", out)  # the prefix is only for preset save/load
            return
        if kind == "preset" and sub == "save":
            self.preset_save(client, words[2:], fixture, out)
        elif kind == "show" and sub == "save":
            self.show_save(client, words[2:], out)
        elif kind == "show" and sub == "stop" and len(words) == 2:
            self.show_stop(out)
        elif len(words) == 3 and (kind, sub) in (("preset", "load"), ("preset", "delete"), ("show", "play"),
                                                  ("show", "delete")):
            if not words[2].isdigit():
                self.error(client, "bad-command", out)
            elif kind == "preset" and sub == "load":
                self.preset_load(client, int(words[2]), fixture, out)
            else:
                action = {("preset", "delete"): self.preset_delete, ("show", "play"): self.show_play,
                          ("show", "delete"): self.show_delete}[(kind, sub)]
                action(client, int(words[2]), out)
        else:
            self.error(client, "bad-command", out)

    def library_blocked(self, client: Client, out: Out, needs_output: bool) -> bool:
        """Sessions lock the whole library; IO0 blocks only what would drive a fixture."""
        if self.cal is not None:
            self.error(client, "calibration-busy", out)
            return True
        if needs_output and self.stop_held:
            self.error(client, "stop-held", out)
            return True
        return False

    def library_changed(self, out: Out) -> None:
        out.library()
        out.state()

    def is_show_driving(self, index: int) -> bool:
        return self.play is not None and (self.play.driven >> index) & 1 == 1

    def typed_look(self, preset: Preset) -> list[int]:
        """Firmware typedLook(): a laser look inside today's limits, a head look as it is."""
        if preset.type == "laser":
            return self.reclamped(list(preset.ch[:CHANNEL_COUNT])) + [0] * (MAX_FIXTURE_CHANNELS - CHANNEL_COUNT)
        return list(preset.ch) + [0] * (MAX_FIXTURE_CHANNELS - len(preset.ch))

    def stored_look(self, index: int) -> list[int]:
        """Firmware sanitizeLook(): fixture `index`'s look as a preset stores it."""
        look = list(self.fixtures[index][:MAX_FIXTURE_CHANNELS])
        if self.patch[index].type == "laser":
            look[:CHANNEL_COUNT] = [self.clamp_channel(i + 1, v) for i, v in enumerate(look[:CHANNEL_COUNT])]
            if look[0] >= MODE_AUTO_MIN:
                look[0] = 0
        return look

    def laser_look_error(self, preset: Preset) -> str | None:
        """Rule 7 and rule 8 for a laser look from the library."""
        mode = preset.ch[0]
        if mode >= MODE_AUTO_MIN:
            return "auto-blocked"
        return "not-calibrated" if mode >= MODE_ON_MIN and not self.limits.calibrated else None

    def preset_save(self, client: Client, args: list[str], fixture: int, out: Out) -> None:
        """Firmware order: session, slot, name, a preset a show uses keeps its type, the fixture, the save."""
        if not args or not args[0].isdigit() or len(args) < 2:
            self.error(client, "bad-command", out)
            return
        if self.library_blocked(client, out, needs_output=False):
            return
        slot = int(args[0])
        index = fixture - 1 if fixture else 0
        kind = self.patch[index].type
        if not 1 <= slot <= PRESET_SLOTS:
            self.error(client, "bad-slot", out)
            return
        if not self.is_valid_name(" ".join(args[1:]).strip()):
            self.error(client, "bad-name", out)
            return
        old = self.presets.get(slot)
        if old is not None and old.type != kind and self.is_preset_in_use(slot):
            self.error(client, "preset-in-use", out)  # its cue would reach nothing otherwise
            return
        if kind == "none":
            self.error(client, "no-fixture", out)
            return
        if not self.save(client, out, quiet=True):
            return
        look = self.stored_look(index)
        self.presets[slot] = Preset(kind, " ".join(args[1:]).strip(), tuple(look[:FIXTURE_CHANNELS[kind]]))
        self.preset_saved(slot, index, look)
        self.library_changed(out)

    def preset_saved(self, slot: int, index: int, look: list[int]) -> None:
        """Firmware presetSaved(): a show built from the preset has a stale copy and stops."""
        if self.play is not None and slot in self.play.looks:
            self.play = None  # the looks stay
        for k in range(MAX_FIXTURES):
            if k == index and not self.is_show_driving(k) and self.stored_look(k) == look:
                self.loaded[k] = slot
            elif self.loaded[k] == slot:
                self.loaded[k] = None  # the preset changed under it

    def preset_load(self, client: Client, slot: int, fixture: int, out: Out) -> None:
        """Firmware order: the slot, then session, IO0, the fixture, its type, rule 7/8."""
        if not 1 <= slot <= PRESET_SLOTS:
            self.error(client, "bad-slot", out)
            return
        preset = self.presets.get(slot)
        if preset is None:
            self.error(client, "empty-slot", out)
            return
        if self.library_blocked(client, out, needs_output=True):
            return
        index = fixture - 1 if fixture else next((i for i, s in enumerate(self.patch) if s.type == preset.type), None)
        if index is None or self.patch[index].type == "none":
            self.error(client, "no-fixture", out)
            return
        if self.patch[index].type != preset.type:
            self.error(client, "wrong-type", out)
            return
        error = self.laser_look_error(preset) if preset.type == "laser" else None
        if error:
            self.error(client, error, out)
            return
        if self.is_show_driving(index):
            self.play = None  # loading takes over from the show
        self.fixtures[index] = self.typed_look(preset)
        self.loaded[index] = slot
        out.state()

    def is_preset_in_use(self, slot: int) -> bool:
        return any(slot in step.cues for show in self.shows.values() for step in show.steps)

    def preset_delete(self, client: Client, slot: int, out: Out) -> None:
        if self.library_blocked(client, out, needs_output=False):
            return
        if not 1 <= slot <= PRESET_SLOTS:
            self.error(client, "bad-slot", out)
            return
        if slot not in self.presets:
            self.error(client, "empty-slot", out)
            return
        if self.is_preset_in_use(slot):
            self.error(client, "preset-in-use", out)
            return
        if not self.save(client, out, quiet=True):
            return
        del self.presets[slot]
        self.loaded = [None if p == slot else p for p in self.loaded]
        self.library_changed(out)

    @staticmethod
    def parse_steps(text: str) -> tuple[tuple[Step, ...], tuple[bool, ...]] | None:
        """Firmware parseSteps(): syntax only. Returns the steps and, per step, whether it had a hold."""
        steps, holds = [], []
        for part in text.lower().split(","):
            if len(steps) == MAX_STEPS:
                return None
            body, colon, hold_text = part.partition(":")
            if colon and not HOLD_PATTERN.fullmatch(hold_text):
                return None
            cues, sub = [0] * MAX_FIXTURES, 0
            for item in body.split("+") if body else []:
                cue, nested = CUE_PATTERN.fullmatch(item), NESTED_PATTERN.fullmatch(item)
                if cue and int(cue[2]) and not cues[int(cue[1]) - 1]:
                    cues[int(cue[1]) - 1] = int(cue[2])
                elif nested and int(nested[1]) and not sub:
                    sub = int(nested[1])
                else:
                    return None
            if not body and not colon:
                return None
            steps.append(Step(tuple(cues), sub, int(hold_text) if colon else 0))
            holds.append(bool(colon))
        return tuple(steps), tuple(holds)

    def check_steps(self, slot: int, steps: tuple[Step, ...], holds: tuple[bool, ...]) -> str | None:
        """Firmware checkSteps(): timing, cue presets exist, the nested show exists."""
        for step, has_hold in zip(steps, holds):
            if has_hold == bool(step.sub):
                return "bad-steps"
            if not step.sub and not HOLD_MIN <= step.hold <= HOLD_MAX:
                return "bad-steps"
            if any(cue and cue not in self.presets for cue in step.cues):
                return "bad-steps"
            if step.sub and step.sub != slot and step.sub not in self.shows:
                return "bad-steps"
        return None

    def unfold(self, slot: int, replaced: int = 0, replacement: Show | None = None):
        """Firmware Unfolder: (error, flat steps, looks, shows). Cues wait in `pending` for the next flat
        step, so a nested step's cues land on the nested show's first step (whose own cues win)."""
        flat: list[FlatStep] = []
        looks: dict[int, Preset] = {}
        shows: set[int] = set()
        pending = [0] * MAX_FIXTURES

        def show_at(n: int) -> Show | None:
            return replacement if n == replaced else self.shows.get(n)

        def walk(n: int, depth: int, path: frozenset[int]) -> str | None:
            show = show_at(n) if 1 <= n <= SHOW_SLOTS else None
            if show is None:
                return "bad-steps"
            if n in path:
                return "show-loop"
            if depth > SHOW_MAX_DEPTH:
                return "show-too-long"
            shows.add(n)
            for step in show.steps:
                for k, cue in enumerate(step.cues):
                    if cue:
                        if cue not in self.presets:
                            return "bad-steps"
                        if cue not in looks:
                            if len(looks) == SHOW_MAX_LOOKS:
                                return "show-too-long"
                            looks[cue] = self.presets[cue]
                        pending[k] = cue
                if step.sub:
                    error = walk(step.sub, depth + 1, path | {n})
                    if error:
                        return error
                    continue
                if len(flat) == SHOW_MAX_FLAT_STEPS:
                    return "show-too-long"
                flat.append(FlatStep(tuple(pending), step.hold))
                pending[:] = [0] * MAX_FIXTURES
            return None

        error = walk(slot, 1, frozenset())
        return error, tuple(flat), looks, frozenset(shows)

    def show_save(self, client: Client, args: list[str], out: Out) -> None:
        """Firmware order: session, slot, name, steps, then this show and every show nesting it must unfold."""
        if len(args) < 3 or not args[0].isdigit():
            self.error(client, "bad-command", out)
            return
        if self.library_blocked(client, out, needs_output=False):
            return
        slot = int(args[0])
        name = " ".join(args[2:]).strip()
        if not 1 <= slot <= SHOW_SLOTS:
            self.error(client, "bad-slot", out)
            return
        if not self.is_valid_name(name):
            self.error(client, "bad-name", out)
            return
        parsed = self.parse_steps(args[1])
        error = "bad-steps" if parsed is None else self.check_steps(slot, *parsed)
        candidate = Show(name, parsed[0]) if parsed else None
        for other in [slot] + [n for n in sorted(self.shows) if n != slot]:
            if error:
                break
            error = self.unfold(other, slot, candidate)[0]
        if error:
            self.error(client, error, out)
            return
        if not self.save(client, out, quiet=True):
            return
        self.shows[slot] = candidate
        self.show_changed(slot)
        self.library_changed(out)

    def show_changed(self, slot: int) -> None:
        """Firmware showChanged(): a playing show built from this one (also nested) is stale."""
        if self.play is not None and slot in self.play.shows:
            self.play = None  # the looks stay

    def show_play(self, client: Client, slot: int, out: Out) -> None:
        """Firmware order: the slot, then session, IO0, unfolds, rule 7/8 for cues that reach the laser, drives something."""
        if not 1 <= slot <= SHOW_SLOTS:
            self.error(client, "bad-slot", out)
            return
        if slot not in self.shows:
            self.error(client, "empty-slot", out)
            return
        if self.library_blocked(client, out, needs_output=True):
            return
        error, flat, looks, shows = self.unfold(slot)
        if error:
            self.error(client, "empty-slot", out)  # firmware: an ActiveShow with no steps
            return
        driven = 0
        for step in flat:
            for k, cue in enumerate(step.looks):
                if cue and looks[cue].type == self.patch[k].type:
                    look_error = self.laser_look_error(looks[cue]) if looks[cue].type == "laser" else None
                    if look_error:
                        self.error(client, look_error, out)
                        return
                    driven |= 1 << k
        if not driven:
            self.error(client, "no-fixture", out)  # nothing it is made for is patched
            return
        self.play = Playback(slot, 0, now(), flat, looks, shows, driven)
        self.apply_step()
        out.state()

    def apply_step(self) -> None:
        """Puts the current step's cues on the fixtures whose type they are for."""
        p = self.play
        for k, cue in enumerate(p.flat[p.step].looks):
            if cue and p.looks[cue].type == self.patch[k].type:
                self.fixtures[k] = self.typed_look(p.looks[cue])
                self.loaded[k] = cue

    def show_stop(self, out: Out) -> None:
        """Stops advancing and keeps the current looks."""
        self.play = None
        out.state()

    def show_delete(self, client: Client, slot: int, out: Out) -> None:
        if self.library_blocked(client, out, needs_output=False):
            return
        if not 1 <= slot <= SHOW_SLOTS:
            self.error(client, "bad-slot", out)
            return
        if slot not in self.shows:
            self.error(client, "empty-slot", out)
            return
        if any(step.sub == slot for show in self.shows.values() for step in show.steps):
            self.error(client, "show-in-use", out)
            return
        if not self.save(client, out, quiet=True):
            return
        del self.shows[slot]
        self.show_changed(slot)
        self.library_changed(out)

    def cmd_lib(self, client: Client, args: list[str], out: Out) -> None:
        if args:
            self.error(client, "bad-command", out)
            return
        out.library(client.cid)

    def tick_playback(self, t: float, out: Out) -> None:
        p = self.play
        if p is None:
            return
        hold = p.flat[p.step].hold / 10
        if t - p.started >= hold:
            late = t - p.started - hold
            p.step = (p.step + 1) % len(p.flat)  # the show loops
            p.started = t - late if late < p.flat[p.step].hold / 10 else t  # keeps the schedule
            self.apply_step()
            out.state()

    # --- timers --------------------------------------------------------------------
    def tick(self, out: Out) -> None:
        """No idle limit (user decision): a session ends on hb loss, never on inactivity alone."""
        t = now()
        self.tick_playback(t, out)
        c = self.cal
        if c is not None:
            if c.owner in self.clients and t - c.last_hb >= self.timing.hb_timeout:
                self.blackout("calibration-timeout", out)  # the firmware uses this reason for debug too
            elif c.phase == "check" and t - c.step_started >= self.timing.check_step:
                c.step = (c.step + 1) % len(CHECK_STEPS)
                if c.step == 0:
                    c.check_complete = True
                c.step_started = t
                out.state()
        for client in self.clients.values():
            if client.admin and t - client.last_admin_action >= self.timing.admin_timeout:
                client.admin = False
                if self.cal is not None and self.cal.owner == client.cid:
                    self.blackout("calibration-owner-left", out)  # a session never outlives its admin
                out.state(client.cid)

    async def ticker(self) -> None:
        while True:
            await asyncio.sleep(TICK_SECONDS)
            out = Out()
            self.tick(out)
            if out.items:
                await self.deliver(out)

    # --- WebSocket ---------------------------------------------------------------
    async def ws_handler(self, ws: ServerConnection) -> None:
        if len(self.clients) >= MAX_CLIENTS:
            await ws.close(1013, "too many clients")
            return
        cid = self.next_cid
        self.next_cid += 1
        address = ws.remote_address
        client = Client(cid, ws, ip=str(address[0]) if address else "?")
        self.clients[cid] = client
        self.log(cid, event="connect")
        out = Out()
        out.state(cid)
        out.library(cid)
        await self.deliver(out)
        try:
            async for message in ws:
                text = message if isinstance(message, str) else ""
                if self.muted:
                    self.log(cid, dropped=text)
                    continue
                self.log(cid, msg=text)
                out = Out()
                self.handle(client, text, out)
                await self.deliver(out)
        except ConnectionClosed:
            pass
        finally:
            del self.clients[cid]
            self.log(cid, event="disconnect")
            out = Out()
            if self.cal is not None and self.cal.owner == cid:
                self.blackout("calibration-owner-left", out)
            await self.deliver(out)

    # --- HTTP ----------------------------------------------------------------------
    async def http_handler(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        try:
            head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), HTTP_HEADER_TIMEOUT_S)
            parts = head.split(b"\r\n", 1)[0].decode("latin-1").split(" ")
            if len(parts) < 2:
                status, ctype, body = "400 Bad Request", "text/plain", b"bad request"
            else:
                url = urlsplit(parts[1])
                status, ctype, body = await self.route(parts[0], url.path, parse_qs(url.query))
            header = (
                f"HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\nContent-Length: {len(body)}\r\n"
                "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
            )
            writer.write(header.encode("latin-1") + body)
            await writer.drain()
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
            pass
        finally:
            writer.close()

    async def route(self, method: str, path: str, query: dict[str, list[str]]) -> tuple[str, str, bytes]:
        if method != "GET":
            return "405 Method Not Allowed", "text/plain", b"GET only"
        if path in PAGES:
            file, ctype = PAGES[path]
            try:
                return "200 OK", ctype, file.read_bytes()
            except OSError as exc:
                return "404 Not Found", "text/plain", f"missing file: {exc}".encode()
        hooks = {
            "/__mock/state": self.hook_state,
            "/__mock/stop-button": self.hook_stop_button,
            "/__mock/config": self.hook_config,
            "/__mock/reset": self.hook_reset,
            "/__mock/debug-raw": self.hook_debug_raw,
            "/__mock/library": self.hook_library,
            "/__mock/mute": self.hook_mute,
        }
        hook = hooks.get(path)
        if hook is None:
            return "404 Not Found", "text/plain", b"not found"
        out = Out()
        try:
            payload = hook({k: v[-1] for k, v in query.items()}, out)
        except ValueError as exc:
            return "400 Bad Request", "text/plain", str(exc).encode()
        await self.deliver(out)
        return "200 OK", "application/json", json.dumps(payload).encode()

    def hook_state(self, _query: dict[str, str], _out: Out) -> dict:
        return {
            "channels": self.channels,  # the laser's (all 0 without one)
            "frame": self.frame(),
            "patch": [{"slot": i + 1, **asdict(s)} for i, s in enumerate(self.patch)],
            "fixtures": [
                {"slot": i + 1, "type": s.type, "addr": s.addr, "channels": self.fixtures[i][:FIXTURE_CHANNELS[s.type]],
                 "frame": self.fixture_frame(i), "preset": self.loaded[i]}
                for i, s in enumerate(self.patch) if s.type != "none"
            ],
            "universe": self.universe(),
            "limits": asdict(self.limits),
            "pin": self.pin,
            "stopHeld": self.stop_held,
            "settingsReset": self.settings_reset,
            "cal": asdict(self.cal) if self.cal else None,
            "clients": [{"id": c.cid, "admin": c.admin} for c in self.clients.values()],
            "wrongTries": self.wrong_tries,
            "lockedFor": max(0.0, round(self.lock_until - now(), 2)),
            "timing": asdict(self.timing),
            "presets": {s: {"type": p.type, "name": p.name, "ch": list(p.ch)} for s, p in self.presets.items()},
            "shows": {s: {"name": sh.name, "steps": format_steps(sh)} for s, sh in self.shows.items()},
            "play": {"show": self.play.show, "step": self.play.step, "steps": len(self.play.flat),
                     "driven": self.play.driven} if self.play else None,
            "preset": self.preset,
            "libraryReset": self.library_reset,
        }

    def hook_mute(self, query: dict[str, str], _out: Out) -> dict:
        self.muted = query.get("on", "1") == "1"
        return {"muted": self.muted}

    def hook_stop_button(self, query: dict[str, str], out: Out) -> dict:
        self.set_stop_button(query.get("held", "1") == "1", out)
        return {"stopHeld": self.stop_held}

    def hook_config(self, query: dict[str, str], _out: Out) -> dict:
        fields = {"hb": "hb_timeout", "checkStep": "check_step", "adminTimeout": "admin_timeout"}
        for key, attr in fields.items():
            if key in query:
                setattr(self.timing, attr, float(query[key]))
        if "failSave" in query:
            self.timing.fail_save = query["failSave"] == "1"
        return asdict(self.timing)

    def hook_reset(self, query: dict[str, str], out: Out) -> dict:
        self.reset_settings(query.get("settingsReset", "0") == "1")
        out.state()
        return {"ok": True}

    def hook_debug_raw(self, query: dict[str, str], out: Out) -> dict:
        if self.cal is None or not self.cal.is_debug:
            raise ValueError("no debug session")
        ch, value = int(query["ch"]), int(query["value"])
        if not (1 <= ch <= CHANNEL_COUNT and 0 <= value <= RAW_MAX):
            raise ValueError("ch 1-10, value 0-255")
        self.cal.raw[ch - 1] = value
        out.state()
        return {"raw": list(self.cal.raw)}

    def hook_library(self, query: dict[str, str], out: Out) -> dict:
        """Empties the library, as if the files were missing (reset=1: corrupt, so libraryReset)."""
        self.presets.clear()
        self.shows.clear()
        self.play = None
        self.set_laser_preset(None)
        self.library_reset = query.get("reset", "0") == "1"
        self.library_changed(out)
        return self.library_json()


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--ws-port", type=int, default=81, help="the pages always connect to port 81")
    parser.add_argument("--pin", default="123456", help="factory PIN (6-8 digits)")
    parser.add_argument("--log", type=Path, help="append every received WebSocket message here (JSON lines)")
    parser.add_argument("--hb-timeout", type=float, default=3.0, help="seconds without hb before a session ends")
    parser.add_argument("--admin-timeout", type=float, default=600.0, help="seconds before an idle admin is logged out")
    parser.add_argument("--check-step", type=float, default=3.0, help="seconds per check step")
    parser.add_argument("--lock-seconds", type=float, default=60.0, help="first PIN lockout (doubles, max 1 h)")
    parser.add_argument("--settings-reset", action="store_true", help="start as if the settings blob was corrupt")
    args = parser.parse_args(argv)
    if not PIN_PATTERN.fullmatch(args.pin):
        parser.error("--pin must be 6-8 digits")
    return args


async def run(args: argparse.Namespace) -> None:
    timing = Timing(
        hb_timeout=args.hb_timeout,
        admin_timeout=args.admin_timeout,
        check_step=args.check_step,
        lock_base=args.lock_seconds,
    )
    mock = MockFirmware(args.pin, timing, args.log, args.settings_reset)
    http_server = await asyncio.start_server(mock.http_handler, args.host, args.http_port)
    async with http_server, serve(mock.ws_handler, args.host, args.ws_port, ping_interval=2, ping_timeout=2):
        print(
            f"mock firmware: http://{args.host}:{args.http_port}/ and /admin, "
            f"WebSocket ws://{args.host}:{args.ws_port}/ (factory PIN {args.pin})",
            flush=True,
        )
        await mock.ticker()


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    except OSError as exc:
        print(f"mock firmware: cannot listen: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
