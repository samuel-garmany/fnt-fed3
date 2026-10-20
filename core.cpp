// See core.h. The schedule half mirrors fnt/fed3/schedule.py; in the device
// half, each function names the device_sim.py method it follows.
#include "core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fed {

// ===========================================================================
// Schedules, rules and cues
// ===========================================================================

// ---------------------------------------------------------------------------
// Text helpers

static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool parseUint(const char* text, size_t len, uint32_t lo, uint32_t hi, uint32_t& out) {
  if (len == 0 || len > 10) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < len; i++) {
    if (!isDigit(text[i])) return false;
    value = value * 10 + (uint64_t)(text[i] - '0');
  }
  if (value < lo || value > hi) return false;
  out = (uint32_t)value;
  return true;
}

bool parseClock(const char* text, size_t len, uint16_t& minutes) {
  const char* colon = (const char*)memchr(text, ':', len);
  if (!colon) return false;
  size_t hlen = colon - text, mlen = len - hlen - 1;
  uint32_t h, m;
  if (hlen < 1 || hlen > 2 || mlen != 2) return false;
  if (!parseUint(text, hlen, 0, 23, h) || !parseUint(colon + 1, mlen, 0, 59, m)) return false;
  minutes = (uint16_t)(h * 60 + m);
  return true;
}

static bool eq(const char* a, size_t len, const char* b) {
  return strlen(b) == len && memcmp(a, b, len) == 0;
}

// Splits on a separator; returns the number of fields (at most max).
static int split(const char* text, size_t len, char sep, const char** starts, size_t* lens, int max) {
  int n = 0;
  size_t begin = 0;
  for (size_t i = 0; i <= len; i++) {
    if (i == len || text[i] == sep) {
      if (n == max) return max + 1;
      starts[n] = text + begin;
      lens[n] = i - begin;
      n++;
      begin = i + 1;
    }
  }
  return n;
}

// ---------------------------------------------------------------------------
// Cues

static const char* const TARGETS[] = {"bar", "poke_l", "poke_r", "pokes", "poke"};
// LEDs each target can light: bit0 bar, bit1 left poke, bit2 right poke.
static const uint8_t TARGET_LEDS[] = {1, 2, 4, 6, 6};

const char* parseCue(const char* text, Cue& out) {
  size_t len = strlen(text);
  out.n = 0;
  if (eq(text, len, "none")) return nullptr;
  const char* acts[MAX_CUE_ACTIONS];
  size_t actLens[MAX_CUE_ACTIONS];
  int n = split(text, len, '+', acts, actLens, MAX_CUE_ACTIONS);
  if (n > MAX_CUE_ACTIONS) return "a cue plays at most 3 actions";
  int sounds = 0, pulses = 0;
  uint8_t lit = 0;
  for (int i = 0; i < n; i++) {
    const char* parts[8];
    size_t plens[8];
    int np = split(acts[i], actLens[i], ':', parts, plens, 7);
    Action& a = out.act[i];
    memset(&a, 0, sizeof(a));
    uint32_t v[6];
    if (eq(parts[0], plens[0], "click")) {
      if (np != 1) return "click takes no parameters";
      a.kind = A_CLICK;
      sounds++;
    } else if (eq(parts[0], plens[0], "tone")) {
      if (np != 3) return "tone is tone:<hz>:<ms>";
      if (!parseUint(parts[1], plens[1], 20, 20000, v[0])) return "tone: hz must be between 20 and 20000";
      if (!parseUint(parts[2], plens[2], 1, 10000, v[1])) return "tone: ms must be between 1 and 10000";
      a.kind = A_TONE;
      a.a = v[0];
      a.b = v[1];
      sounds++;
    } else if (eq(parts[0], plens[0], "noise")) {
      if (np != 2) return "noise is noise:<ms>";
      if (!parseUint(parts[1], plens[1], 1, 10000, v[0])) return "noise: ms must be between 1 and 10000";
      a.kind = A_NOISE;
      a.b = v[0];
      sounds++;
    } else if (eq(parts[0], plens[0], "flash")) {
      if (np != 7) return "flash is flash:<target>:<r>:<g>:<b>:<w>:<ms>";
      int target = -1;
      for (int t = 0; t < 5; t++) {
        if (eq(parts[1], plens[1], TARGETS[t])) target = t;
      }
      if (target < 0) return "flash target must be one of: bar, poke_l, poke_r, pokes, poke";
      for (int c = 0; c < 4; c++) {
        if (!parseUint(parts[2 + c], plens[2 + c], 0, 255, v[c])) return "flash: colour must be between 0 and 255";
        a.rgbw[c] = (uint8_t)v[c];
      }
      if (!parseUint(parts[6], plens[6], 1, 60000, v[4])) return "flash: ms must be between 1 and 60000";
      if (lit & TARGET_LEDS[target]) return "two flashes light the same LEDs";
      lit |= TARGET_LEDS[target];
      a.kind = A_FLASH;
      a.target = (uint8_t)target;
      a.b = v[4];
    } else if (eq(parts[0], plens[0], "bnc")) {
      if (np != 4) return "bnc is bnc:<width_ms>:<hz>:<count>";
      if (!parseUint(parts[1], plens[1], 1, 1000, v[0])) return "bnc: width must be between 1 and 1000";
      if (!parseUint(parts[2], plens[2], 1, 500, v[1])) return "bnc: hz must be between 1 and 500";
      if (!parseUint(parts[3], plens[3], 1, 1000, v[2])) return "bnc: count must be between 1 and 1000";
      if (v[0] * v[1] >= 1000) return "bnc: the pulse width must be shorter than the period";
      a.kind = A_BNC;
      a.a = v[0];
      a.b = v[1];
      a.c = v[2];
      pulses++;
    } else {
      return "unknown cue action";
    }
  }
  if (sounds > 1) return "plays two sounds at once; the buzzer plays one at a time";
  if (pulses > 1) return "has two BNC pulse trains; there is one BNC output";
  out.n = (uint8_t)n;
  return nullptr;
}

void formatCue(const Cue& cue, char* out, size_t size) {
  if (cue.n == 0) {
    snprintf(out, size, "none");
    return;
  }
  size_t used = 0;
  out[0] = 0;
  for (int i = 0; i < cue.n && used < size; i++) {
    const Action& a = cue.act[i];
    const char* sep = i ? "+" : "";
    int w = 0;
    switch (a.kind) {
      case A_CLICK: w = snprintf(out + used, size - used, "%sclick", sep); break;
      case A_TONE: w = snprintf(out + used, size - used, "%stone:%u:%u", sep, a.a, a.b); break;
      case A_NOISE: w = snprintf(out + used, size - used, "%snoise:%u", sep, a.b); break;
      case A_FLASH:
        w = snprintf(out + used, size - used, "%sflash:%s:%u:%u:%u:%u:%u", sep, TARGETS[a.target],
                     a.rgbw[0], a.rgbw[1], a.rgbw[2], a.rgbw[3], a.b);
        break;
      case A_BNC: w = snprintf(out + used, size - used, "%sbnc:%u:%u:%u", sep, a.a, a.b, a.c); break;
    }
    if (w > 0) used += (size_t)w;
  }
}

// ---------------------------------------------------------------------------
// Rules

struct KeySpec {
  const char* name;
  const char* const* choices;   // enum values, or null for a number
  uint8_t nChoices;
  uint32_t lo, hi, def;
};

static const char* const ACTIVE_CHOICES[] = {"left", "right", "both", "none"};
static const char* const COUNT_CHOICES[] = {"shared", "per_port"};
static const char* const KIND_CHOICES[] = {"fixed", "random", "progressive"};
static const char* const REWARD_CHOICES[] = {"pellet", "none"};
static const char* const BOOL_CHOICES[] = {"0", "1"};

static const KeySpec KEYS[NUM_VALUES] = {
    {"active", ACTIVE_CHOICES, 4, 0, 0, ACTIVE_LEFT},
    {"count", COUNT_CHOICES, 2, 0, 0, COUNT_SHARED},
    {"ratio", nullptr, 0, 1, 999, 1},
    {"ratio_kind", KIND_CHOICES, 3, 0, 0, RK_FIXED},
    {"timeout", nullptr, 0, 0, 3600, 0},
    {"min_poke_ms", nullptr, 0, 0, 5000, 0},
    {"reward", REWARD_CHOICES, 2, 0, 0, REWARD_PELLET},
    {"quota", nullptr, 0, 0, 9999, 0},
    {"quota_period", nullptr, 0, 0, 7 * SECONDS_PER_DAY, 0},
    {"free_feed", BOOL_CHOICES, 2, 0, 0, 0},
    {"refill_delay", nullptr, 0, 0, 3600, 5},
};
static const char* const CUE_KEYS[NUM_CUES] = {"cue.reward", "cue.counted", "cue.inactive",
                                               "cue.timeout", "cue.depleted"};

void ruleDefaults(Rule& rule) {
  for (int k = 0; k < NUM_VALUES; k++) rule.v[k] = KEYS[k].def;
  for (int c = 0; c < NUM_CUES; c++) rule.cue[c].n = 0;
}

const char* keyName(int key) {
  return key < NUM_VALUES ? KEYS[key].name : CUE_KEYS[key - NUM_VALUES];
}

int keyIndex(const char* name, size_t len) {
  for (int k = 0; k < NUM_KEYS; k++) {
    if (eq(name, len, keyName(k))) return k;
  }
  return -1;
}

