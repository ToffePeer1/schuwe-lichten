# Protocol

The ESP32 runs a WPA2 access point and serves:

| Port | Path | What |
|---|---|---|
| 80 | `/` | the phone page (`src/web/index.html`) |
| 80 | `/admin` | the admin page (`src/web/admin.html`) |
| 80 | `/patterns.json` | pattern names, categories and drawings (`src/web/patterns.json`) |
| 81 | WebSocket | the text protocol below, up to 8 clients |

The USB serial console (115200 baud) speaks the same commands, one per line. Serial counts as physical access,
so it is always admin (no PIN), it is exempt from the heartbeat rule, and only serial can run `pin reset`.
Lines are at most 255 bytes.

## Commands (client → ESP32)

| Command | Who | Effect |
|---|---|---|
| `<ch> <value>` | anyone | Set channel 1-10 to 0-255. CH1 ≥ 128 is refused, CH1 ≥ 64 needs a calibrated limit, CH6/CH7 are clamped. Stops a running show. |
| `0` | anyone | All channels 0. Ends a calibration or debug session. |
| `off` | anyone | CH1 = 0, other channels kept. Ends a calibration or debug session. |
| `?` | anyone | Send me the state (serial: a text status report). |
| `hb` | session owner | Heartbeat, about once a second. 3 s without one ends the session. |
| `auth <pin>` / `logout` | anyone | Log this connection in or out as admin. |
| `pin <old> <new>` | admin | Change the PIN (6-8 digits). |
| `pin reset` | serial | Restore the factory PIN from `include/wifi_secrets.h`. |
| `addr <1-503>` | admin | DMX start address. Blacks out and makes the limit uncalibrated. |
| `cal start` / `cal stop` | admin | Start or abandon calibration. |
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
  "ch": [96, 102, 0, 0, 0, 0, 52, 40, 224, 128],
  "limits": { "ch7": [30, 100], "ch6": { "on": true, "range": [0, 127] }, "calibrated": true, "allowAuto": false },
  "addr": 1,
  "stopHeld": false,
  "admin": false,
  "cal": null,
  "preset": 3,
  "play": { "show": 1, "step": 0, "steps": 2, "holdLeftMs": 4200 },
  "settingsReset": false,
  "libraryReset": false
}
```

- `ch` is the requested look. The firmware still clamps every frame, so what goes out on the wire can differ (see `?` on serial).
- `limits.ch7` is `[low, high]`, where low is the value that puts the beam **lowest**. It may be larger than high on an inverted mount.
- `preset` is the last loaded preset, or `null` after a manual change. `play` is `null` unless a show runs. `play.step` is 0-based.
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
| `{"type":"saved"}` | Settings were saved (limits, address or PIN). |

Error codes: `bad-command`, `not-admin`, `serial-only`, `calibration-busy`, `not-owner`, `stop-held`, `auto-blocked`, `not-calibrated`,
`wrong-phase`, `need-worst-case-look`, `need-both-marks`, `need-horizontal-marks`, `equal-ends`, `bad-pin`, `locked`, `not-calibrating`,
`save-failed`, `bad-slot`, `empty-slot`, `bad-name`, `bad-steps`, `preset-in-use`.

Stop reasons: `button` (IO0), `off`, `blackout` (`0`), `calibration-stopped`, `calibration-timeout` (no heartbeat), `calibration-owner-left`,
`address-changed`, `calibration-saved`, `debug-stopped`.

The pages translate codes and reasons to Dutch. `tools/mock_firmware/mock_firmware.py` implements this protocol in Python for the UI tests.
