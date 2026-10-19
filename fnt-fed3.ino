// FED3 firmware v3 for FNT.
//
// The device runs a schedule FNT uploads (docs/fed3/design.md in fnt):
// rules, cues and phase changes all happen in core.cpp, so the PC can crash,
// reboot or be unplugged without the animals noticing. FNT watches, records
// and edits. core.cpp is tested on a PC (test/); this file is the FED3's
// hardware behind core.h's Hal interface.
//
// Pin assignments and the motor's jam-clearing movements follow the FED3
// library (Kravitz lab, CC BY-SA 3.0); everything here is non-blocking so the
// main loop keeps running.
#include "core.h"

#include <Adafruit_AHTX0.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_SharpMem.h>
#include <Arduino.h>
#include <Fonts/FreeSans9pt7b.h>
#include <RTClib.h>
#include <SPI.h>
#include <SdFat.h>
#include <Stepper.h>
#include <Wire.h>

namespace fed {

class ArduinoHal : public Hal {
 public:
  void begin();
  void service();             // motor, lights, sound, display: once per loop
  uint8_t resetCause();

  uint32_t millis() override;
  bool rtcValid() override;
  uint32_t rtcNow() override;
  void rtcSet(uint32_t localSeconds) override;
  bool sdOk() override;
  int32_t fileSize(const char* name) override;
  int32_t fileRead(const char* name, uint32_t offset, char* buf, uint32_t n) override;
  bool fileAppend(const char* name, const char* data, uint32_t n) override;
  bool fileWrite(const char* name, const char* data, uint32_t n) override;
  bool fileRemove(const char* name) override;
  bool fileRename(const char* from, const char* to) override;
  void send(const char* line) override;
  int readByte() override;
  bool pokeEvent(PokeEvent& event) override;
  bool wellFull() override;
  void startDispense() override;
  uint8_t dispenseEvent() override;
  void playCue(uint8_t slot, const Cue& cue, uint8_t side) override;
  void read(Readings& out) override;
  uint32_t random32() override;
  void show(const Screen& screen) override;
  void feedWatchdog() override;
  void reboot() override;
};

namespace {

// Pins, as wired on the FED3 board (FED3 library, FED3.h).
const uint8_t PIN_NEOPIXEL = A1;
const uint8_t PIN_MOTOR_ENABLE = 13;   // also powers the NeoPixels
const uint8_t PIN_GREEN_LED = 8;
const uint8_t PIN_WELL = 1;
const uint8_t PIN_LEFT = 6;
const uint8_t PIN_RIGHT = 5;
const uint8_t PIN_BUZZER = 0;
const uint8_t PIN_VBAT = A7;
const uint8_t PIN_SD_CS = 4;
const uint8_t PIN_BNC = A0;
const uint8_t PIN_SHARP_SCK = 12, PIN_SHARP_MOSI = 11, PIN_SHARP_SS = 10;
const uint8_t PIXEL_RIGHT_POKE = 8, PIXEL_LEFT_POKE = 9;

const int STEPS_PER_REV = 2038;
const int MOTOR_RPM = 250;
const uint16_t STEPS_PER_PASS = 40;      // motor steps per loop: ~5 ms
const uint16_t MAX_TURNS = 30;           // then report a jam and retry later
const uint32_t SD_RETRY_MS = 10000;

SdFat sd;
RTC_PCF8523 rtc;
Adafruit_NeoPixel strip(10, PIN_NEOPIXEL, NEO_GRBW + NEO_KHZ800);
Adafruit_SharpMem display(PIN_SHARP_SCK, PIN_SHARP_MOSI, PIN_SHARP_SS, 144, 168);
Stepper stepper(STEPS_PER_REV, A2, A3, A4, A5);
Adafruit_AHTX0 aht;

bool haveAht = false;
bool sdReady = false;
uint32_t sdRetryMs = 0;
bool rtcGood = false;
bool rtcCached = false;
uint32_t rtcBase = 0, rtcBaseMs = 0, rtcLast = 0;
uint32_t rngState = 1;

// --- pokes: edges timestamped in interrupts -------------------------------

const uint8_t QUEUE = 16;
volatile uint8_t qSide[QUEUE], qEdge[QUEUE];
volatile uint32_t qMs[QUEUE];
volatile uint8_t qHead = 0, qTail = 0;
volatile bool broken[2] = {false, false};

void pushEdge(uint8_t side, bool isBroken) {
  if (broken[side] == isBroken) return;
  broken[side] = isBroken;
  uint8_t next = (uint8_t)((qHead + 1) % QUEUE);
  if (next == qTail) return;   // full; the loop's resync repairs the state
  qSide[qHead] = side;
  qEdge[qHead] = isBroken ? EDGE_BREAK : EDGE_RELEASE;
  qMs[qHead] = ::millis();
  qHead = next;
}

void leftIsr() { pushEdge(0, digitalRead(PIN_LEFT) == LOW); }
void rightIsr() { pushEdge(1, digitalRead(PIN_RIGHT) == LOW); }

// --- motor: the FED3 dispense and jam-clearing sequence, a step at a time --

enum Segment : uint8_t { S_BASE, S_WAIT, S_MINOR, S_VIBRATE, S_CLEAR_BACK, S_CLEAR_WAIT, S_CLEAR_FWD, S_END };

struct Motor {
  bool active;
  uint8_t event;
  uint16_t turn;
  uint8_t segment;
  uint16_t iter, iterMax;
  int32_t steps;
  bool waiting;
  uint32_t waitUntil;
} motor;

// --- lights, sound, BNC ----------------------------------------------------

bool barOn = false, leftOn = false, rightOn = false;
uint32_t barUntil = 0, leftUntil = 0, rightUntil = 0;
uint32_t noiseUntil = 0, noiseNext = 0;
volatile uint16_t bncWidth = 0, bncPeriod = 0, bncTick = 0, bncRemaining = 0;

void power(bool on) {
  bool needed = on || motor.active || barOn || leftOn || rightOn;
  if (needed && digitalRead(PIN_MOTOR_ENABLE) == LOW) {
    digitalWrite(PIN_MOTOR_ENABLE, HIGH);
    delay(2);   // let the driver and pixels settle
  } else if (!needed) {
    digitalWrite(PIN_MOTOR_ENABLE, LOW);
  }
}

void releaseMotor() {
  digitalWrite(A2, LOW);
  digitalWrite(A3, LOW);
  digitalWrite(A4, LOW);
  digitalWrite(A5, LOW);
  motor.active = false;
  power(false);
}

bool wellDetected() {
  if (digitalRead(PIN_WELL) != LOW) return false;
  delayMicroseconds(100);
  return digitalRead(PIN_WELL) == LOW;   // debounce
}

void finishDispense(uint8_t event) {
  releaseMotor();
  motor.event = event;
}

// The next movement of the current turn; false when the turn is over.
bool nextMove() {
  uint16_t t = motor.turn;
  for (;;) {
    switch (motor.segment) {
      case S_BASE:
        motor.segment = S_WAIT;
        motor.steps = -300;
        return true;
      case S_WAIT:
        motor.segment = S_MINOR;
        motor.waiting = true;
        motor.waitUntil = ::millis() + 1500;
        return true;
      case S_MINOR:
        motor.segment = S_VIBRATE;
        motor.iter = 0;
        if (t % 5 == 0) {
          motor.steps = 100;
          return true;
        }
        break;
      case S_VIBRATE:
        if (t % 10 == 0 && t % 20 != 0 && motor.iter < 60) {
          motor.steps = motor.iter % 2 == 0 ? 120 : -60;
          motor.iter++;
          return true;
        }
        motor.segment = S_CLEAR_BACK;
        motor.iter = 0;
        motor.iterMax = 21 + (uint16_t)random(0, 20);
        break;
      case S_CLEAR_BACK:
        if (t % 20 == 0 && motor.iter < motor.iterMax) {
          motor.steps = -(int32_t)motor.iter * 4;
          motor.iter++;
          return true;
        }
        motor.segment = S_CLEAR_WAIT;
        break;
      case S_CLEAR_WAIT:
        motor.segment = S_CLEAR_FWD;
        motor.iter = 0;
        if (t % 20 == 0) {
          motor.waiting = true;
          motor.waitUntil = ::millis() + 250;
          return true;
        }
        break;
      case S_CLEAR_FWD:
        if (t % 20 == 0 && motor.iter < motor.iterMax) {
          motor.steps = (int32_t)motor.iter * 4;
          motor.iter++;
          return true;
        }
        motor.segment = S_END;
        break;
      default:
        return false;
    }
  }
}

void serviceMotor() {
  if (!motor.active) return;
  if (wellDetected()) return finishDispense(DISPENSE_LANDED);
  if (motor.waiting) {
    if ((int32_t)(::millis() - motor.waitUntil) < 0) return;
    motor.waiting = false;
  }
  if (motor.steps != 0) {
    for (uint16_t i = 0; i < STEPS_PER_PASS && motor.steps != 0; i++) {
      int dir = motor.steps > 0 ? 1 : -1;
      stepper.step(dir);
      motor.steps -= dir;
      if (wellDetected()) return finishDispense(DISPENSE_LANDED);
    }
    return;
  }
  if (nextMove()) return;
  motor.turn++;
  if (motor.turn > MAX_TURNS) return finishDispense(DISPENSE_JAMMED);
  motor.segment = S_BASE;
}

void setPixels(uint8_t first, uint8_t last, const uint8_t* rgbw) {
  for (uint8_t i = first; i <= last; i++) strip.setPixelColor(i, rgbw[0], rgbw[1], rgbw[2], rgbw[3]);
}

void serviceLights() {
  static const uint8_t off[4] = {0, 0, 0, 0};
  uint32_t now = ::millis();
  bool changed = false;
  if (barOn && (int32_t)(now - barUntil) >= 0) {
    setPixels(0, 7, off);
    barOn = false;
    changed = true;
  }
  if (leftOn && (int32_t)(now - leftUntil) >= 0) {
    setPixels(PIXEL_LEFT_POKE, PIXEL_LEFT_POKE, off);
    leftOn = false;
    changed = true;
  }
  if (rightOn && (int32_t)(now - rightUntil) >= 0) {
    setPixels(PIXEL_RIGHT_POKE, PIXEL_RIGHT_POKE, off);
    rightOn = false;
    changed = true;
  }
  if (changed) {
    power(true);
    strip.show();
    power(false);
  }
  if (noiseUntil && (int32_t)(now - noiseNext) >= 0) {
    if ((int32_t)(now - noiseUntil) >= 0) {
      noiseUntil = 0;
    } else {
      tone(PIN_BUZZER, (unsigned int)random(50, 250), 50);
      noiseNext = now + 50;
    }
  }
}

// --- display ---------------------------------------------------------------

struct Drawn {
  uint16_t id;
  char phase[MAX_NAME + 1];
  uint32_t left, right, pellets, minute;
  bool clockOk, jammed, running;
} drawn;
bool everDrawn = false;
uint32_t lastDrawMs = 0;

void draw(const Screen& s) {
  display.clearDisplay();
  display.setFont(&FreeSans9pt7b);
  display.setTextColor(0);   // black
  char line[40];
  display.setCursor(4, 16);
  snprintf(line, sizeof(line), "FED3  #%u", s.id);
  display.print(line);
  display.setCursor(4, 38);
  snprintf(line, sizeof(line), "%.14s", s.phase);
  display.print(line);
  display.setCursor(4, 60);
  snprintf(line, sizeof(line), "Left: %lu", (unsigned long)s.left);
  display.print(line);
  display.setCursor(4, 80);
  snprintf(line, sizeof(line), "Right: %lu", (unsigned long)s.right);
  display.print(line);
  display.setCursor(4, 100);
  snprintf(line, sizeof(line), "Pellets: %lu", (unsigned long)s.pellets);
  display.print(line);
  display.setCursor(4, 120);
  display.print(s.jammed ? "JAM - retrying" : s.running ? "running" : "idle");
  display.setCursor(4, 140);
  if (s.clockOk) {
    DateTime t(s.localSeconds);
    snprintf(line, sizeof(line), "%02u/%02u  %02u:%02u", t.month(), t.day(), t.hour(), t.minute());
    display.print(line);
  } else {
    display.print("clock not set");
  }
  display.refresh();
}

void sdDateTime(uint16_t* date, uint16_t* time) {
  DateTime now = rtc.now();
  *date = FS_DATE(now.year(), now.month(), now.day());
  *time = FS_TIME(now.hour(), now.minute(), now.second());
}

// --- watchdog (SAMD21 registers, as Adafruit_SleepyDog does) ---------------

void watchdogEnable() {
  // Generic clock 2 from the 32 kHz ultra-low-power oscillator, divided by 32.
  GCLK->GENDIV.reg = GCLK_GENDIV_ID(2) | GCLK_GENDIV_DIV(4);
  GCLK->GENCTRL.reg = GCLK_GENCTRL_ID(2) | GCLK_GENCTRL_GENEN | GCLK_GENCTRL_SRC_OSCULP32K | GCLK_GENCTRL_DIVSEL;
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  GCLK->CLKCTRL.reg = GCLK_CLKCTRL_ID_WDT | GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK2;
  WDT->CTRL.reg = 0;
  while (WDT->STATUS.bit.SYNCBUSY) {}
  WDT->CONFIG.reg = WDT_CONFIG_PER_8K;   // 8192 cycles at 1024 Hz: 8 s
  WDT->CTRL.reg = WDT_CTRL_ENABLE;
  while (WDT->STATUS.bit.SYNCBUSY) {}
}

// --- BNC pulses on a 1 kHz timer (TC3), so their timing doesn't depend on the loop

void bncTimerStart() {
  GCLK->CLKCTRL.reg = (uint16_t)(GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK0 | GCLK_CLKCTRL_ID(GCM_TCC2_TC3));
  while (GCLK->STATUS.bit.SYNCBUSY) {}
  TC3->COUNT16.CTRLA.reg &= ~TC_CTRLA_ENABLE;
  while (TC3->COUNT16.STATUS.bit.SYNCBUSY) {}
  TC3->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_WAVEGEN_MFRQ | TC_CTRLA_PRESCALER_DIV64;
  while (TC3->COUNT16.STATUS.bit.SYNCBUSY) {}
  TC3->COUNT16.CC[0].reg = 749;   // 48 MHz / 64 / 750 = 1 kHz
  while (TC3->COUNT16.STATUS.bit.SYNCBUSY) {}
  TC3->COUNT16.INTENSET.reg = TC_INTENSET_MC0;
  NVIC_SetPriority(TC3_IRQn, 2);
  NVIC_EnableIRQ(TC3_IRQn);
  TC3->COUNT16.CTRLA.reg |= TC_CTRLA_ENABLE;
  while (TC3->COUNT16.STATUS.bit.SYNCBUSY) {}
}

}  // namespace

void bncTimerTick() {
  if (!bncRemaining) return;
  if (bncTick == 0) {
    digitalWrite(PIN_BNC, HIGH);
    digitalWrite(PIN_GREEN_LED, HIGH);
  }
  if (bncTick == bncWidth) {
    digitalWrite(PIN_BNC, LOW);
    digitalWrite(PIN_GREEN_LED, LOW);
  }
  if (++bncTick >= bncPeriod) {
    bncTick = 0;
    bncRemaining--;
  }
}

// --- Hal -------------------------------------------------------------------

void ArduinoHal::begin() {
  Serial.begin(115200);
  pinMode(PIN_WELL, INPUT);
  pinMode(PIN_LEFT, INPUT);
  pinMode(PIN_RIGHT, INPUT);
  pinMode(PIN_VBAT, INPUT);
  pinMode(PIN_MOTOR_ENABLE, OUTPUT);
  pinMode(PIN_GREEN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BNC, OUTPUT);
  pinMode(A2, OUTPUT);
  pinMode(A3, OUTPUT);
  pinMode(A4, OUTPUT);
  pinMode(A5, OUTPUT);
  digitalWrite(PIN_MOTOR_ENABLE, LOW);
  watchdogEnable();

  // Seed from the chip's unique serial number and pin noise.
  rngState = *(volatile uint32_t*)0x0080A00C ^ *(volatile uint32_t*)0x0080A040 ^ analogRead(PIN_VBAT) ^ micros();
  if (!rngState) rngState = 1;
  randomSeed(rngState);

  rtcGood = rtc.begin() && rtc.initialized() && !rtc.lostPower();
  if (rtcGood) rtc.start();
  strip.begin();
  strip.show();
  stepper.setSpeed(MOTOR_RPM);
  display.begin();
  display.setRotation(3);
  display.clearDisplay();
  display.refresh();
  haveAht = aht.begin();
  FsDateTime::setCallback(sdDateTime);
  sdReady = sd.begin(PIN_SD_CS, SD_SCK_MHZ(4));
  broken[0] = digitalRead(PIN_LEFT) == LOW;
  broken[1] = digitalRead(PIN_RIGHT) == LOW;
  attachInterrupt(digitalPinToInterrupt(PIN_LEFT), leftIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_RIGHT), rightIsr, CHANGE);
  bncTimerStart();
  memset(&motor, 0, sizeof(motor));
}

