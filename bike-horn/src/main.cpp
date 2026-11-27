// Bike horn for ESP32-CAM + any Bluetooth speaker, playing your own WAV files.
//
// Copy your sounds onto a microSD card (FAT32) and put it in the ESP32-CAM:
//   /horn.wav  -> HORN button (GPIO 12 -> GND)
//   /bell.wav  -> BELL button (GPIO 13 -> GND)
//
// At startup the files are loaded from the card into the board's 4 MB PSRAM,
// so playback starts instantly. The ESP32 then acts as a Bluetooth A2DP
// source (like a phone) and streams them to the speaker.
//
// WAV files can be any sample rate, mono or stereo, 8/16/24-bit or 32-bit float.
// They are converted to mono 44.1 kHz when loaded. Total length of all
// sounds: up to about 20 seconds.

#include <Arduino.h>
#include "FS.h"
#include "SD_MMC.h"
#include "BluetoothA2DPSource.h"

// ---------------------------------------------------------------- settings --
// Exact Bluetooth name of your speaker, as shown on your phone.
static const char *SPEAKER_NAME = "JBL GO 3";

enum PlayMode {
  PLAY_WHILE_HELD,  // plays while the button is held, loops, stops on release
  PLAY_ONCE         // a press plays the whole file; pressing again restarts it
};

struct SoundButton {
  int pin;           // button between this pin and GND
  const char *file;  // path on the SD card
  PlayMode mode;
};

// GPIO 12 and 13 are the only free pins next to the SD card (1-bit mode).
static SoundButton BUTTONS[] = {
  {12, "/horn.wav", PLAY_WHILE_HELD},
  {13, "/bell.wav", PLAY_ONCE},
};

static const int   PIN_STATUS    = 33;    // small red LED on the back (active LOW)
static const int   PIN_FLASH     = 4;     // big white flash LED
static const bool  USE_FLASH_LED = true;  // strobe the flash LED while a sound plays
static const float VOLUME        = 1.0f;  // 0..1, applied to all sounds
// ---------------------------------------------------------------------------

static const uint32_t SR = 44100;
static const int NUM_BUTTONS = sizeof(BUTTONS) / sizeof(BUTTONS[0]);
static const int FADE_SAMPLES = SR / 50;  // 20 ms fade on release, avoids clicks

BluetoothA2DPSource a2dp;

struct Voice {
  int16_t *data = nullptr;  // mono, 44.1 kHz
  uint32_t len = 0;
  volatile bool held = false;
  volatile bool trigger = false;
  volatile bool playing = false;
  uint32_t pos = 0;
  int fade = 0;             // >0 while fading out
};
static Voice voices[NUM_BUTTONS];

