// The device core: everything that decides what happens.
//
// Schedules, rules and cues (the text FNT uploads, parsed), phases, recovery
// after a reboot, the data file and the serial protocol. Portable C++ with no
// Arduino dependency: fnt-fed3.ino runs it on the FED3, and test/host_hal.cpp
// runs it on a PC against fnt's simulator. The contract is fnt/fed3/schedule.py
// and fnt/fed3/device_sim.py; when this code and those disagree, they're right.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace fed {

// ===========================================================================
// Schedules, rules and cues
// ===========================================================================

const int MAX_LINE = 100;          // longest schedule directive
const int MAX_NAME = 24;           // phase names
const int MAX_UNTILS = 6;
const int MAX_WINDOWS = 8;
const int MAX_PHASES = 256;
const uint32_t MAX_SCHED_BYTES = 65536;
const int MAX_CUE_ACTIONS = 3;
const uint32_t SECONDS_PER_DAY = 86400;

// ---------------------------------------------------------------------------
// Cues

enum ActionKind : uint8_t { A_CLICK, A_TONE, A_NOISE, A_FLASH, A_BNC };
enum FlashTarget : uint8_t { T_BAR, T_POKE_L, T_POKE_R, T_POKES, T_POKE };

struct Action {
  uint8_t kind;
  uint8_t target;       // flash
  uint8_t rgbw[4];      // flash
  uint16_t a, b, c;     // tone: hz, ms | noise: -, ms | flash: -, ms | bnc: width, hz, count
};

struct Cue {
  uint8_t n;            // 0: none
  Action act[MAX_CUE_ACTIONS];
};

const char* parseCue(const char* text, Cue& out);   // nullptr, or why not
void formatCue(const Cue& cue, char* out, size_t size);

// ---------------------------------------------------------------------------
// Rules

// Key indexes, in canonical order. Values 0-10 live in Rule::v, cues in Rule::cue.
enum Key : uint8_t {
  K_ACTIVE, K_COUNT, K_RATIO, K_RATIO_KIND, K_TIMEOUT, K_MIN_POKE_MS, K_REWARD,
  K_QUOTA, K_QUOTA_PERIOD, K_FREE_FEED, K_REFILL_DELAY,
  K_CUE_REWARD, K_CUE_COUNTED, K_CUE_INACTIVE, K_CUE_TIMEOUT, K_CUE_DEPLETED,
  NUM_KEYS
};
const int NUM_VALUES = K_CUE_REWARD;
const int NUM_CUES = NUM_KEYS - K_CUE_REWARD;

enum Active : uint8_t { ACTIVE_LEFT, ACTIVE_RIGHT, ACTIVE_BOTH, ACTIVE_NONE };
enum CountMode : uint8_t { COUNT_SHARED, COUNT_PER_PORT };
enum RatioKind : uint8_t { RK_FIXED, RK_RANDOM, RK_PROGRESSIVE };
enum Reward : uint8_t { REWARD_PELLET, REWARD_NONE };
enum CueSlot : uint8_t { CUE_REWARD, CUE_COUNTED, CUE_INACTIVE, CUE_TIMEOUT, CUE_DEPLETED };

struct Rule {
  uint32_t v[NUM_VALUES];
  Cue cue[NUM_CUES];
};

void ruleDefaults(Rule& rule);
int keyIndex(const char* name, size_t len);
const char* keyName(int key);
const char* setKey(Rule& rule, int key, const char* value, size_t len);
void formatKey(const Rule& rule, int key, char* out, size_t size);
// "key=value key=value" into a rule; mask gets one bit per key set.
const char* parseSettings(const char* text, Rule& rule, uint32_t& mask, uint32_t refuseMask);

// ---------------------------------------------------------------------------
// End conditions, windows, phases

enum UntilKind : uint8_t { U_AFTER, U_AT, U_REWARDS, U_POKES, U_GOTO };
enum PokeSide : uint8_t { PS_LEFT, PS_RIGHT, PS_ANY };