static char errBuf[96];

const char* setKey(Rule& rule, int key, const char* value, size_t len) {
  if (key >= NUM_VALUES) {
    char text[MAX_LINE + 1];
    if (len > MAX_LINE) return "cue too long";
    memcpy(text, value, len);
    text[len] = 0;
    const char* err = parseCue(text, rule.cue[key - NUM_VALUES]);
    if (err) {
      snprintf(errBuf, sizeof(errBuf), "%s: %s", keyName(key), err);
      return errBuf;
    }
    return nullptr;
  }
  const KeySpec& spec = KEYS[key];
  if (spec.choices) {
    for (int c = 0; c < spec.nChoices; c++) {
      if (eq(value, len, spec.choices[c])) {
        rule.v[key] = (uint32_t)c;
        return nullptr;
      }
    }
    snprintf(errBuf, sizeof(errBuf), "%s: not an allowed value", spec.name);
    return errBuf;
  }
  if (!parseUint(value, len, spec.lo, spec.hi, rule.v[key])) {
    snprintf(errBuf, sizeof(errBuf), "%s: must be between %lu and %lu", spec.name,
             (unsigned long)spec.lo, (unsigned long)spec.hi);
    return errBuf;
  }
  return nullptr;
}

void formatKey(const Rule& rule, int key, char* out, size_t size) {
  if (key >= NUM_VALUES) {
    formatCue(rule.cue[key - NUM_VALUES], out, size);
  } else if (KEYS[key].choices) {
    snprintf(out, size, "%s", KEYS[key].choices[rule.v[key]]);
  } else {
    snprintf(out, size, "%lu", (unsigned long)rule.v[key]);
  }
}

const char* parseSettings(const char* text, Rule& rule, uint32_t& mask, uint32_t refuseMask) {
  size_t len = strlen(text);
  size_t i = 0;
  bool any = false;
  uint32_t lineMask = 0;
  while (i < len) {
    while (i < len && isSpace(text[i])) i++;
    if (i >= len) break;
    size_t start = i;
    while (i < len && !isSpace(text[i])) i++;
    const char* token = text + start;
    size_t tlen = i - start;
    const char* eqs = (const char*)memchr(token, '=', tlen);
    if (!eqs) return "a setting is key=value";
    int key = keyIndex(token, eqs - token);
    if (key < 0) return "unknown key";
    const char* err = setKey(rule, key, eqs + 1, tlen - (eqs - token) - 1);
    if (err) return err;
    lineMask |= 1UL << key;
    any = true;
  }
  if (!any) return "no settings given";
  if (lineMask & refuseMask) return "a key is set twice";
  mask |= lineMask;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Until, windows, phases

const char* parseUntil(const char* text, Until& out) {
  memset(&out, 0, sizeof(out));
  size_t len = strlen(text);
  const char* keys[3];
  size_t klens[3];
  const char* vals[3];
  size_t vlens[3];
  int n = 0;
  size_t i = 0;
  while (i < len) {
    while (i < len && isSpace(text[i])) i++;
    if (i >= len) break;
    size_t start = i;
    while (i < len && !isSpace(text[i])) i++;
    const char* token = text + start;
    size_t tlen = i - start;
    if (n == 0 && eq(token, tlen, "goto")) {
      while (i < len && isSpace(text[i])) i++;
      if (i < len) return "bad condition";
      out.kind = U_GOTO;
      return nullptr;
    }
    const char* eqs = (const char*)memchr(token, '=', tlen);
    if (!eqs || n == 2) return "bad condition";
    keys[n] = token;
    klens[n] = eqs - token;
    vals[n] = eqs + 1;
    vlens[n] = tlen - klens[n] - 1;
    for (int j = 0; j < n; j++) {
      if (klens[j] == klens[n] && memcmp(keys[j], keys[n], klens[n]) == 0) return "bad condition";
    }
    n++;
  }
  if (n == 0) return "UNTIL needs a condition";
  if (n == 1 && eq(keys[0], klens[0], "after")) {
    out.kind = U_AFTER;
    if (!parseUint(vals[0], vlens[0], 1, 366 * SECONDS_PER_DAY, out.value)) return "after: out of range";
    return nullptr;
  }
  if (n == 1 && eq(keys[0], klens[0], "rewards")) {
    out.kind = U_REWARDS;
    if (!parseUint(vals[0], vlens[0], 1, 99999, out.value)) return "rewards: out of range";
    return nullptr;
  }
  int at = -1, day = -1, pokes = -1, side = -1;
  for (int j = 0; j < n; j++) {
    if (eq(keys[j], klens[j], "at")) at = j;
    else if (eq(keys[j], klens[j], "day")) day = j;
    else if (eq(keys[j], klens[j], "pokes")) pokes = j;
    else if (eq(keys[j], klens[j], "side")) side = j;
    else return "bad condition";
  }
  if (at >= 0 && pokes < 0 && side < 0 && (n == 1 || day >= 0)) {
    out.kind = U_AT;
    if (!parseClock(vals[at], vlens[at], out.clock)) return "at: not a clock time";
    if (day >= 0) {
      uint32_t d;
      if (!parseUint(vals[day], vlens[day], 1, 366, d)) return "day: out of range";
      out.day = (uint16_t)d;
    }
    return nullptr;
  }
  if (n == 2 && pokes >= 0 && side >= 0) {
    out.kind = U_POKES;
    if (!parseUint(vals[pokes], vlens[pokes], 1, 999999, out.value)) return "pokes: out of range";
    if (eq(vals[side], vlens[side], "left")) out.side = PS_LEFT;
    else if (eq(vals[side], vlens[side], "right")) out.side = PS_RIGHT;
    else if (eq(vals[side], vlens[side], "any")) out.side = PS_ANY;
    else return "side must be left, right or any";
    return nullptr;
  }
  return "bad condition";
}

bool isTimeUntil(const Until& until) { return until.kind == U_AFTER || until.kind == U_AT; }

const char* phaseNameProblem(const char* name, size_t len) {
  if (len == 0 || isSpace(name[0]) || isSpace(name[len - 1])) return "must not be empty or start or end with a space";
  if (memchr(name, ',', len)) return "must not contain a comma";
  if (len > (size_t)MAX_NAME) return "is too long";
  for (size_t i = 0; i < len; i++) {
    if (name[i] < ' ' || name[i] > '~') return "must be plain ASCII";
  }
  return nullptr;
}

bool windowsOverlap(uint16_t s1, uint16_t e1, uint16_t s2, uint16_t e2) {
  uint16_t a[2][2], b[2][2];
  int na = 0, nb = 0;
  if (s1 < e1) { a[na][0] = s1; a[na++][1] = e1; }
  else { a[na][0] = s1; a[na++][1] = 1440; a[na][0] = 0; a[na++][1] = e1; }
  if (s2 < e2) { b[nb][0] = s2; b[nb++][1] = e2; }
  else { b[nb][0] = s2; b[nb++][1] = 1440; b[nb][0] = 0; b[nb++][1] = e2; }
  for (int i = 0; i < na; i++) {
    for (int j = 0; j < nb; j++) {
      if (a[i][0] < b[j][1] && b[j][0] < a[i][1]) return true;
    }
  }
  return false;
}

bool windowActive(const Window& w, uint32_t localSeconds) {
  uint16_t minute = (uint16_t)((localSeconds % SECONDS_PER_DAY) / 60);
  if (w.start < w.end) return w.start <= minute && minute < w.end;
  return minute >= w.start || minute < w.end;
}

void effectiveRule(const Phase& phase, uint32_t localSeconds, bool clockOk, Rule& out) {
  out = phase.rule;
  if (!clockOk) return;
  for (int i = 0; i < phase.nWin; i++) {
    const Window& w = phase.win[i];
    if (!windowActive(w, localSeconds)) continue;
    for (int k = 0; k < NUM_KEYS; k++) {
      if (!(w.mask & (1UL << k))) continue;
      if (k < NUM_VALUES) out.v[k] = w.rule.v[k];
      else out.cue[k - NUM_VALUES] = w.rule.cue[k - NUM_VALUES];
    }
  }
}

bool dueTime(const Until& until, uint32_t phaseStart, uint32_t t0, uint32_t& due) {
  if (until.kind == U_AFTER) {
    due = phaseStart + until.value;
    return true;
  }
  if (until.kind != U_AT) return false;
  if (until.day) {
    due = t0 - t0 % SECONDS_PER_DAY + (uint32_t)(until.day - 1) * SECONDS_PER_DAY + until.clock * 60UL;
    if (due < phaseStart) due = phaseStart;
    return true;
  }
  due = phaseStart - phaseStart % SECONDS_PER_DAY + until.clock * 60UL;
  if (due <= phaseStart) due += SECONDS_PER_DAY;
  return true;
}

uint32_t progressiveRequirement(uint32_t rewards) {
  if (rewards > 60) return 1000000;     // past any real breakpoint; avoids overflow
  double value = 5.0 * exp(0.2 * (double)(rewards + 1)) - 5.0;
  return (uint32_t)floor(value + 0.5);
}

// ---------------------------------------------------------------------------
// CRC-32, as zlib.crc32

uint32_t crc32Update(uint32_t crc, const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1)));
  }
  return ~crc;
}

// ---------------------------------------------------------------------------
// Schedule lines

