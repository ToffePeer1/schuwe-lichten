# Safety design

> **This is a hobby project, not a certified safety system.** A 500 mW laser can blind people instantly,
> also through a reflection. The height limit, the stop button and everything below are conveniences that
> make mistakes less likely. They don't make it safe to point a laser at an audience.
> Follow your local laser regulations, keep the beam above head height, and keep the laser's own power switch within reach.

The firmware, not the phone, enforces every rule here. The page only mirrors them so it can explain itself.
All of the logic lives in `lib/laser_core` as pure functions and is covered by host unit tests (`test/test_controller` and friends).
"Rule N" in code comments refers to the numbered list below.

The rules are about the **laser**, wherever it sits in the patch. Other fixtures on the same DMX line
(see [PROTOCOL.md](PROTOCOL.md#fixtures-and-the-patch)) get no limits of their own: the moving head's built-in lasers are much weaker,
and its random-walk and sound modes are allowed (user decision, to be revisited after testing). The stops still reach every fixture.

## Rules

1. **Boot is dark.** At power-up CH1 (mode) is 0 and every other channel is 0.
   A preset or show never resumes by itself after a reboot.
2. **Height limit in firmware.** CH7 (vertical position) is clamped between the calibrated low and high values
   on every command **and again on every outgoing DMX frame**, also inside presets and shows.
   CH7 is always capped at 127, so the laser's own vertical movement range (128-255) can never be sent.
   An optional side limit does the same for CH6. While it is on, CH6 128-255 (the laser's automatic sweep) is blocked too.
   It is on by default, at full width.
   The limits can only be changed inside a PIN-protected calibration (see below).
3. **Phones leaving doesn't stop anything.** Locking the phone, switching apps or leaving the WiFi keeps the current look
   or show running, so the ESP32 can play a show on its own. The stops are:
   - `off` (the UIT button) and `0`, accepted from **every** client and from serial, in every mode, always.
     Without a fixture prefix they reach every fixture.
   - the IO0 button on the ESP32, which zeroes every fixture
   - the end of a calibration or debug session
4. **DMX is sent continuously**, 40 frames per second, whether anything changed or not.
5. **The laser is only switched on by an explicit user action.** CH1 is raised above 63 only by a command from a person.
   A preset load, a show start, a builder choice and the Lichttafel switch count as such. Nothing raises it on a timer, after a reconnect or on boot.
6. **IO0 is a convenience stop, not an emergency stop.** The laser's own power switch is the real kill switch.
7. **Auto and sound modes are permanently disabled.** The laser's built-in auto/sound modes (CH1 128-255) were found to ignore
   the height limit completely. They are rejected as commands and sent as CH1 = 0 on every frame.
   Only the admin Debug session (rule 9) can send them, for exploration.
8. **No light before calibration.** The laser can't be switched on (CH1 ≥ 64) until a height limit has been calibrated and saved,
   except inside calibration itself. Moving the laser to another slot or address, removing it from the patch,
   or loading corrupt settings makes it uncalibrated again.
9. **Debug session.** A PIN-protected admin session that sends all 10 channels raw (0-255, **no limits**), to find out what the laser really does.
   One owner at a time. Everything starts at 0, and every page shows a red banner while it runs.
   It ends with a blackout on `off`/`0` from anyone, IO0, `dbg stop`, or when the owner leaves, hides the page or stops sending heartbeats for 3 s.

## Calibration

The limit depends on the venue, so it is set on site in `/admin` (PIN):

- The firmware pins the look. It uses pattern #1 (a square) at maximum size, shown as a **Ruit** (diamond: the square rotated by CH3 = 64)
  or spinning, because rotation makes a pattern reach lower than when it is straight.
  Marks are only accepted in those worst-case looks, not in the flat square.
- The owner moves CH6/CH7 only, marks the lowest and highest safe positions, and saves.
  An optional check round first steps through the widest patterns, the zoom circles growing, a vertical flip and a pulse,
  all at the low limit, so you can see that nothing dips below it.
- Saving writes the limits to flash, reads them back to verify, and ends with a blackout.
  Stopping without saving keeps the old limits.

## Stop button (IO0)

- The board's BOOT button doubles as a stop button. A press blacks out at once and ends any session or show.
- While it is held, every frame is forced to 0 and commands are refused. Releasing it doesn't restore anything.
- IO0 is also a boot strapping pin, so the button only arms after it has been seen released once after boot.
  The serial console warns if it stays low.

## Robustness

- Only the DMX task is on the task watchdog (5 s, then a reboot, which is dark per rule 1).
  The web/command loop deliberately isn't: a socket write to a phone that vanished can block it for seconds,
  and rebooting then would switch the laser off for everyone.
- Settings (limits, patch, PIN) are one CRC-checked blob in NVS, verified after every write.
  A corrupt blob loads as "uncalibrated", which keeps the laser off (rule 8). Blobs from before the patch existed still load.
- The patch is checked on load and on every change (no overlap, nothing past slot 512, one laser).
  Every frame checks it again, and an invalid patch sends nothing, so another fixture can never overwrite the laser's clamped channels.
- Changing the patch, and starting a calibration or debug session, black out every fixture.
- Presets and shows live on LittleFS, written to a temp file, read back and renamed.
  They survive firmware updates (never use PlatformIO's "upload filesystem", which wipes them).
- The WiFi uses WPA2, and `/admin` needs a 6-8 digit PIN with a doubling lockout after 5 wrong tries.
  The PIN travels over the WiFi as plain text. It keeps guests out, not a determined attacker on the same network.