uint8_t ArduinoHal::resetCause() {
  uint8_t cause = PM->RCAUSE.reg;
  if (cause & PM_RCAUSE_WDT) return RESET_WATCHDOG;
  if (cause & PM_RCAUSE_SYST) return RESET_SOFTWARE;
  if (cause & PM_RCAUSE_EXT) return RESET_EXTERNAL;
  return RESET_POWER;
}

void ArduinoHal::service() {
  serviceMotor();
  serviceLights();
  if (!sdReady && (int32_t)(::millis() - sdRetryMs) >= 0) {
    sdRetryMs = ::millis() + SD_RETRY_MS;
    sdReady = sd.begin(PIN_SD_CS, SD_SCK_MHZ(4));
  }
}

uint32_t ArduinoHal::millis() { return ::millis(); }

bool ArduinoHal::rtcValid() { return rtcGood; }

// Read the RTC over I2C once a second and count milliseconds in between.
uint32_t ArduinoHal::rtcNow() {
  uint32_t now = ::millis();
  if (!rtcCached || now - rtcBaseMs >= 1000) {
    rtcBase = rtc.now().unixtime();
    rtcBaseMs = now;
    rtcCached = true;
  }
  uint32_t value = rtcBase + (now - rtcBaseMs) / 1000;
  if (value < rtcLast && rtcLast - value < 2) value = rtcLast;   // never step back a second
  rtcLast = value;
  return value;
}