struct Until {
  uint8_t kind;
  uint8_t side;         // pokes
  uint16_t clock;       // at: minutes after midnight
  uint16_t day;         // at: 0 for the next occurrence
  uint32_t value;       // after: seconds | rewards, pokes: count
};

const char* parseUntil(const char* text, Until& out);
bool isTimeUntil(const Until& until);

struct Window {
  uint16_t start, end;  // minutes after midnight
  uint32_t mask;        // keys this window sets
  Rule rule;
};

struct Phase {
  char name[MAX_NAME + 1];
  uint8_t nUntil;
  Until untils[MAX_UNTILS];
  Rule rule;
  uint8_t nWin;
  Window win[MAX_WINDOWS];
};

const char* phaseNameProblem(const char* name, size_t len);
bool windowsOverlap(uint16_t s1, uint16_t e1, uint16_t s2, uint16_t e2);
bool windowActive(const Window& window, uint32_t localSeconds);
void effectiveRule(const Phase& phase, uint32_t localSeconds, bool clockOk, Rule& out);

// The device's timing rule (schedule.due_time). Times are local standard
// seconds. Returns false for conditions that aren't times.
bool dueTime(const Until& until, uint32_t phaseStart, uint32_t t0, uint32_t& due);

uint32_t progressiveRequirement(uint32_t rewards);

// ---------------------------------------------------------------------------
// Schedules

uint32_t crc32Update(uint32_t crc, const void* data, size_t n);

// Validates a schedule one line at a time, as it is uploaded.
class ScheduleBuilder {
 public:
  void reset();
  const char* add(const char* line, size_t len);
  const char* finish() const;
  uint32_t crc() const { return crc_; }
  uint16_t phases() const { return phases_; }
  uint16_t lines() const { return lines_; }

 private:
  bool named_ = false;
  uint16_t phases_ = 0;
  uint16_t lines_ = 0;
  uint32_t crc_ = 0;
  uint32_t size_ = 0;
  uint32_t seenKeys_ = 0;
  uint8_t nUntil_ = 0;
  uint8_t nWin_ = 0;
  uint16_t winStart_[MAX_WINDOWS];
  uint16_t winEnd_[MAX_WINDOWS];
};

// Reads one phase out of a stored schedule, fed line by line.
class PhaseLoader {
 public:
  PhaseLoader(uint16_t index, Phase& out);
  const char* line(const char* text, size_t len);
  uint16_t phases() const { return count_; }
  bool found() const { return count_ > index_; }

 private:
  uint16_t index_;
  uint16_t count_ = 0;
  Phase& out_;
  bool loading_ = false;
};

// Small text helpers shared with the device.
bool parseUint(const char* text, size_t len, uint32_t lo, uint32_t hi, uint32_t& out);
bool parseClock(const char* text, size_t len, uint16_t& minutes);

// ===========================================================================
// The hardware, as an interface: fnt-fed3.ino implements it on the FED3,
// test/host_hal.cpp with a simulated clock, motor and SD card.
// ===========================================================================

enum ResetCause : uint8_t { RESET_POWER, RESET_WATCHDOG, RESET_SOFTWARE, RESET_EXTERNAL };
enum DispenseEvent : uint8_t { DISPENSE_NONE, DISPENSE_LANDED, DISPENSE_JAMMED };
enum PokeEdge : uint8_t { EDGE_BREAK, EDGE_RELEASE };

struct PokeEvent {
  uint8_t side;         // 0 left, 1 right
  uint8_t edge;
  uint32_t ms;          // millis() when the edge happened
};

struct Readings {
  float battery, temperature, humidity;
};

// What the display shows; the HAL decides how and when to redraw.
struct Screen {
  uint16_t id;
  const char* phase;
  uint32_t left, right, pellets;
  bool clockOk, jammed, running;
  uint32_t localSeconds;
};