static void splitDirective(const char* line, size_t len, const char*& dir, size_t& dlen, const char*& rest) {
  const char* space = (const char*)memchr(line, ' ', len);
  dir = line;
  dlen = space ? (size_t)(space - line) : len;
  rest = space ? space + 1 : line + len;
}

static const char* parseWindowHead(const char* rest, uint16_t& start, uint16_t& end, const char*& settings) {
  const char* space = strchr(rest, ' ');
  size_t spanLen = space ? (size_t)(space - rest) : strlen(rest);
  settings = space ? space + 1 : rest + spanLen;
  const char* dash = (const char*)memchr(rest, '-', spanLen);
  if (!dash) return "WINDOW is WINDOW HH:MM-HH:MM key=value ...";
  if (!parseClock(rest, dash - rest, start) || !parseClock(dash + 1, spanLen - (dash - rest) - 1, end))
    return "not a clock time (HH:MM, 00:00-23:59)";
  if (start == end) return "a window must not start and end at the same time";
  return nullptr;
}

void ScheduleBuilder::reset() {
  named_ = false;
  phases_ = lines_ = 0;
  crc_ = size_ = seenKeys_ = 0;
  nUntil_ = nWin_ = 0;
}

const char* ScheduleBuilder::add(const char* line, size_t len) {
  if (len > (size_t)MAX_LINE) return "line too long";
  size_ += len + 1;
  if (size_ > MAX_SCHED_BYTES) return "schedule larger than 65536 bytes";
  char text[MAX_LINE + 1];
  memcpy(text, line, len);
  text[len] = 0;
  const char* dir;
  size_t dlen;
  const char* rest;
  splitDirective(text, len, dir, dlen, rest);
  if (!named_) {
    if (!eq(dir, dlen, "NAME") || !*rest) return "a schedule starts with NAME";
    named_ = true;
  } else if (eq(dir, dlen, "PHASE")) {
    const char* problem = phaseNameProblem(rest, strlen(rest));
    if (problem) {
      snprintf(errBuf, sizeof(errBuf), "phase name %s", problem);
      return errBuf;
    }
    if (phases_ == MAX_PHASES) return "more than 256 phases";
    phases_++;
    seenKeys_ = 0;
    nUntil_ = nWin_ = 0;
  } else if (phases_ == 0) {
    return "directive before the first PHASE";
  } else if (eq(dir, dlen, "UNTIL")) {
    if (nUntil_ == MAX_UNTILS) return "more than 6 end conditions";
    Until until;
    const char* err = parseUntil(rest, until);
    if (err) return err;
    nUntil_++;
  } else if (eq(dir, dlen, "SET")) {
    Rule scratch;
    ruleDefaults(scratch);
    const char* err = parseSettings(rest, scratch, seenKeys_, seenKeys_);
    if (err) return err;
  } else if (eq(dir, dlen, "WINDOW")) {
    uint16_t start, end;
    const char* settings;
    const char* err = parseWindowHead(rest, start, end, settings);
    if (err) return err;
    Rule scratch;
    ruleDefaults(scratch);
    uint32_t mask = 0;
    err = parseSettings(settings, scratch, mask, 0);
    if (err) return err;
    bool existing = false;
    for (int i = 0; i < nWin_; i++) {
      if (winStart_[i] == start && winEnd_[i] == end) existing = true;
    }
    if (!existing) {
      for (int i = 0; i < nWin_; i++) {
        if (windowsOverlap(start, end, winStart_[i], winEnd_[i])) return "windows overlap";
      }
      if (nWin_ == MAX_WINDOWS) return "more than 8 windows";
      winStart_[nWin_] = start;
      winEnd_[nWin_] = end;
      nWin_++;
    }
  } else {
    return "unknown directive";
  }
  crc_ = crc32Update(crc_, line, len);
  crc_ = crc32Update(crc_, "\n", 1);
  lines_++;
  return nullptr;
}

const char* ScheduleBuilder::finish() const {
  if (!named_) return "empty schedule";
  if (phases_ == 0) return "a schedule needs at least one phase";
  return nullptr;
}

PhaseLoader::PhaseLoader(uint16_t index, Phase& out) : index_(index), out_(out) {}

const char* PhaseLoader::line(const char* text, size_t len) {
  if (len > (size_t)MAX_LINE) return "line too long";
  char buf[MAX_LINE + 1];
  memcpy(buf, text, len);
  buf[len] = 0;
  const char* dir;
  size_t dlen;
  const char* rest;
  splitDirective(buf, len, dir, dlen, rest);
  if (eq(dir, dlen, "NAME")) return nullptr;
  if (eq(dir, dlen, "PHASE")) {
    loading_ = count_ == index_;
    count_++;
    if (loading_) {
      memset(&out_, 0, sizeof(out_));
      size_t n = strlen(rest);
      if (n > (size_t)MAX_NAME) n = MAX_NAME;
      memcpy(out_.name, rest, n);
      out_.name[n] = 0;
      ruleDefaults(out_.rule);
    }
    return nullptr;
  }
  if (!loading_) return nullptr;
  if (eq(dir, dlen, "UNTIL")) {
    if (out_.nUntil == MAX_UNTILS) return "too many end conditions";
    return parseUntil(rest, out_.untils[out_.nUntil++]);
  }
  if (eq(dir, dlen, "SET")) {
    uint32_t mask = 0;
    return parseSettings(rest, out_.rule, mask, 0);
  }
  if (eq(dir, dlen, "WINDOW")) {
    uint16_t start, end;
    const char* settings;
    const char* err = parseWindowHead(rest, start, end, settings);
    if (err) return err;
    Window* w = nullptr;
    for (int i = 0; i < out_.nWin; i++) {
      if (out_.win[i].start == start && out_.win[i].end == end) w = &out_.win[i];
    }
    if (!w) {
      if (out_.nWin == MAX_WINDOWS) return "too many windows";
      w = &out_.win[out_.nWin++];
      w->start = start;
      w->end = end;
      w->mask = 0;
      ruleDefaults(w->rule);
    }
    return parseSettings(settings, w->rule, w->mask, 0);
  }
  return "unknown directive";
}

// ===========================================================================
// The device
// ===========================================================================

static const char HEADER[] =
    "time,ms,seq,phase,event,side,outcome,left,right,pellets,"
    "phase_rewards,phase_left,phase_right,retrieval_ms,poke_ms,detail";
static const char* const OUTCOMES[] = {"reward", "counted", "inactive", "timeout",
                                       "short", "pellet_in_well", "depleted"};
static const char* const REASONS[] = {"", "free", "manual", "retry"};
static const char* const RESETS[] = {"power", "watchdog", "software", "external"};
static const char* const DISPENSE_RESULTS[] = {"started", "pellet_in_well", "jam"};
static const int ROW_COLUMNS = 16;
static const char IDLE_FILE[] = "IDLE.CSV";

// ---------------------------------------------------------------------------
// Calendar arithmetic (Howard Hinnant's civil-from-days)

static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = (int)(y - era * 400);
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static void civilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int doe = (int)(z - era * 146097);
  const int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = (int)(yoe + era * 400);
  const int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
}

void formatIso(uint32_t localSeconds, int16_t offsetMin, char* out) {
  int y, m, d;
  civilFromDays(localSeconds / 86400, y, m, d);
  uint32_t s = localSeconds % 86400;
  int off = offsetMin < 0 ? -offsetMin : offsetMin;
  sprintf(out, "%04d-%02d-%02dT%02lu:%02lu:%02lu%c%02d:%02d", y, m, d, (unsigned long)(s / 3600),
          (unsigned long)(s / 60 % 60), (unsigned long)(s % 60), offsetMin < 0 ? '-' : '+', off / 60,
          off % 60);
}

static bool digits(const char* p, int n, int& out) {
  out = 0;
  for (int i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') return false;
    out = out * 10 + (p[i] - '0');
  }
  return true;
}

// YYYY-MM-DDTHH:MM:SS followed by Z or +HH:MM / -HH:MM, and nothing else.
bool parseIso(const char* t, int64_t& utc, int16_t& offsetMin) {
  size_t len = strlen(t);
  if (len != 20 && len != 25) return false;
  int y, mo, d, h, mi, s;
  if (!digits(t, 4, y) || t[4] != '-' || !digits(t + 5, 2, mo) || t[7] != '-' ||
      !digits(t + 8, 2, d) || t[10] != 'T' || !digits(t + 11, 2, h) || t[13] != ':' ||
      !digits(t + 14, 2, mi) || t[16] != ':' || !digits(t + 17, 2, s))
    return false;
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) return false;
  int off = 0;
  if (len == 20) {
    if (t[19] != 'Z') return false;
  } else {
    int oh, om;
    if ((t[19] != '+' && t[19] != '-') || !digits(t + 20, 2, oh) || t[22] != ':' ||
        !digits(t + 23, 2, om) || oh > 23 || om > 59)
      return false;
    off = (oh * 60 + om) * (t[19] == '-' ? -1 : 1);
  }
  int64_t localSecs = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
  utc = localSecs - (int64_t)off * 60;
  offsetMin = (int16_t)off;
  return true;
}

// ---------------------------------------------------------------------------
// Small parsing helpers

static const char* field(const char* text, const char* key, char* out, size_t size) {
  // key=value lines; returns out or nullptr.
  size_t klen = strlen(key);
  const char* p = text;
  while (p && *p) {
    if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
      const char* v = p + klen + 1;
      size_t n = strcspn(v, "\n");
      if (n >= size) n = size - 1;
      memcpy(out, v, n);
      out[n] = 0;
      return out;
    }
    p = strchr(p, '\n');
    if (p) p++;
  }
  return nullptr;
}

