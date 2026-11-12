"""Headless UI test for src/web/index.html and src/web/admin.html against the mock firmware.

Starts tools/mock_firmware/mock_firmware.py with short timeouts, drives both pages in the
system Edge (Playwright), and checks both the DOM and what the mock received (its log).
The main page (index) is checked for presets, the quick builder, shows, the desk faders,
tooltips, banners, UIT, reconnects and layout at 844x390 and in portrait.
Screenshots go to <OS temp dir>/dmx_ui_test/. Exits non-zero if any check fails.

Run:
    uv run --no-project --with playwright --with websockets python tools/mock_firmware/test_ui.py
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import time
import urllib.request
from contextlib import ExitStack
from pathlib import Path
from typing import Callable

from playwright.sync_api import Browser, Page, sync_playwright
from playwright.sync_api import TimeoutError as PlaywrightTimeout
from websockets.sync.client import connect as ws_connect

ROOT = Path(__file__).resolve().parents[2]
MOCK = Path(__file__).with_name("mock_firmware.py")
ADMIN_HTML = ROOT / "src" / "web" / "admin.html"
INDEX_HTML = ROOT / "src" / "web" / "index.html"
BASE = "http://127.0.0.1:8080"
WS_URL = "ws://127.0.0.1:81/"
OUT_DIR = Path(tempfile.gettempdir()) / "dmx_ui_test"
LOG_PATH = OUT_DIR / "mock_log.jsonl"
FACTORY_PIN = "123456"
NEW_PIN = "654321"
LANDSCAPE = (844, 390)
PORTRAIT = (390, 844)
MOCK_ARGS = ["--hb-timeout", "3", "--lock-seconds", "3", "--check-step", "0.4"]
INVERTED_TEXT = "Laagste stand 94 · Hoogste stand 10 (omgekeerd: een hoger getal is lager)"
CAL_BANNER = "KALIBRATIE: begrenzing aangepast"
DEBUG_BANNER = "DEBUG: geen begrenzing, richt op een muur"
ERROR_CODES = (
    "bad-command not-admin serial-only calibration-busy not-owner stop-held auto-blocked not-calibrated "
    "wrong-phase need-worst-case-look need-both-marks need-horizontal-marks equal-ends bad-pin locked "
    "not-calibrating save-failed"
).split()
LIBRARY_ERROR_CODES = "bad-slot empty-slot bad-name bad-steps preset-in-use".split()
PATTERNS = json.loads((ROOT / "src" / "web" / "patterns.json").read_text(encoding="utf-8"))["patterns"]
PATTERN_VALUES = [(2 * n - 1) * 128 // 51 for n in range(1, 52)]  # CH2 value of pattern n (both pages and the mock)
INDEX_SESSION_TEXT = "De bediening is vergrendeld"
UNCAL_TEXT = "De laser kan pas aan na het instellen van de hoogtegrens"
STOP_REASONS = (
    "button off blackout calibration-stopped calibration-timeout calibration-owner-left address-changed "
    "calibration-saved debug-stopped"
).split()
OBSOLETE_IN_PAGES = ("need-spin", "check-not-finished", "auto-disabled", "factoryPin", "allowAuto", "idleLeft",
                     "cal color", "cal spin", "cal alive", "auto on", "Straalpad vrij", "Nog bezig")

SET_HIDDEN_JS = """(hidden) => {
  Object.defineProperty(document, 'hidden', { configurable: true, get: () => hidden });
  Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => (hidden ? 'hidden' : 'visible') });
  document.dispatchEvent(new Event('visibilitychange'));
}"""
SET_RANGE_JS = """([sel, value, end]) => {
  const el = document.querySelector(sel);
  el.value = value;
  el.dispatchEvent(new Event('input', { bubbles: true }));
  if (end) el.dispatchEvent(new Event('change', { bubbles: true }));
}"""
LAYOUT_JS = """() => {
  const vw = document.documentElement.clientWidth;
  const problems = [];
  if (document.documentElement.scrollWidth > vw + 1) problems.push(`page scrolls sideways (${document.documentElement.scrollWidth} > ${vw})`);
  const shown = (el) => el.getClientRects().length > 0 && getComputedStyle(el).visibility !== 'hidden';
  const sel = 'button, a, .lcd, .label, .words, .srow, .snote, .toast, .hint, h1, h2, .stepname, .question, .v, input, .banner, .pill, .sessbanner, .tl, .edname';
  for (const el of document.querySelectorAll(sel)) {
    if (!shown(el)) continue;
    const r = el.getBoundingClientRect();
    const name = el.id || String(el.className || el.tagName);
    if (r.right > vw + 1 || r.left < -1) problems.push(`${name} outside the viewport (${Math.round(r.left)}..${Math.round(r.right)})`);
    if (el.tagName !== 'INPUT' && el.scrollWidth > el.clientWidth + 1) problems.push(`${name} text clipped (${el.scrollWidth} > ${el.clientWidth})`);
  }
  const box = document.getElementById('scroller');
  const scrolls = box ? box.scrollHeight > box.clientHeight + 1 : false;
  return { problems, scrolls };
}"""
# Main page: nothing outside the viewport, no clipped text, the page itself never scrolls,
# and the key controls (nav, UIT, the overlay footers) are fully on screen.
INDEX_LAYOUT_JS = """() => {
  const vw = document.documentElement.clientWidth, vh = document.documentElement.clientHeight;
  const problems = [];
  if (document.documentElement.scrollWidth > vw + 1) problems.push(`page scrolls sideways (${document.documentElement.scrollWidth} > ${vw})`);
  if (document.documentElement.scrollHeight > vh + 1) problems.push(`page scrolls vertically (${document.documentElement.scrollHeight} > ${vh})`);
  const shown = (el) => el.getClientRects().length > 0 && getComputedStyle(el).visibility !== 'hidden';
  const sel = 'button, a, .lcd, .label, .banner, .nm, .mt, .name, .meta, .zone, .lbl, .num, .pname, .pcat, .sname, .smeta, '
    + '.stepchip span, .stepchip b, .btab span, .btab small, .pcell span, .opt b, .opt small, .chip, h3, h4, p, .hint, .toast, .conn, .no, figcaption';
  for (const el of document.querySelectorAll(sel)) {
    if (!shown(el)) continue;
    const r = el.getBoundingClientRect();
    const name = el.id || `${el.className || el.tagName}:${(el.textContent || '').trim().slice(0, 24)}`;
    if (r.right > vw + 1 || r.left < -1) problems.push(`${name} outside the viewport (${Math.round(r.left)}..${Math.round(r.right)})`);
    if (el.tagName !== 'INPUT' && el.scrollWidth > el.clientWidth + 1) problems.push(`${name} text clipped (${el.scrollWidth} > ${el.clientWidth})`);
  }
  for (const el of document.querySelectorAll('.nav, #offBtn, #saveBtn, #now, .bfoot, .sfoot, .edtop, .btabs, .pickhead')) {
    if (!shown(el)) continue;
    const r = el.getBoundingClientRect();
    if (r.bottom > vh + 1 || r.top < -1) problems.push(`${el.id || el.className} cut off (${Math.round(r.top)}..${Math.round(r.bottom)})`);
  }
  return problems;
}"""
PATTERN_FETCHES_JS = "performance.getEntriesByType('resource').filter((e) => e.name.endsWith('/patterns.json')).length"


# --- results -------------------------------------------------------------------------
class Results:
    def __init__(self) -> None:
        self.items: list[tuple[str, bool, str]] = []

    def check(self, name: str, ok: object, detail: str = "") -> bool:
        passed = bool(ok)
        self.items.append((name, passed, detail))
        suffix = f"  -> {detail}" if detail and not passed else ""
        print(f"{'PASS' if passed else 'FAIL'}  {name}{suffix}", flush=True)
        return passed

    def info(self, text: str) -> None:
        print(f"INFO  {text}", flush=True)

    @property
    def failures(self) -> list[str]:
        return [name for name, ok, _ in self.items if not ok]


def wait_until(predicate: Callable[[], object], timeout: float = 4.0, interval: float = 0.05) -> object:
    end = time.monotonic() + timeout
    while True:
        try:
            value = predicate()
        except Exception:  # noqa: BLE001 - polling: any failure means "not yet"
            value = None
        if value:
            return value
        if time.monotonic() > end:
            return None
        time.sleep(interval)


# --- mock -------------------------------------------------------------------------------
class MockLog:
    def __init__(self, path: Path) -> None:
        self.path = path

    def entries(self) -> list[dict]:
        if not self.path.exists():
            return []
        out = []
        for line in self.path.read_text(encoding="utf-8").splitlines():
            try:
                out.append(json.loads(line))
            except json.JSONDecodeError:
                continue
        return out

    def mark(self) -> int:
        return len(self.entries())

    def msgs(self, since: int = 0, client: int | None = None) -> list[str]:
        """Commands received, without the index page's '?' link probes."""
        return [e["msg"] for e in self.entries()[since:]
                if "msg" in e and e["msg"] != "?" and (client is None or e["client"] == client)]

    def dropped(self, since: int = 0) -> list[str]:
        """Messages the mock ignored while muted (a dead link)."""
        return [e["dropped"] for e in self.entries()[since:] if "dropped" in e]

    def wait_msg(self, text: str, since: int, client: int | None = None, timeout: float = 3.0) -> bool:
        return bool(wait_until(lambda: text in self.msgs(since, client), timeout))

    def wait_event(self, event: str, since: int, timeout: float = 4.0) -> int | None:
        def find() -> int | None:
            for e in self.entries()[since:]:
                if e.get("event") == event:
                    return e["client"]
            return None

        return wait_until(find, timeout)  # type: ignore[return-value]


def hook(path: str) -> dict:
    with urllib.request.urlopen(BASE + path, timeout=3) as response:
        return json.loads(response.read())


def mock_cal() -> dict | None:
    return hook("/__mock/state")["cal"]


def start_mock() -> subprocess.Popen:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    LOG_PATH.unlink(missing_ok=True)
    try:
        hook("/__mock/state")
        raise SystemExit("Something already listens on 127.0.0.1:8080 (an old mock?). Stop it first.")
    except OSError:
        pass
    stdout = (OUT_DIR / "mock_stdout.txt").open("w", encoding="utf-8")
    proc = subprocess.Popen(
        [sys.executable, str(MOCK), "--log", str(LOG_PATH), "--pin", FACTORY_PIN, *MOCK_ARGS],
        stdout=stdout,
        stderr=subprocess.STDOUT,
    )
    if not wait_until(lambda: hook("/__mock/state"), timeout=10):
        proc.kill()
        raise SystemExit(f"mock firmware did not start, see {OUT_DIR / 'mock_stdout.txt'}")
    return proc


class Raw:
    """A plain WebSocket client for protocol checks the pages can't reach."""

    def __init__(self) -> None:
        self.stack = ExitStack()
        self.ws = self.stack.enter_context(ws_connect(WS_URL, open_timeout=3, max_queue=None))  # idle clients must keep answering pings
        self.state: dict | None = None
        self.recv_until(lambda m: m.get("type") == "state")

    def send(self, text: str) -> None:
        self.ws.send(text)

    def recv_until(self, predicate: Callable[[dict], bool], timeout: float = 3.0) -> dict | None:
        end = time.monotonic() + timeout
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return None
            try:
                msg = json.loads(self.ws.recv(timeout=left))
            except TimeoutError:
                return None
            if msg.get("type") == "state":
                self.state = msg
            if predicate(msg):
                return msg

    def drain(self, quiet: float = 0.1) -> None:
        """Drop everything already received, so a later wait can't match an old broadcast."""
        while self.recv_until(lambda _m: True, timeout=quiet) is not None:
            pass

    def error_for(self, command: str) -> str | None:
        self.drain()
        self.send(command)
        msg = self.recv_until(lambda m: m.get("type") in ("error", "auth"))
        return None if msg is None else msg.get("code", f"auth:{msg.get('ok')}")

    def event(self, command: str, kind: str) -> dict | None:
        self.drain()
        self.send(command)
        return self.recv_until(lambda m: m.get("type") == kind)

    def state_where(self, command: str, predicate: Callable[[dict], bool], timeout: float = 3.0) -> dict | None:
        self.drain()
        self.send(command)
        return self.recv_until(lambda m: m.get("type") == "state" and predicate(m), timeout)

    def close(self) -> None:
        self.stack.close()


