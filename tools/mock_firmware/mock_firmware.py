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

Library details, mirroring lib/laser_core/src/controller.cpp and src/library_service.cpp:
- play.step is 0-based (like cal.step); play.steps is the step count.
- 'preset load' sets state.preset to that slot and stops a running show; a show step sets
  it to the step's preset; a manual channel command, 'off', '0' or any blackout clears it.
- 'preset save' (presetSaved): a playing show that uses the slot stops (the look stays).
  If no show is playing afterwards, state.preset = slot when the current look equals the
  saved one, else it is cleared if it was that slot. A show still playing keeps its marking.
- 'preset delete' clears state.preset if it was that slot.
- 'show play' refuses (not-calibrated) if ANY step would switch the laser on while
  uncalibrated. 'show stop' with nothing playing is a harmless no-op.
- 'show save' over the playing show, or 'show delete' of it, stops playback (the look stays).
- Step timing keeps its schedule: a late tick shortens the next hold by the lateness.
- Library changes broadcast the library and the state; no 'saved' event is sent for them.

Run:
    uv run --no-project --with websockets python tools/mock_firmware/mock_firmware.py

Test hooks (HTTP GET on the page port; not part of the firmware):
    /__mock/state                         internal state as JSON
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

CHANNEL_COUNT = 10
POSITION_MAX = 127
RAW_MAX = 255
MODE_ON_MIN = 64
MODE_AUTO_MIN = 128  # auto/sound: permanently unavailable outside debug (rule 7)
ADDR_MIN, ADDR_MAX = 1, 503
MAX_CLIENTS = 8  # WEBSOCKETS_SERVER_CLIENT_MAX
MAX_LINE_LENGTH = 255  # "show save" lines can be long
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
PRESET_SLOTS = 32
SHOW_SLOTS = 16
MAX_STEPS = 24
HOLD_MIN, HOLD_MAX = 1, 6000  # tenths of a second
NAME_MAX_BYTES = 32
STEP_PATTERN = re.compile(r"([0-9]{1,2}):([0-9]{1,4})")

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
    name: str
    ch: tuple[int, ...]


@dataclass(frozen=True)
class Show:
    name: str
    steps: tuple[tuple[int, int], ...]  # (preset slot, hold in tenths)