static uint32_t toUint(const char* s) { return (uint32_t)strtoul(s, nullptr, 10); }

// "%.Nf" without printf's float support, which the SAMD core leaves out.
static const char* fixed(char* out, float value, int decimals) {
  if (value != value) {
    strcpy(out, "nan");
    return out;
  }
  long scale = decimals == 2 ? 100 : decimals == 1 ? 10 : 1;
  bool negative = value < 0;
  long whole = (long)((negative ? -value : value) * scale + 0.5f);
  if (decimals == 0) sprintf(out, "%s%ld", negative ? "-" : "", whole);
  else sprintf(out, "%s%ld.%0*ld", negative ? "-" : "", whole / scale, decimals, whole % scale);
  return out;
}

// Splits a CSV row into its first ROW_COLUMNS - 1 fields plus the rest.
static int splitRow(char* line, char** cols) {
  int n = 0;
  cols[n++] = line;
  for (char* p = line; *p && n < ROW_COLUMNS; p++) {
    if (*p == ',') {
      *p = 0;
      cols[n++] = p + 1;
    }
  }
  return n;
}

// ---------------------------------------------------------------------------

Device::Device(Hal& hal) : hal_(hal) {
  memset(hold_, 0, sizeof(hold_));
  memset(&read_, 0, sizeof(read_));
  ruleDefaults(phase_.rule);
}

bool Device::clockOk() { return hal_.rtcValid(); }
uint32_t Device::local() { return hal_.rtcNow(); }
void Device::iso(uint32_t localSeconds, char* out) { formatIso(localSeconds, offsetMin_, out); }

// --- card ------------------------------------------------------------------

static void newName(const char* name, char* out) {
  // SCHED.TXT -> SCHED.NEW
  strcpy(out, name);
  char* dot = strrchr(out, '.');
  if (dot) strcpy(dot, ".NEW");
}

bool Device::writeSmall(const char* name, const char* text) {
  char tmp[16];
  newName(name, tmp);
  if (!hal_.fileWrite(tmp, text, strlen(text))) return false;
  hal_.fileRemove(name);
  return hal_.fileRename(tmp, name);
}

bool Device::promote(const char* from, const char* name) {
  char tmp[16];
  newName(name, tmp);
  hal_.fileRemove(tmp);
  if (!hal_.fileRename(from, tmp)) return false;
  hal_.fileRemove(name);
  return hal_.fileRename(tmp, name);
}

// A .NEW file left by a power cut: complete if the original is gone (the
// original is only removed once the .NEW is fully written), else abandoned.
void Device::recover(const char* name) {
  char tmp[16];
  newName(name, tmp);
  if (hal_.fileSize(tmp) < 0) return;
  if (hal_.fileSize(name) < 0) hal_.fileRename(tmp, name);
  else hal_.fileRemove(tmp);
}

static int readSmall(Hal& hal, const char* name, char* buf, uint32_t size) {
  int32_t n = hal.fileRead(name, 0, buf, size - 1);
  if (n < 0) return -1;
  buf[n] = 0;
  return n;
}

void Device::loadDevice() {
  char text[96], value[24];
  id_ = 1;
  offsetMin_ = 0;
  if (readSmall(hal_, "DEVICE.TXT", text, sizeof(text)) < 0) return;
  if (field(text, "id", value, sizeof(value))) id_ = (uint16_t)toUint(value);
  if (field(text, "offset", value, sizeof(value)) && strlen(value) == 6) {
    int h = atoi(value + 1), m = atoi(value + 4);
    offsetMin_ = (int16_t)((h * 60 + m) * (value[0] == '-' ? -1 : 1));
  }
}

void Device::saveDevice() {
  char text[64];
  int off = offsetMin_ < 0 ? -offsetMin_ : offsetMin_;
  sprintf(text, "id=%u\noffset=%c%02d:%02d\n", id_, offsetMin_ < 0 ? '-' : '+', off / 60, off % 60);
  writeSmall("DEVICE.TXT", text);
}

void Device::loadRun() {
  char text[256], value[40];
  strcpy(runId_, "-");
  strcpy(file_, IDLE_FILE);
  ended_ = true;
  phaseIndex_ = 0;
  havePhaseStart_ = false;
  t0_ = 0;
  if (readSmall(hal_, "RUN.TXT", text, sizeof(text)) < 0) return;
  if (field(text, "run", value, sizeof(value))) snprintf(runId_, sizeof(runId_), "%.8s", value);
  if (field(text, "file", value, sizeof(value))) snprintf(file_, sizeof(file_), "%.12s", value);
  if (field(text, "ended", value, sizeof(value))) ended_ = strcmp(value, "1") == 0;
  if (field(text, "phase", value, sizeof(value))) phaseIndex_ = (uint16_t)toUint(value);
  int64_t utc;
  int16_t off;
  if (field(text, "t0", value, sizeof(value)) && parseIso(value, utc, off)) {
    t0_ = (uint32_t)(utc + offsetMin_ * 60);
    phaseStart_ = t0_;
    havePhaseStart_ = true;
  }
  if (field(text, "phase_start", value, sizeof(value)) && parseIso(value, utc, off)) {
    phaseStart_ = (uint32_t)(utc + offsetMin_ * 60);
    havePhaseStart_ = true;
  }
}

void Device::saveRun() {
  char text[256], t0[32], start[32];
  t0[0] = start[0] = 0;
  if (runId_[0] != '-') iso(t0_, t0);
  if (havePhaseStart_) iso(phaseStart_, start);
  snprintf(text, sizeof(text), "run=%s\nt0=%s\nfile=%s\nended=%d\nphase=%u\nphase_start=%s\n",
           runId_, t0, file_, ended_ ? 1 : 0, phaseIndex_, start);
  if (!writeSmall("RUN.TXT", text)) sdError_ = true;
}

// Calls fn(line, len) for each line of a file; stops early if fn returns false.
template <typename Fn>
static bool eachLine(Hal& hal, const char* name, Fn fn) {
  int32_t size = hal.fileSize(name);
  if (size < 0) return false;
  char buf[160];
  char line[MAX_COMMAND + 2];
  size_t used = 0;
  bool overflow = false;
  uint32_t offset = 0;
  while (offset < (uint32_t)size) {
    int32_t n = hal.fileRead(name, offset, buf, sizeof(buf));
    if (n <= 0) return false;
    offset += (uint32_t)n;
    for (int32_t i = 0; i < n; i++) {
      if (buf[i] == '\n') {
        line[used] = 0;
        if (overflow || !fn(line, used)) return !overflow && false;
        used = 0;
      } else if (used < MAX_COMMAND) {
        line[used++] = buf[i];
      } else {
        overflow = true;
      }
    }
    hal.feedWatchdog();
  }
  return true;
}

bool Device::validateSchedule(const char* name, uint32_t& crc, uint16_t& phases) {
  ScheduleBuilder builder;
  builder.reset();
  bool ok = true;
  bool read = eachLine(hal_, name, [&](const char* line, size_t len) {
    if (builder.add(line, len)) ok = false;
    return ok;
  });
  if (!read || !ok || builder.finish()) return false;
  crc = builder.crc();
  phases = builder.phases();
  return true;
}

bool Device::loadPhase(uint16_t index, Phase& out) {
  if (schedDefault_) {
    memset(&out, 0, sizeof(out));
    strcpy(out.name, "Default");
    ruleDefaults(out.rule);
    return index == 0;
  }
  PhaseLoader loader(index, out);
  bool ok = true;
  bool read = eachLine(hal_, "SCHED.TXT", [&](const char* line, size_t len) {
    if (loader.line(line, len)) ok = false;
    return ok;
  });
  return read && ok && loader.found();
}

void Device::loadSchedule() {
  uint16_t phases;
  schedDefault_ = !validateSchedule("SCHED.TXT", schedCrc_, phases);
  nPhases_ = schedDefault_ ? 1 : phases;
  uint16_t nextPhases;
  nextPresent_ = validateSchedule("NEXT.TXT", nextCrc_, nextPhases);
}

// The line ending just before `end` (an offset just past a newline).
static bool lineBefore(Hal& hal, const char* name, uint32_t end, char* out, uint32_t& start) {
  if (end == 0) return false;
  uint32_t from = end > 320 ? end - 320 : 0;
  char buf[321];
  int32_t n = hal.fileRead(name, from, buf, end - from);
  if (n != (int32_t)(end - from)) return false;
  int32_t stop = n - 1;              // the newline ending this line
  if (buf[stop] != '\n') stop = n;   // last line without a newline
  int32_t i = stop - 1;
  while (i >= 0 && buf[i] != '\n') i--;
  if (i < 0 && from > 0) return false;  // longer than any row
  start = from + (uint32_t)(i + 1);
  memcpy(out, buf + i + 1, stop - i - 1);
  out[stop - i - 1] = 0;
  return true;
}