// ------------------------------------------------------------- WAV loading --
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// Reads one sample (any supported format) as float -1..1.
static float sampleAt(const uint8_t *p, int fmt, int bits) {
  if (fmt == 3) { float f; memcpy(&f, p, 4); return f; }
  switch (bits) {
    case 8:  return (p[0] - 128) / 128.0f;
    case 16: return (int16_t)rd16(p) / 32768.0f;
    case 24: return ((int32_t)((p[0] << 8) | (p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8) / 8388608.0f;
    case 32: return (int32_t)rd32(p) / 2147483648.0f;
  }
  return 0;
}

// Parses a WAV file held in memory and converts it to mono 44.1 kHz int16.
static bool decodeWav(const uint8_t *buf, size_t size, Voice &v, const char *name) {
  if (size < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
    Serial.printf("%s: not a WAV file\n", name);
    return false;
  }
  int fmt = 0, ch = 0, bits = 0;
  uint32_t rate = 0;
  const uint8_t *pcm = nullptr;
  uint32_t pcmLen = 0;
  for (size_t off = 12; off + 8 <= size;) {
    uint32_t id = rd32(buf + off), len = rd32(buf + off + 4);
    const uint8_t *body = buf + off + 8;
    if (len > size - off - 8) len = size - off - 8;  // truncated file
    if (id == 0x20746d66 && len >= 16) {             // "fmt "
      fmt = rd16(body); ch = rd16(body + 2); rate = rd32(body + 4); bits = rd16(body + 14);
      if (fmt == 0xFFFE && len >= 26) fmt = rd16(body + 24);  // WAVE_FORMAT_EXTENSIBLE
    } else if (id == 0x61746164) {                   // "data"
      pcm = body; pcmLen = len;
    }
    off += 8 + len + (len & 1);
  }
  bool ok = (fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
            (fmt == 3 && bits == 32);
  if (!ok || ch < 1 || rate == 0 || !pcm) {
    Serial.printf("%s: unsupported WAV (format %d, %d-bit). Export as 16-bit PCM.\n", name, fmt, bits);
    return false;
  }

  int bps = bits / 8, frameBytes = bps * ch;
  uint32_t inFrames = pcmLen / frameBytes;
  uint32_t outFrames = (uint64_t)inFrames * SR / rate;
  if (outFrames == 0) {
    Serial.printf("%s: empty\n", name);
    return false;
  }
  v.data = (int16_t *)ps_malloc(outFrames * sizeof(int16_t));
  if (!v.data) {
    Serial.printf("%s: too long, not enough memory\n", name);
    return false;
  }
  auto mono = [&](uint32_t i) {
    if (i >= inFrames) return 0.0f;
    float s = 0;
    for (int c = 0; c < ch; c++) s += sampleAt(pcm + i * frameBytes + c * bps, fmt, bits);
    return s / ch;
  };
  double step = (double)rate / SR;  // linear resampling
  for (uint32_t o = 0; o < outFrames; o++) {
    double p = o * step;
    uint32_t i = (uint32_t)p;
    float f = p - i, a = mono(i), b = mono(i + 1);
    float s = (a + (b - a) * f) * VOLUME;
    v.data[o] = (int16_t)(constrain(s, -1.0f, 1.0f) * 32767);
  }
  v.len = outFrames;
  Serial.printf("%s: %.1f s (%u Hz, %d ch, %d-bit)\n", name, outFrames / (float)SR, (unsigned)rate, ch, bits);
  return true;
}

static bool loadWav(const char *path, Voice &v) {
  File f = SD_MMC.open(path);
  if (!f) {
    Serial.printf("%s: not found on SD card\n", path);
    return false;
  }
  size_t size = f.size();
  uint8_t *buf = (uint8_t *)ps_malloc(size);
  if (!buf) {
    Serial.printf("%s: file too big (%u bytes)\n", path, (unsigned)size);
    f.close();
    return false;
  }
  size_t got = f.read(buf, size);
  f.close();
  bool ok = decodeWav(buf, got, v, path);
  free(buf);
  return ok;
}

// Fallback so a missing file is still audible: a short beep.
static void makeBeep(Voice &v) {
  v.len = SR / 4;
  v.data = (int16_t *)malloc(v.len * sizeof(int16_t));
  if (!v.data) { v.len = 0; return; }
  for (uint32_t i = 0; i < v.len; i++) v.data[i] = (i / 50) % 2 ? 12000 : -12000;  // 441 Hz
}

// ------------------------------------------------------------------ audio --
// Called by the Bluetooth stack whenever it needs more audio. When nothing is
// playing it sends silence, which keeps the speaker awake and connected.
int32_t getFrames(Frame *frame, int32_t count) {
  for (int b = 0; b < NUM_BUTTONS; b++) {
    Voice &v = voices[b];
    if (v.trigger) {
      v.trigger = false;
      v.playing = v.len > 0;
      v.pos = 0;
      v.fade = 0;
    }
  }
  for (int i = 0; i < count; i++) {
    int32_t mix = 0;
    for (int b = 0; b < NUM_BUTTONS; b++) {
      Voice &v = voices[b];
      if (!v.playing) continue;
      if (BUTTONS[b].mode == PLAY_WHILE_HELD && !v.held && v.fade == 0) v.fade = FADE_SAMPLES;
      int32_t s = v.data[v.pos];
      if (v.fade > 0) {
        s = s * v.fade / FADE_SAMPLES;
        if (--v.fade == 0) v.playing = false;
      }
      mix += s;
      if (++v.pos >= v.len) {
        v.pos = 0;  // loop only while a PLAY_WHILE_HELD button is held
        if (!(BUTTONS[b].mode == PLAY_WHILE_HELD && v.held)) { v.playing = false; v.fade = 0; }
      }
    }
    int16_t out = (int16_t)constrain(mix, -32767, 32767);
    frame[i].channel1 = out;
    frame[i].channel2 = out;
  }
  return count;
}

// ---------------------------------------------------------------- buttons --
struct Debounce {
  bool stable = false, lastRead = false;
  uint32_t changedAt = 0;
  bool update(bool r) {  // returns true on a new press
    if (r != lastRead) { lastRead = r; changedAt = millis(); }
    if (millis() - changedAt > 15 && r != stable) { stable = r; return stable; }
    return false;
  }
};
static Debounce debounce[NUM_BUTTONS];
static bool sdOk = false;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_STATUS, OUTPUT);

  // 1-bit SD mode leaves GPIO 4, 12 and 13 free.
  sdOk = SD_MMC.begin("/sdcard", true);
  if (!sdOk) Serial.println("No SD card found (FAT32 formatted?)");

  for (int b = 0; b < NUM_BUTTONS; b++) {
    if (!(sdOk && loadWav(BUTTONS[b].file, voices[b]))) makeBeep(voices[b]);
  }
  if (sdOk) SD_MMC.end();  // done with the card

  for (int b = 0; b < NUM_BUTTONS; b++) pinMode(BUTTONS[b].pin, INPUT_PULLUP);
  pinMode(PIN_FLASH, OUTPUT);
  digitalWrite(PIN_FLASH, LOW);

  a2dp.set_auto_reconnect(true);
  a2dp.set_data_callback_in_frames(getFrames);
  a2dp.set_volume(127);  // max; adjust loudness on the speaker itself
  a2dp.start(SPEAKER_NAME);
  Serial.printf("Looking for Bluetooth speaker \"%s\"...\n", SPEAKER_NAME);
}

void loop() {
  uint32_t now = millis();
  bool anyPlaying = false;
  for (int b = 0; b < NUM_BUTTONS; b++) {
    bool pressed = digitalRead(BUTTONS[b].pin) == LOW;
    bool newPress = debounce[b].update(pressed);
    voices[b].held = debounce[b].stable;  // set before trigger, the audio task reads both
    if (newPress) voices[b].trigger = true;
    anyPlaying |= voices[b].playing || voices[b].trigger;
  }

  // Status LED: solid when connected, blinking while searching,
  // fast blinking if the SD card is missing.
  bool on = a2dp.is_connected() ? true : (now / (sdOk ? 300 : 80)) % 2;
  digitalWrite(PIN_STATUS, on ? LOW : HIGH);

  if (USE_FLASH_LED) digitalWrite(PIN_FLASH, anyPlaying && (now / 80) % 2);

  delay(2);
}