class Hal {
 public:
  virtual ~Hal() {}

  virtual uint32_t millis() = 0;

  // Real-time clock, in local standard seconds since 1970 (no time zone).
  virtual bool rtcValid() = 0;
  virtual uint32_t rtcNow() = 0;
  virtual void rtcSet(uint32_t localSeconds) = 0;

  // SD card. Paths are short 8.3 names in the root directory.
  virtual bool sdOk() = 0;
  virtual int32_t fileSize(const char* name) = 0;           // -1: missing
  virtual int32_t fileRead(const char* name, uint32_t offset, char* buf, uint32_t n) = 0;
  virtual bool fileAppend(const char* name, const char* data, uint32_t n) = 0;
  virtual bool fileWrite(const char* name, const char* data, uint32_t n) = 0;  // truncate
  virtual bool fileRemove(const char* name) = 0;
  virtual bool fileRename(const char* from, const char* to) = 0;

  // One line to the host, without its newline. Dropped if nobody is listening.
  virtual void send(const char* line) = 0;
  virtual int readByte() = 0;                                // -1: nothing waiting

  virtual bool pokeEvent(PokeEvent& event) = 0;
  virtual bool wellFull() = 0;
  virtual void startDispense() = 0;
  virtual uint8_t dispenseEvent() = 0;

  // slot is a CueSlot, or 255 for TEST; side is where the triggering poke was.
  virtual void playCue(uint8_t slot, const Cue& cue, uint8_t side) = 0;
  // Bar and poke lights on steadily (white), for finding a device on the
  // bench. Cues still flash over them; off means dark again.
  virtual void lights(bool on) = 0;

  virtual void read(Readings& out) = 0;
  virtual uint32_t random32() = 0;
  virtual void show(const Screen& screen) = 0;
  virtual void feedWatchdog() = 0;
  virtual void reboot() = 0;
};

// ===========================================================================
// The device
// ===========================================================================

const char* const FIRMWARE = "3.1";
const int PROTO = 3;
const int MAX_COMMAND = 128;
const uint32_t JAM_RETRY_MS = 600000UL;
const uint32_t STATUS_EVERY_MS = 3600000UL;
const uint32_t MAX_HOLD_MS = 60000UL;
const uint32_t STAGING_TIMEOUT_MS = 10000UL;
const uint32_t TIME_JUMP_LIMIT_S = 600;
const uint32_t CATCH_UP_S = 60;

enum Outcome : uint8_t {
  O_REWARD, O_COUNTED, O_INACTIVE, O_TIMEOUT, O_SHORT, O_PELLET_IN_WELL, O_DEPLETED
};
enum Well : uint8_t { WELL_EMPTY, WELL_DISPENSING, WELL_FULL };
enum Reason : uint8_t { R_REWARD, R_FREE, R_MANUAL, R_RETRY };

class Device {
 public:
  explicit Device(Hal& hal);

  void boot(uint8_t cause);
  void loop();                          // one pass of the main loop
  void handleLine(const char* line);    // one command, without its newline
  bool reading() const { return read_.active; }
  void serviceRead(int maxRows);
  void currentRule(Rule& out);          // the rule in force now
  bool needsFineTicks() const;          // host build: something is timing in ms
  const char* dataFile() const { return file_; }

 private:
  Hal& hal_;

  // Device
  uint16_t id_ = 1;
  int16_t offsetMin_ = 0;

  // Schedule
  Phase phase_;
  uint16_t nPhases_ = 1;
  bool schedDefault_ = true;
  uint32_t schedCrc_ = 0;
  bool nextPresent_ = false;
  uint32_t nextCrc_ = 0;

  // Run
  char runId_[9] = "-";
  bool ended_ = true;
  uint32_t t0_ = 0;
  char file_[13] = "IDLE.CSV";
  uint16_t phaseIndex_ = 0;
  uint32_t phaseStart_ = 0;
  bool havePhaseStart_ = false;