// device_sim.SimDevice._restore_counts
void Device::restoreCounts() {
  seq_ = left_ = right_ = pellets_ = 0;
  phaseRewards_ = phaseLeft_ = phaseRight_ = 0;
  quotaUsed_ = 0;
  haveQuotaPeriod_ = false;
  int32_t size = hal_.fileSize(file_);
  if (size <= 0) {
    char header[sizeof(HEADER) + 1];
    sprintf(header, "%s\n", HEADER);
    if (!hal_.fileWrite(file_, header, strlen(header))) sdError_ = true;
    return;
  }
  char line[MAX_COMMAND * 3];
  uint32_t start;
  if (!lineBefore(hal_, file_, (uint32_t)size, line, start) || start == 0) return;
  char* cols[ROW_COLUMNS];
  if (splitRow(line, cols) < ROW_COLUMNS) return;
  seq_ = toUint(cols[2]);
  left_ = toUint(cols[7]);
  right_ = toUint(cols[8]);
  pellets_ = toUint(cols[9]);
  if (toUint(cols[3]) == phaseIndex_) {
    phaseRewards_ = toUint(cols[10]);
    phaseLeft_ = toUint(cols[11]);
    phaseRight_ = toUint(cols[12]);
  }
  quotaUsed_ = phaseRewards_;
  uint32_t period = phase_.rule.v[K_QUOTA_PERIOD];
  if (!period || !clockOk() || !havePhaseStart_) return;
  // Rewards in the current quota period: scan back to its start, so the scan
  // is bounded by the period rather than the length of the run.
  int64_t elapsed = (int64_t)local() - phaseStart_;
  int64_t index = elapsed >= 0 ? elapsed / period : -((-elapsed + period - 1) / period);
  int64_t periodStart = phaseStart_ + index * (int64_t)period;
  uint32_t used = 0;
  uint32_t end = (uint32_t)size;
  while (lineBefore(hal_, file_, end, line, start) && start > 0) {
    end = start;
    if (splitRow(line, cols) < ROW_COLUMNS) break;
    int64_t utc;
    int16_t off;
    if (!parseIso(cols[0], utc, off)) continue;
    int64_t when = utc + offsetMin_ * 60;
    if (when < periodStart || toUint(cols[3]) != phaseIndex_) break;
    if (strcmp(cols[4], "poke") == 0 && strcmp(cols[6], "reward") == 0) used++;
    hal_.feedWatchdog();
  }
  quotaUsed_ = used;
  quotaPeriod_ = (int32_t)index;
  haveQuotaPeriod_ = true;
}

// device_sim.SimDevice._row
void Device::row(const char* event, const char* side, const char* outcome, int32_t phase,
                 const char* retrievalMs, const char* pokeMs, const char* detail,
                 uint32_t atLocal, uint32_t atMs, bool atClockOk) {
  seq_++;
  char time[32];
  time[0] = 0;
  if (atClockOk) iso(atLocal, time);
  char line[MAX_COMMAND * 3];
  int n = snprintf(line, sizeof(line), "ROW,%s,%lu,%lu,%ld,%s,%s,%s,%lu,%lu,%lu,%lu,%lu,%lu,%s,%s,%s",
                   time, (unsigned long)atMs, (unsigned long)seq_,
                   (long)(phase < 0 ? phaseIndex_ : phase), event, side, outcome,
                   (unsigned long)left_, (unsigned long)right_, (unsigned long)pellets_,
                   (unsigned long)phaseRewards_, (unsigned long)phaseLeft_,
                   (unsigned long)phaseRight_, retrievalMs, pokeMs, detail);
  if (n < 0 || n >= (int)sizeof(line) - 1) n = sizeof(line) - 2;
  line[n] = '\n';
  if (hal_.fileAppend(file_, line + 4, n - 3)) {
    line[n] = 0;
    hal_.send(line);
    sdError_ = false;
  } else {
    sdError_ = true;
    hal_.send("ERR,SD,append failed");
  }
}

void Device::simpleRow(const char* event, const char* detail) {
  row(event, "", "", -1, "", "", detail, local(), hal_.millis(), clockOk());
}

// --- boot ------------------------------------------------------------------

// device_sim.SimDevice.boot
void Device::boot(uint8_t cause) {
  lights_ = false;
  hal_.lights(false);
  memset(hold_, 0, sizeof(hold_));
  memset(&read_, 0, sizeof(read_));
  staging_ = stagingFailed_ = false;
  well_ = hal_.wellFull() ? WELL_FULL : WELL_EMPTY;
  pelletMs_ = hal_.millis();
  haveRetrieve_ = false;
  jammed_ = false;
  timeoutActive_ = false;
  counters_[0] = counters_[1] = counters_[2] = 0;
  nextStatusMs_ = hal_.millis() + STATUS_EVERY_MS;
  inputLen_ = 0;
  inputOverflow_ = false;
  sdError_ = !hal_.sdOk();

  const char* const managed[] = {"DEVICE.TXT", "RUN.TXT", "SCHED.TXT", "NEXT.TXT"};
  for (const char* name : managed) recover(name);
  loadDevice();
  loadSchedule();
  loadRun();
  if (phaseIndex_ >= nPhases_) phaseIndex_ = nPhases_ - 1;
  if (!loadPhase(phaseIndex_, phase_)) {
    schedDefault_ = true;
    nPhases_ = 1;
    phaseIndex_ = 0;
    loadPhase(0, phase_);
  }
  restoreCounts();

  char line[96];
  char sched[12];
  if (schedDefault_) strcpy(sched, "default");
  else sprintf(sched, "%08lx", (unsigned long)schedCrc_);
  snprintf(line, sizeof(line), "BOOT,fw=%s,reset=%s,clock=%s,sched=%s,run=%s", FIRMWARE,
           RESETS[cause < 4 ? cause : 0], clockOk() ? "ok" : "unset", sched,
           activeRun() ? runId_ : "-");
  hal_.send(line);
  snprintf(line, sizeof(line), "reset=%s;ratio counters reset", RESETS[cause < 4 ? cause : 0]);
  simpleRow("boot", line);
  tick();
}

// --- rules -----------------------------------------------------------------

void Device::currentRule(Rule& out) { effectiveRule(phase_, local(), clockOk(), out); }

// device_sim.SimDevice._depleted
bool Device::depleted(const Rule& rule) {
  uint32_t quota = rule.v[K_QUOTA];
  if (!quota) return false;
  uint32_t period = rule.v[K_QUOTA_PERIOD];
  if (period && clockOk() && havePhaseStart_) {
    int64_t elapsed = (int64_t)local() - phaseStart_;
    int32_t index = (int32_t)(elapsed >= 0 ? elapsed / period : -((-elapsed + period - 1) / period));
    if (!haveQuotaPeriod_ || index != quotaPeriod_) {
      quotaPeriod_ = index;
      haveQuotaPeriod_ = true;
      quotaUsed_ = 0;
    }
  }
  return quotaUsed_ >= quota;
}

// device_sim.SimDevice._count
uint8_t Device::count(uint8_t side, const Rule& rule) {
  if (rule.v[K_RATIO_KIND] == RK_RANDOM) {
    return hal_.random32() % rule.v[K_RATIO] == 0 ? O_REWARD : O_COUNTED;
  }
  int key = (rule.v[K_ACTIVE] == ACTIVE_BOTH && rule.v[K_COUNT] == COUNT_PER_PORT) ? side : 2;
  counters_[key]++;
  uint32_t need = rule.v[K_RATIO_KIND] == RK_PROGRESSIVE ? progressiveRequirement(phaseRewards_)
                                                          : rule.v[K_RATIO];
  if (counters_[key] >= need) {
    counters_[key] = 0;
    return O_REWARD;
  }
  return O_COUNTED;
}

void Device::cue(uint8_t slot, const Rule& rule, uint8_t side) {
  if (rule.cue[slot].n) hal_.playCue(slot, rule.cue[slot], side);
}

// device_sim.SimDevice._decide
uint8_t Device::decide(uint8_t side, bool isShort) {
  Rule rule;
  currentRule(rule);
  if (side == 0) {
    left_++;
    phaseLeft_++;
  } else {
    right_++;
    phaseRight_++;
  }
  uint8_t active = (uint8_t)rule.v[K_ACTIVE];
  uint8_t outcome;
  if (isShort) {
    outcome = O_SHORT;
  } else if (well_ != WELL_EMPTY) {
    outcome = O_PELLET_IN_WELL;
  } else if (active == ACTIVE_NONE || (active != ACTIVE_BOTH && active != side)) {
    outcome = O_INACTIVE;
  } else if (timeoutActive_ && (int32_t)(hal_.millis() - timeoutUntilMs_) < 0) {
    outcome = O_TIMEOUT;
  } else if (depleted(rule)) {
    outcome = O_DEPLETED;
  } else {
    outcome = count(side, rule);
  }
  if (outcome == O_REWARD) {
    phaseRewards_++;
    quotaUsed_++;
    timeoutActive_ = rule.v[K_TIMEOUT] > 0;
    timeoutUntilMs_ = hal_.millis() + rule.v[K_TIMEOUT] * 1000UL;
    cue(CUE_REWARD, rule, side);
    if (rule.v[K_REWARD] == REWARD_PELLET) dispense(R_REWARD);
  } else if (outcome == O_COUNTED) {
    cue(CUE_COUNTED, rule, side);
  } else if (outcome == O_INACTIVE) {
    cue(CUE_INACTIVE, rule, side);
  } else if (outcome == O_TIMEOUT) {
    cue(CUE_TIMEOUT, rule, side);
  } else if (outcome == O_DEPLETED) {
    cue(CUE_DEPLETED, rule, side);
  }
  hold_[side].outcome = outcome;
  hold_[side].phase = phaseIndex_;
  hold_[side].decided = true;
  checkCounts();
  return outcome;
}

