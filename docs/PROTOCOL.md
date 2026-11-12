# Protocol

The ESP32 runs a WPA2 access point and serves:

| Port | Path | What |
|---|---|---|
| 80 | `/` | the phone page (`src/web/index.html`) |
| 80 | `/admin` | the admin page (`src/web/admin.html`) |
| 80 | `/patterns.json` | pattern names, categories and drawings (`src/web/patterns.json`) |
| 81 | WebSocket | the text protocol below, up to 8 clients |

## Fixtures and the patch

One DMX line can carry several fixtures. The **patch** says which type sits in which of the 4 slots, at which DMX address.
The order on the cable doesn't matter, only the addresses. Footprints must fit 1-512 and must not overlap,
and there is at most one laser (its height limit is a single setting). The default patch is one laser at address 1.

| Type | Channels | Rules |
|---|---|---|
| `laser` | 10 | Everything in [SAFETY.md](SAFETY.md): height and side limit, no auto/sound, no light before calibration. |
| `head` | 19 | None (user decision). `off` darkens CH7-19 (dimmer, strobe, colours, lasers, macro) and keeps pan and tilt. |

Moving head channels, from its manual (not yet checked on the real fixture): 1 pan (0-540°), 2 pan speed (fast to slow),
3 bee-eye motor (0-127 angle, 128-191 turns back, 192-255 forward), 4-6 motors 1-3 (0-180°), 7 master dimmer,
8 strobe (0-9 none, 10-255 = 1-25 Hz), 9-12 bee-eye red/green/blue/white, 13-16 wash + beam red/green/blue/white,
17 red laser, 18 green laser, 19 macro (0-50 DMX, 51-150 random walk, 151-250 sound, 251-255 reset).

The USB serial console (115200 baud) speaks the same commands, one per line. Serial counts as physical access,
so it is always admin (no PIN), it is exempt from the heartbeat rule, and only serial can run `pin reset`.
Lines are at most 255 bytes.

## Commands (client → ESP32)

| Command | Who | Effect |
|---|---|---|
| `<ch> <value>` | anyone | Set a channel of fixture 1 (during a calibration or debug session: of the laser) to 0-255. On the laser: CH1 ≥ 128 is refused, CH1 ≥ 64 needs a calibrated limit, CH6/CH7 are clamped, and a running show stops. |
| `f<1-4> <ch> <value>` | anyone | The same for the fixture in that slot. |
| `0` | anyone | Every channel of every fixture to 0. Ends a calibration or debug session. `f<n> 0`: only that fixture. |
| `off` | anyone | Every fixture off (laser CH1 = 0, head CH7-19 = 0), positions kept. Ends a session. `f<n> off`: only that fixture. |
| `?` | anyone | Send me the state (serial: a text status report). |
| `hb` | session owner | Heartbeat, about once a second. 3 s without one ends the session. |
| `auth <pin>` / `logout` | anyone | Log this connection in or out as admin. |
| `pin <old> <new>` | admin | Change the PIN (6-8 digits). |
| `pin reset` | serial | Restore the factory PIN from `include/wifi_secrets.h`. |
| `patch <1-4> laser\|head <1-512>` | admin | Put a fixture in a slot. Blacks out everything. Moving or removing the laser makes its limit uncalibrated. |
| `patch <1-4> none` | admin | Empty the slot. |
| `cal start` / `cal stop` | admin | Start or abandon calibration of the laser (blacks out every fixture). |
| `cal look square\|diamond\|spin` | owner | The pinned look during calibration. |
| `lim mark low7\|high7\|left6\|right6` | owner | Mark the current CH7/CH6 position (only in `diamond` or `spin`). |
| `lim h on\|off` | owner | Use the side limit (CH6). |
| `lim check` / `lim redo` | owner | Start the optional check round / go back to adjusting. |
| `lim save` | owner | Save the marked limits, then black out. |
| `dbg start` / `dbg stop` | admin | Raw debug session: the owner's `<ch> <value>` is sent without any limits. |
| `preset save <1-32> <name>` | anyone | Save the current look into the slot (overwrites). |
| `preset load <slot>` / `preset delete <slot>` | anyone | Load (stops a show) or delete a preset. A preset used by a show can't be deleted. |
| `show save <1-16> <p:t,...> <name>` | anyone | A looping show: preset `p` for `t` tenths of a second (1-6000), 1-24 steps. |
| `show play <slot>` / `show stop` / `show delete <slot>` | anyone | Play, stop (the look stays) or delete a show. |
| `lib` | anyone | Send me the library message. |

