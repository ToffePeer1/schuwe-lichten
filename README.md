# DMX Laser Controller

ESP32 pocket controller: phone → WiFi → ESP32 → DMX → RGB pattern laser.

## One-time setup

1. Install the **CP210x driver** (Silicon Labs "CP210x Universal Windows Driver") so the board shows up as a COM port (here: COM5).
2. In this folder, run `uv sync`. This installs PlatformIO into `.venv`.
3. If `include/wifi_secrets.h` is missing (fresh clone), copy `include/wifi_secrets.example.h` to it and set a WiFi password (8+ characters).

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
To choose a build, click the `env:...` item in the status bar.

### Which build?

| Build | Command | Use it for |
| --- | --- | --- |
| `esp32dev` (default) | `uv run pio run -t upload` | Normal use. CH7 (vertical position) is clamped in firmware. |
| `findlimit` | `uv run pio run -e findlimit -t upload` | **Stage 1 test only**: the CH7 clamp is off, so you can find the safe vertical limit. Aim the laser at a wall, away from people. Flash `esp32dev` again afterwards. |

## Using it

**Phone:** join WiFi **`Laser-Controller`** (password is in `include/wifi_secrets.h`) and open **<http://192.168.4.1>**.
The big red OFF button turns the laser off. Locking the phone or switching apps also turns it off.

**Serial monitor:** run `uv run pio device monitor` (Ctrl+C to quit). You can type commands:

| Command | Effect |
| --- | --- |
| `7 40` | set channel 7 to 40 (any channel 1-10, value 0-255) |
| `0` | every channel to 0 |
| `off` | laser off (CH1 = 0) |
| `?` | show current values and DMX frame count |

Safety: if no command arrives for 1 second, the firmware sets every channel to 0.
So over serial the laser goes off again a second after each command. Use the phone page for testing.

## Tests

The safety logic has unit tests that run on the PC (no ESP32 needed):

```bash
bash tools/run_host_tests.sh
```

## Troubleshooting

- **No COM port:** the CP210x driver is missing, or the USB cable only does charging.
- **`UnicodeEncodeError` from pio in Git Bash:** run `export PYTHONIOENCODING=utf-8` first.
- **Laser flickers or ignores DMX:** check that the laser is set to DMX address 1, and check the XLR cable. A normal 3-pin microphone cable works for short runs.
  The ESP32 sends 3.3 V logic into the 5 V DMX module, which usually works but is worth mentioning if things act up.
