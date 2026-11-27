// Bike horn for ESP32-CAM + any Bluetooth speaker.
//
// The ESP32 acts as a Bluetooth A2DP *source* (like a phone) and streams
// audio it synthesizes on the fly to the speaker, so no sound files are needed.
//
//   HORN button (GPIO 13 -> GND): two-tone car-style horn while held
//   BELL button (GPIO 14 -> GND): loud double "ring-ring" bell
//
// The stream keeps playing silence when idle. The speaker does not fall
// asleep, and a button press is heard right away instead of after a reconnect.

#include <Arduino.h>
#include "BluetoothA2DPSource.h"

// ---------------------------------------------------------------- settings --
// Exact Bluetooth name of your speaker, as shown on your phone.
static const char *SPEAKER_NAME = "JBL GO 3";

static const int PIN_HORN   = 13;  // button to GND
static const int PIN_BELL   = 14;  // button to GND
static const int PIN_STATUS = 33;  // small red LED on the back (active LOW)
static const int PIN_FLASH  = 4;   // big white flash LED, blinks with the horn

static const bool  USE_FLASH_LED = true;
static const float MASTER_GAIN   = 0.9f;  // 0..1, keep below 1 to avoid clipping

// Horn: two detuned tones, roughly a major third apart like real car horns.
static const float HORN_F1 = 415.0f;
static const float HORN_F2 = 519.0f;

// Bell: inharmonic partials of a small metal dome.
static const float BELL_BASE = 2300.0f;
// ---------------------------------------------------------------------------

static const float SR = 44100.0f;
BluetoothA2DPSource a2dp;

// Sine lookup table (cheap enough to run inside the Bluetooth callback).
static const int SINE_N = 1024;
static float sineTab[SINE_N];
static inline float sinLookup(float phase) {  // phase in [0,1)
  return sineTab[(int)(phase * SINE_N) & (SINE_N - 1)];
}

// Shared between loop() (buttons) and the audio callback.
static volatile bool hornHeld = false;
static volatile bool bellTrigger = false;

// ---- horn state
static float hornPh1 = 0, hornPh2 = 0, hornEnv = 0;
static const float HORN_ATTACK  = 1.0f / (0.008f * SR);  // 8 ms
static const float HORN_RELEASE = 1.0f / (0.040f * SR);  // 40 ms
static const unsigned HORN_MIN_MS = 250;  // short tap still gives a real beep

// ---- bell state
static const int BELL_PARTIALS = 4;
static const float bellRatio[BELL_PARTIALS] = {1.0f, 1.51f, 2.74f, 3.91f};
static const float bellAmp[BELL_PARTIALS]   = {1.0f, 0.6f, 0.35f, 0.2f};
static const float bellTau[BELL_PARTIALS]   = {0.55f, 0.35f, 0.18f, 0.10f};  // s
static float bellDecay[BELL_PARTIALS];
static float bellPh[BELL_PARTIALS] = {0};
static float bellEnv[BELL_PARTIALS] = {0};
static int32_t bellSample = -1;           // position in the ring-ring pattern
static const int32_t BELL_SECOND_STRIKE = (int32_t)(0.16f * SR);
static const int32_t BELL_LENGTH = (int32_t)(1.4f * SR);

static void strikeBell() {
  for (int k = 0; k < BELL_PARTIALS; k++) bellEnv[k] = 1.0f;
}

// Called by the Bluetooth stack whenever it needs more audio.
int32_t getFrames(Frame *frame, int32_t count) {
  const float inc1 = HORN_F1 / SR, inc2 = HORN_F2 / SR;

  if (bellTrigger) {
    bellTrigger = false;
    bellSample = 0;
    strikeBell();
  }

  for (int i = 0; i < count; i++) {
    float out = 0;

    // Horn: two sawtooth waves, soft-clipped for a brassy, loud sound.
    float target = hornHeld ? 1.0f : 0.0f;
    if (hornEnv < target) hornEnv = fminf(target, hornEnv + HORN_ATTACK);
    else if (hornEnv > target) hornEnv = fmaxf(target, hornEnv - HORN_RELEASE);
    if (hornEnv > 0) {
      hornPh1 += inc1; if (hornPh1 >= 1) hornPh1 -= 1;
      hornPh2 += inc2; if (hornPh2 >= 1) hornPh2 -= 1;
      float s = (2 * hornPh1 - 1) + (2 * hornPh2 - 1);
      s *= 1.6f;
      s = s / (1.0f + fabsf(s));  // soft clip
      out += s * hornEnv;
    }

    // Bell: decaying partials, struck twice.
    if (bellSample >= 0) {
      if (bellSample == BELL_SECOND_STRIKE) strikeBell();
      float b = 0;
      for (int k = 0; k < BELL_PARTIALS; k++) {
        bellPh[k] += BELL_BASE * bellRatio[k] / SR;
        if (bellPh[k] >= 1) bellPh[k] -= 1;
        b += sinLookup(bellPh[k]) * bellAmp[k] * bellEnv[k];
        bellEnv[k] *= bellDecay[k];
      }
      out += b * 0.55f;
      if (++bellSample >= BELL_LENGTH) bellSample = -1;
    }

    if (out > 1) out = 1;
    if (out < -1) out = -1;
    int16_t v = (int16_t)(out * MASTER_GAIN * 32767);
    frame[i].channel1 = v;
    frame[i].channel2 = v;
  }
  return count;
}

// ---------------------------------------------------------------- buttons --
struct Button {
  int pin;
  bool stable = false;   // true = pressed
  bool lastRead = false;
  uint32_t changedAt = 0;
  bool update() {        // returns true on a new press
    bool r = digitalRead(pin) == LOW;
    if (r != lastRead) { lastRead = r; changedAt = millis(); }
    if (millis() - changedAt > 15 && r != stable) {
      stable = r;
      return stable;
    }
    return false;
  }
};

static Button hornBtn{PIN_HORN}, bellBtn{PIN_BELL};
static uint32_t hornPressedAt = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_HORN, INPUT_PULLUP);
  pinMode(PIN_BELL, INPUT_PULLUP);
  pinMode(PIN_STATUS, OUTPUT);
  pinMode(PIN_FLASH, OUTPUT);
  digitalWrite(PIN_FLASH, LOW);

  for (int i = 0; i < SINE_N; i++) sineTab[i] = sinf(2 * PI * i / SINE_N);
  for (int k = 0; k < BELL_PARTIALS; k++) bellDecay[k] = expf(-1.0f / (bellTau[k] * SR));

  a2dp.set_auto_reconnect(true);
  a2dp.set_data_callback_in_frames(getFrames);
  a2dp.set_volume(127);  // max; adjust loudness on the speaker itself
  a2dp.start(SPEAKER_NAME);
  Serial.printf("Looking for Bluetooth speaker \"%s\"...\n", SPEAKER_NAME);
}

void loop() {
  uint32_t now = millis();

  if (hornBtn.update()) hornPressedAt = now;
  hornHeld = hornBtn.stable || (now - hornPressedAt < HORN_MIN_MS && hornPressedAt != 0);

  if (bellBtn.update()) bellTrigger = true;

  bool connected = a2dp.is_connected();
  // Status LED: solid when connected, blinking while searching.
  digitalWrite(PIN_STATUS, connected ? LOW : ((now / 300) % 2 ? LOW : HIGH));

  // Flash LED strobes with the horn as an extra visual warning.
  if (USE_FLASH_LED) digitalWrite(PIN_FLASH, hornHeld && (now / 80) % 2);

  delay(2);
}
