// A FED3 on the PC: simulated clock, motor, well and SD card around the real
// device core, exposed as a C API for Python (test/fed3_core.py).
//
// The SD card is a directory, so a test can look at exactly the files the
// firmware would leave on a card.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include "../core.h"

using namespace fed;

namespace {

class HostHal : public Hal {
 public:
  std::string dir;
  int64_t worldMs = 0;        // true time, UTC milliseconds
  int64_t rtcOffsetMs = 0;    // RTC local time minus true UTC time
  bool rtcOk = true;
  int64_t bootWorldMs = 0;
  bool sdFailing = false;
  std::vector<std::string> sent;
  std::vector<std::string> cues;
  std::deque<PokeEvent> pokes;
  bool well = false;
  bool dispensing = false;
  uint32_t dispenseDueMs = 0;
  int jamsPending = 0;
  uint32_t rng = 2463534242u;
  bool rebooted = false;

  std::string path(const char* name) const { return dir + "/" + name; }

  uint32_t millis() override { return (uint32_t)(worldMs - bootWorldMs); }
  bool rtcValid() override { return rtcOk; }
  uint32_t rtcNow() override {
    int64_t ms = worldMs + rtcOffsetMs;
    return (uint32_t)(ms >= 0 ? ms / 1000 : (ms - 999) / 1000);
  }
  void rtcSet(uint32_t localSeconds) override {
    rtcOffsetMs = (int64_t)localSeconds * 1000 - worldMs;
    rtcOk = true;
  }