void ArduinoHal::rtcSet(uint32_t localSeconds) {
  rtc.adjust(DateTime(localSeconds));
  rtc.start();
  rtcGood = true;
  rtcCached = false;
  rtcLast = 0;
}

static bool sdFail() {
  sdReady = false;
  sdRetryMs = ::millis() + SD_RETRY_MS;
  return false;
}

bool ArduinoHal::sdOk() { return sdReady; }

int32_t ArduinoHal::fileSize(const char* name) {
  if (!sdReady) return -1;
  auto f = sd.open(name, O_RDONLY);
  if (!f) return -1;
  int32_t size = (int32_t)f.fileSize();
  f.close();
  return size;
}

int32_t ArduinoHal::fileRead(const char* name, uint32_t offset, char* buf, uint32_t n) {
  if (!sdReady) return -1;
  auto f = sd.open(name, O_RDONLY);
  if (!f) return -1;
  int32_t got = -1;
  if (f.seekSet(offset)) got = f.read(buf, n);
  f.close();
  return got;
}

bool ArduinoHal::fileAppend(const char* name, const char* data, uint32_t n) {
  if (!sdReady) return false;
  auto f = sd.open(name, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return sdFail();
  bool ok = f.write(data, n) == n;
  ok = f.close() && ok;
  return ok ? true : sdFail();
}

bool ArduinoHal::fileWrite(const char* name, const char* data, uint32_t n) {
  if (!sdReady) return false;
  auto f = sd.open(name, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return sdFail();
  bool ok = n == 0 || f.write(data, n) == n;
  ok = f.close() && ok;
  return ok ? true : sdFail();
}

bool ArduinoHal::fileRemove(const char* name) { return sdReady && sd.remove(name); }

bool ArduinoHal::fileRename(const char* from, const char* to) {
  return sdReady && !sd.exists(to) && sd.rename(from, to);
}

void ArduinoHal::send(const char* line) {
  // Only while a program has the port open: with nobody reading, each write
  // would block for the USB timeout. The rows are on the card regardless.
  if (!Serial.dtr()) return;
  // One write per line: each write can wait out the USB timeout on its own.
  static char out[MAX_COMMAND * 4 + 2];
  size_t n = strlen(line);
  if (n > sizeof(out) - 2) n = sizeof(out) - 2;
  memcpy(out, line, n);
  out[n] = '\n';
  Serial.write(out, n + 1);
}

int ArduinoHal::readByte() { return Serial.available() ? Serial.read() : -1; }

bool ArduinoHal::pokeEvent(PokeEvent& event) {
  noInterrupts();
  // A missed edge (queue full, or a glitch shorter than the ISR) leaves the
  // recorded state out of step with the pin; put it right.
  for (uint8_t side = 0; side < 2; side++) {
    bool level = digitalRead(side == 0 ? PIN_LEFT : PIN_RIGHT) == LOW;
    if (level != broken[side] && qHead == qTail) pushEdge(side, level);
  }
  bool have = qHead != qTail;
  if (have) {
    event.side = qSide[qTail];
    event.edge = qEdge[qTail];
    event.ms = qMs[qTail];
    qTail = (uint8_t)((qTail + 1) % QUEUE);
  }
  interrupts();
  return have;
}

bool ArduinoHal::wellFull() { return wellDetected(); }

void ArduinoHal::startDispense() {
  memset(&motor, 0, sizeof(motor));
  motor.active = true;
  motor.turn = 1;
  motor.segment = S_BASE;
  power(true);
}

uint8_t ArduinoHal::dispenseEvent() {
  uint8_t event = motor.event;
  motor.event = DISPENSE_NONE;
  return event;
}

void ArduinoHal::playCue(uint8_t slot, const Cue& cue, uint8_t side) {
  (void)slot;   // every slot sounds the same; the slot matters to the log, not the hardware
  bool lights = false;
  uint32_t now = ::millis();
  for (uint8_t i = 0; i < cue.n; i++) {
    const Action& a = cue.act[i];
    switch (a.kind) {
      case A_CLICK:
        tone(PIN_BUZZER, 800, 8);
        break;
      case A_TONE:
        tone(PIN_BUZZER, a.a, a.b);
        break;
      case A_NOISE:
        noiseUntil = now + a.b;
        noiseNext = now;
        break;
      case A_FLASH: {
        uint8_t target = a.target;
        if (target == T_POKE) target = side == 0 ? T_POKE_L : T_POKE_R;
        if (target == T_BAR) {
          setPixels(0, 7, a.rgbw);
          barOn = true;
          barUntil = now + a.b;
        }
        if (target == T_POKE_L || target == T_POKES) {
          setPixels(PIXEL_LEFT_POKE, PIXEL_LEFT_POKE, a.rgbw);
          leftOn = true;
          leftUntil = now + a.b;
        }
        if (target == T_POKE_R || target == T_POKES) {
          setPixels(PIXEL_RIGHT_POKE, PIXEL_RIGHT_POKE, a.rgbw);
          rightOn = true;
          rightUntil = now + a.b;
        }
        lights = true;
        break;
      }
      case A_BNC:
        noInterrupts();
        bncWidth = a.a;
        bncPeriod = (uint16_t)(1000 / a.b);
        bncTick = 0;
        bncRemaining = a.c;
        interrupts();
        break;
    }
  }
  if (lights) {
    power(true);
    strip.show();
  }
}

void ArduinoHal::read(Readings& out) {
  analogReadResolution(10);
  out.battery = analogRead(PIN_VBAT) * 2.0f * 3.3f / 1024.0f;
  out.temperature = out.humidity = NAN;
  if (haveAht) {
    sensors_event_t humidity, temperature;
    if (aht.getEvent(&humidity, &temperature)) {
      out.temperature = temperature.temperature;
      out.humidity = humidity.relative_humidity;
    }
  }
}

uint32_t ArduinoHal::random32() {
  rngState ^= rngState << 13;
  rngState ^= rngState >> 17;
  rngState ^= rngState << 5;
  return rngState;
}

void ArduinoHal::show(const Screen& s) {
  uint32_t now = ::millis();
  uint32_t minute = s.localSeconds / 60;
  bool changed = !everDrawn || s.id != drawn.id || strcmp(s.phase, drawn.phase) != 0 ||
                 s.left != drawn.left || s.right != drawn.right || s.pellets != drawn.pellets ||
                 minute != drawn.minute || s.clockOk != drawn.clockOk ||
                 s.jammed != drawn.jammed || s.running != drawn.running;
  // At most twice a second: a redraw takes tens of milliseconds.
  if (!changed || (everDrawn && now - lastDrawMs < 500)) return;
  drawn.id = s.id;
  snprintf(drawn.phase, sizeof(drawn.phase), "%s", s.phase);
  drawn.left = s.left;
  drawn.right = s.right;
  drawn.pellets = s.pellets;
  drawn.minute = minute;
  drawn.clockOk = s.clockOk;
  drawn.jammed = s.jammed;
  drawn.running = s.running;
  everDrawn = true;
  lastDrawMs = now;
  draw(s);
}

void ArduinoHal::feedWatchdog() {
  if (!WDT->STATUS.bit.SYNCBUSY) WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
}

void ArduinoHal::reboot() {
  Serial.flush();
  NVIC_SystemReset();
}

}  // namespace fed

void TC3_Handler() {
  TC3->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0;
  fed::bncTimerTick();
}

fed::ArduinoHal hal;
fed::Device device(hal);

void setup() {
  hal.begin();
  device.boot(hal.resetCause());
}

void loop() {
  device.loop();
  hal.service();
}