# --- page helpers ----------------------------------------------------------------------
class Ui:
    def __init__(self, browser: Browser, results: Results, log: MockLog) -> None:
        self.browser = browser
        self.r = results
        self.log = log
        self.js_errors: list[str] = []

    def open(self, path: str, label: str, size: tuple[int, int] = LANDSCAPE) -> tuple[Page, int | None]:
        context = self.browser.new_context(
            viewport={"width": size[0], "height": size[1]}, device_scale_factor=2, is_mobile=True, has_touch=True
        )
        page = context.new_page()
        page.on("pageerror", lambda exc: self.js_errors.append(f"{label}: {exc}"))
        page.on("console", lambda m: m.type == "error" and self.js_errors.append(f"{label} console: {m.text}"))
        mark = self.log.mark()
        page.goto(BASE + path)
        return page, self.log.wait_event("connect", mark)

    def shot(self, page: Page, name: str) -> Path:
        path = OUT_DIR / f"{name}.png"
        page.screenshot(path=str(path))
        return path

    def layout(self, page: Page, name: str, must_fit: bool = True) -> None:
        result = page.evaluate(LAYOUT_JS)
        self.r.check(f"layout {name}: nothing clipped or off-screen", not result["problems"], "; ".join(result["problems"]))
        if must_fit:
            self.r.check(f"layout {name}: fits without scrolling", not result["scrolls"])
        elif result["scrolls"]:
            self.r.info(f"layout {name}: main area scrolls vertically")

    def index_layout(self, page: Page, name: str) -> None:
        problems = page.evaluate(INDEX_LAYOUT_JS)
        self.r.check(f"layout {name}: nothing clipped, cut off or off-screen", not problems, "; ".join(problems))


def visible(page: Page, selector: str, timeout: float = 3000) -> bool:
    try:
        page.locator(selector).first.wait_for(state="visible", timeout=timeout)
        return True
    except PlaywrightTimeout:
        return False


def hidden(page: Page, selector: str, timeout: float = 3000) -> bool:
    try:
        page.locator(selector).first.wait_for(state="hidden", timeout=timeout)
        return True
    except PlaywrightTimeout:
        return False


def text(page: Page, selector: str) -> str:
    return page.locator(selector).first.text_content() or ""


def text_has(page: Page, selector: str, needle: str, timeout: float = 3.0) -> bool:
    return bool(wait_until(lambda: needle in text(page, selector), timeout))


def box(page: Page, selector: str) -> tuple[float, float, float, float] | None:
    b = page.locator(selector).first.bounding_box()
    return None if b is None else (round(b["x"]), round(b["y"]), round(b["width"]), round(b["height"]))


def set_hidden(page: Page, is_hidden: bool) -> None:
    page.evaluate(SET_HIDDEN_JS, is_hidden)


def set_range(page: Page, selector: str, value: int, end: bool = True) -> None:
    page.evaluate(SET_RANGE_JS, [selector, str(value), end])


def dismiss_toast(page: Page) -> None:
    """The toast floats over the header; close it before tapping something it may cover."""
    if page.locator("#msg").is_visible():
        page.click("#msgClose")
        hidden(page, "#msg")


def tap(page: Page, selector: str) -> None:
    dismiss_toast(page)
    page.click(selector)


def nudge(page: Page, step: int, times: int = 1, axis: int = 7) -> None:
    for _ in range(times):
        page.click(f'#axis{axis} [data-step="{step}"]')


def login(page: Page, pin: str) -> bool:
    page.fill("#pinInput", pin)
    page.click("#loginBtn")
    return visible(page, "#view-overview")


def start_calibration(page: Page) -> bool:
    tap(page, '#nav [data-tab="kalibratie"]')
    page.click("#calStartBtn")
    return visible(page, "#view-adjust")


def start_debug(page: Page) -> bool:
    tap(page, '#nav [data-tab="debug"]')
    page.click("#dbgStartBtn")
    return visible(page, "#view-debug")


def mark_inverted(page: Page, ui: Ui, a_id: int | None) -> None:
    """From the start at 64: low end 94, high end 10 (an inverted mount)."""
    mark = ui.log.mark()
    nudge(page, 10, 3)
    ui.log.wait_msg("7 94", mark, a_id)
    page.wait_for_timeout(700)
    page.click("#mark-low7")
    text_has(page, "#mark-low7 b", "94")
    set_range(page, "#slider7", 10)
    ui.log.wait_msg("7 10", mark, a_id)
    page.wait_for_timeout(700)
    page.click("#mark-high7")
    text_has(page, "#mark-high7 b", "10")


def index_tab(page: Page, tab: str) -> None:
    tap(page, f'#nav [data-tab="{tab}"]')


def fader_y(box_: dict, frac: float) -> float:
    """Screen y of a fader position (0 = bottom, 1 = top); 6 px inset like the page."""
    return box_["y"] + 6 + (1 - frac) * (box_["height"] - 12)


def drag_fader(page: Page, ch: int, frac_from: float, frac_to: float) -> None:
    b = page.locator(f'#desk [data-ch="{ch}"] .track').bounding_box()
    x = b["x"] + b["width"] / 2
    page.mouse.move(x, fader_y(b, frac_from))
    page.mouse.down()
    page.mouse.move(x, fader_y(b, frac_to), steps=6)
    page.mouse.up()


def channel_msgs(log: MockLog, since: int, client: int | None, ch: int) -> list[int]:
    return [int(m.split()[1]) for m in log.msgs(since, client) if re.fullmatch(rf"{ch} \d+", m)]