@dataclass
class Playback:
    show: int
    step: int  # 0-based
    started: float


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
        self.preset: int | None = None
        self.reset_settings(settings_reset)

    # --- settings ----------------------------------------------------------------
    def reset_settings(self, settings_reset: bool) -> None:
        self.limits = Limits()
        self.addr = 1
        self.pin = self.factory_pin
        self.settings_reset = settings_reset
        self.channels = [0] * CHANNEL_COUNT
        self.cal: Session | None = None
        self.play = None
        self.preset = None
        self.wrong_tries = 0
        self.lockouts = 0
        self.lock_until = 0.0

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
            "ch": list(self.channels),
            "limits": {
                "ch7": [lim.ch7_low, lim.ch7_high],
                "ch6": {"on": lim.ch6_enabled, "range": [lim.ch6_left, lim.ch6_right]},
                "calibrated": lim.calibrated,
                "allowAuto": False,  # kept for old pages; auto/sound no longer exists
            },
            "addr": self.addr,
            "stopHeld": self.stop_held,
            "admin": client.admin,
            "cal": self.cal_json(client),
            "settingsReset": self.settings_reset,
            "preset": self.preset,
            "play": self.play_json(),
            "libraryReset": self.library_reset,
        }

    def play_json(self) -> dict | None:
        p = self.play
        show = self.shows.get(p.show) if p else None
        if p is None or show is None:
            return None
        hold = show.steps[p.step][1] / 10
        left = max(0, round((hold - (now() - p.started)) * 1000))
        return {"show": p.show, "step": p.step, "steps": len(show.steps), "holdLeftMs": left}

    def library_json(self) -> dict:
        return {
            "type": "library",
            "presets": [{"slot": s, "name": p.name, "ch": list(p.ch)} for s, p in sorted(self.presets.items())],
            "shows": [{"slot": s, "name": sh.name, "steps": [list(st) for st in sh.steps]}
                      for s, sh in sorted(self.shows.items())],
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
        """The DMX frame that would go out (exposed through /__mock/state)."""
        if self.stop_held:
            return [0] * CHANNEL_COUNT
        if self.cal is not None:
            return list(self.cal.raw) if self.cal.is_debug else self.calibration_frame(self.cal)
        frame = [self.clamp_channel(i + 1, v) for i, v in enumerate(self.channels)]
        if not self.is_mode_allowed(frame[0]):
            frame[0] = 0
        return frame

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
        self.channels = [self.clamp_channel(i + 1, v) for i, v in enumerate(self.channels)]

    # --- stopping ----------------------------------------------------------------
    def blackout(self, reason: str, out: Out) -> None:
        self.cal = None
        self.channels = [0] * CHANNEL_COUNT
        self.play = None  # a stop also cancels playback, and nothing resumes it
        self.preset = None
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
        words = line.split()
        if line == "hb":
            if self.cal is not None and self.cal.owner == client.cid:
                t = now()
                self.cal.last_hb = t
                if client.admin:
                    client.last_admin_action = t  # an owner still on the page stays logged in
            return
        if line == "?":
            out.state(client.cid)
            return
        if line in ("0", "off"):
            self.stop_command(line, out)
            return
        if words[0].isdigit():
            self.channel_command(client, words, out)
            return
        handlers: dict[str, Callable[[Client, list[str], Out], None]] = {
            "auth": self.cmd_auth,
            "logout": self.cmd_logout,
            "pin": self.cmd_pin,
            "addr": self.cmd_addr,
            "cal": self.cmd_cal,
            "dbg": self.cmd_dbg,
            "lim": self.cmd_lim,
            "lib": self.cmd_lib,
        }
        if words[0] in ("preset", "show"):
            self.library_command(client, words[0], line, out)
            return
        handler = handlers.get(words[0])
        if handler is None:
            self.error(client, "bad-command", out)
            return
        handler(client, words[1:], out)

    def stop_command(self, line: str, out: Out) -> None:
        """'off' and '0': accepted from anyone, always; they end calibration and debug."""
        reason = "blackout" if line == "0" else "off"
        if self.cal is not None or line == "0":
            self.blackout(reason, out)
            return
        self.channels[0] = 0
        self.play = None
        self.preset = None
        out.all({"type": "stop", "reason": reason})
        out.state()

    def channel_command(self, client: Client, words: list[str], out: Out) -> None:
        if len(words) != 2 or not words[1].isdigit():
            self.error(client, "bad-command", out)
            return
        ch, value = int(words[0]), int(words[1])
        if not (1 <= ch <= CHANNEL_COUNT and 0 <= value <= RAW_MAX):
            self.error(client, "bad-command", out)
            return
        if self.stop_held:
            self.error(client, "stop-held", out)
            return
        c = self.cal
        if c is not None:
            self.session_channel(client, c, ch, value, out)
            return
        if ch == 1 and not self.is_mode_allowed(value):
            self.error(client, "auto-blocked" if value >= MODE_AUTO_MIN else "not-calibrated", out)
            return
        self.channels[ch - 1] = self.clamp_channel(ch, value)
        self.play = None  # a manual change takes over the look
        self.preset = None
        out.state()

    def session_channel(self, client: Client, c: Session, ch: int, value: int, out: Out) -> None:
        if c.owner != client.cid:
            self.error(client, "calibration-busy", out)
            return
        if c.is_debug:
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

    def cmd_addr(self, client: Client, args: list[str], out: Out) -> None:
        if len(args) != 1 or not args[0].isdigit() or not ADDR_MIN <= int(args[0]) <= ADDR_MAX:
            self.error(client, "bad-command", out)
            return
        if not self.require_admin(client, out):
            return
        if self.cal is not None:
            self.error(client, "calibration-busy", out)
            return
        if not self.save(client, out):
            return
        self.addr = int(args[0])
        self.limits = replace(self.limits, calibrated=False)  # the clamp must be re-verified
        self.cal = None
        self.channels = [0] * CHANNEL_COUNT
        self.play = None
        self.preset = None
        self.reclamp()  # firmware commitSettings re-clamps after the blackout
        out.all({"type": "stop", "reason": "address-changed"})
        out.state()

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
        self.channels = [0] * CHANNEL_COUNT  # nothing resumes after calibration ends
        self.play = None
        self.preset = None
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
                self.channels = [0] * CHANNEL_COUNT
                self.play = None
                self.preset = None
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

    def library_command(self, client: Client, kind: str, line: str, out: Out) -> None:
        words = line.split(" ")
        sub = words[1] if len(words) > 1 else ""
        if kind == "preset" and sub == "save":
            self.preset_save(client, words[2:], out)
        elif kind == "show" and sub == "save":
            self.show_save(client, words[2:], out)
        elif kind == "show" and sub == "stop" and len(words) == 2:
            self.show_stop(out)
        elif len(words) == 3 and (kind, sub) in (("preset", "load"), ("preset", "delete"), ("show", "play"),
                                                  ("show", "delete")):
            slot = self.parse_slot(words[2], PRESET_SLOTS if kind == "preset" else SHOW_SLOTS)
            if isinstance(slot, str):
                self.error(client, slot, out)
                return
            action = {"load": self.preset_load, "play": self.show_play}.get(sub)
            if action is None:
                action = self.preset_delete if kind == "preset" else self.show_delete
            action(client, slot, out)
        else:
            self.error(client, "bad-command", out)

    def library_blocked(self, client: Client, out: Out, needs_output: bool) -> bool:
        """Sessions lock the whole library; IO0 blocks only what would drive the laser."""
        if self.cal is not None:
            self.error(client, "calibration-busy", out)
            return True
        if needs_output and self.stop_held:
            self.error(client, "stop-held", out)
            return True
        return False

    def look_of(self, preset: Preset) -> list[int]:
        """A stored look as it may go out now: auto/sound never, CH6/CH7 clamped to today's limits."""
        look = [self.clamp_channel(i + 1, v) for i, v in enumerate(preset.ch)]
        if look[0] >= MODE_AUTO_MIN:
            look[0] = 0
        return look

    def needs_calibration(self, preset: Preset) -> bool:
        return preset.ch[0] >= MODE_ON_MIN and not self.limits.calibrated

    def library_changed(self, out: Out) -> None:
        out.library()
        out.state()

    def preset_save(self, client: Client, args: list[str], out: Out) -> None:
        if not args:
            self.error(client, "bad-command", out)
            return
        slot = self.parse_slot(args[0], PRESET_SLOTS)
        if isinstance(slot, str):
            self.error(client, slot, out)
            return
        name = " ".join(args[1:]).strip()
        if not self.is_valid_name(name):
            self.error(client, "bad-name", out)
            return
        if self.library_blocked(client, out, needs_output=False) or not self.save(client, out, quiet=True):
            return
        look = self.stored_look()
        self.presets[slot] = Preset(name, tuple(look))
        self.preset_saved(slot, look)
        self.library_changed(out)

    def stored_look(self) -> list[int]:
        """Firmware sanitizeLook(): the current look as a preset stores it."""
        look = [self.clamp_channel(i + 1, v) for i, v in enumerate(self.channels)]
        if look[0] >= MODE_AUTO_MIN:
            look[0] = 0
        return look

    def preset_saved(self, slot: int, look: list[int]) -> None:
        """Firmware presetSaved(): a show using the slot has a stale copy, so it stops."""
        p = self.play
        if p is not None and p.show in self.shows and any(s == slot for s, _ in self.shows[p.show].steps):
            self.play = None  # the current look stays
        if self.play is not None:
            return  # the show keeps marking its own step
        if self.stored_look() == look:  # after a blackout the state can hold CH7 below the limit
            self.preset = slot
        elif self.preset == slot:
            self.preset = None

    def preset_load(self, client: Client, slot: int, out: Out) -> None:
        if self.library_blocked(client, out, needs_output=True):
            return
        preset = self.presets.get(slot)
        if preset is None:
            self.error(client, "empty-slot", out)
            return
        if self.needs_calibration(preset):
            self.error(client, "not-calibrated", out)
            return
        self.play = None  # loading a preset takes over from a running show
        self.channels = self.look_of(preset)
        self.preset = slot
        out.state()

    def preset_delete(self, client: Client, slot: int, out: Out) -> None:
        if self.library_blocked(client, out, needs_output=False):
            return
        if slot not in self.presets:
            self.error(client, "empty-slot", out)
            return
        if any(p == slot for show in self.shows.values() for p, _ in show.steps):
            self.error(client, "preset-in-use", out)
            return
        if not self.save(client, out, quiet=True):
            return
        del self.presets[slot]
        if self.preset == slot:
            self.preset = None
        self.library_changed(out)

    def parse_steps(self, text: str) -> tuple[tuple[int, int], ...] | None:
        parts = text.split(",")
        if not 1 <= len(parts) <= MAX_STEPS:
            return None
        steps = []
        for part in parts:
            m = STEP_PATTERN.fullmatch(part)
            if m is None:
                return None
            slot, hold = int(m[1]), int(m[2])
            if slot not in self.presets or not HOLD_MIN <= hold <= HOLD_MAX:
                return None
            steps.append((slot, hold))
        return tuple(steps)

    def show_save(self, client: Client, args: list[str], out: Out) -> None:
        if len(args) < 2:
            self.error(client, "bad-command", out)
            return
        slot = self.parse_slot(args[0], SHOW_SLOTS)
        if isinstance(slot, str):
            self.error(client, slot, out)
            return
        steps = self.parse_steps(args[1])
        if steps is None:
            self.error(client, "bad-steps", out)
            return
        name = " ".join(args[2:]).strip()
        if not self.is_valid_name(name):
            self.error(client, "bad-name", out)
            return
        if self.library_blocked(client, out, needs_output=False) or not self.save(client, out, quiet=True):
            return
        self.shows[slot] = Show(name, steps)
        if self.play is not None and self.play.show == slot:
            self.play = None  # a playing copy would be stale; the current look stays
        self.library_changed(out)

    def show_play(self, client: Client, slot: int, out: Out) -> None:
        if self.library_blocked(client, out, needs_output=True):
            return
        show = self.shows.get(slot)
        if show is None:
            self.error(client, "empty-slot", out)
            return
        if any(self.needs_calibration(self.presets[p]) for p, _ in show.steps):
            self.error(client, "not-calibrated", out)
            return
        self.play = Playback(show=slot, step=0, started=now())
        self.load_step(show)
        out.state()

    def load_step(self, show: Show) -> None:
        slot = show.steps[self.play.step][0]
        self.channels = self.look_of(self.presets[slot])
        self.preset = slot

    def show_stop(self, out: Out) -> None:
        """Stops advancing and keeps the current look."""
        self.play = None
        out.state()

    def show_delete(self, client: Client, slot: int, out: Out) -> None:
        if self.library_blocked(client, out, needs_output=False):
            return
        if slot not in self.shows:
            self.error(client, "empty-slot", out)
            return
        if not self.save(client, out, quiet=True):
            return
        del self.shows[slot]
        if self.play is not None and self.play.show == slot:
            self.play = None  # keeps the current look
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
        show = self.shows.get(p.show)
        if show is None:
            self.play = None
            out.state()
            return
        hold = show.steps[p.step][1] / 10
        if t - p.started >= hold:
            late = t - p.started - hold
            p.step = (p.step + 1) % len(show.steps)  # the show loops
            p.started = t - late if late < show.steps[p.step][1] / 10 else t  # keeps the schedule
            self.load_step(show)
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
            "channels": self.channels,
            "frame": self.frame(),
            "limits": asdict(self.limits),
            "addr": self.addr,
            "pin": self.pin,
            "stopHeld": self.stop_held,
            "settingsReset": self.settings_reset,
            "cal": asdict(self.cal) if self.cal else None,
            "clients": [{"id": c.cid, "admin": c.admin} for c in self.clients.values()],
            "wrongTries": self.wrong_tries,
            "lockedFor": max(0.0, round(self.lock_until - now(), 2)),
            "timing": asdict(self.timing),
            "presets": {s: {"name": p.name, "ch": list(p.ch)} for s, p in self.presets.items()},
            "shows": {s: {"name": sh.name, "steps": [list(st) for st in sh.steps]} for s, sh in self.shows.items()},
            "play": asdict(self.play) if self.play else None,
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
        self.preset = None
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