// device_sim.SimDevice._dispense
uint8_t Device::dispense(uint8_t reason) {
  if (well_ != WELL_EMPTY) return 1;
  if (jammed_) return 2;
  well_ = WELL_DISPENSING;
  reason_ = reason;
  hal_.startDispense();
  return 0;
}

// --- phases ----------------------------------------------------------------

// device_sim.SimDevice._enter_phase
// One spare phase for loading into; two phases don't both fit on the stack.
static Phase scratch;

void Device::enterPhase(uint16_t index, uint32_t start, const char* reason) {
  Phase& next = scratch;
  if (!loadPhase(index, next)) {
    sdError_ = true;   // can't read the next phase: stay in this one
    return;
  }
  phase_ = next;
  phaseIndex_ = index;
  phaseStart_ = start;
  havePhaseStart_ = true;
  phaseRewards_ = phaseLeft_ = phaseRight_ = 0;
  quotaUsed_ = 0;
  haveQuotaPeriod_ = false;
  counters_[0] = counters_[1] = counters_[2] = 0;
  timeoutActive_ = false;
  saveRun();
  char detail[96];
  if (reason && strcmp(reason, "catch-up") == 0) {
    char when[32];
    iso(start, when);
    snprintf(detail, sizeof(detail), "%s;%s;start=%s", phase_.name, reason, when);
  } else if (reason && *reason) {
    snprintf(detail, sizeof(detail), "%s;%s", phase_.name, reason);
  } else {
    snprintf(detail, sizeof(detail), "%s", phase_.name);
  }
  simpleRow("phase", detail);
}

bool Device::nextDue(uint32_t& due) {
  bool any = false;
  for (int i = 0; i < phase_.nUntil; i++) {
    uint32_t when;
    if (dueTime(phase_.untils[i], phaseStart_, t0_, when) && (!any || when < due)) {
      due = when;
      any = true;
    }
  }
  return any;
}

bool Device::canAdvance() { return activeRun() && clockOk() && phaseIndex_ + 1 < nPhases_; }

// device_sim.SimDevice._check_time
void Device::checkTime() {
  while (canAdvance()) {
    uint32_t due;
    if (!nextDue(due) || due > local()) return;
    uint16_t before = phaseIndex_;
    bool late = local() - due > CATCH_UP_S;
    enterPhase(phaseIndex_ + 1, due, late ? "catch-up" : "");
    if (phaseIndex_ == before) return;   // couldn't load it
    hal_.feedWatchdog();
  }
}

// device_sim.SimDevice._check_counts
void Device::checkCounts() {
  if (!activeRun() || phaseIndex_ + 1 >= nPhases_) return;
  for (int i = 0; i < phase_.nUntil; i++) {
    const Until& u = phase_.untils[i];
    bool met = false;
    if (u.kind == U_REWARDS) {
      met = phaseRewards_ >= u.value;
    } else if (u.kind == U_POKES) {
      uint32_t n = u.side == PS_LEFT ? phaseLeft_ : u.side == PS_RIGHT ? phaseRight_
                                                                       : phaseLeft_ + phaseRight_;
      met = n >= u.value;
    }
    if (met) {
      enterPhase(phaseIndex_ + 1, local(), "");
      return;
    }
  }
}

// --- the loop --------------------------------------------------------------

void Device::logPoke(uint8_t side, uint32_t durationMs, bool held) {
  Hold& h = hold_[side];
  char ms[12];
  sprintf(ms, "%lu", (unsigned long)durationMs);
  row("poke", side == 0 ? "L" : "R", OUTCOMES[h.outcome], h.phase, "", ms, held ? "held" : "",
      h.breakLocal, h.breakMs, clockOk());
  h.logged = true;
}

void Device::handlePokes() {
  PokeEvent ev;
  while (hal_.pokeEvent(ev)) {
    Hold& h = hold_[ev.side];
    Rule rule;
    currentRule(rule);
    if (ev.edge == EDGE_BREAK) {
      if (h.active) continue;
      memset(&h, 0, sizeof(h));
      h.active = true;
      h.breakMs = ev.ms;
      h.breakLocal = local();
      if (rule.v[K_MIN_POKE_MS] == 0) decide(ev.side, false);
    } else {
      if (!h.active) continue;
      uint32_t duration = ev.ms - h.breakMs;
      if (!h.decided) decide(ev.side, duration < rule.v[K_MIN_POKE_MS]);
      if (!h.logged) logPoke(ev.side, duration, false);
      h.active = false;
    }
  }
  for (uint8_t side = 0; side < 2; side++) {
    Hold& h = hold_[side];
    if (!h.active) continue;
    uint32_t held = hal_.millis() - h.breakMs;
    if (!h.decided) {
      Rule rule;
      currentRule(rule);
      if (held >= rule.v[K_MIN_POKE_MS]) decide(side, false);
    }
    if (h.decided && !h.logged && held >= MAX_HOLD_MS) logPoke(side, MAX_HOLD_MS, true);
  }
}

void Device::handleWell() {
  uint8_t event = hal_.dispenseEvent();
  if (event == DISPENSE_LANDED) {
    jammed_ = false;
    well_ = WELL_FULL;
    pellets_++;
    pelletMs_ = hal_.millis();
    simpleRow("dispense", REASONS[reason_]);
  } else if (event == DISPENSE_JAMMED) {
    well_ = WELL_EMPTY;
    // An earned or free pellet is owed, so it's retried every 10 minutes. One
    // fed by hand is a bench check nobody is owed: report it and stop, rather
    // than run the motor every 10 minutes on an empty hopper.
    jammed_ = reason_ != R_MANUAL;
    jamMs_ = hal_.millis();
    simpleRow("jam", reason_ == R_REWARD ? "reward" : REASONS[reason_]);
  }
  if (well_ == WELL_FULL && !hal_.wellFull()) {
    well_ = WELL_EMPTY;
    haveRetrieve_ = true;
    lastRetrieveMs_ = hal_.millis();
    char ms[12];
    sprintf(ms, "%lu", (unsigned long)(hal_.millis() - pelletMs_));
    row("retrieve", "", "", -1, ms, "", "", local(), hal_.millis(), clockOk());
  }
}

// device_sim.SimDevice._tick
void Device::tick() {
  handleWell();
  checkTime();
  Rule rule;
  currentRule(rule);
  uint32_t now = hal_.millis();
  if (jammed_) {
    if (now - jamMs_ >= JAM_RETRY_MS) {
      jammed_ = false;
      dispense(R_RETRY);
    }
  } else if (rule.v[K_FREE_FEED] && well_ == WELL_EMPTY) {
    if (!haveRetrieve_ || now - lastRetrieveMs_ >= rule.v[K_REFILL_DELAY] * 1000UL) dispense(R_FREE);
  }
  if ((int32_t)(now - nextStatusMs_) >= 0) {
    nextStatusMs_ += STATUS_EVERY_MS;
    Readings r;
    hal_.read(r);
    char detail[64], b[16], t[16], h[16];
    snprintf(detail, sizeof(detail), "batt=%s;temp=%s;hum=%s", fixed(b, r.battery, 2),
             fixed(t, r.temperature, 1), fixed(h, r.humidity, 0));
    simpleRow("status", detail);
  }
}

void Device::loop() {
  hal_.feedWatchdog();
  tick();
  handlePokes();
  pollInput();
  if (read_.active) serviceRead(4);
  if (staging_ && hal_.millis() - stagingMs_ > STAGING_TIMEOUT_MS) staging_ = false;
  showScreen();
}

bool Device::needsFineTicks() const {
  return hold_[0].active || hold_[1].active || well_ == WELL_DISPENSING;
}

void Device::showScreen() {
  Screen s;
  s.id = id_;
  s.phase = phase_.name;
  s.left = left_;
  s.right = right_;
  s.pellets = pellets_;
  s.clockOk = clockOk();
  s.jammed = jammed_;
  s.running = activeRun();
  s.localSeconds = local();
  hal_.show(s);
}

// --- serial ----------------------------------------------------------------

void Device::pollInput() {
  int c;
  int budget = 256;
  while (budget-- > 0 && (c = hal_.readByte()) >= 0) {
    if (c == '\r') continue;
    if (c == '\n') {
      input_[inputLen_] = 0;
      if (inputOverflow_) reply("ERR,TOO_LONG,");
      else if (inputLen_) handleLine(input_);
      inputLen_ = 0;
      inputOverflow_ = false;
    } else if (inputLen_ < MAX_COMMAND) {
      input_[inputLen_++] = (char)c;
    } else {
      inputOverflow_ = true;
    }
  }
}