def pattern_name(value: int) -> str:
    return PATTERNS[min(50, value * 51 // 256)]["name"]


def mock_lib() -> dict:
    state = hook("/__mock/state")
    return {"presets": state["presets"], "shows": state["shows"], "play": state["play"], "preset": state["preset"]}


# --- scenarios: login, index ------------------------------------------------------------------
def test_login(ui: Ui, a: Page, a_id: int | None) -> None:
    r, log = ui.r, ui.log
    r.check("admin: page is Dutch (lang=nl)", a.evaluate("document.documentElement.lang") == "nl")
    r.check("admin: login screen shown", visible(a, "#view-login"))
    r.check("admin: back link '← Terug' to /", a.get_attribute("#backLink", "href") == "/"
            and text(a, "#backLink").strip() == "← Terug")
    ui.shot(a, "admin_login")
    ui.layout(a, "admin login")
    mark = log.mark()
    a.fill("#pinInput", "000000")
    a.click("#loginBtn")
    r.check("admin: wrong PIN sent as 'auth 000000'", log.wait_msg("auth 000000", mark, a_id))
    r.check("admin: wrong PIN shows attempts left", text_has(a, "#loginNote", "Verkeerde pincode. Nog 4 pogingen"),
            text(a, "#loginNote"))
    r.check("admin: still on the login screen after a wrong PIN", a.locator("#view-login").is_visible())
    mark = log.mark()
    r.check("admin: right PIN opens the Kalibratie tab", login(a, FACTORY_PIN))
    r.check("admin: right PIN sent", log.wait_msg(f"auth {FACTORY_PIN}", mark, a_id))
    r.check("admin: tabs Kalibratie / Debug / Instellingen",
            [t.strip() for t in a.locator("#nav button").all_text_contents()] == ["Kalibratie", "Debug", "Instellingen"])
    r.check("admin: overview says 'Nog niet ingesteld'", text_has(a, "#limitWords", "Nog niet ingesteld"))
    r.check("admin: overview shows the default full-width side limit", text_has(a, "#hWords", "Volle breedte"),
            text(a, "#hWords"))
    r.check("admin: no factory-PIN warning", "abriekspincode" not in a.inner_text("body")
            and a.locator("#factoryWarn").count() == 0)
    r.check("admin: PIN not kept in storage",
            a.evaluate(f"!JSON.stringify(Object.assign({{}}, localStorage, sessionStorage)).includes('{FACTORY_PIN}')"))
    ui.shot(a, "admin_overview_uncalibrated")
    ui.layout(a, "admin overview")


def test_heartbeat(ui: Ui, a_id: int | None) -> None:
    mark = ui.log.mark()
    time.sleep(3.2)
    count = ui.log.msgs(mark, a_id).count("hb")
    ui.r.check("admin: sends hb about every 1000 ms", 2 <= count <= 5, f"{count} hb in 3.2 s")


def index_connected(page: Page) -> bool:
    return bool(wait_until(lambda: "ok" in (page.get_attribute("#conn", "class") or "").split()))


def test_index_uncalibrated(ui: Ui) -> tuple[Page, int | None]:
    r, log = ui.r, ui.log
    i, i_id = ui.open("/", "index")
    r.check("index: connects", index_connected(i))
    mark = log.mark()
    r.check("index: page is Dutch (lang=nl)", i.evaluate("document.documentElement.lang") == "nl")
    r.check("index: nav Presets / Shows / Lichttafel / Uitleg",
            [t.strip() for t in i.locator("#nav button").all_text_contents()] == ["Presets", "Shows", "Lichttafel", "Uitleg"])
    r.check("index: side panel has Nu card, '＋ Bewaar als preset', UIT and a Beheer link",
            i.locator("#now").is_visible() and text(i, "#saveBtn").strip() == "＋ Bewaar als preset"
            and text(i, "#offBtn").strip() == "UIT" and i.get_attribute("#adminLink", "href") == "/admin"
            and text(i, "#adminLink").strip() == "Beheer")
    r.check("index: uncalibrated banner text", text_has(i, "#uncalBanner", UNCAL_TEXT), text(i, "#uncalBanner"))
    r.check("index: uncalibrated banner links to /admin", i.locator('#uncalBanner a[href="/admin"]').count() == 1)
    r.check("index: Nu card says 'Laser uit'", text_has(i, "#nowName", "Laser uit"), text(i, "#nowName"))
    r.check("index: /patterns.json fetched", bool(wait_until(lambda: i.evaluate(PATTERN_FETCHES_JS) == 1)))
    r.check("index: empty preset list explains how to start", text_has(i, "#tiles", "Nog geen presets"))
    ui.shot(i, "index_uncalibrated")
    ui.index_layout(i, "index uncalibrated")
    tap(i, "#addTile")
    r.check("builder (uncalibrated): opens", visible(i, "#builder"))
    i.click('#bPane [data-pattern="2"]')
    r.check("builder (uncalibrated): a shape tap sends the look ('2 7')", log.wait_msg("2 7", mark, i_id))
    i.wait_for_timeout(300)
    r.check("builder (uncalibrated): CH1 never sent", not channel_msgs(log, mark, i_id, 1), str(log.msgs(mark, i_id)))
    r.check("builder (uncalibrated): Opslaan disabled", i.locator("#bSave").is_disabled())
    r.check("builder (uncalibrated): status explains the height limit", text_has(i, "#bStatus", "hoogtegrens"), text(i, "#bStatus"))
    tap(i, "#bCancel")
    index_tab(i, "desk")
    tap(i, '#desk [data-ch="1"] .power')
    i.wait_for_timeout(300)
    r.check("desk (uncalibrated): the switch can't turn it on", not channel_msgs(log, mark, i_id, 1), str(log.msgs(mark, i_id)))
    r.check("desk (uncalibrated): explains why", text_has(i, "#msgText", UNCAL_TEXT), text(i, "#msgText"))
    r.check("desk (uncalibrated): POS-V says 'Geen grens'", text_has(i, '#desk [data-ch="7"] .zone', "Geen grens"))
    i.wait_for_timeout(800)
    r.check("index: sends no hb", "hb" not in log.msgs(mark, i_id))
    index_tab(i, "presets")
    return i, i_id


def test_lockout(ui: Ui, a_id: int | None) -> None:
    r, log = ui.r, ui.log
    b, b_id = ui.open("/admin", "admin-B")
    visible(b, "#view-login")
    mark = log.mark()

    def auth_tries() -> list[str]:
        return [m for m in log.msgs(mark, b_id) if m.startswith("auth ")]

    for k in range(5):
        b.fill("#pinInput", f"99999{k}")
        b.click("#loginBtn")
        wait_until(lambda: len(auth_tries()) > k and not text(b, "#loginBtn").startswith("Bezig"))
    tries = auth_tries()
    r.check("lockout: 5 wrong PINs sent", len(tries) == 5, str(tries))
    r.check("lockout: countdown shown", text_has(b, "#loginNote", "Probeer het opnieuw over 0:0"), text(b, "#loginNote"))
    r.check("lockout: login button disabled while locked", b.locator("#loginBtn").is_disabled())
    ui.shot(b, "admin_locked")
    raw = Raw()
    locked = raw.event(f"auth {FACTORY_PIN}", "auth")
    r.check("lockout: even the right PIN is refused while locked",
            locked is not None and not locked["ok"] and locked["lockedSeconds"] > 0, str(locked))
    raw.close()
    admins = {c["id"]: c["admin"] for c in hook("/__mock/state")["clients"]}
    r.check("lockout: existing admin session kept", admins.get(a_id) is True, str(admins))
    r.check("lockout: login possible again after the lock",
            bool(wait_until(lambda: b.locator("#loginBtn").is_enabled(), timeout=6)))
    r.check("lockout: right PIN works after the lock", login(b, FACTORY_PIN))
    mark = log.mark()
    tap(b, '#nav [data-tab="instellingen"]')
    b.click("#logoutBtn")
    r.check("admin: Uitloggen (Instellingen) sends 'logout'", log.wait_msg("logout", mark, b_id))
    r.check("admin: back on the login screen after logout", visible(b, "#view-login"))
    b.context.close()


# --- scenarios: calibration ----------------------------------------------------------------------
def test_cal_start(ui: Ui, a: Page, a_id: int | None, i: Page) -> int:
    """Returns the log mark from just before 'cal start' (to prove no 'lim check' was needed)."""
    r, log = ui.r, ui.log
    header = box(a, ".top")
    mark = log.mark()
    a.click("#calStartBtn")
    r.check("cal start: 'cal start' sent at once (no confirmation)", log.wait_msg("cal start", mark, a_id, timeout=1.0))
    r.check("cal start: no confirm sheet in the page", a.locator("#sheet, [role=dialog]").count() == 0)
    r.check("cal start: adjust view shown", visible(a, "#view-adjust"))
    r.check("cal start: CH7 starts at 64", text_has(a, "#lcd7", "064"), text(a, "#lcd7"))
    r.check("cal start: CH6 starts at 0", text_has(a, "#tabVal6", "000"), text(a, "#tabVal6"))
    r.check("cal start: default look is Ruit", "on" in (a.get_attribute('#lookSeg [data-look="diamond"]', "class") or ""))
    r.check("cal start: Zijgrens on by default", text_has(a, "#hState", "AAN"))
    r.check("cal start: summary says 'volle breedte'", text_has(a, "#summary", "volle breedte"), text(a, "#summary"))
    r.check("cal start: marks enabled in Ruit", a.locator("#mark-low7").is_enabled())
    r.check("cal start: no colour picker", a.locator("#swatches, [data-color], .sw").count() == 0)
    r.check("cal start: no idle countdown or 'Nog bezig'", a.locator("#idleLcd, #aliveBtn").count() == 0)
    r.check("cal start: red calibration banner on admin", visible(a, "#sessBanner") and CAL_BANNER in text(a, "#sessBanner"))
    r.check("cal start: header height unchanged", box(a, ".top")[3] == header[3], f"{header} -> {box(a, '.top')}")
    r.check("index: calibration banner shown", visible(i, "#sessionBanner") and "KALIBRATIE" in text(i, "#sessionBanner")
            and INDEX_SESSION_TEXT in text(i, "#sessionBanner"), text(i, "#sessionBanner"))
    r.check("index: controls locked during calibration",
            i.evaluate("getComputedStyle(document.querySelector('#tiles')).pointerEvents") == "none"
            and i.evaluate("getComputedStyle(document.querySelector('#saveBtn')).pointerEvents") == "none")
    r.check("index: UIT stays usable during calibration",
            i.evaluate("getComputedStyle(document.querySelector('#offBtn')).pointerEvents") != "none")
    r.check("index: Nu card says the controls are locked", text_has(i, "#nowName", "Bediening vergrendeld"))
    r.info(f"admin: screen wake = {a.evaluate('document.body.dataset.wake || null')}")
    ui.shot(i, "index_calibrating")
    ui.shot(a, "admin_calibration_start")
    return mark


def test_look_and_marks(ui: Ui, a: Page, a_id: int | None) -> None:
    r, log = ui.r, ui.log
    before = {sel: box(a, sel) for sel in ("#saveBtn", "#lcd7", "#mark-low7", "#offBtn")}
    mark = log.mark()
    tap(a, '#lookSeg [data-look="square"]')
    r.check("look: Vierkant sends 'cal look square'", log.wait_msg("cal look square", mark, a_id))
    r.check("look: marks disabled in Vierkant", bool(wait_until(lambda: a.locator("#mark-low7").is_disabled())))
    r.check("look: hint explains marks need Ruit or Draaien", text_has(a, "#lookHint", "Ruit of Draaien"), text(a, "#lookHint"))
    after = {sel: box(a, sel) for sel in before}
    r.check("look: nothing moves when the hint changes", before == after, f"{before} -> {after}")
    ui.shot(a, "admin_calibration_square")
    tap(a, '#lookSeg [data-look="spin"]')
    r.check("look: Draaien sends 'cal look spin'", log.wait_msg("cal look spin", mark, a_id))
    r.check("look: marks enabled in Draaien", bool(wait_until(lambda: a.locator("#mark-low7").is_enabled())))
    tap(a, '#lookSeg [data-look="diamond"]')
    r.check("look: Ruit sends 'cal look diamond'", log.wait_msg("cal look diamond", mark, a_id))
    r.check("look: Ruit selected again", bool(wait_until(
        lambda: "on" in (a.get_attribute('#lookSeg [data-look="diamond"]', "class") or ""))))


def test_move_and_mark(ui: Ui, a: Page, a_id: int | None) -> None:
    r, log = ui.r, ui.log
    mark = log.mark()
    nudge(a, 10, 3)
    r.check("move: +10 x3 sends 7 74 / 7 84 / 7 94", log.wait_msg("7 94", mark, a_id)
            and [m for m in log.msgs(mark, a_id) if m.startswith("7 ")] == ["7 74", "7 84", "7 94"],
            str(log.msgs(mark, a_id)))
    r.check("move: readout shows 094", text_has(a, "#lcd7", "094"))
    a.wait_for_timeout(700)
    a.click("#mark-low7")
    r.check("mark: low end stored at 94", text_has(a, "#mark-low7 b", "94"), text(a, "#mark-low7 b"))
    r.check("save: disabled with one mark", a.locator("#saveBtn").is_disabled())
    r.check("check: disabled with one mark", a.locator("#checkBtn").is_disabled())
    r.check("save: hint asks for the high end", "hoogste" in text(a, "#saveHint"), text(a, "#saveHint"))
    mark = log.mark()
    set_range(a, "#slider7", 20)
    r.check("slider: sends '7 20'", log.wait_msg("7 20", mark, a_id))
    nudge(a, -10, 2)
    r.check("clamp: -10 from 10 sends '7 0', never negative", log.wait_msg("7 0", mark, a_id)
            and not any(m.startswith("7 -") for m in log.msgs(mark, a_id)), str(log.msgs(mark, a_id)))
    nudge(a, -10)
    a.wait_for_timeout(200)
    r.check("clamp: -10 at 0 sends nothing", log.msgs(mark, a_id).count("7 0") == 1, str(log.msgs(mark, a_id)))
    nudge(a, 10)
    r.check("move: back to 10", log.wait_msg("7 10", mark, a_id) and text_has(a, "#lcd7", "010"))
    a.wait_for_timeout(700)
    a.click("#mark-high7")
    r.check("mark: high end stored at 10", text_has(a, "#mark-high7 b", "10"), text(a, "#mark-high7 b"))
    r.check("summary: low 94 / high 10 / inverted", text_has(a, "#summary", "Laagste stand94")
            and "Hoogste stand10" in text(a, "#summary") and "omgekeerd" in text(a, "#summary"), text(a, "#summary"))
    r.check("save: enabled with both marks", a.locator("#saveBtn").is_enabled())
    r.check("check: enabled with both marks (optional)", a.locator("#checkBtn").is_enabled())
    ui.shot(a, "admin_calibration")
    ui.layout(a, "admin calibration")


def test_horizontal(ui: Ui, a: Page, a_id: int | None) -> None:
    r, log = ui.r, ui.log
    tap(a, "#tab6")
    r.check("side: CH6 panel shown", visible(a, "#mark-left6"))
    r.check("side: left/right pre-marked 0 / 127", text(a, "#mark-left6 b") == "0" and text(a, "#mark-right6 b") == "127",
            f"{text(a, '#mark-left6 b')} / {text(a, '#mark-right6 b')}")
    r.check("side: hint says marking is optional", "optioneel" in text(a, "#lookHint"), text(a, "#lookHint"))
    mark = log.mark()
    nudge(a, 10, axis=6)
    r.check("side: +10 sends '6 10'", log.wait_msg("6 10", mark, a_id))
    a.wait_for_timeout(700)
    a.click("#mark-left6")
    r.check("side: narrowed limit in the summary (10 · 127)", text_has(a, "#summary", "10 · 127"), text(a, "#summary"))
    ui.shot(a, "admin_calibration_side")
    ui.layout(a, "admin calibration side")
    nudge(a, -10, axis=6)
    log.wait_msg("6 0", mark, a_id)
    a.wait_for_timeout(700)
    a.click("#mark-left6")
    r.check("side: back to 'volle breedte'", text_has(a, "#summary", "volle breedte"), text(a, "#summary"))
    tap(a, "#hBtn")
    r.check("side: 'lim h off' sent", log.wait_msg("lim h off", mark, a_id))
    r.check("side: summary says 'uit'", bool(wait_until(lambda: text(a, "#summary").endswith("Zijgrensuit"))), text(a, "#summary"))
    r.check("side: Zijkant tab disabled and CH7 panel back", a.locator("#tab6").is_disabled() and visible(a, "#mark-low7"))
    tap(a, "#hBtn")
    r.check("side: 'lim h on' sent", log.wait_msg("lim h on", mark, a_id))
    r.check("side: full width again", text_has(a, "#summary", "volle breedte"))


def test_save_without_check(ui: Ui, a: Page, a_id: int | None, since_start: int) -> None:
    r, log = ui.r, ui.log
    mark = log.mark()
    a.click("#saveBtn")
    r.check("save (no check): 'lim save' sent from adjust", log.wait_msg("lim save", mark, a_id))
    r.check("save (no check): no 'lim check' was needed", "lim check" not in log.msgs(since_start, a_id))
    r.check("save (no check): success message", text_has(a, "#msgText", "opgeslagen") or text_has(a, "#msgText", "Opgeslagen"),
            text(a, "#msgText"))
    r.check("save (no check): back on the overview", visible(a, "#view-overview"))
    r.check("save (no check): overview shows the limits in words", text_has(a, "#limitWords", INVERTED_TEXT), text(a, "#limitWords"))
    limits = hook("/__mock/state")["limits"]
    r.check("save (no check): mock stored 94 / 10, side full width",
            (limits["ch7_low"], limits["ch7_high"], limits["calibrated"], limits["ch6_enabled"], limits["ch6_left"],
             limits["ch6_right"]) == (94, 10, True, True, 0, 127), str(limits))
    r.check("save (no check): banner gone", hidden(a, "#sessBanner"))
    ui.shot(a, "admin_overview_saved")


def test_index_mapped(ui: Ui, i: Page, i_id: int | None) -> None:
    """The desk after the inverted calibration (low end 94, high end 10, side full width)."""
    r, log = ui.r, ui.log
    r.check("index: uncalibrated banner gone", hidden(i, "#uncalBanner"))
    r.check("index: calibration banner gone", hidden(i, "#sessionBanner"))
    index_tab(i, "desk")
    r.check("desk: 10 faders", i.locator("#desk .strip").count() == 10)
    pos_v = '#desk [data-ch="7"] .track'
    r.check("desk: POS-V spans the limit (10..94)",
            (i.get_attribute(pos_v, "aria-valuemin"), i.get_attribute(pos_v, "aria-valuemax")) == ("10", "94"))
    mark = log.mark()
    drag_fader(i, 7, 0.5, -0.1)
    r.check("desk: POS-V bottom = low end (94)", log.wait_msg("7 94", mark, i_id), str(log.msgs(mark, i_id)))
    r.check("desk: POS-V bottom says 'Laagst'", text_has(i, '#desk [data-ch="7"] .zone', "Laagst"))
    drag_fader(i, 7, 0.5, 1.1)
    r.check("desk: POS-V top = high end (10)", log.wait_msg("7 10", mark, i_id), str(log.msgs(mark, i_id)))
    r.check("desk: POS-V never leaves the limit", all(10 <= v <= 94 for v in channel_msgs(log, mark, i_id, 7)),
            str(channel_msgs(log, mark, i_id, 7)))
    r.check("desk: POS-H spans the full-width side limit (0..127)",
            i.get_attribute('#desk [data-ch="6"] .track', "aria-valuemax") == "127")
    power = '#desk [data-ch="1"] .power'
    r.check("desk: Modus is a switch, not a fader", i.locator(power).count() == 1
            and i.locator('#desk [data-ch="1"] .track').count() == 0)
    mark = log.mark()
    i.wait_for_timeout(200)
    r.check("desk: opening the desk sends no CH1 (rule 5)", not channel_msgs(log, mark, i_id, 1), str(log.msgs(mark, i_id)))
    tap(i, power)
    r.check("desk: tapping the switch sends '1 96'", log.wait_msg("1 96", mark, i_id), str(log.msgs(mark, i_id)))
    r.check("desk: switch lit and says AAN", bool(wait_until(lambda: "on" in (i.get_attribute(power, "class") or "").split()))
            and text_has(i, f"{power} .word", "AAN") and i.get_attribute(power, "aria-pressed") == "true")
    tap(i, power)
    r.check("desk: tapping again sends '1 0'", log.wait_msg("1 0", mark, i_id), str(log.msgs(mark, i_id)))
    # The user's case: UIT, change some values, then switch it back on from the desk (no preset needed).
    tap(i, power)
    log.wait_msg("1 96", mark, i_id)
    mark = log.mark()
    i.click("#offBtn")
    r.check("desk: UIT turns it off; the switch offers ZET AAN", log.wait_msg("off", mark, i_id) and text_has(i, f"{power} .word", "ZET AAN"))
    r.check("desk: the Nu card points to the switch", text_has(i, "#nowMeta", "ZET AAN"), text(i, "#nowMeta"))
    drag_fader(i, 8, 0.2, 0.6)
    tap(i, power)
    r.check("desk: after UIT and a fader change, the switch turns it back on",
            log.wait_msg("1 96", mark, i_id) and bool(wait_until(lambda: hook("/__mock/state")["frame"][0] == 96)),
            str(log.msgs(mark, i_id)))
    tap(i, power)
    log.wait_msg("1 0", mark, i_id)
    r.check("desk: Modus only ever sends 0 or 96", set(channel_msgs(log, mark, i_id, 1)) <= {0, 96},
            str(channel_msgs(log, mark, i_id, 1)))
    r.check("desk: Patroon has 51 positions", i.locator('#desk [data-ch="2"] .detent').count() == 51)
    mark = log.mark()
    drag_fader(i, 2, 0.1, 0.42)
    wait_until(lambda: channel_msgs(log, mark, i_id, 2))
    i.wait_for_timeout(200)
    sent = channel_msgs(log, mark, i_id, 2)
    r.check("desk: Patroon snaps to pattern values", bool(sent) and all(v in PATTERN_VALUES for v in sent), str(sent))
    last = sent[-1] if sent else 0
    pname = '#desk [data-ch="2"] .zone'
    r.check("desk: Patroon shows the pattern name", text_has(i, pname, pattern_name(last)),
            f"{text(i, pname)} vs {pattern_name(last)}")
    r.check("desk: CH10 is a plain 0-255 fader", i.get_attribute('#desk [data-ch="10"] .track', "aria-valuemax") == "255"
            and i.locator('#desk [data-ch="10"] .detent').count() == 0)
    mark = log.mark()
    i.focus('#desk [data-ch="10"] .track')
    i.keyboard.press("ArrowUp")
    r.check("desk: keyboard ArrowUp on CH10 sends '10 1'", log.wait_msg("10 1", mark, i_id), str(log.msgs(mark, i_id)))
    ui.shot(i, "index_desk")
    ui.index_layout(i, "index desk")
    index_tab(i, "presets")


def test_optional_check(ui: Ui, a: Page, a_id: int | None) -> None:
    r, log = ui.r, ui.log
    r.check("check round: calibration starts again", start_calibration(a))
    r.check("check round: starts at 64 again, not the saved end", text_has(a, "#lcd7", "064"), text(a, "#lcd7"))
    mark_inverted(a, ui, a_id)
    mark = log.mark()
    a.click("#checkBtn")
    r.check("check: 'lim check' sent", log.wait_msg("lim check", mark, a_id))
    r.check("check: check view shown", visible(a, "#view-check"))
    first = text(a, "#checkStep")
    r.check("check: step text 'Stap n/15: name'", bool(re.fullmatch(r"Stap \d+/15: .+", first)), first)
    r.check("check: Opslaan available at once", a.locator("#checkSaveBtn").is_enabled())
    r.check("check: steps advance", bool(wait_until(lambda: text(a, "#checkStep") != first, timeout=3)))
    ui.shot(a, "admin_check")
    ui.layout(a, "admin check")
    a.click("#redoBtn")
    r.check("check: Terug sends 'lim redo'", log.wait_msg("lim redo", mark, a_id))
    r.check("check: back to adjusting", visible(a, "#view-adjust"))
    hook("/__mock/config?checkStep=30")
    mark = log.mark()
    a.click("#checkBtn")
    visible(a, "#view-check")
    cal = mock_cal() or {}
    r.check("check: round still at its first step", cal.get("phase") == "check" and cal.get("step") == 0
            and cal.get("check_complete") is False, str(cal))
    a.click("#checkSaveBtn")
    r.check("check: Opslaan mid-round sends 'lim save'", log.wait_msg("lim save", mark, a_id))
    r.check("check: saved mid-round, back on the overview", visible(a, "#view-overview")
            and text_has(a, "#limitWords", INVERTED_TEXT))
    r.check("check: mock stored 94 / 10", (hook("/__mock/state")["limits"]["ch7_low"], hook("/__mock/state")["limits"]["ch7_high"])
            == (94, 10))
    hook("/__mock/config?checkStep=0.4")


def test_long_idle(ui: Ui, a: Page, a_id: int | None) -> None:
    """No idle limit: a session survives while hb keeps coming, even past the admin expiry."""
    r = ui.r
    r.check("idle: calibration started", start_calibration(a))
    hook("/__mock/config?adminTimeout=2")
    time.sleep(4.5)
    state = hook("/__mock/state")
    admins = {c["id"]: c["admin"] for c in state["clients"]}
    r.check("idle: session still running after 4.5 s without actions", state["cal"] is not None)
    r.check("idle: owner's hb keeps the admin login alive", admins.get(a_id) is True, str(admins))
    r.check("idle: page still on the adjust view", a.locator("#view-adjust").is_visible())
    hook("/__mock/config?adminTimeout=600")


def test_off_from_index(ui: Ui, a: Page, i: Page, i_id: int | None) -> None:
    r, log = ui.r, ui.log
    mark = log.mark()
    i.click("#offBtn")
    r.check("off from index: 'off' sent by index", log.wait_msg("off", mark, i_id))
    r.check("off from index: admin calibration ended", visible(a, "#view-overview"))
    r.check("off from index: admin explains why", text_has(a, "#msgText", "UIT ingedrukt"), text(a, "#msgText"))
    r.check("off from index: saved limits unchanged", text_has(a, "#limitWords", INVERTED_TEXT))
    r.check("off from index: index banner gone", hidden(i, "#sessionBanner"))
    r.check("off from index: index shows the stop reason in Dutch", text_has(i, "#msgText", "UIT ingedrukt"), text(i, "#msgText"))


# --- scenarios: main page (calibrated: low end 94, high end 10) ---------------------------------------------
TESTLOOK = [96, 102, 140, 0, 0, 0, 52, 40, 224, 128]  # #21 Ster, alle kleuren, draaien, klein, midden, punten


def test_builder(ui: Ui, i: Page, i_id: int | None) -> None:
    r, log = ui.r, ui.log
    index_tab(i, "presets")
    mark = log.mark()
    tap(i, "#addTile")
    r.check("builder: opens from '＋ Nieuw'", visible(i, "#builder"))
    i.wait_for_timeout(800)
    r.check("builder: opening sends nothing (rule 5)", log.msgs(mark, i_id) == [], str(log.msgs(mark, i_id)))
    r.check("builder: Opslaan disabled until a shape is chosen", i.locator("#bSave").is_disabled())
    cats = [t.strip() for t in i.locator("#bPane .cats .chip").all_text_contents()]
    r.check("builder: category chips", cats == ["Vormen", "Lijnen", "Bogen en golven", "Figuren", "Cijfers", "Zoomcirkels"], str(cats))
    seen: list[int] = []
    for cat in cats:
        i.click(f'#bPane .cats [data-cat="{cat}"]')
        seen += [int(n) for n in i.locator("#bPane .pcell").evaluate_all("els => els.map(e => e.dataset.pattern)")]
    r.check("builder: all 50 drawings, #48 (duplicate of #47) hidden", sorted(seen) == [n for n in range(1, 52) if n != 48],
            str(sorted(seen)))
    i.click('#bPane .cats [data-cat="Vormen"]')
    r.check("builder: the user's names under the drawings", text(i, '#bPane [data-pattern="21"] span') == "Ster")
    ui.shot(i, "index_builder_empty")
    r.check("builder: still nothing sent after browsing", log.msgs(mark, i_id) == [], str(log.msgs(mark, i_id)))
    i.click('#bPane [data-pattern="21"]')
    r.check("builder: a shape tap switches the laser on ('1 96')", log.wait_msg("1 96", mark, i_id), str(log.msgs(mark, i_id)))
    i.wait_for_timeout(200)
    sent = log.msgs(mark, i_id)
    r.check("builder: the look goes first, CH1 = 96 last", "2 102" in sent and "7 52" in sent and sent[-1] == "1 96", str(sent))
    r.check("builder: status says live", text_has(i, "#bStatus", "Live op de laser"), text(i, "#bStatus"))
    for tab, opt, expected in (("kleur", 2, "9 224"), ("beweging", 1, "3 140"), ("grootte", 2, "8 40"), ("lijnen", 1, "10 128")):
        i.click(f'#bTabs [data-tab="{tab}"]')
        if tab == "kleur":
            r.check("builder: Kleur offers only Mix / Eén kleur, wisselend / Alle kleuren",
                    [t.strip() for t in i.locator("#bPane .opt b").all_text_contents()] == ["Mix", "Eén kleur, wisselend", "Alle kleuren"])
            r.check("builder: Mix has a slider (0-63)", i.get_attribute("#mixSlider", "max") == "63")
            ui.shot(i, "index_builder_color")
        i.click(f'#bPane .opt[data-opt="{opt}"]')
        r.check(f"builder: {tab} choice sends '{expected}'", log.wait_msg(expected, mark, i_id), str(log.msgs(mark, i_id)))
    r.check("builder: CH1 = 96 sent only once", log.msgs(mark, i_id).count("1 96") == 1, str(log.msgs(mark, i_id)))
    raw = Raw()
    raw.send("off")  # someone else presses UIT while the builder is open
    raw.close()
    r.check("builder: after UIT, Opslaan is disabled", bool(wait_until(lambda: i.locator("#bSave").is_disabled())))
    r.check("builder: after UIT, status says tap a choice", text_has(i, "#bStatus", "Tik een keuze"), text(i, "#bStatus"))
    i.click('#bTabs [data-tab="lijnen"]')
    mark = log.mark()
    set_range(i, "#speedSlider", 40)
    i.wait_for_timeout(300)
    sent = log.msgs(mark, i_id)
    r.check("builder: releasing a slider after UIT sends the value but not CH1", "10 168" in sent and "1 96" not in sent, str(sent))
    i.click('#bPane .opt[data-opt="1"]')
    r.check("builder: a choice tap switches it back on", log.wait_msg("1 96", mark, i_id), str(log.msgs(mark, i_id)))
    r.check("builder: Opslaan enabled again", bool(wait_until(lambda: i.locator("#bSave").is_enabled())))
    set_range(i, "#speedSlider", 0)  # back to TESTLOOK
    i.wait_for_timeout(200)
    i.click('#bTabs [data-tab="vorm"]')
    ui.shot(i, "index_builder")
    ui.index_layout(i, "index builder")
    i.fill("#bName", "Testlook")
    mark = log.mark()
    i.click("#bSave")
    r.check("builder: Opslaan sends 'preset save 1 Testlook'", log.wait_msg("preset save 1 Testlook", mark, i_id),
            str(log.msgs(mark, i_id)))
    r.check("builder: closes after the library echo", hidden(i, "#builder"))
    r.check("builder: new tile shown", text_has(i, '.tile[data-slot="1"] .name', "Testlook"))
    r.check("builder: mock stored the look", (mock_lib()["presets"].get("1") or {}).get("ch") == TESTLOOK, str(mock_lib()["presets"]))
    r.check("builder: saved preset highlighted (state.preset)", bool(wait_until(
        lambda: "on" in (i.get_attribute('.tile[data-slot="1"]', "class") or "").split())))
    r.check("builder: Nu card shows the preset", text_has(i, "#nowName", "Testlook"), text(i, "#nowName"))
    r.check("builder: confirmation in Dutch", text_has(i, "#msgText", "bewaard"), text(i, "#msgText"))


def test_save_sheet(ui: Ui, i: Page, i_id: int | None) -> None:
    r, log = ui.r, ui.log
    index_tab(i, "desk")
    mark = log.mark()
    i.click('#desk [data-ch="2"] .pstep button[aria-label="Volgend patroon"]')
    r.check("desk: Patroon ▲ steps to the next pattern ('2 107')", log.wait_msg("2 107", mark, i_id), str(log.msgs(mark, i_id)))
    r.check("desk: scribble line shows its name", text_has(i, '#desk [data-ch="2"] .zone', "Sinusgolf"))
    r.check("sheet: manual change clears the preset highlight", text_has(i, "#nowName", "Eigen instelling"), text(i, "#nowName"))
    tap(i, "#saveBtn")
    r.check("sheet: '＋ Bewaar als preset' opens the sheet", visible(i, "#sheet"))
    r.check("sheet: shows the current look", text_has(i, "#sLook", "#22"), text(i, "#sLook"))
    ui.shot(i, "index_save_sheet")
    ui.index_layout(i, "index save sheet")
    i.fill("#sName", "Tweede")
    i.click("#sSave")
    r.check("sheet: sends 'preset save 2 Tweede'", log.wait_msg("preset save 2 Tweede", mark, i_id), str(log.msgs(mark, i_id)))
    r.check("sheet: closes after saving", hidden(i, "#sheet"))
    r.check("sheet: mock stored the current look", (mock_lib()["presets"].get("2") or {}).get("ch", [0, 0])[1] == 107)
    r.check("sheet: stays on the desk", i.locator("#view-desk").is_visible())
    r.check("sheet: Nu card shows the saved preset", text_has(i, "#nowName", "Tweede"), text(i, "#nowName"))


def test_tiles(ui: Ui, i: Page, i_id: int | None) -> None:
    r, log = ui.r, ui.log
    index_tab(i, "presets")
    r.check("tiles: two presets plus '＋ Nieuw'", i.locator(".tile[data-slot]").count() == 2 and i.locator("#addTile").is_visible())
    mark = log.mark()
    i.click('.tile[data-slot="1"] .hit')
    r.check("tiles: tap sends 'preset load 1'", log.wait_msg("preset load 1", mark, i_id), str(log.msgs(mark, i_id)))
    r.check("tiles: mock shows the preset", bool(wait_until(lambda: hook("/__mock/state")["channels"] == TESTLOOK)))
    r.check("tiles: tile 1 highlighted, tile 2 not", bool(wait_until(
        lambda: "on" in (i.get_attribute('.tile[data-slot="1"]', "class") or "").split()
        and "on" not in (i.get_attribute('.tile[data-slot="2"]', "class") or "").split())))
    r.check("tiles: Nu card shows 'Testlook'", text_has(i, "#nowName", "Testlook"))
    r.check("tiles: meta names the pattern", text_has(i, '.tile[data-slot="1"] .meta', "Ster"), text(i, '.tile[data-slot="1"] .meta'))
    ui.shot(i, "index_presets")
    ui.index_layout(i, "index presets")


def test_shows(ui: Ui, i: Page, i_id: int | None) -> None:
    r, log = ui.r, ui.log
    index_tab(i, "shows")
    r.check("shows: empty state explains shows", text_has(i, "#shows", "Nog geen shows"))
    r.check("shows: header button '＋ Nieuwe show'", text(i, "#tabAction").strip() == "＋ Nieuwe show")
    tap(i, "#tabAction")
    r.check("shows: editor opens with the preset picker", visible(i, "#editor") and visible(i, "#ePicker"))
    i.click('#ePickGrid [data-pick="1"]')
    r.check("shows: step 1 added", visible(i, '#eSteps .srow[data-k="0"]'))
    i.fill('#eSteps .srow[data-k="0"] input', "0,5")
    i.press('#eSteps .srow[data-k="0"] input', "Enter")
    r.check("shows: hold time typed as '0,5'", bool(wait_until(lambda: i.input_value('#eSteps .srow[data-k="0"] input') == "0,5")))
    i.click("#eAdd")
    i.click('#ePickGrid [data-pick="2"]')
    r.check("shows: step 2 copies the last hold time", bool(wait_until(lambda: i.input_value('#eSteps .srow[data-k="1"] input') == "0,5")))
    i.click('#eSteps .srow[data-k="1"] .hold button:last-child')
    r.check("shows: + makes a step longer (0,5 -> 1)", bool(wait_until(lambda: i.input_value('#eSteps .srow[data-k="1"] input') == "1")))
    i.click('#eSteps .srow[data-k="1"] .hold button:first-child')
    i.fill("#eName", "Testshow")
    ui.shot(i, "index_show_editor")
    ui.index_layout(i, "index show editor")
    mark = log.mark()
    i.click("#eSave")
    r.check("shows: Opslaan sends 'show save 1 1:5,2:5 Testshow'", log.wait_msg("show save 1 1:5,2:5 Testshow", mark, i_id),
            str(log.msgs(mark, i_id)))
    r.check("shows: editor closes", hidden(i, "#editor"))
    chips = [t.strip() for t in i.locator('.show[data-show="1"] .stepchip').all_inner_texts()]
    r.check("shows: steps as chips (preset name + seconds)", len(chips) == 2 and "Testlook" in chips[0] and "0,5 s" in chips[0]
            and "Tweede" in chips[1], str(chips))
    i.click('[data-play="1"]')
    r.check("shows: play sends 'show play 1'", log.wait_msg("show play 1", mark, i_id), str(log.msgs(mark, i_id)))
    r.check("shows: Nu card follows the playback", text_has(i, "#nowLabel", "Show loopt") and text_has(i, "#nowName", "Testshow"),
            f"{text(i, '#nowLabel')} / {text(i, '#nowName')}")
    r.check("shows: Nu card reaches step 2", text_has(i, "#nowMeta", "Stap 2/2"), text(i, "#nowMeta"))
    r.check("shows: and loops back to step 1", text_has(i, "#nowMeta", "Stap 1/2"), text(i, "#nowMeta"))
    r.check("shows: progress bar with 2 segments", i.locator("#nowProg i").count() == 2)
    r.check("shows: playing step highlighted", i.locator('.show[data-show="1"] .stepchip.cur').count() == 1)
    r.check("shows: play button turns into stop", text(i, '[data-play="1"]') == "■")
    ui.shot(i, "index_shows")
    ui.index_layout(i, "index shows")
    i.click('[data-play="1"]')
    r.check("shows: stop sends 'show stop'", log.wait_msg("show stop", mark, i_id))
    r.check("shows: Nu card leaves 'Show loopt'", bool(wait_until(lambda: text(i, "#nowLabel") != "Show loopt")), text(i, "#nowLabel"))
    r.check("shows: mock stopped, look kept", mock_lib()["play"] is None and hook("/__mock/state")["channels"][0] == 96)
    i.click('[data-edit="1"]')
    r.check("shows: Bewerken opens the show", visible(i, "#editor") and i.input_value("#eName") == "Testshow"
            and i.locator("#eSteps .srow").count() == 2)
    i.click('#eSteps [data-up="1"]')
    mark = log.mark()
    i.click("#eSave")
    r.check("shows: reordered and re-saved ('show save 1 2:5,1:5 Testshow')",
            log.wait_msg("show save 1 2:5,1:5 Testshow", mark, i_id), str(log.msgs(mark, i_id)))
    hidden(i, "#editor")


def test_delete(ui: Ui, i: Page, i_id: int | None) -> None:
    r, log = ui.r, ui.log
    index_tab(i, "presets")
    tap(i, "#tabAction")
    r.check("delete: 'Bewerken' shows delete buttons", visible(i, '.tile[data-slot="1"] .del') and text(i, "#tabAction") == "Klaar")
    r.check("delete: no '＋ Nieuw' while editing", i.locator("#addTile").count() == 0)
    i.click('.tile[data-slot="1"] .del')
    r.check("delete: asks first", visible(i, '.tile[data-slot="1"] .confirm'))
    ui.shot(i, "index_presets_edit")
    ui.index_layout(i, "index presets edit")
    mark = log.mark()
    i.click('.tile[data-slot="1"] .confirm .btn.red')
    r.check("delete: sends 'preset delete 1'", log.wait_msg("preset delete 1", mark, i_id))
    r.check("delete: preset-in-use explained in Dutch, naming the show",
            text_has(i, "#msgText", "zit nog in de show ‘Testshow’"), text(i, "#msgText"))
    r.check("delete: preset 1 kept", "1" in mock_lib()["presets"] and i.locator('.tile[data-slot="1"]').count() == 1)
    tap(i, "#tabAction")
    index_tab(i, "shows")
    i.click('[data-edit="1"]')
    i.click("#eDelete")
    r.check("delete show: asks to tap again", text_has(i, "#eDelete", "Zeker"))
    mark = log.mark()
    i.click("#eDelete")
    r.check("delete show: sends 'show delete 1'", log.wait_msg("show delete 1", mark, i_id))
    r.check("delete show: card gone", bool(wait_until(lambda: i.locator('.show[data-show="1"]').count() == 0)))
    index_tab(i, "presets")
    tap(i, "#tabAction")
    i.click('.tile[data-slot="2"] .del')
    i.click('.tile[data-slot="2"] .confirm .btn.red')
    r.check("delete: sends 'preset delete 2'", log.wait_msg("preset delete 2", mark, i_id))
    r.check("delete: tile gone", bool(wait_until(lambda: i.locator('.tile[data-slot="2"]').count() == 0)))
    r.check("delete: mock has only preset 1 left", list(mock_lib()["presets"]) == ["1"], str(mock_lib()["presets"]))
    r.check("delete: confirmed in Dutch", text_has(i, "#msgText", "verwijderd"), text(i, "#msgText"))
    tap(i, "#tabAction")


def test_tooltips_and_guide(ui: Ui, i: Page) -> None:
    r = ui.r
    index_tab(i, "desk")
    tap(i, '#desk [data-ch="7"] .lbl')
    r.check("tip: tapping POS-V opens its explanation", visible(i, "#tip") and "Positie verticaal" in text(i, "#tip"))
    r.check("tip: POS-V tells the limit (94 / 10)", "94" in text(i, "#tip") and "10" in text(i, "#tip"), text(i, "#tip"))
    r.check("tip: unknown behaviour marked TESTEN", i.locator("#tip .test").count() >= 1)
    ui.shot(i, "index_desk_tip")
    ui.index_layout(i, "index desk tooltip")
    i.click('#desk [data-ch="3"] .num')
    r.check("tip: tapping elsewhere closes it", hidden(i, "#tip"))
    tap(i, '#desk [data-ch="9"] .lbl')
    r.check("tip: Kleur says a fixed colour isn't possible", text_has(i, "#tip", "Eén vaste losse kleur"), text(i, "#tip"))
    tap(i, '#desk [data-ch="8"] .lbl')
    i.click("#tip .more")
    r.check("tip: 'Meer in Uitleg' opens the guide at Grootte", visible(i, "#view-guide") and text_has(i, "#garticle h3", "CH8 · Grootte"))
    topics = i.locator("#gnav button").count()
    r.check("guide: basics, safety and all 10 channels", topics == 12, str(topics))
    i.click('#gnav [data-topic="ch9"]')
    r.check("guide: CH9 based on the hardware test", text_has(i, "#garticle", "kan deze laser niet")
            and i.locator("#garticle .test").count() >= 1, text(i, "#garticle")[:200])
    i.click('#gnav [data-topic="veilig"]')
    r.check("guide: safety page mentions UIT and IO0", text_has(i, "#garticle", "IO0") and "UIT" in text(i, "#garticle"))
    i.click('#gnav [data-topic="ch3"]')
    ui.shot(i, "index_guide")
    ui.index_layout(i, "index guide")


def test_off_and_library_reset(ui: Ui, i: Page, i_id: int | None) -> None:
    r, log = ui.r, ui.log
    index_tab(i, "presets")
    mark = log.mark()
    i.click('.tile[data-slot="1"] .hit')
    log.wait_msg("preset load 1", mark, i_id)
    wait_until(lambda: hook("/__mock/state")["channels"][0] == 96)
    i.click("#offBtn")
    r.check("UIT: sends 'off'", log.wait_msg("off", mark, i_id))
    r.check("UIT: Nu card says 'Laser uit'", text_has(i, "#nowName", "Laser uit"), text(i, "#nowName"))
    r.check("UIT: mock laser off", hook("/__mock/state")["frame"][0] == 0)
    hook("/__mock/library?reset=1")
    r.check("library reset: banner shown", visible(i, "#libraryBanner") and "beschadigd" in text(i, "#libraryBanner"))
    r.check("library reset: tiles gone", bool(wait_until(lambda: i.locator(".tile[data-slot]").count() == 0)))
    ui.shot(i, "index_library_reset")
    ui.index_layout(i, "index library reset")
    hook("/__mock/library?reset=0")
    r.check("library reset: banner gone again", hidden(i, "#libraryBanner"))


def test_index_reconnect(ui: Ui, i: Page, i_id: int | None) -> int | None:
    r, log = ui.r, ui.log
    mark = log.mark()
    set_hidden(i, True)
    r.check("index hidden: socket closed (frees the slot)", log.wait_event("disconnect", mark) == i_id)
    mark = log.mark()
    set_hidden(i, False)
    new_id = log.wait_event("connect", mark)
    r.check("index visible: reconnects", new_id is not None and index_connected(i))
    i.wait_for_timeout(1700)
    sent = log.msgs(mark, new_id)
    r.check("index visible: nothing re-sent after the reconnect", not [m for m in sent if m != "lib"], str(sent))
    r.check("index: /patterns.json fetched only once", i.evaluate(PATTERN_FETCHES_JS) == 1, str(i.evaluate(PATTERN_FETCHES_JS)))
    return new_id


def test_dead_link(ui: Ui, i: Page) -> None:
    """The socket stays open but nothing comes back (the phone left the WiFi, the ESP32 hung)."""
    r, log = ui.r, ui.log
    raw = Raw()
    raw.send("1 96")
    raw.close()
    wait_until(lambda: hook("/__mock/state")["frame"][0] == 96)
    hook("/__mock/mute?on=1")
    mark = log.mark()
    i.click("#offBtn")
    r.check("dead link: UIT unconfirmed -> error toast", text_has(i, "#msgText", "niet bevestigd", timeout=4), text(i, "#msgText"))
    r.check("dead link: header says 'Geen verbinding' within ~6 s",
            bool(wait_until(lambda: "Geen verbinding" in text(i, "#conn"), timeout=10)), text(i, "#conn"))
    dropped = log.dropped(mark)
    r.check("dead link: the page probed with '?' before giving up", "?" in dropped and "off" in dropped, str(dropped))
    hook("/__mock/mute?on=0")
    raw = Raw()
    raw.send("off")
    raw.close()
    r.check("dead link: recovers once the link works again", text_has(i, "#nowName", "Laser uit", timeout=12)
            and "Verbonden" in text(i, "#conn"), text(i, "#conn"))
    r.check("dead link: library received again after recovering", bool(wait_until(lambda: i.evaluate("() => libSeen"), timeout=6)))


def seed_library() -> None:
    """Two presets and a show through a raw client, for the portrait screenshots."""
    raw = Raw()
    for command in ("2 102", "3 140", "9 224", "1 96", "preset save 1 Ster draaiend", "2 7", "10 128", "preset save 2 Puntjescirkel",
                    "show save 1 1:50,2:30 Avondshow", "show play 1"):
        raw.send(command)
        time.sleep(0.05)
    raw.close()


def test_index_portrait(ui: Ui) -> None:
    r = ui.r
    seed_library()
    p, _ = ui.open("/", "index-portrait", PORTRAIT)
    r.check("portrait: connects", index_connected(p))
    r.check("portrait: tiles shown", visible(p, '.tile[data-slot="2"]'))
    ui.shot(p, "index_portrait_presets")
    ui.index_layout(p, "index portrait presets")
    index_tab(p, "shows")
    ui.shot(p, "index_portrait_shows")
    ui.index_layout(p, "index portrait shows")
    index_tab(p, "desk")
    ui.shot(p, "index_portrait_desk")
    ui.index_layout(p, "index portrait desk")
    index_tab(p, "guide")
    ui.index_layout(p, "index portrait guide")
    index_tab(p, "presets")
    tap(p, "#addTile")
    visible(p, "#builder")
    ui.shot(p, "index_portrait_builder")
    ui.index_layout(p, "index portrait builder")
    tap(p, "#bCancel")
    p.click("#offBtn")
    p.context.close()
    hook("/__mock/library?reset=0")


# --- scenarios: debug ------------------------------------------------------------------------------
def test_debug_session(ui: Ui, a: Page, a_id: int | None, i: Page) -> None:
    r, log = ui.r, ui.log
    tap(a, '#nav [data-tab="debug"]')
    r.check("debug: intro with 'Debug starten'", visible(a, "#view-debug-idle") and a.locator("#dbgStartBtn").is_enabled())
    ui.shot(a, "admin_debug_idle")
    ui.layout(a, "admin debug intro")
    mark = log.mark()
    a.click("#dbgStartBtn")
    r.check("debug: 'dbg start' sent", log.wait_msg("dbg start", mark, a_id))
    r.check("debug: session view shown", visible(a, "#view-debug"))
    r.check("debug: red banner on admin", visible(a, "#sessBanner") and text(a, "#sessBanner") == DEBUG_BANNER, text(a, "#sessBanner"))
    r.check("debug: index banner 'DEBUG: geen begrenzing'", text_has(i, "#sessionBanner", "DEBUG: geen begrenzing"),
            text(i, "#sessionBanner"))
    r.check("debug: index controls locked",
            i.evaluate("getComputedStyle(document.querySelector('#desk')).pointerEvents") == "none")
    r.check("debug: all 10 channels start at 000",
            [t.strip() for t in a.locator("#dbgTiles .lcd").all_text_contents()] == ["000"] * 10)
    a.click('#dbgTiles [data-ch="7"]')
    r.check("debug: CH7 selected with its real behaviour", text_has(a, "#edName", "Verticale positie")
            and "64" in text(a, "#edNote"), text(a, "#edNote"))
    set_range(a, "#dbgSlider", 200)
    r.check("debug: raw '7 200' sent", log.wait_msg("7 200", mark, a_id))
    r.check("debug: mock frame carries CH7 = 200 (no limit)", bool(wait_until(lambda: hook("/__mock/state")["frame"][6] == 200)))
    r.check("debug: page shows 200", text_has(a, '#dbgTiles [data-ch="7"] .lcd', "200") and text_has(a, "#dbgLcd", "200"))
    a.click('#dbgStepper [data-step="1"]')
    r.check("debug: +1 sends '7 201'", log.wait_msg("7 201", mark, a_id))
    hook("/__mock/debug-raw?ch=9&value=33")
    r.check("debug: page mirrors cal.raw (CH9 = 033)", text_has(a, '#dbgTiles [data-ch="9"] .lcd', "033"))
    set_range(a, "#dbgSlider", 150, end=False)  # finger still down
    log.wait_msg("7 150", mark, a_id)
    hook("/__mock/debug-raw?ch=7&value=5")
    a.wait_for_timeout(400)
    r.check("debug: an echo doesn't fight the drag", a.input_value("#dbgSlider") == "150" and text(a, "#dbgLcd") == "150",
            f"{a.input_value('#dbgSlider')} / {text(a, '#dbgLcd')}")
    set_range(a, "#dbgSlider", 150)
    r.check("debug: release keeps the user's value", bool(wait_until(lambda: hook("/__mock/state")["frame"][6] == 150)))
    for ch, value in ((1, 96), (2, 2), (3, 64)):
        a.click(f'#dbgTiles [data-ch="{ch}"]')
        set_range(a, "#dbgSlider", value)
    a.click('#dbgTiles [data-ch="7"]')
    wait_until(lambda: hook("/__mock/state")["frame"][2] == 64)
    ui.shot(a, "admin_debug")
    ui.layout(a, "admin debug")
    mark = log.mark()
    raw = Raw()
    raw.send("0")
    r.check("debug: '0' from another client", log.wait_msg("0", mark))
    r.check("debug: '0' ends the session", visible(a, "#view-debug-idle") and mock_cal() is None)
    r.check("debug: admin explains the stop", text_has(a, "#msgText", "Alles is op 0 gezet"), text(a, "#msgText"))
    r.check("debug: index banner gone", hidden(i, "#sessionBanner"))
    r.check("debug: index explains the stop in Dutch", text_has(i, "#msgText", "Alles is op 0 gezet"), text(i, "#msgText"))
    raw.close()


def test_debug_exits(ui: Ui, a: Page, a_id: int | None, i: Page) -> int | None:
    r, log = ui.r, ui.log
    r.check("debug stop: session started", start_debug(a))
    mark = log.mark()
    a.click("#sessStopBtn")
    r.check("debug stop: 'Debug stoppen' sends 'dbg stop'", log.wait_msg("dbg stop", mark, a_id))
    r.check("debug stop: explained", text_has(a, "#msgText", "Debug gestopt"), text(a, "#msgText"))
    r.check("debug stop: back on the intro", visible(a, "#view-debug-idle"))
    r.check("debug 'Alles 0': session started", start_debug(a))
    mark = log.mark()
    a.click("#allZeroBtn")
    r.check("debug 'Alles 0': zeroes all 10 channels", log.wait_msg("1 0", mark, a_id) and log.wait_msg("10 0", mark, a_id))
    r.check("debug 'Alles 0': the debug session keeps running", mock_cal() is not None
            and not visible(a, "#view-debug-idle"))
    a.click("#sessStopBtn")
    r.check("debug 'Alles 0': stopped again afterwards", bool(wait_until(lambda: mock_cal() is None)))
    r.check("debug hidden: session started", start_debug(a))
    mark = log.mark()
    set_hidden(a, True)
    r.check("debug hidden: socket closed when the page is hidden", log.wait_event("disconnect", mark) == a_id)
    r.check("debug hidden: mock ended the session", bool(wait_until(lambda: mock_cal() is None)))
    r.check("debug hidden: index banner gone", hidden(i, "#sessionBanner"))
    mark = log.mark()
    set_hidden(a, False)
    new_id = log.wait_event("connect", mark)
    r.check("debug hidden: reconnects and logs in again", new_id is not None and log.wait_msg(f"auth {FACTORY_PIN}", mark, new_id))
    r.check("debug hidden: back on the Debug intro", visible(a, "#view-debug-idle"))
    r.check("debug hidden: explains the session stopped", text_has(a, "#msgText", "debugsessie is gestopt"), text(a, "#msgText"))
    a.wait_for_timeout(1200)
    r.check("debug hidden: session NOT resumed", "dbg start" not in log.msgs(mark, new_id))
    return new_id


# --- scenarios: settings, stop, PIN, address ----------------------------------------------------------
def test_settings(ui: Ui, a: Page) -> None:
    r = ui.r
    tap(a, '#nav [data-tab="instellingen"]')
    r.check("settings: view shown", visible(a, "#view-settings"))
    r.check("settings: saved limits in words", text_has(a, "#setLimitWords", INVERTED_TEXT), text(a, "#setLimitWords"))
    r.check("settings: side limit in words", text_has(a, "#setHWords", "Zijgrens: volle breedte"), text(a, "#setHWords"))
    r.check("settings: no auto/sound toggle", a.locator("#autoBtn, #autoState").count() == 0
            and not re.search(r"geluid|\bauto\b", a.inner_text("#view-settings"), re.I))
    r.check("settings: logout button here", a.locator("#logoutBtn").is_visible())
    ui.shot(a, "admin_settings")
    ui.layout(a, "admin settings")


def test_stop_button(ui: Ui, a: Page, i: Page) -> None:
    r = ui.r
    hook("/__mock/stop-button?held=1")
    r.check("IO0: admin status says 'IO0 ingedrukt'", text_has(a, "#now", "IO0 ingedrukt"), text(a, "#now"))
    r.check("IO0: index shows the stop banner", visible(i, "#stopBanner") and "IO0" in text(i, "#stopBanner"))
    r.check("IO0: index Nu card says 'IO0 ingedrukt'", text_has(i, "#now", "IO0 ingedrukt"), text(i, "#now"))
    r.check("IO0: index controls locked", i.evaluate("getComputedStyle(document.querySelector('#tiles')).pointerEvents") == "none")
    ui.shot(i, "index_stop_held")
    r.check("IO0: admin explains the stop", text_has(a, "#msgText", "Stopknop (IO0) ingedrukt"))
    tap(a, '#nav [data-tab="kalibratie"]')
    r.check("IO0: Kalibratie starten disabled while held", a.locator("#calStartBtn").is_disabled())
    hook("/__mock/stop-button?held=0")
    r.check("IO0: status back to normal after release", bool(wait_until(lambda: "IO0" not in text(a, "#now")))
            and hidden(i, "#stopBanner"))


def test_pin_change(ui: Ui, a: Page, a_id: int | None) -> None:
    r, log = ui.r, ui.log
    tap(a, '#nav [data-tab="instellingen"]')
    mark = log.mark()
    a.fill("#pinOld", FACTORY_PIN)
    a.fill("#pinNew", NEW_PIN)
    a.fill("#pinNew2", "654320")
    a.click("#pinBtn")
    r.check("PIN: mismatch caught in the page", text_has(a, "#msgText", "niet gelijk"))
    r.check("PIN: nothing sent on a mismatch", not any(m.startswith("pin ") for m in log.msgs(mark, a_id)))
    a.fill("#pinOld", "111111")
    a.fill("#pinNew2", NEW_PIN)
    a.click("#pinBtn")
    r.check("PIN: wrong current PIN sent", log.wait_msg(f"pin 111111 {NEW_PIN}", mark, a_id))
    r.check("PIN: wrong current PIN explained", text_has(a, "#msgText", "Huidige pincode klopt niet"), text(a, "#msgText"))
    r.check("PIN: still logged in after a wrong current PIN", a.locator("#view-settings").is_visible())
    a.fill("#pinOld", FACTORY_PIN)
    a.click("#pinBtn")
    r.check("PIN: 'pin <old> <new>' sent", log.wait_msg(f"pin {FACTORY_PIN} {NEW_PIN}", mark, a_id))
    r.check("PIN: success message", text_has(a, "#msgText", "Pincode gewijzigd"), text(a, "#msgText"))
    r.check("PIN: mock has the new PIN", hook("/__mock/state")["pin"] == NEW_PIN)


def test_hidden_ends_calibration(ui: Ui, a: Page, a_id: int | None, i: Page) -> int | None:
    r, log = ui.r, ui.log
    r.check("hidden: calibration started", start_calibration(a))
    mark = log.mark()
    set_hidden(a, True)
    r.check("hidden: socket closed when the page is hidden", log.wait_event("disconnect", mark) == a_id)
    r.check("hidden: mock ended the calibration", bool(wait_until(lambda: mock_cal() is None)))
    r.check("hidden: index calibration banner gone", hidden(i, "#sessionBanner"))
    mark = log.mark()
    set_hidden(a, False)
    new_id = log.wait_event("connect", mark)
    r.check("visible: reconnects", new_id is not None)
    r.check("visible: re-sends auth with the new PIN", log.wait_msg(f"auth {NEW_PIN}", mark, new_id))
    r.check("visible: back on the overview", visible(a, "#view-overview"))
    r.check("visible: explains the calibration stopped", text_has(a, "#msgText", "verbinding was even weg"), text(a, "#msgText"))
    a.wait_for_timeout(1200)
    r.check("visible: calibration NOT resumed", "cal start" not in log.msgs(mark, new_id))
    return new_id


def test_address(ui: Ui, a: Page, a_id: int | None, i: Page) -> None:
    r, log = ui.r, ui.log
    tap(a, '#nav [data-tab="instellingen"]')
    visible(a, "#view-settings")
    mark = log.mark()
    a.fill("#addrInput", "5")
    a.click("#addrBtn")
    r.check("addr: 'addr 5' sent", log.wait_msg("addr 5", mark, a_id))
    state = hook("/__mock/state")
    r.check("addr: mock uncalibrated at address 5", state["addr"] == 5 and not state["limits"]["calibrated"], str(state))
    r.check("addr: stop reason shown", text_has(a, "#msgText", "DMX-adres gewijzigd"), text(a, "#msgText"))
    tap(a, '#nav [data-tab="kalibratie"]')
    r.check("addr: overview says 'Nog niet ingesteld' again", text_has(a, "#limitWords", "Nog niet ingesteld"))
    r.check("addr: index uncalibrated banner back", visible(i, "#uncalBanner"))


def test_logout(ui: Ui, a: Page, a_id: int | None) -> None:
    mark = ui.log.mark()
    tap(a, '#nav [data-tab="instellingen"]')
    a.click("#logoutBtn")
    ui.r.check("logout: 'logout' sent", ui.log.wait_msg("logout", mark, a_id))
    ui.r.check("logout: login screen", visible(a, "#view-login"))


def test_portrait(ui: Ui) -> None:
    r = ui.r
    p, p_id = ui.open("/admin", "admin-portrait", PORTRAIT)
    r.check("portrait: login", login(p, NEW_PIN))
    ui.shot(p, "admin_portrait_overview")
    ui.layout(p, "portrait overview", must_fit=False)
    r.check("portrait: calibration starts", start_calibration(p))
    r.check("portrait: UIT button visible", p.locator("#offBtn").is_visible())
    ui.shot(p, "admin_portrait_calibration")
    ui.layout(p, "portrait calibration", must_fit=False)
    mark = ui.log.mark()
    p.click("#sessStopBtn")
    r.check("portrait: Kalibratie stoppen sends 'cal stop'", ui.log.wait_msg("cal stop", mark, p_id))
    r.check("portrait: back on the overview", visible(p, "#view-overview"))
    r.check("portrait: debug starts", start_debug(p))
    ui.shot(p, "admin_portrait_debug")
    ui.layout(p, "portrait debug", must_fit=False)
    p.click("#sessStopBtn")
    r.check("portrait: debug stopped", visible(p, "#view-debug-idle"))
    tap(p, '#nav [data-tab="instellingen"]')
    ui.shot(p, "admin_portrait_settings")
    ui.layout(p, "portrait settings", must_fit=False)
    p.context.close()


def test_back_link(ui: Ui) -> None:
    p, _ = ui.open("/admin", "admin-back")
    visible(p, "#view-login")
    p.click("#backLink")
    ui.r.check("back link: opens the main page", bool(wait_until(lambda: p.url == BASE + "/")) and index_connected(p))
    p.context.close()


# --- protocol checks with raw clients ------------------------------------------------------------------
def test_protocol(r: Results) -> None:
    """State-machine checks on the mock with raw clients (the pages prevent most of these)."""
    owner, other = Raw(), Raw()
    r.check("proto: auth ok", (owner.event(f"auth {NEW_PIN}", "auth") or {}).get("ok") is True)
    owner.send("cal start")
    mine = owner.recv_until(lambda m: m.get("type") == "state" and m.get("cal") is not None)
    theirs = other.recv_until(lambda m: m.get("type") == "state" and m.get("cal") is not None)
    r.check("proto: cal state is per client (owner true / other false)",
            mine is not None and theirs is not None and mine["cal"]["owner"] is True
            and theirs["cal"]["owner"] is False and theirs["admin"] is False)
    cal = (mine or {}).get("cal") or {}
    r.check("proto: starts at CH7 64 / CH6 0 in Ruit", (cal.get("ch7"), cal.get("ch6"), cal.get("look")) == (64, 0, "diamond"),
            str(cal))
    r.check("proto: side limit on, pre-marked 0 / 127", cal.get("hOn") is True
            and cal.get("marks") == {"low7": None, "high7": None, "left6": 0, "right6": 127}, str(cal))
    r.check("proto: no idleLeft / factoryPin, allowAuto false", "idleLeft" not in cal and "factoryPin" not in (mine or {})
            and (mine or {}).get("limits", {}).get("allowAuto") is False)
    owner.send("cal look square")
    r.check("proto: mark in Vierkant -> need-worst-case-look", owner.error_for("lim mark low7") == "need-worst-case-look")
    owner.send("cal look diamond")
    for gone in ("cal color 5", "cal spin on", "auto on"):
        r.check(f"proto: '{gone}' no longer exists -> bad-command", owner.error_for(gone) == "bad-command")
    owner.send("lim mark low7")
    r.check("proto: check with one mark -> need-both-marks", owner.error_for("lim check") == "need-both-marks")
    r.check("proto: save with one mark -> need-both-marks", owner.error_for("lim save") == "need-both-marks")
    owner.send("lim mark high7")
    r.check("proto: save with equal ends -> equal-ends", owner.error_for("lim save") == "equal-ends")
    owner.send("7 30")
    owner.send("lim mark high7")
    r.check("proto: non-owner channel -> calibration-busy", other.error_for("7 50") == "calibration-busy")
    r.check("proto: non-admin cal command -> not-admin", other.error_for("cal stop") == "not-admin")
    r.check("proto: pin reset over WebSocket -> serial-only", owner.error_for("pin reset") == "serial-only")
    r.check("proto: owner moving CH2 -> calibration-busy", owner.error_for("2 40") == "calibration-busy")
    r.check("proto: dbg start while calibrating -> calibration-busy", owner.error_for("dbg start") == "calibration-busy")
    r.check("proto: dbg stop in a calibration -> wrong-phase", owner.error_for("dbg stop") == "wrong-phase")
    check = owner.state_where("lim check", lambda m: (m.get("cal") or {}).get("phase") == "check")
    r.check("proto: check phase entered", check is not None)
    r.check("proto: CH7 pinned during check -> calibration-busy", owner.error_for("7 40") == "calibration-busy")
    r.check("proto: look during check -> wrong-phase", owner.error_for("cal look spin") == "wrong-phase")
    saved = owner.event("lim save", "saved")
    stop = owner.recv_until(lambda m: m.get("type") == "stop")
    r.check("proto: save at the first check step -> saved + calibration-saved",
            saved is not None and (stop or {}).get("reason") == "calibration-saved")
    test_protocol_rules(r, owner, other)
    test_protocol_library(r, owner, other)
    test_protocol_debug(r, owner, other)
    test_protocol_tail(r, owner, other)
    owner.close()
    other.close()


def test_protocol_rules(r: Results, owner: Raw, other: Raw) -> None:
    r.check("proto rule 7: auto/sound refused even when calibrated", other.error_for("1 160") == "auto-blocked")
    other.send("1 96")
    r.check("proto rule 8: manual allowed once calibrated",
            bool(other.recv_until(lambda m: m.get("type") == "state" and m["ch"][0] == 96)))
    other.send("7 100")
    r.check("proto: CH7 clamped to the limit (30..64)",
            bool(other.recv_until(lambda m: m.get("type") == "state" and m["ch"][6] == 64)))
    other.send("6 200")
    r.check("proto: full-width side limit blocks the CH6 sweep (200 -> 127)",
            bool(other.recv_until(lambda m: m.get("type") == "state" and m["ch"][5] == 127)))
    owner.send("cal start")
    owner.recv_until(lambda m: m.get("type") == "state" and m.get("cal"))
    stop = other.event("0", "stop")
    r.check("proto: '0' from anyone ends calibration", (stop or {}).get("reason") == "blackout" and mock_cal() is None, str(stop))
    owner.send("cal start")
    owner.recv_until(lambda m: m.get("type") == "state" and m.get("cal"))
    for _ in range(9):  # 4.5 s of hb only: no idle limit any more
        owner.send("hb")
        time.sleep(0.5)
    r.check("proto: hb-only session survives 4.5 s (no idle limit)", mock_cal() is not None)
    stop = owner.event("cal stop", "stop")
    r.check("proto: cal stop -> calibration-stopped", (stop or {}).get("reason") == "calibration-stopped")
    other.drain()
    owner.send("cal start")
    stop = other.recv_until(lambda m: m.get("type") == "stop", timeout=5)
    r.check("proto: no hb for 3 s ends calibration (calibration-timeout)", (stop or {}).get("reason") == "calibration-timeout",
            str(stop))


def library_then_state(raw: Raw, command: str, predicate: Callable[[dict], bool]) -> tuple[dict | None, dict | None]:
    raw.drain()
    raw.send(command)
    lib = raw.recv_until(lambda m: m.get("type") == "library")
    state = raw.recv_until(lambda m: m.get("type") == "state" and predicate(m))
    return lib, state


def test_protocol_library(r: Results, owner: Raw, other: Raw) -> None:
    """Step 2 library protocol on the mock (CLAUDE.md + firmware presetSaved/presetDeleted)."""
    hook("/__mock/library?reset=0")
    r.check("proto lib: 'lib' answers with the (empty) library",
            other.event("lib", "library") == {"type": "library", "presets": [], "shows": []})
    other.send("1 96")
    other.send("2 102")
    other.send("7 40")  # after a blackout CH7 is 0 in the state; the saved look is clamped, so set it inside the limit
    lib, state = library_then_state(other, "preset save 3 Eerste look", lambda m: m.get("preset") == 3)
    presets = (lib or {}).get("presets", [])
    r.check("proto lib: save broadcasts the library", len(presets) == 1 and presets[0]["slot"] == 3
            and presets[0]["name"] == "Eerste look" and presets[0]["ch"][:2] == [96, 102], str(lib))
    r.check("proto lib: save marks the saved slot as loaded (state.preset = 3)", state is not None)
    bad = (("preset save 33 X", "bad-slot"), ("preset save 3", "bad-name"), ('preset save 3 a"b', "bad-name"),
           ("preset save 3 " + "x" * 33, "bad-name"), ("preset load 4", "empty-slot"), ("preset load x", "bad-command"),
           ("show save 1 4:10 X", "bad-steps"), ("show save 1 3:0 X", "bad-steps"), ("show save 1 3:6001 X", "bad-steps"),
           ("show save 17 3:10 X", "bad-slot"), ("show save 1 " + ",".join(["3:10"] * 25) + " X", "bad-steps"),
           ("show play 2", "empty-slot"), ("preset delete 9", "empty-slot"))
    for command, code in bad:
        r.check(f"proto lib: '{command[:40]}' -> {code}", other.error_for(command) == code)
    other.send("2 7")
    other.send("preset save 5 Tweede")
    lib, _ = library_then_state(other, "show save 1 3:10,5:10 Lus", lambda m: True)
    r.check("proto lib: show saved", [s["steps"] for s in (lib or {}).get("shows", [])] == [[[3, 10], [5, 10]]], str(lib))
    r.check("proto lib: preset used by a show -> preset-in-use", other.error_for("preset delete 3") == "preset-in-use")
    state = other.state_where("show play 1", lambda m: m.get("play") is not None)
    play = (state or {}).get("play") or {}
    r.check("proto lib: play state {show, step (0-based), steps, holdLeftMs}", play.get("show") == 1 and play.get("step") == 0
            and play.get("steps") == 2 and 0 < play.get("holdLeftMs", 0) <= 1000 and (state or {}).get("preset") == 3, str(state))
    stepped = other.recv_until(lambda m: m.get("type") == "state" and (m.get("play") or {}).get("step") == 1, timeout=2)
    r.check("proto lib: playback steps on its own (step 1 loads preset 5)", stepped is not None and stepped["preset"] == 5
            and stepped["ch"][1] == 7, str(stepped))
    owner.send("cal start")
    r.check("proto lib: library edits during a session -> calibration-busy", other.error_for("preset save 6 X") == "calibration-busy")
    owner.send("cal stop")
    other.send("1 96")
    other.send("2 102")
    other.state_where("show play 1", lambda m: m.get("play") is not None)
    _, state = library_then_state(other, "preset save 6 Los", lambda m: True)
    r.check("proto lib: saving a preset the show doesn't use keeps it playing and marking its step",
            state is not None and state.get("play") is not None and state.get("preset") in (3, 5), str(state))
    _, state = library_then_state(other, "preset save 3 Eerste look", lambda m: m.get("play") is None)
    r.check("proto lib: re-saving a preset the playing show uses stops the show, look kept",
            state is not None and state["ch"][0] == 96 and mock_lib()["play"] is None, str(state))
    r.check("proto lib: ... and marks the saved slot when the look matches", state is not None and state.get("preset") == 3, str(state))
    state = other.state_where("preset load 6", lambda m: m.get("preset") == 6)
    r.check("proto lib: preset load sets state.preset", state is not None)
    _, state = library_then_state(other, "preset delete 6", lambda m: m.get("preset") is None)
    r.check("proto lib: deleting the loaded preset clears state.preset", state is not None)
    other.state_where("show play 1", lambda m: m.get("play") is not None)
    state = other.state_where("show stop", lambda m: m.get("play") is None)
    r.check("proto lib: show stop keeps the current look", state is not None and state["ch"][0] == 96, str(state))
    other.state_where("show play 1", lambda m: m.get("play") is not None)
    _, state = library_then_state(other, "show save 1 5:10,3:10 Lus", lambda m: m.get("play") is None)
    r.check("proto lib: re-saving the playing show stops it", state is not None)
    other.state_where("show play 1", lambda m: m.get("play") is not None)
    state = other.state_where("4 10", lambda m: m.get("play") is None)
    r.check("proto lib: a manual channel takes over (play and preset cleared)", state is not None and state.get("preset") is None)
    other.state_where("show play 1", lambda m: m.get("play") is not None)
    hook("/__mock/stop-button?held=1")
    r.check("proto lib: IO0 stops playback", mock_lib()["play"] is None)
    r.check("proto lib: load while IO0 held -> stop-held", other.error_for("preset load 3") == "stop-held")
    hook("/__mock/stop-button?held=0")
    other.state_where("show play 1", lambda m: m.get("play") is not None)
    _, state = library_then_state(other, "show delete 1", lambda m: m.get("play") is None)
    r.check("proto lib: deleting the playing show stops it", state is not None)
    other.drain()


def test_protocol_debug(r: Results, owner: Raw, other: Raw) -> None:
    state = owner.state_where("dbg start", lambda m: (m.get("cal") or {}).get("phase") == "debug")
    cal = (state or {}).get("cal") or {}
    r.check("proto debug: phase debug, raw all 0", cal.get("raw") == [0] * 10 and cal.get("owner") is True, str(cal))
    theirs = other.recv_until(lambda m: m.get("type") == "state" and (m.get("cal") or {}).get("phase") == "debug")
    r.check("proto debug: other client sees owner false", theirs is not None and theirs["cal"]["owner"] is False)
    state = owner.state_where("7 200", lambda m: ((m.get("cal") or {}).get("raw") or [0] * 10)[6] == 200)
    r.check("proto debug: owner's '7 200' is raw (no clamp)", state is not None and hook("/__mock/state")["frame"][6] == 200)
    owner.state_where("1 200", lambda m: ((m.get("cal") or {}).get("raw") or [0] * 10)[0] == 200)
    r.check("proto debug: CH1 200 allowed inside debug (rule 9)", hook("/__mock/state")["frame"][0] == 200)
    r.check("proto debug: non-owner channel -> calibration-busy", other.error_for("7 50") == "calibration-busy")
    r.check("proto debug: non-admin dbg start -> not-admin", other.error_for("dbg start") == "not-admin")
    r.check("proto debug: cal start while debugging -> calibration-busy", owner.error_for("cal start") == "calibration-busy")
    r.check("proto debug: lim mark -> wrong-phase", owner.error_for("lim mark low7") == "wrong-phase")
    r.check("proto debug: cal look -> wrong-phase", owner.error_for("cal look spin") == "wrong-phase")
    stop = owner.event("dbg stop", "stop")
    frame = hook("/__mock/state")["frame"]
    r.check("proto debug: dbg stop -> debug-stopped, laser off, CH7 back in the limit",
            (stop or {}).get("reason") == "debug-stopped" and frame[0] == 0 and 30 <= frame[6] <= 64, f"{stop} {frame}")
    owner.state_where("dbg start", lambda m: (m.get("cal") or {}).get("phase") == "debug")
    stop = other.event("off", "stop")
    r.check("proto debug: 'off' from anyone ends it", (stop or {}).get("reason") == "off" and mock_cal() is None, str(stop))
    owner.state_where("dbg start", lambda m: (m.get("cal") or {}).get("phase") == "debug")
    stop = owner.event("cal stop", "stop")
    r.check("proto debug: cal stop ends debug too (debug-stopped)", (stop or {}).get("reason") == "debug-stopped", str(stop))
    other.drain()
    owner.send("dbg start")
    stop = other.recv_until(lambda m: m.get("type") == "stop", timeout=5)
    r.check("proto debug: no hb for 3 s ends it (calibration-timeout)", (stop or {}).get("reason") == "calibration-timeout",
            str(stop))


def test_protocol_tail(r: Results, owner: Raw, other: Raw) -> None:
    other.drain()
    owner.send("addr 7")
    stop = other.recv_until(lambda m: m.get("type") == "stop")
    r.check("proto: address change -> stop address-changed", (stop or {}).get("reason") == "address-changed")
    r.check("proto rule 8: laser can't switch on uncalibrated", other.error_for("1 96") == "not-calibrated")
    r.check("proto rule 8: a preset that switches the laser on is refused uncalibrated",
            other.error_for("preset load 3") == "not-calibrated")
    hook("/__mock/library?reset=0")
    r.check("proto: unknown command -> bad-command", other.error_for("bogus") == "bad-command")
    wrong = other.event("auth 000000", "auth")
    r.check("proto: wrong PIN -> auth ok:false with attemptsLeft 4", wrong is not None and not wrong["ok"]
            and wrong["attemptsLeft"] == 4, str(wrong))
    owner.event(f"auth {NEW_PIN}", "auth")  # a right PIN resets the counter


def test_sources(r: Results) -> None:
    for path, label, codes in ((ADMIN_HTML, "admin", ERROR_CODES), (INDEX_HTML, "index", ERROR_CODES + LIBRARY_ERROR_CODES)):
        src = path.read_text(encoding="utf-8")
        for table, keys in (("ERROR_TEXT", codes), ("STOP_TEXT", STOP_REASONS)):
            block = re.search(table + r" = \{(.*?)\n\};", src, re.S)
            body = block.group(1) if block else ""
            missing = [k for k in keys if not re.search(r"(^|\s)'?" + re.escape(k) + r"'?\s*:", body)]
            r.check(f"{label}: every {table} entry translated", not missing, ", ".join(missing))
        stale = [word for word in OBSOLETE_IN_PAGES if word in src]
        r.check(f"{label}: nothing left of the removed features", not stale, ", ".join(stale))


# --- main -------------------------------------------------------------------------------------
def run_browser_tests(r: Results, log: MockLog) -> list[str]:
    with sync_playwright() as p:
        browser = p.chromium.launch(channel="msedge", headless=True)
        ui = Ui(browser, r, log)
        a, a_id = ui.open("/admin", "admin")
        test_login(ui, a, a_id)
        test_heartbeat(ui, a_id)
        i, i_id = test_index_uncalibrated(ui)
        test_lockout(ui, a_id)
        since_start = test_cal_start(ui, a, a_id, i)
        test_look_and_marks(ui, a, a_id)
        test_move_and_mark(ui, a, a_id)
        test_horizontal(ui, a, a_id)
        test_save_without_check(ui, a, a_id, since_start)
        test_index_mapped(ui, i, i_id)
        test_optional_check(ui, a, a_id)
        test_long_idle(ui, a, a_id)
        test_off_from_index(ui, a, i, i_id)
        test_builder(ui, i, i_id)
        test_save_sheet(ui, i, i_id)
        test_tiles(ui, i, i_id)
        test_shows(ui, i, i_id)
        test_delete(ui, i, i_id)
        test_tooltips_and_guide(ui, i)
        test_off_and_library_reset(ui, i, i_id)
        i_id = test_index_reconnect(ui, i, i_id)
        test_dead_link(ui, i)  # the page reconnects with a new id; i_id is not used after this
        test_index_portrait(ui)
        index_tab(i, "presets")
        test_debug_session(ui, a, a_id, i)
        a_id = test_debug_exits(ui, a, a_id, i)
        test_settings(ui, a)
        test_stop_button(ui, a, i)
        test_pin_change(ui, a, a_id)
        a_id = test_hidden_ends_calibration(ui, a, a_id, i)
        test_address(ui, a, a_id, i)
        test_logout(ui, a, a_id)
        test_portrait(ui)
        test_back_link(ui)
        browser.close()
        return ui.js_errors


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")  # check names contain non-cp1252 text
    results = Results()
    log = MockLog(LOG_PATH)
    proc = start_mock()
    try:
        js_errors = run_browser_tests(results, log)
        test_protocol(results)
        test_sources(results)
        results.check("no JavaScript errors on any page", not js_errors, " | ".join(js_errors))
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
    failed = results.failures
    print(f"\n{len(results.items) - len(failed)}/{len(results.items)} checks passed. Screenshots: {OUT_DIR}")
    if failed:
        print("FAILED:\n  " + "\n  ".join(failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