Names are 1-32 bytes of UTF-8 without control characters, `"` or `\`. They are the rest of the line after the slot (or the steps).

## Messages (ESP32 → client)

Every message is one JSON object with a `type`.

**`state`**: sent on connect, after every change, and about once a second while a session or show runs. Each client gets its own copy
(`admin` and `cal.owner` are per client).

```json
{
  "type": "state",
  "fixtures": [
    { "slot": 1, "type": "laser", "addr": 1, "ch": [96, 102, 0, 0, 0, 0, 52, 40, 224, 128], "preset": 3 },
    { "slot": 2, "type": "head", "addr": 11, "ch": [128, 0, 0, 90, 90, 90, 255, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0], "preset": null }
  ],
  "limits": { "ch7": [30, 100], "ch6": { "on": true, "range": [0, 127] }, "calibrated": true, "allowAuto": false },
  "stopHeld": false,
  "admin": false,
  "cal": null,
  "play": { "show": 1, "step": 0, "steps": 2, "holdLeftMs": 4200 },
  "settingsReset": false,
  "libraryReset": false
}
```

- `fixtures` lists the patched slots in order. `ch` is what each fixture was asked for. The firmware applies the type's rules on every frame,
  so what goes out on the wire can differ (see `?` on serial).
- `limits` are the laser's. `limits.ch7` is `[low, high]`, where low is the value that puts the beam **lowest**. It may be larger than high on an inverted mount.
- `preset` is the fixture's last loaded preset, or `null` after a manual change. Presets and shows hold laser looks for now.
  `play` is `null` unless a show runs. `play.step` is 0-based.
- `settingsReset` / `libraryReset`: stored settings or presets/shows were corrupt and loaded as empty.
- `cal` is `null`, or a session:

```json
{
  "phase": "adjust", "owner": true, "look": "diamond", "ch7": 64, "ch6": 0,
  "marks": { "low7": 30, "high7": null, "left6": 0, "right6": 127 },
  "hOn": true, "step": 0, "steps": 15, "stepName": "Vierkant", "checkDone": false,
  "raw": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
}
```

`phase` is `adjust`, `check` (the check round, `step` of `steps`) or `debug` (`raw` holds the 10 raw channels).

**`library`**: sent on connect and to everyone after every library change. Only used slots are listed.

```json
{ "type": "library",
  "presets": [ { "slot": 1, "name": "Ster draaiend", "ch": [96, 102, 140, 0, 0, 0, 52, 40, 224, 0] } ],
  "shows": [ { "slot": 1, "name": "Avondshow", "steps": [[1, 50], [2, 30]] } ] }
```

**Events:**

| Message | When |
|---|---|
| `{"type":"error","code":"..."}` | A command was refused (codes below). |
| `{"type":"stop","reason":"..."}` | A blackout happened (reasons below). |
| `{"type":"auth","ok":true,"lockedSeconds":0,"attemptsLeft":5}` | Answer to `auth`. |
| `{"type":"saved"}` | Settings were saved (limits, patch or PIN). |

Error codes: `bad-command`, `not-admin`, `serial-only`, `calibration-busy`, `not-owner`, `stop-held`, `auto-blocked`, `not-calibrated`,
`wrong-phase`, `need-worst-case-look`, `need-both-marks`, `need-horizontal-marks`, `equal-ends`, `bad-pin`, `locked`, `not-calibrating`,
`save-failed`, `bad-slot`, `empty-slot`, `bad-name`, `bad-steps`, `preset-in-use`,
`no-fixture` (nothing patched where the command points, or no laser for a laser-only action), `bad-patch`.

Stop reasons: `button` (IO0), `off`, `blackout` (`0`), `calibration-stopped`, `calibration-timeout` (no heartbeat), `calibration-owner-left`,
`patch-changed`, `calibration-saved`, `debug-stopped`.

The pages translate codes and reasons to Dutch. `tools/mock_firmware/mock_firmware.py` implements this protocol in Python for the UI tests.