static bool startsWith(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

// device_sim.SimDevice._command
void Device::handleLine(const char* line) {
  char out[160];
  if (strlen(line) > (size_t)MAX_COMMAND) return reply("ERR,TOO_LONG,");
  if (strcmp(line, "PING") == 0) {
    snprintf(out, sizeof(out), "PONG,id=%u,fw=%s,proto=%d", id_, FIRMWARE, PROTO);
    return reply(out);
  }
  if (strcmp(line, "STATUS") == 0) return status();
  if (startsWith(line, "TIME:")) return setTime(line + 5, false);
  if (startsWith(line, "TIME!:")) return setTime(line + 6, true);
  if (startsWith(line, "ID:")) {
    uint32_t id;
    if (!parseUint(line + 3, strlen(line + 3), 1, 999, id) || strlen(line + 3) > 3)
      return reply("ERR,BAD_ID,1-999");
    id_ = (uint16_t)id;
    saveDevice();
    snprintf(out, sizeof(out), "ID,id=%u", id_);
    return reply(out);
  }
  if (strcmp(line, "SCHED:BEGIN") == 0) {
    builder_.reset();
    staging_ = true;
    stagingFailed_ = false;
    stagingMs_ = hal_.millis();
    hal_.fileWrite("STAGE.TXT", "", 0);
    return reply("OK");
  }
  if (startsWith(line, "S:")) return stage(line + 2);
  if (startsWith(line, "SCHED:COMMIT,")) return commit(line + 13);
  if (startsWith(line, "RUN:BEGIN,")) return runBegin(line + 10);
  if (strcmp(line, "RUN:END") == 0) return runEnd();
  if (startsWith(line, "GOTO:")) return gotoPhase(line + 5);
  if (startsWith(line, "READ:")) return startRead(line + 5);
  if (strcmp(line, "FEED") == 0) {
    snprintf(out, sizeof(out), "FEED,result=%s", DISPENSE_RESULTS[dispense(R_MANUAL)]);
    return reply(out);
  }
  if (startsWith(line, "LIGHTS:")) {
    const char* arg = line + 7;
    if (strcmp(arg, "on") != 0 && strcmp(arg, "off") != 0) return reply("ERR,BAD_LIGHTS,on|off");
    lights_ = strcmp(arg, "on") == 0;
    hal_.lights(lights_);
    snprintf(out, sizeof(out), "LIGHTS,on=%d", lights_ ? 1 : 0);
    return reply(out);
  }
  if (startsWith(line, "TEST:")) {
    Cue c;
    const char* err = parseCue(line + 5, c);
    if (err) {
      snprintf(out, sizeof(out), "ERR,BAD_CUE,%s", err);
      return reply(out);
    }
    hal_.playCue(255, c, 0);
    return reply("OK");
  }
  if (strcmp(line, "REBOOT") == 0) {
    hal_.reboot();          // does not return on the device
    boot(RESET_SOFTWARE);   // the host build reboots in place
    return;
  }
  snprintf(out, sizeof(out), "ERR,UNKNOWN,%.40s", line);
  reply(out);
}

// device_sim.SimDevice._status
void Device::status() {
  Rule rule;
  currentRule(rule);
  char next[48] = "-";
  uint32_t due;
  if (canAdvance() && nextDue(due)) {
    char when[32];
    iso(due, when);
    snprintf(next, sizeof(next), "at=%s", when);
  } else {
    for (int i = 0; i < phase_.nUntil; i++) {
      const Until& u = phase_.untils[i];
      if (u.kind == U_REWARDS) {
        snprintf(next, sizeof(next), "rewards=%lu/%lu", (unsigned long)phaseRewards_, (unsigned long)u.value);
        break;
      }
      if (u.kind == U_POKES) {
        uint32_t n = u.side == PS_LEFT ? phaseLeft_ : u.side == PS_RIGHT ? phaseRight_
                                                                         : phaseLeft_ + phaseRight_;
        snprintf(next, sizeof(next), "pokes=%lu/%lu", (unsigned long)n, (unsigned long)u.value);
        break;
      }
      if (u.kind == U_GOTO) {
        strcpy(next, "goto");
        break;
      }
    }
  }
  char time[32] = "", start[32] = "", sched[12], nextSched[12];
  if (clockOk()) iso(local(), time);
  if (havePhaseStart_) iso(phaseStart_, start);
  if (schedDefault_) strcpy(sched, "default");
  else sprintf(sched, "%08lx", (unsigned long)schedCrc_);
  if (nextPresent_) sprintf(nextSched, "%08lx", (unsigned long)nextCrc_);
  else strcpy(nextSched, "-");
  Readings r;
  hal_.read(r);
  char b[16], t[16], h[16];
  bool isDepleted = depleted(rule);
  char out[MAX_COMMAND * 4];
  snprintf(out, sizeof(out),
           "STATUS,id=%u,fw=%s,time=%s,clock=%s,sd=%s,run=%s,ended=%d,sched=%s,next_sched=%s,"
           "phase=%u,phase_name=%s,phase_start=%s,next=%s,file=%s,seq=%lu,l=%lu,r=%lu,pellets=%lu,"
           "phase_rewards=%lu,phase_left=%lu,phase_right=%lu,depleted=%d,well=%d,jam=%d,uptime=%lu,"
           "batt=%s,temp=%s,hum=%s,lights=%d",
           id_, FIRMWARE, time, clockOk() ? "ok" : "unset", (sdError_ || !hal_.sdOk()) ? "err" : "ok",
           runId_, ended_ ? 1 : 0, sched, nextSched, phaseIndex_, phase_.name, start, next, file_,
           (unsigned long)seq_, (unsigned long)left_, (unsigned long)right_, (unsigned long)pellets_,
           (unsigned long)phaseRewards_, (unsigned long)phaseLeft_, (unsigned long)phaseRight_,
           isDepleted ? 1 : 0, well_ == WELL_FULL ? 1 : 0, jammed_ ? 1 : 0,
           (unsigned long)(hal_.millis() / 1000), fixed(b, r.battery, 2), fixed(t, r.temperature, 1),
           fixed(h, r.humidity, 0), lights_ ? 1 : 0);
  reply(out);
}

// device_sim.SimDevice._set_time
void Device::setTime(const char* arg, bool force) {
  int64_t utc;
  int16_t offset;
  if (!parseIso(arg, utc, offset)) return reply("ERR,BAD_TIME,");
  char before[32] = "unset";
  if (clockOk()) iso(local(), before);
  int64_t currentUtc = (int64_t)local() - offsetMin_ * 60;
  int64_t jump = utc - currentUtc;
  char out[128];
  if (clockOk() && activeRun() && !force && (jump > TIME_JUMP_LIMIT_S || jump < -(int64_t)TIME_JUMP_LIMIT_S)) {
    snprintf(out, sizeof(out), "ERR,TIME_JUMP,%ld", (long)jump);
    return reply(out);
  }
  // Times already recorded in local seconds move with the offset, so a
  // change of offset doesn't shift the run.
  int32_t shift = (offset - offsetMin_) * 60;
  if (runId_[0] != '-') t0_ += shift;
  if (havePhaseStart_) phaseStart_ += shift;
  offsetMin_ = offset;
  hal_.rtcSet((uint32_t)(utc + offset * 60));
  saveDevice();
  char after[32], detail[96];
  iso(local(), after);
  snprintf(detail, sizeof(detail), "from=%s;to=%s", before, after);
  simpleRow("clock", detail);
  snprintf(out, sizeof(out), "TIME,before=%s,after=%s", before, after);
  reply(out);
  tick();
}

void Device::stage(const char* line) {
  char out[128];
  if (!staging_) return reply("ERR,SCHED,0,no SCHED:BEGIN");
  if (stagingFailed_) {
    snprintf(out, sizeof(out), "ERR,SCHED,%u,an earlier line failed", builder_.lines() + 1);
    return reply(out);
  }
  stagingMs_ = hal_.millis();
  size_t len = strlen(line);
  const char* err = builder_.add(line, len);
  if (err) {
    stagingFailed_ = true;
    snprintf(stagingErr_, sizeof(stagingErr_), "%s", err);
    snprintf(out, sizeof(out), "ERR,SCHED,%u,%s", builder_.lines() + 1, err);
    return reply(out);
  }
  char buf[MAX_LINE + 2];
  memcpy(buf, line, len);
  buf[len] = '\n';
  if (!hal_.fileAppend("STAGE.TXT", buf, len + 1)) {
    stagingFailed_ = true;
    strcpy(stagingErr_, "SD write failed");
    return reply("ERR,SD,staging write failed");
  }
  reply("OK");
}

// device_sim.SimDevice._commit
void Device::commit(const char* arg) {
  char out[128];
  bool was = staging_ && hal_.millis() - stagingMs_ <= STAGING_TIMEOUT_MS;
  staging_ = false;
  if (!was) return reply("ERR,SCHED_REJECTED,no upload in progress");
  if (stagingFailed_) {
    snprintf(out, sizeof(out), "ERR,SCHED_REJECTED,%s", stagingErr_);
    return reply(out);
  }
  const char* err = builder_.finish();
  if (err) {
    snprintf(out, sizeof(out), "ERR,SCHED_REJECTED,%s", err);
    return reply(out);
  }
  char want[12];
  sprintf(want, "%08lx", (unsigned long)builder_.crc());
  const char* comma = strchr(arg, ',');
  size_t crcLen = comma ? (size_t)(comma - arg) : strlen(arg);
  bool match = crcLen == 8;
  for (size_t i = 0; match && i < 8; i++) {
    char c = arg[i];
    if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
    match = c == want[i];
  }
  if (!match) {
    snprintf(out, sizeof(out), "ERR,SCHED_REJECTED,crc is %s", want);
    return reply(out);
  }
  if (activeRun()) {
    uint32_t expected;
    if (comma && parseUint(comma + 1, strlen(comma + 1), 0, 65535, expected) && expected != phaseIndex_) {
      snprintf(out, sizeof(out), "ERR,SCHED_STALE,phase=%u", phaseIndex_);
      return reply(out);
    }
    if (phaseIndex_ >= builder_.phases()) {
      snprintf(out, sizeof(out), "ERR,SCHED_REJECTED,no phase %u", phaseIndex_);
      return reply(out);
    }
    if (!promote("STAGE.TXT", "SCHED.TXT")) return reply("ERR,SD,could not store schedule");
    schedDefault_ = false;
    schedCrc_ = builder_.crc();
    nPhases_ = builder_.phases();
    if (loadPhase(phaseIndex_, scratch)) phase_ = scratch;
    char detail[24];
    sprintf(detail, "crc=%s", want);
    simpleRow("schedule", detail);
    snprintf(out, sizeof(out), "SCHED,crc=%s,phases=%u,slot=now", want, nPhases_);
    reply(out);
    tick();
    checkCounts();
    return;
  }
  if (!promote("STAGE.TXT", "NEXT.TXT")) return reply("ERR,SD,could not store schedule");
  nextPresent_ = true;
  nextCrc_ = builder_.crc();
  snprintf(out, sizeof(out), "SCHED,crc=%s,phases=%u,slot=next", want, builder_.phases());
  reply(out);
}

static bool validRunId(const char* s, size_t len) {
  if (len < 1 || len > 8) return false;
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

// device_sim.SimDevice._run_begin
void Device::runBegin(const char* arg) {
  char buf[MAX_COMMAND + 1];
  snprintf(buf, sizeof(buf), "%s", arg);
  char* parts[4];
  int n = 0;
  parts[n++] = buf;
  for (char* p = buf; *p && n < 4; p++) {
    if (*p == ',') {
      *p = 0;
      parts[n++] = p + 1;
    }
  }
  int64_t utc;
  int16_t off;
  char out[96];
  if (n != 3 || !validRunId(parts[0], strlen(parts[0])) || !parseIso(parts[1], utc, off))
    return reply("ERR,BAD_RUN,RUN:BEGIN,<id>,<t0>,<crc>");
  if (strcmp(parts[0], runId_) == 0 && !ended_) {
    snprintf(out, sizeof(out), "RUN,run=%s,file=%s,phase=%u", runId_, file_, phaseIndex_);
    return reply(out);
  }
  char have[12];
  if (nextPresent_) sprintf(have, "%08lx", (unsigned long)nextCrc_);
  else if (!schedDefault_) sprintf(have, "%08lx", (unsigned long)schedCrc_);
  else strcpy(have, "default");
  for (char* p = parts[2]; *p; p++) {
    if (*p >= 'A' && *p <= 'F') *p = (char)(*p - 'A' + 'a');
  }
  if (strcmp(have, parts[2]) != 0) {
    snprintf(out, sizeof(out), "ERR,RUN_SCHED,have=%s", have);
    return reply(out);
  }
  if (activeRun()) {
    char detail[32];
    snprintf(detail, sizeof(detail), "replaced by %.8s", parts[0]);
    simpleRow("run_end", detail);
  }
  if (nextPresent_) {
    if (!promote("NEXT.TXT", "SCHED.TXT")) return reply("ERR,SD,could not store schedule");
    schedDefault_ = false;
    schedCrc_ = nextCrc_;
    uint16_t phases;
    uint32_t crc;
    nPhases_ = validateSchedule("SCHED.TXT", crc, phases) ? phases : 1;
    nextPresent_ = false;
  }
  snprintf(runId_, sizeof(runId_), "%.8s", parts[0]);
  if (lights_) {            // a run starts with the lights as its cues leave them
    lights_ = false;
    hal_.lights(false);
  }
  t0_ = (uint32_t)(utc + offsetMin_ * 60);
  ended_ = false;
  snprintf(file_, sizeof(file_), "%s.CSV", runId_);
  char header[sizeof(HEADER) + 1];
  sprintf(header, "%s\n", HEADER);
  if (!hal_.fileWrite(file_, header, strlen(header))) sdError_ = true;
  seq_ = left_ = right_ = pellets_ = 0;
  phaseIndex_ = 0;
  phaseStart_ = t0_;
  havePhaseStart_ = true;
  phaseRewards_ = phaseLeft_ = phaseRight_ = 0;
  quotaUsed_ = 0;
  haveQuotaPeriod_ = false;
  counters_[0] = counters_[1] = counters_[2] = 0;
  timeoutActive_ = false;
  loadPhase(0, phase_);
  saveRun();
  char detail[80], when[32];
  iso(t0_, when);
  snprintf(detail, sizeof(detail), "t0=%s;%s", when, phase_.name);
  simpleRow("run_start", detail);
  snprintf(out, sizeof(out), "RUN,run=%s,file=%s,phase=0", runId_, file_);
  reply(out);
  tick();
}

// device_sim.SimDevice._run_end
void Device::runEnd() {
  char out[48];
  if (runId_[0] == '-') return reply("ERR,NO_RUN,");
  if (!ended_) {
    ended_ = true;
    saveRun();
    simpleRow("run_end", "");
  }
  snprintf(out, sizeof(out), "RUN,run=%s,ended=1", runId_);
  reply(out);
}

// device_sim.SimDevice._goto
void Device::gotoPhase(const char* arg) {
  char out[64];
  if (!activeRun()) return reply("ERR,NO_RUN,");
  const char* comma = strchr(arg, ',');
  uint32_t from, to;
  if (!comma || !parseUint(arg, comma - arg, 0, 65535, from) ||
      !parseUint(comma + 1, strlen(comma + 1), 0, 65535, to))
    return reply("ERR,BAD_GOTO,GOTO:<from>,<to>");
  if (from != phaseIndex_) {
    snprintf(out, sizeof(out), "ERR,GOTO_STALE,phase=%u", phaseIndex_);
    return reply(out);
  }
  if (to >= nPhases_) {
    snprintf(out, sizeof(out), "ERR,BAD_PHASE,%lu", (unsigned long)to);
    return reply(out);
  }
  enterPhase((uint16_t)to, local(), "goto");
  snprintf(out, sizeof(out), "PHASE,n=%lu,name=%s", (unsigned long)to, phase_.name);
  reply(out);
}

// device_sim.SimDevice._read, streamed a few rows per loop.
void Device::startRead(const char* arg) {
  char out[64];
  const char* comma = strchr(arg, ',');
  uint32_t from;
  size_t idLen = comma ? (size_t)(comma - arg) : strlen(arg);
  bool idle = idLen == 1 && arg[0] == '-';
  if (!comma || (!idle && !validRunId(arg, idLen)) || !parseUint(comma + 1, strlen(comma + 1), 0, 0xFFFFFFFFUL, from)) {
    snprintf(out, sizeof(out), "ERR,NO_FILE,%.*s", (int)(idLen > 8 ? 8 : idLen), arg);
    return reply(out);
  }
  memset(&read_, 0, sizeof(read_));
  memcpy(read_.run, arg, idLen);
  if (idle) strcpy(read_.file, IDLE_FILE);
  else snprintf(read_.file, sizeof(read_.file), "%s.CSV", read_.run);
  int32_t size = hal_.fileSize(read_.file);
  if (size < 0) {
    snprintf(out, sizeof(out), "ERR,NO_FILE,%s", read_.run);
    return reply(out);
  }
  // Binary search for a line start at or before the first wanted row, so a
  // request for recent rows doesn't read weeks of file.
  char buf[MAX_COMMAND * 3];
  int32_t n = hal_.fileRead(read_.file, 0, buf, sizeof(buf) - 1);
  buf[n > 0 ? n : 0] = 0;
  char* nl = strchr(buf, '\n');
  uint32_t lo = nl ? (uint32_t)(nl - buf + 1) : (uint32_t)size;
  uint32_t hi = (uint32_t)size;
  while (hi > lo && hi - lo > 1024) {
    uint32_t mid = lo + (hi - lo) / 2;
    n = hal_.fileRead(read_.file, mid, buf, sizeof(buf) - 1);
    if (n <= 0) break;
    buf[n] = 0;
    char* first = strchr(buf, '\n');
    if (!first || !strchr(first + 1, '\n')) {
      hi = mid;
      continue;
    }
    char* cols[ROW_COLUMNS];
    char* lineStart = first + 1;
    *strchr(lineStart, '\n') = 0;
    uint32_t at = mid + (uint32_t)(lineStart - buf);
    if (splitRow(lineStart, cols) >= 3 && toUint(cols[2]) < from) lo = at;
    else hi = mid;
    hal_.feedWatchdog();
  }
  read_.pos = lo;
  read_.from = from;
  read_.active = true;
}

void Device::serviceRead(int maxRows) {
  char buf[MAX_COMMAND * 3 + 8];
  while (read_.active && maxRows > 0) {
    int32_t n = hal_.fileRead(read_.file, read_.pos, buf + 4, MAX_COMMAND * 3 - 1);
    char* nl = n > 0 ? (char*)memchr(buf + 4, '\n', n) : nullptr;
    if (!nl) {
      char out[48];
      snprintf(out, sizeof(out), "READ_END,run=%s,last=%lu", read_.run, (unsigned long)read_.last);
      reply(out);
      read_.active = false;
      return;
    }
    *nl = 0;
    read_.pos += (uint32_t)(nl - (buf + 4)) + 1;
    char copy[MAX_COMMAND * 3];
    strcpy(copy, buf + 4);
    char* cols[ROW_COLUMNS];
    if (splitRow(copy, cols) < 3) continue;
    uint32_t seq = toUint(cols[2]);
    read_.last = seq;
    if (seq >= read_.from) {
      memcpy(buf, "ROW,", 4);
      reply(buf);
      maxRows--;
    }
  }
}

}  // namespace fed
