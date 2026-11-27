# Bike horn: ESP32-CAM + Bluetooth speaker

Press a button on the handlebar and your small Bluetooth speaker plays a loud
two-tone **horn** (for as long as you hold it) or a **"ring-ring" bell**.
The ESP32 connects to the speaker like a phone does (Bluetooth A2DP) and
generates the sounds itself. No SD card or audio files are needed.

`demo.wav` shows what it sounds like: car horn → bell → horn and bell together.
`demo_city_bus.wav` plays the deeper city-bus style horn (tap, tap, long press).

## Parts
- ESP32-CAM + its USB programmer board (ESP32-CAM-MB). The camera is not used and can be unplugged.
- A Bluetooth speaker
- 2 push buttons (waterproof handlebar buttons are ideal), some wire
- A USB power bank to power the ESP32 (5 V)

## Wiring
Buttons connect between a GPIO and GND (internal pull-ups are used, no resistors):

| Function    | ESP32-CAM pin | Other side |
|-------------|---------------|------------|
| Horn button | **IO13**      | GND        |
| Bell button | **IO14**      | GND        |
| Power       | 5V + GND      | power bank |

LEDs: the small red LED on the back blinks while it searches for the speaker
and stays on once connected. The big white flash LED strobes while the horn sounds.
To turn the strobe off, set `USE_FLASH_LED = false`.

## Flash it
1. Install [VS Code + PlatformIO](https://platformio.org/install/ide?install=vscode).
2. Open this `bike-horn` folder.
3. In `src/main.cpp`, set `SPEAKER_NAME` to your speaker's exact Bluetooth name (as your phone shows it).
4. Plug the ESP32-CAM into the MB board, connect USB, click **Upload** (→).
   If upload hangs on "Connecting...", hold **IO0**, tap **RST**, then release IO0.
5. Disconnect the speaker from your phone (a speaker only accepts one source), turn it on,
   and press RST on the ESP32. The red LED stops blinking when it is connected.

## Choose the horn sound
In `src/main.cpp`, set `HORN_STYLE`:

- `HORN_CAR`: normal two-tone car horn
- `HORN_CITY_BUS` (default): deeper, buzzier horn with an air-horn "scoop" at the start.
  This is a synthesized approximation of a city bus, not the real thing.
- `HORN_RECORDING`: your own recording, e.g. a real Paris bus.

### Use a real recording (e.g. a Paris RATP bus)
1. Record the horn yourself with your phone, or download a clip you're allowed to use.
2. Cut it to the horn only (max 4 s) and export it as **WAV 16-bit**. Audacity works for this.
3. Run `python3 tools/wav2header.py bus_horn.wav`. This creates `src/custom_horn.h`.
4. Set `HORN_STYLE = HORN_RECORDING` and upload.

A short press plays the clip once. If you hold the button longer than the clip,
playback loops from 30% of the way in (change this with `--loop 0.5`).

## Tips
- Turn the speaker's volume all the way up. The ESP32 already sends at maximum.
- Bluetooth adds about 0.1–0.2 s of delay. The connection stays open (playing silence)
  so there is no extra wait when you press.
- Tune the sound in `main.cpp`: the frequencies in `hornSample()`, `BELL_BASE`, `MASTER_GAIN`.
- Check your local rules. Some countries (e.g. Germany) only allow a bell on bicycles.
  Never imitate an emergency siren.
