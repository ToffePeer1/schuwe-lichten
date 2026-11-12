# DMX Laser Controller

ESP32 pocket controller: phone → WiFi → ESP32 → DMX → RGB pattern laser.

## One-time setup

1. Install the **CP210x driver** (Silicon Labs "CP210x Universal Windows Driver") so the board shows up as a COM port (here: COM5).
2. In this folder, run `uv sync`. This installs PlatformIO into `.venv`.
3. If `include/wifi_secrets.h` is missing (fresh clone), copy `include/wifi_secrets.example.h` to it and set a WiFi password (8+ characters)
   and a factory admin PIN (6-8 digits). The example values deliberately fail the build.

## Flashing

**This board does not switch into upload mode on its own. You have to hold the `IO0` button**
(the small one next to USB that looks like it says "100"; leave `EN` alone).

1. Close any open serial monitor, because it blocks the port.
2. Start the upload:

   ```bash
   uv run pio run -t upload
   ```

3. When the terminal shows `Connecting........`, **press and hold IO0**.
   Release it when you see `Writing at 0x...`.
4. Wait for `Hard resetting via RTS pin...` and `[SUCCESS]`. The board restarts by itself.

If it fails with `Wrong boot mode detected (0x13)`, you pressed IO0 too late. Run the command again and hold IO0 a bit earlier.
If it says it can't find or open the port, add `--upload-port COM5` (check Device Manager → Ports for the number).

### From VSCode instead

PlatformIO's buttons are in the blue status bar at the bottom:
**✓** builds, **→** uploads, and the **plug** icon opens the serial monitor.
The same IO0 trick applies while it says `Connecting...`.

Your limits, PIN and DMX address are stored separately from the program, so they survive a normal flash.

## Using it

**Phone:** join the ESP32's WiFi (the name is `AP_SSID` in `include/config.h`, the password is in `include/wifi_secrets.h`)
and open **<http://192.168.4.1>**. Turn the phone sideways: the page is made for landscape.
The laser keeps doing what it does when you lock the phone, switch apps or leave the WiFi. That includes a running show.
Stop it with the big red **UIT** button on the page, or with the **IO0 button** on the ESP32 (instant blackout, works without a phone).
Nothing switches it back on by itself. The laser's own power switch is the real emergency stop.

The page has four tabs:

- **Presets:** tap a tile to show that look. **＋ Nieuw** opens a quick builder (shape, colour, movement, size, height, lines/dots).
  Each choice is shown live on the laser. **＋ Bewaar als preset** saves whatever the laser shows right now.
- **Shows:** a list of presets, each with a hold time, that loops. The ESP32 plays it on its own, also with no phone connected.
  Any manual change, loading a preset, UIT or IO0 stops it.
- **Lichttafel:** a fader per DMX channel for precise control. Tap a fader's name for an explanation.
- **Uitleg:** what every control does.

**Beheer** (bottom right) opens `/admin`. Everyone on the WiFi can use presets and shows; only calibration and settings need the PIN.

### First time at a venue: set the height limit (`/admin`)

The laser can't be switched on until a height limit has been calibrated and saved.

1. Open **<http://192.168.4.1/admin>** and log in with the PIN (`ADMIN_PIN` in `include/wifi_secrets.h`).
2. Tap **Kalibratie starten**. The laser draws the big square as a **Ruit** (diamond), because rotated patterns reach lower than a straight square.
3. Move it with the − / + buttons until its bottom corner just touches the lowest safe height, then tap **Dit is de laagste stand**.
   Move it up and tap **Dit is de hoogste stand**. The left/right limit is on at full width; narrow it only if you need to.
4. Tap **Opslaan**. Optionally tap **Controleren** first: the laser then shows the widest patterns at the low limit, one after the other.

The limit is saved on the ESP32 and used from then on, also after a restart.
The laser's built-in auto/sound modes are disabled, because they ignore the height limit.
The DMX address (default 1) is also set in `/admin` and must match the address on the laser.

The **Debug** tab in `/admin` lets you set all 10 channels directly, **without any limits**, to find out what the laser really does.
Aim the laser at a wall. The session ends when you leave or lock the page, press OFF, or press IO0.

**Serial monitor:** run `uv run pio device monitor` (Ctrl+C to quit). USB counts as admin (no PIN needed):

| Command | Effect |
| --- | --- |
| `7 40` | set channel 7 to 40 (any channel 1-10, value 0-255) |
| `0` | every channel to 0 |
| `off` | laser off (CH1 = 0) |
| `?` | show values, limits, calibration state, DMX frame count, unused task stack and free memory |
| `cal start` ... `lim save` | the same calibration commands the admin page uses |
| `dbg start` / `dbg stop` | debug session: raw channels, no limits |
| `pin reset` | forgot the PIN? restores the factory PIN |
| `preset save 3 Blauwe tunnel` | save the current look as preset 3 (slots 1-32) |
| `preset load 3` / `preset delete 3` | load or delete a preset |
| `show save 1 3:50,4:20 Opwarmen` | show 1: preset 3 for 5.0 s, then preset 4 for 2.0 s, looping (times in tenths of a second) |
| `show play 1` / `show stop` / `show delete 1` | play, stop (the look stays) or delete a show |
| `lib` | list the presets and shows |

## Tests

The safety logic has unit tests that run on the PC (no ESP32 needed):

```bash
bash tools/run_host_tests.sh
```

The web pages are tested in a headless browser against a mock of the firmware:

```bash
uv run --no-project --with playwright --with websockets python tools/mock_firmware/test_ui.py
```

## Troubleshooting

- **No COM port:** the CP210x driver is missing, or the USB cable only does charging.
- **`UnicodeEncodeError` from pio in Git Bash:** run `export PYTHONIOENCODING=utf-8` first.
- **Serial shows "IO0 held low since boot":** the stop button is stuck or held by the USB serial lines; it won't work until it has been released once.
- **Laser flickers or ignores DMX:** check that the laser's DMX address matches the one in `/admin` (default 1), and check the XLR cable. A normal 3-pin microphone cable works for short runs.
  The ESP32 sends 3.3 V logic into the 5 V DMX module, which usually works but is worth mentioning if things act up.