  // Counts
  uint32_t seq_ = 0, left_ = 0, right_ = 0, pellets_ = 0;
  uint32_t phaseRewards_ = 0, phaseLeft_ = 0, phaseRight_ = 0;
  uint32_t quotaUsed_ = 0;
  bool haveQuotaPeriod_ = false;
  int32_t quotaPeriod_ = 0;
  uint32_t counters_[3] = {0, 0, 0};    // left, right, shared
  bool timeoutActive_ = false;
  uint32_t timeoutUntilMs_ = 0;

  // Well and motor
  uint8_t well_ = WELL_EMPTY;
  uint8_t reason_ = R_REWARD;
  uint32_t pelletMs_ = 0;
  bool haveRetrieve_ = false;
  uint32_t lastRetrieveMs_ = 0;
  bool jammed_ = false;
  bool lights_ = false;     // LIGHTS:on; never survives a boot or RUN:BEGIN
  uint32_t jamMs_ = 0;

  // Pokes being held
  struct Hold {
    bool active, decided, logged;
    uint32_t breakMs, breakLocal;
    uint8_t outcome;
    uint16_t phase;
  } hold_[2];

  // Upload in progress
  ScheduleBuilder builder_;
  bool staging_ = false;
  bool stagingFailed_ = false;
  char stagingErr_[80];
  uint32_t stagingMs_ = 0;

  // READ in progress
  struct ReadJob {
    bool active;
    char run[9];
    char file[13];
    uint32_t pos, from, last;
  } read_;

  uint32_t nextStatusMs_ = 0;
  char input_[MAX_COMMAND + 2];
  uint16_t inputLen_ = 0;
  bool inputOverflow_ = false;
  bool sdError_ = false;

  // Clock
  bool clockOk();
  uint32_t local();
  void iso(uint32_t localSeconds, char* out);

  // Card
  void loadDevice();
  void saveDevice();
  void loadRun();
  void saveRun();
  bool writeSmall(const char* name, const char* text);
  bool promote(const char* from, const char* name);
  void recover(const char* name);
  bool validateSchedule(const char* name, uint32_t& crc, uint16_t& phases);
  bool loadPhase(uint16_t index, Phase& out);
  void loadSchedule();
  void restoreCounts();
  void row(const char* event, const char* side, const char* outcome, int32_t phase,
           const char* retrievalMs, const char* pokeMs, const char* detail,
           uint32_t atLocal, uint32_t atMs, bool atClockOk);
  void simpleRow(const char* event, const char* detail);

  // Behaviour
  void tick();
  bool activeRun() const { return runId_[0] != '-' && !ended_; }
  bool canAdvance();
  bool nextDue(uint32_t& due);
  void checkTime();
  void checkCounts();
  void enterPhase(uint16_t index, uint32_t start, const char* reason);
  bool depleted(const Rule& rule);
  uint8_t decide(uint8_t side, bool isShort);
  uint8_t count(uint8_t side, const Rule& rule);
  void cue(uint8_t slot, const Rule& rule, uint8_t side);
  uint8_t dispense(uint8_t reason);   // 0 started, 1 pellet in well, 2 jammed
  void handlePokes();
  void handleWell();
  void logPoke(uint8_t side, uint32_t durationMs, bool held);

  // Commands
  void reply(const char* line) { hal_.send(line); }
  void status();
  void setTime(const char* arg, bool force);
  void stage(const char* line);
  void commit(const char* arg);
  void runBegin(const char* arg);
  void runEnd();
  void gotoPhase(const char* arg);
  void startRead(const char* arg);
  void pollInput();
  void showScreen();
};

// Exposed for the host tests.
bool parseIso(const char* text, int64_t& utc, int16_t& offsetMin);
void formatIso(uint32_t localSeconds, int16_t offsetMin, char* out);

}  // namespace fed
