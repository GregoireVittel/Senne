# Bike horn: ESP32-CAM + Bluetooth speaker

Press a button on the handlebar and your Bluetooth speaker plays **your own WAV
files**: a horn while you hold one button, another sound when you press the second.
The ESP32 connects to the speaker like a phone does (Bluetooth A2DP).
The sounds come from a microSD card in the ESP32-CAM's card slot.

## Parts
- ESP32-CAM + its USB programmer board (ESP32-CAM-MB). The camera is not used and can be unplugged.
- A microSD card (≤ 32 GB, formatted **FAT32**)
- A Bluetooth speaker
- 2 push buttons (waterproof handlebar buttons are ideal), some wire
- A USB power bank (5 V)

## Wiring
Buttons connect between a GPIO and GND (internal pull-ups are used, no resistors):

| Function      | ESP32-CAM pin | Other side | Sound file on SD |
|---------------|---------------|------------|------------------|
| Horn button   | **IO12**      | GND        | `horn.wav`       |
| Second button | **IO13**      | GND        | `bell.wav`       |
| Power         | 5V + GND      | power bank |                  |

Don't hold the buttons while the board starts up.

## Your sounds
Copy two files to the root of the SD card:

- `horn.wav`: plays **while the button is held**. It loops if you hold longer than the file, and stops when you let go.
- `bell.wav`: a press plays the **whole file once**. Pressing again restarts it.

Any WAV works: any sample rate, mono or stereo, 8/16/24-bit or 32-bit float.
MP3 doesn't work. Convert it first, e.g. in Audacity: *File → Export → WAV*.
Keep the total length of both files under about 20 seconds. Cut silence at the start
so the sound plays right away. Use loud, normalized files: the bike is noisy.

To change file names, buttons, or behavior, edit the `BUTTONS` table at the top
of `src/main.cpp`. Each entry has the format `{pin, "/file.wav", PLAY_WHILE_HELD or PLAY_ONCE}`.
After you change the files on the card, press **RST**. Sounds are loaded at startup.

## Flash it
1. Install [VS Code + PlatformIO](https://platformio.org/install/ide?install=vscode).
2. Open this `bike-horn` folder.
3. In `src/main.cpp`, set `SPEAKER_NAME` to your speaker's exact Bluetooth name (as your phone shows it).
4. Plug the ESP32-CAM into the MB board, connect USB, click **Upload** (→).
   If upload hangs on "Connecting...", hold **IO0**, tap **RST**, then release IO0.
5. Disconnect the speaker from your phone (it accepts one source only), turn it on,
   and press RST on the ESP32.

The Serial Monitor (115200 baud) lists each loaded file and any problem with it.

## LEDs
- Small red LED on the back: blinks while searching for the speaker and stays on once connected.
  Fast blinking means no SD card was found.
- Big white flash LED: strobes while a sound plays, as an extra warning at night.
  To turn it off, set `USE_FLASH_LED = false`.

If a file is missing or unreadable, its button plays a short beep instead.

## Tips
- Turn the speaker's volume all the way up. The ESP32 already sends at maximum.
- Bluetooth adds about 0.1–0.2 s of delay. The connection stays open (playing silence)
  so there is no extra wait when you press.
- Check your local rules. Some countries (e.g. Germany) only allow a bell on bicycles.
  Never imitate an emergency siren.