  bool sdOk() override { return !sdFailing; }
  int32_t fileSize(const char* name) override {
    if (sdFailing) return -1;
    FILE* f = fopen(path(name).c_str(), "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return (int32_t)n;
  }
  int32_t fileRead(const char* name, uint32_t offset, char* buf, uint32_t n) override {
    if (sdFailing) return -1;
    FILE* f = fopen(path(name).c_str(), "rb");
    if (!f) return -1;
    fseek(f, offset, SEEK_SET);
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return (int32_t)got;
  }
  bool fileAppend(const char* name, const char* data, uint32_t n) override {
    if (sdFailing) return false;
    FILE* f = fopen(path(name).c_str(), "ab");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
  }
  bool fileWrite(const char* name, const char* data, uint32_t n) override {
    if (sdFailing) return false;
    FILE* f = fopen(path(name).c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
  }
  bool fileRemove(const char* name) override {
    return !sdFailing && remove(path(name).c_str()) == 0;
  }
  bool fileRename(const char* from, const char* to) override {
    if (sdFailing || fileSize(to) >= 0) return false;   // FAT can't rename onto a file
    return rename(path(from).c_str(), path(to).c_str()) == 0;
  }

  void send(const char* line) override { sent.push_back(line); }
  int readByte() override { return -1; }

  bool pokeEvent(PokeEvent& event) override {
    if (pokes.empty()) return false;
    event = pokes.front();
    pokes.pop_front();
    return true;
  }
  bool wellFull() override { return well; }
  void startDispense() override {
    dispensing = true;
    dispenseDueMs = millis() + 1000;
  }
  uint8_t dispenseEvent() override {
    if (!dispensing || (int32_t)(millis() - dispenseDueMs) < 0) return DISPENSE_NONE;
    dispensing = false;
    if (jamsPending > 0) {
      jamsPending--;
      return DISPENSE_JAMMED;
    }
    well = true;
    return DISPENSE_LANDED;
  }

  void playCue(uint8_t slot, const Cue& cue, uint8_t) override {
    static const char* const SLOTS[] = {"reward", "counted", "inactive", "timeout", "depleted"};
    char text[128];
    formatCue(cue, text, sizeof(text));
    cues.push_back(std::string(slot < 5 ? SLOTS[slot] : "test") + "\t" + text);
  }

  void read(Readings& out) override {
    out.battery = 4.10f;
    out.temperature = 22.0f;
    out.humidity = 40.0f;
  }
  uint32_t random32() override {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  }
  void show(const Screen&) override {}
  void feedWatchdog() override {}
  void reboot() override { restart(); }

  // A reset: millis starts again and the motor stops. The well and the card
  // are physical and stay as they are.
  void restart() {
    bootWorldMs = worldMs;
    dispensing = false;
    pokes.clear();
    rebooted = true;
  }
};

struct Host {
  HostHal hal;
  Device device;
  size_t sentRead = 0;
  size_t cuesRead = 0;
  Host() : device(hal) {}

  void advance(int64_t ms) {
    while (ms > 0) {
      int64_t step = device.needsFineTicks() ? 10 : 1000;
      // Land on whole seconds when idle, as a real loop running constantly would
      // catch a boundary within milliseconds of it.
      if (step == 1000) {
        int64_t toSecond = 1000 - ((hal.worldMs + hal.rtcOffsetMs) % 1000 + 1000) % 1000;
        step = toSecond;
      }
      if (step > ms) step = ms;
      hal.worldMs += step;
      ms -= step;
      device.loop();
      while (device.reading()) device.serviceRead(64);
    }
  }
};

int copyLines(const std::vector<std::string>& lines, size_t& from, char* buf, int size) {
  std::string out;
  for (size_t i = from; i < lines.size(); i++) {
    out += lines[i];
    out += '\n';
  }
  if ((int)out.size() + 1 > size) return -(int)(out.size() + 1);
  memcpy(buf, out.c_str(), out.size() + 1);
  from = lines.size();
  return (int)out.size();
}

}  // namespace

extern "C" {

void* fed_new(const char* dir, long long world_ms, int offset_min, int clock_set,
              unsigned seed) {
  Host* host = new Host();
  host->hal.dir = dir;
  host->hal.worldMs = world_ms;
  host->hal.bootWorldMs = world_ms;
  host->hal.rtcOffsetMs = (long long)offset_min * 60000;
  host->hal.rtcOk = clock_set != 0;
  if (seed) host->hal.rng = seed;
  host->device.boot(RESET_POWER);
  return host;
}

void fed_free(void* h) { delete (Host*)h; }

void fed_reboot(void* h, int cause, int lose_clock) {
  Host* host = (Host*)h;
  if (lose_clock) host->hal.rtcOk = false;
  host->hal.restart();
  host->device.boot((uint8_t)cause);
}

void fed_power_off(void* h, long long ms, int lose_clock) {
  Host* host = (Host*)h;
  host->hal.worldMs += ms;
  fed_reboot(h, RESET_POWER, lose_clock);
}

void fed_advance(void* h, long long ms) { ((Host*)h)->advance(ms); }

void fed_poke(void* h, int side, int edge) {
  Host* host = (Host*)h;
  PokeEvent ev;
  ev.side = (uint8_t)side;
  ev.edge = (uint8_t)edge;
  ev.ms = host->hal.millis();
  host->hal.pokes.push_back(ev);
  host->device.loop();
}

void fed_retrieve(void* h) {
  Host* host = (Host*)h;
  host->hal.well = false;
  host->device.loop();
}

void fed_command(void* h, const char* line) {
  Host* host = (Host*)h;
  host->device.handleLine(line);
  while (host->device.reading()) host->device.serviceRead(64);
}

int fed_take(void* h, char* buf, int size) {
  Host* host = (Host*)h;
  return copyLines(host->hal.sent, host->sentRead, buf, size);
}

int fed_cues(void* h, char* buf, int size) {
  Host* host = (Host*)h;
  return copyLines(host->hal.cues, host->cuesRead, buf, size);
}

void fed_set_jams(void* h, int n) { ((Host*)h)->hal.jamsPending = n; }
int fed_get_jams(void* h) { return ((Host*)h)->hal.jamsPending; }
void fed_set_sd_failing(void* h, int failing) { ((Host*)h)->hal.sdFailing = failing != 0; }
void fed_drift(void* h, long long ms) { ((Host*)h)->hal.rtcOffsetMs += ms; }
long long fed_world(void* h) { return ((Host*)h)->hal.worldMs; }
const char* fed_file(void* h) { return ((Host*)h)->device.dataFile(); }

int fed_rule(void* h, char* buf, int size) {
  Host* host = (Host*)h;
  Rule rule;
  host->device.currentRule(rule);
  std::string out;
  char value[128];
  for (int k = 0; k < NUM_KEYS; k++) {
    formatKey(rule, k, value, sizeof(value));
    if (k) out += ' ';
    out += keyName(k);
    out += '=';
    out += value;
  }
  if ((int)out.size() + 1 > size) return -1;
  memcpy(buf, out.c_str(), out.size() + 1);
  return (int)out.size();
}

}  // extern "C"
