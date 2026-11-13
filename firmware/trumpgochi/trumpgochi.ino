// Gochi – one-button Tamagotchi-style satire game (Trumpgochi, Bidengochi, Zemangochi, Babišgochi)
// Waveshare ESP32-S3-ePaper-1.54 V2 (same board as the QR Snapshot Booth box; booth NVS "box" is not touched)
//
// Button (BOOT, GPIO0): short = next item, long (>= 600 ms) = do it, double = back. The press that wakes the
// board only wakes it. Presses are queued by an interrupt, so a press during an e-paper refresh is not lost.
// Stats decay by real time (PCF85063 RTC), also while the board deep-sleeps (wakes every 15 min to redraw).
// Serial 115200: ? status (?S details), R<0|90|180|270> rotate the screen, T<unix> set clock, V battery, S<b|e|r|n>=<0..100> set stat, F<hours> fast-forward, N new game.
// Characters: up to 8 packs in LittleFS /pack0..7.bin, sent by gochi-uploader.html over Web Serial:
//   PB<slot>.<bytes> begin, PW<hex> data (<= 512 bytes a line), PE<crc32 hex> end; PX<slot> removes one.
// Welcome screen: with packs installed and no living gochi, all characters as tiles (small face + pack id);
// short = next tile, long = choose it.
// Demo: entering the welcome screen (upload/reset, game over) first shows every character once as a card (title + big
// face), 2 s each; then the tiles. Short press skips to the tiles, long press picks the one shown.
// A gochi dies when any stat has been at 0 for 30 min (the title counts down "DYING! n MIN"); a long press on the game-over screen goes back to the welcome screen.
// No packs at all: the built-in Trump is played.
// Battery bond: Energy can't go above the battery % (min 5); on a charger Energy refills +10/h and losses are halved;
// below 20 % the gochi looks tired and asks "CHARGE ME!"; below 10 % losses double; charged to full after >= 1 h on the
// charger = +10 Ego and "FULL CHARGE!" (once per 20 h).
// Pack v1: "GOCH" 1 id[12] start[4] decay10[4] act[4][4](int8) pauseH[4] 17 then 17 x (w h rows MSB first):
//   0 title, 1-4 stat labels, 5-8 action labels, 9-12 action messages, 13-16 moods (happy, hungry, tired, angry).
//   Stat roles are fixed: 0 food, 1 ego/mood, 2 ratings, 3 energy (mood rule below).
//
// Author: Pavel Kotyza <kotyza@gmail.com>

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <GxEPD2_BW.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "soc/gpio_reg.h"

static const char *VERSION = "0.6.4";
static const uint32_t DEATH_S = 30 * 60;         // a stat at 0 this long = game over ("DYING! n MIN" meanwhile)
static const uint32_t DYING_WAKE_S = 5 * 60;     // asleep while dying: wake this often to update the countdown
static const uint32_t UNPLUGGED_MS = 3000;       // USB host gone this long before we may sleep
static const uint32_t SERIAL_AWAKE_MS = 60000;   // stay awake this long after the last serial byte (uploader page)
static uint32_t lastSerialMs = 0;
static const uint32_t WAKE_EVERY_S = 15 * 60;     // timer wake: decay + redraw
static const uint32_t IDLE_MS = 30000;            // awake without input -> sleep
static const uint32_t TIMER_WAKE_MS = 600;        // after a timer wake: time to spot a computer on USB
static const uint32_t LONG_MS = 600, DOUBLE_MS = 350, DEBOUNCE_MS = 25;
static const int FULL_REFRESH_EVERY = 20;
static uint16_t rotDeg = 0;                       // screen rotation 0/90/180/270 (NVS "rot", serial R<deg>)
static int gfxRotation(int deg) { return (1 + deg / 90) & 3; }   // as booth_qr: 0 = upright on this board

// ---- Pins (see esp32-qr-box skill) ----
#define VBAT_PWR 17
#define EPD_PWR  6
#define EPD_BUSY 8
#define EPD_RST  9
#define EPD_DC   10
#define EPD_CS   11
#define EPD_SCK  12
#define EPD_MOSI 13
#define I2C_SDA  47
#define I2C_SCL  48
#define RTC_ADDR 0x51
#define BAT_ADC  4
#define BTN      0
#define AUDIO_PWR 42   // LOW = on: the audio codec shares the I2C bus; unpowered it can hold SDA low (rtc_check powers it too)

static const int BAT_EMPTY_MV = 3350;

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
static Preferences prefs;
RTC_DATA_ATTR static uint32_t refreshes = 0;

// ---------- game state (NVS "gochi", key "st") ----------
enum { BURGERS, EGO, RATINGS, ENERGY, NSTAT };
static const char *STAT_NAME[NSTAT] = { "BURGER", "EGO", "RATING", "ENERGY" };

// ---------- character (built-in Trump, replaced by an uploaded pack) ----------
enum { A_FEED, A_RALLY, A_GOLF, A_SLEEP, NACT };
static const char *ACT_NAME[NACT] = { "FEED", "RALLY", "GOLF", "SLEEP" };
static const char *MSG_TXT[NACT] = { "+25 BURGERS", "RALLY! +EGO", "FORE! +NRG", "ZZZ... 1h" };
static const uint8_t B_START[NSTAT] = { 70, 60, 75, 80 };
static const float B_DECAY[NSTAT] = { 8, 5, 3, 4 };
static const int8_t B_ACT[NACT][NSTAT] = { { 25, 0, 0, 0 }, { 0, 20, 3, -15 }, { 0, 0, -5, 15 }, { 0, 0, 0, 35 } };
static const uint8_t B_PAUSE[NACT] = { 0, 0, 0, 1 };
static uint8_t START[NSTAT];
static float DECAY_PER_H[NSTAT];
static int8_t ACT[NACT][NSTAT];
static uint8_t PAUSE_H[NACT];
static char packId[13];

enum { BM_TITLE, BM_STAT0 = 1, BM_ACT0 = 5, BM_MSG0 = 9, BM_MOOD0 = 13, BM_N = 17 };
static uint8_t bmLimit(int i, bool h) {              // max w / h per bitmap slot
  if (i == BM_TITLE || (i >= BM_MSG0 && i < BM_MOOD0)) return h ? 18 : 160;
  if (i < BM_ACT0) return h ? 11 : 40;
  if (i < BM_MSG0) return h ? 22 : 46;
  return 80;
}
struct Bm { uint8_t w, h; const uint8_t *d; };
static Bm bms[BM_N];                                   // w 0 = not in the pack: built-in text / face
static uint8_t *packBuf = nullptr;

static void useBuiltin() {
  memcpy(START, B_START, sizeof START); memcpy(DECAY_PER_H, B_DECAY, sizeof DECAY_PER_H);
  memcpy(ACT, B_ACT, sizeof ACT); memcpy(PAUSE_H, B_PAUSE, sizeof PAUSE_H);
  memset(bms, 0, sizeof bms); strcpy(packId, "builtin");
  free(packBuf); packBuf = nullptr;
}

enum { SLOT_NONE = -2, SLOT_BUILTIN = -1, NSLOT = 8 };
struct State {
  uint8_t ver;
  int8_t slot;          // SLOT_NONE = welcome screen, SLOT_BUILTIN, 0..7 = /pack<slot>.bin
  uint8_t dead;
  float v[NSTAT];
  uint32_t last;        // unix of the last decay
  uint32_t sleepUntil;  // no decay before this (Sleep action)
  uint32_t born;
  uint32_t zeroSince;   // unix when a stat reached 0 (0 = all above 0)
} st;

static void newGame(int slot, uint32_t now) {
  st = State{ 2, (int8_t)slot, 0, { (float)START[0], (float)START[1], (float)START[2], (float)START[3] }, now, 0, now, 0 };
}

static void save() { prefs.putBytes("st", &st, sizeof st); }

static bool load() {
  return prefs.getBytes("st", &st, sizeof st) == sizeof st && st.ver == 2;
}

// ---------- battery bond (values set by batSample()) ----------
static int batNowPct = 100;                            // last measured battery %
RTC_DATA_ATTR static bool charging = false;            // on a charger (voltage rising / full) or a computer
static bool bonusShow = false;                          // "FULLY CHARGED!" once on the next draw
static const float CHARGE_GAIN_H = 10;                 // Energy per hour while charging
static float energyCap() { return max(5, batNowPct); }
static float lossMul() { return charging ? 0.5f : batNowPct < 10 ? 2.0f : 1.0f; }

static void addStat(int i, float d) {
  st.v[i] = constrain(st.v[i] + d, 0.0f, 100.0f);
  if (i == ENERGY && st.v[i] > energyCap()) st.v[i] = energyCap();
}

// Decay from..to (unix), skipping time spent asleep (Sleep action); notes when the first stat reached 0.
static void decay(uint32_t from, uint32_t to) {
  if (st.sleepUntil > from) from = min(st.sleepUntil, to);
  if (to <= from) return;
  float h = (to - from) / 3600.0f;
  for (int i = 0; i < NSTAT; i++) {
    float rate = DECAY_PER_H[i] * lossMul() - (i == ENERGY && charging ? CHARGE_GAIN_H : 0);   // per hour, < 0 = gain
    float d = rate * h;
    if (st.v[i] > 0 && d >= st.v[i] && rate > 0) {
      uint32_t t0 = from + (uint32_t)(st.v[i] / rate * 3600);
      if (!st.zeroSince || t0 < st.zeroSince) st.zeroSince = t0;
    }
    addStat(i, -d);
  }
}

static bool anyZero() { for (int i = 0; i < NSTAT; i++) if (st.v[i] <= 0) return true; return false; }

static void catchUp(uint32_t now) {
  if (st.slot == SLOT_NONE || st.dead) return;
  if (now > st.last) { decay(st.last, now); st.last = now; }
  if (!anyZero()) st.zeroSince = 0;
  else if (!st.zeroSince) st.zeroSince = now;
  if (st.zeroSince && now - st.zeroSince >= DEATH_S) st.dead = 1;
}

// ---------- pack ----------
// Checks a pack; on success points the character tables into `b` (kept until the next pack). nullptr = OK.
static const char *parsePack(uint8_t *b, size_t n) {
  if (n < 46 || memcmp(b, "GOCH", 4) || b[4] != 1) return "not a gochi pack v1";
  if (b[45] != BM_N) return "bad bitmap count";
  Bm t[BM_N];
  size_t o = 46;
  for (int i = 0; i < BM_N; i++) {
    if (o + 2 > n) return "pack too short";
    uint8_t w = b[o], h = b[o + 1];
    o += 2;
    size_t len = (w + 7) / 8 * h;
    if (w > bmLimit(i, false) || h > bmLimit(i, true)) return "bitmap too big";
    if (o + len > n) return "pack too short";
    t[i] = { w, h, b + o };
    o += len;
  }
  if (o != n) return "extra bytes";
  for (int i = 0; i < 4; i++) if (b[17 + i] > 100 || b[41 + i] > 24) return "bad numbers";
  for (int i = 0; i < 12; i++) packId[i] = isprint(b[5 + i]) ? b[5 + i] : 0;
  packId[12] = 0;
  for (int i = 0; i < NSTAT; i++) { START[i] = b[17 + i]; DECAY_PER_H[i] = b[21 + i] / 10.0f; }
  for (int a = 0; a < NACT; a++) { for (int i = 0; i < NSTAT; i++) ACT[a][i] = (int8_t)b[25 + a * 4 + i]; PAUSE_H[a] = b[41 + a]; }
  memcpy(bms, t, sizeof t);
  return nullptr;
}

// Reads a pack file; on success it becomes the character. nullptr = OK.
static const char *loadPackFile(const char *path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "no pack";
  size_t n = f.size();
  uint8_t *b = (uint8_t *)malloc(n ? n : 1);
  if (!b) { f.close(); return "out of memory"; }
  bool ok = f.read(b, n) == n;
  f.close();
  const char *e = ok ? parsePack(b, n) : "read error";
  if (e) { free(b); return e; }
  free(packBuf);
  packBuf = b;
  return nullptr;
}

static void slotPath(int slot, char *p) { snprintf(p, 16, "/pack%d.bin", slot); }
static bool slotExists(int slot) { char p[16]; slotPath(slot, p); return LittleFS.exists(p); }
static int packMask() { int m = 0; for (int i = 0; i < NSLOT; i++) if (slotExists(i)) m |= 1 << i; return m; }
static bool loadSlot(int slot) {
  if (slot < 0) { useBuiltin(); return slot == SLOT_BUILTIN; }
  char p[16]; slotPath(slot, p);
  const char *e = loadPackFile(p);
  if (e) { Serial.printf("pack %d: %s\n", slot, e); useBuiltin(); }
  return !e;
}

static uint32_t crc32Upd(uint32_t c, const uint8_t *p, size_t n) {   // zlib CRC-32 (start 0xFFFFFFFF, end ~)
  while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320UL & (0 - (c & 1))); }
  return c;
}

// ---------- RTC (PCF85063, from booth_qr) ----------
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + (int)doe - 719468;
}
static void civilFromDays(int64_t z, int &y, unsigned &m, unsigned &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400) + (m <= 2);
}
static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static int i2cBoot = -1;                               // "?C": SDA*2 + SCL as found before the recovery (from booth_qr)
static void i2cRecover() {   // a reset mid-read can leave the RTC holding SDA low
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  i2cBoot = digitalRead(I2C_SDA) * 2 + digitalRead(I2C_SCL);
  pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SCL, HIGH);
  delayMicroseconds(10);
  for (int i = 0; i < 32 && digitalRead(I2C_SDA) == LOW; i++) {
    digitalWrite(I2C_SCL, LOW);  delayMicroseconds(10);
    digitalWrite(I2C_SCL, HIGH); delayMicroseconds(10);
  }
  pinMode(I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SDA, LOW);  delayMicroseconds(10);
  digitalWrite(I2C_SDA, HIGH); delayMicroseconds(10);
}

static bool rtcRead(uint32_t &t) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, 7) != 7) return false;
  uint8_t r[7];
  for (auto &b : r) b = Wire.read();
  if (r[0] & 0x80) return false;
  int64_t days = daysFromCivil(2000 + bcd2bin(r[6]), bcd2bin(r[5] & 0x1F), bcd2bin(r[3] & 0x3F));
  t = days * 86400 + bcd2bin(r[2] & 0x3F) * 3600 + bcd2bin(r[1] & 0x7F) * 60 + bcd2bin(r[0] & 0x7F);
  return true;
}

static bool rtcWrite(uint32_t t) {
  int y; unsigned m, d;
  civilFromDays(t / 86400, y, m, d);
  uint32_t s = t % 86400;
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  Wire.write(bin2bcd(s % 60));
  Wire.write(bin2bcd(s / 60 % 60));
  Wire.write(bin2bcd(s / 3600));
  Wire.write(bin2bcd(d));
  Wire.write((t / 86400 + 4) % 7);
  Wire.write(bin2bcd(m));
  Wire.write(bin2bcd(y - 2000));
  return Wire.endTransmission() == 0;
}

// "?C": raw PCF85063 registers 0x00..0x0A + I2C state + bus scan, for diagnosis (booth_qr "R")
static void rtcDump() {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x00);
  int e = Wire.endTransmission(false);
  int n = e ? 0 : Wire.requestFrom(RTC_ADDR, 11);
  Serial.printf("OK rtc i2c %d n %d", e, n);
  for (int i = 0; i < n; i++) Serial.printf(" %02x", Wire.read());
  Serial.printf(" boot sda/scl %d/%d scan", i2cBoot >> 1, i2cBoot & 1);
  for (int a = 1; a < 127; a++) { Wire.beginTransmission(a); if (Wire.endTransmission(true) == 0) Serial.printf(" %02x", a); }
  Serial.println();
}
static void i2cRestart() { Wire.end(); i2cRecover(); Wire.begin(I2C_SDA, I2C_SCL); }   // bus stuck: free it, start the driver again

// ---------- battery (from booth_qr) ----------
static int batMv() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BAT_ADC);
  return sum / 16 * 2;
}
static int batPct(int mv) {
  static const int16_t T[][2] = { {4200,100},{4150,95},{4110,90},{4080,85},{4020,80},{3980,75},{3950,70},{3910,65},{3870,60},
    {3850,55},{3840,50},{3820,45},{3800,40},{3790,35},{3770,30},{3750,25},{3730,20},{3710,15},{3690,10},{3610,5},{3270,0} };
  if (mv >= T[0][0]) return 100;
  for (size_t i = 1; i < sizeof T / sizeof T[0]; i++)
    if (mv >= T[i][0]) return T[i][1] + (mv - T[i][0]) * (T[i - 1][1] - T[i][1]) / (T[i - 1][0] - T[i][0]);
  return 0;
}

// Battery % and charging state. Charging = a computer on USB, or the cell near full (>= 4.18 V: a charger holds it there),
// or the voltage rose >= 20 mV since a sample >= 10 min ago (any charger); it ends when the voltage falls again.
static const uint32_t BONUS_GAP_S = 20 * 3600;
RTC_DATA_ATTR static int refMv = 0;
RTC_DATA_ATTR static uint32_t refT = 0, chgSince = 0;
static void batSample(uint32_t now) {
  int mv = batMv();
  batNowPct = batPct(mv);
  bool was = charging;
  if (HWCDC::isPlugged() || mv >= 4180) charging = true;
  if (!refT || now < refT) { refMv = mv; refT = now; }
  else if (now - refT >= 600) {
    if (mv - refMv >= 20) charging = true;
    else if (mv < refMv - 10 && !HWCDC::isPlugged()) charging = false;
    refMv = mv; refT = now;
  }
  if (charging && !was) chgSince = now;
  if (!charging) chgSince = 0;
}

// Full after >= 1 h on the charger: +10 Ego, at most once per BONUS_GAP_S (NVS "bon"). Call after catchUp().
static void chargeBonus(uint32_t now) {
  if (st.slot == SLOT_NONE || st.dead || !charging || !chgSince || now - chgSince < 3600 || batNowPct < 95) return;
  uint32_t last = prefs.getUInt("bon", 0);
  if (last && now - last < BONUS_GAP_S) return;
  prefs.putUInt("bon", now);
  addStat(EGO, 10);
  bonusShow = true;
}

// ---------- button: ISR queues edges, readBtn() classifies them by their own timestamps ----------
enum Btn { B_NONE, B_SHORT, B_LONG, B_DOUBLE };
static volatile uint32_t qT[32];
static volatile uint8_t qDown[32], qHead = 0, qTail = 0;

static void IRAM_ATTR btnIsr() {
  uint8_t n = (qHead + 1) & 31;
  if (n == qTail) return;                              // full: drop
  qT[qHead] = (uint32_t)(esp_timer_get_time() / 1000);
  qDown[qHead] = (REG_READ(GPIO_IN_REG) & (1UL << BTN)) == 0;   // register read: IRAM-safe
  qHead = n;
}

static bool isDown = false, ignoreHeld = false, pendingShort = false;
static uint32_t downAt = 0, pendingAt = 0;
static bool allowDouble = true;                        // games turn it off: no 350 ms delay on short presses

static uint32_t nowMs() { return (uint32_t)(esp_timer_get_time() / 1000); }

static int queuedBtn = B_NONE;                         // second event when one release yields two

static int readBtn() {   // returns Btn (int: Arduino prototype generator)
  if (queuedBtn != B_NONE) { int r = queuedBtn; queuedBtn = B_NONE; return r; }
  while (qTail != qHead) {
    uint32_t t = qT[qTail];
    bool d = qDown[qTail];
    qTail = (qTail + 1) & 31;
    if (d == isDown) continue;                         // repeated level (bounce)
    isDown = d;
    if (d) { downAt = t; continue; }
    if (ignoreHeld) { ignoreHeld = false; continue; }  // release of the wake press
    uint32_t dur = t - downAt;
    if (dur < DEBOUNCE_MS) continue;
    if (dur >= LONG_MS) {
      if (pendingShort) { pendingShort = false; queuedBtn = B_LONG; return B_SHORT; }
      return B_LONG;
    }
    if (!allowDouble) return B_SHORT;
    if (pendingShort && downAt - pendingAt <= DOUBLE_MS) { pendingShort = false; return B_DOUBLE; }
    bool earlier = pendingShort;                       // presses queued during a long refresh: keep the first one
    pendingShort = true; pendingAt = t;
    if (earlier) return B_SHORT;
  }
  if (pendingShort && !isDown && nowMs() - pendingAt > DOUBLE_MS) { pendingShort = false; return B_SHORT; }
  return B_NONE;
}

// ---------- screen ----------
static int sel = -1;                                   // highlighted action, -1 = none (timer wake)
static int cand = -1;                                  // welcome screen: slot shown
static int msgIdx = -1;                                // action message shown instead of the title once
static bool forceFull = false;
static const uint32_t DEMO_STEP_MS = 2000, DEMO_BATT_MS = 180000;   // welcome slides: time per slide, longest run on battery
static bool demo = false;                              // slides running (one round): the candidate's card
static bool demoGame = false;                          // slide shows the game screen (else the big face card)
static uint32_t demoAtMs = 0, demoStartMs = 0;         // current slide drawn at; slides started at
static const uint32_t SPLASH_MS = 3000;               // "Welcome to GOCHI", then the slides start by themselves
static bool splash = false;                            // splash showing (only while demo)
RTC_DATA_ATTR static bool gate = false;                // "ARE YOU 21+?" before the splash (kept over deep sleep)
static int gateSel = 0;                                // 21+ buttons: 0 YES, 1 NO (short = switch, long = confirm)
static bool nope = false;                              // NO chosen: "SORRY" screen, any press = back to the question
static char cardTag[8] = "";                           // slides: "k/n" drawn on the game screen

static void text(const char *t, int size, int x, int y, uint16_t c = GxEPD_BLACK) {
  display.setTextColor(c); display.setTextSize(size); display.setCursor(x, y); display.print(t);
}

// Pack bitmap i centred in the box x,y,w,h; false = not in the pack (caller draws the built-in text).
static bool drawBm(int i, int x, int y, int w, int h, uint16_t c) {
  if (!bms[i].w) return false;
  display.drawBitmap(x + (w - bms[i].w) / 2, y + (h - bms[i].h) / 2, bms[i].d, bms[i].w, bms[i].h, c);
  return true;
}

static int mood() {                                    // 0 happy, 1 hungry, 2 tired, 3 angry
  if (st.zeroSince) return 3;                          // dying
  if (st.v[ENERGY] < 25 || (batNowPct < 20 && !charging)) return 2;
  if (st.v[BURGERS] < 25) return 1;
  if (st.v[EGO] < 25 || st.v[RATINGS] < 25) return 3;
  return 0;
}

// Built-in caricature (no pack): head, hair swoop, tie, mood face.
static void drawFace() {
  const int cx = 100, cy = 54, r = 26;
  display.drawCircle(cx, cy, r, GxEPD_BLACK);
  display.drawCircle(cx, cy, r - 1, GxEPD_BLACK);
  display.fillRoundRect(cx - 26, cy - 30, 54, 14, 7, GxEPD_BLACK);        // hair
  display.fillTriangle(cx + 26, cy - 30, cx + 40, cy - 20, cx + 24, cy - 16, GxEPD_BLACK);
  display.fillTriangle(cx, 82, cx - 6, 88, cx + 6, 88, GxEPD_BLACK);      // tie
  display.fillTriangle(cx - 6, 88, cx + 6, 88, cx, 97, GxEPD_BLACK);
  const int ey = cy - 2, my = cy + 13;
  int m = mood();
  if (m == 2) {                                                           // tired
    display.drawFastHLine(cx - 14, ey, 9, GxEPD_BLACK);
    display.drawFastHLine(cx + 5, ey, 9, GxEPD_BLACK);
    display.drawFastHLine(cx - 6, my, 12, GxEPD_BLACK);
    text("Zzz", 1, cx + 30, cy - 4);
  } else if (m == 1) {                                                    // hungry
    display.fillCircle(cx - 9, ey, 3, GxEPD_BLACK);
    display.fillCircle(cx + 9, ey, 3, GxEPD_BLACK);
    display.drawCircle(cx, my, 6, GxEPD_BLACK);
    display.drawCircle(cx, my, 5, GxEPD_BLACK);
  } else if (m == 3) {                                                    // angry
    display.fillCircle(cx - 9, ey, 3, GxEPD_BLACK);
    display.fillCircle(cx + 9, ey, 3, GxEPD_BLACK);
    display.drawLine(cx - 15, ey - 9, cx - 4, ey - 5, GxEPD_BLACK);
    display.drawLine(cx + 15, ey - 9, cx + 4, ey - 5, GxEPD_BLACK);
    display.drawLine(cx - 9, my + 4, cx, my, GxEPD_BLACK);
    display.drawLine(cx, my, cx + 9, my + 4, GxEPD_BLACK);
  } else {                                                                // happy
    display.fillCircle(cx - 9, ey, 3, GxEPD_BLACK);
    display.fillCircle(cx + 9, ey, 3, GxEPD_BLACK);
    display.drawLine(cx - 10, my - 2, cx, my + 3, GxEPD_BLACK);
    display.drawLine(cx, my + 3, cx + 10, my - 2, GxEPD_BLACK);
  }
}

static void draw(bool full) {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  if (st.zeroSince && !st.dead) {                      // countdown beats every other title
    uint32_t gone = st.last > st.zeroSince ? st.last - st.zeroSince : 0;
    char t[20]; snprintf(t, sizeof t, "DYING! %lu MIN", (unsigned long)((gone < DEATH_S ? DEATH_S - gone : 0) + 59) / 60);
    text(t, 2, 4, 2);
  } else if (bonusShow) text("FULL CHARGE!", 2, 4, 2);
  else if (msgIdx >= 0) { if (!drawBm(BM_MSG0 + msgIdx, 2, 0, 160, 18, GxEPD_BLACK)) text(MSG_TXT[msgIdx], 2, 4, 2); }
  else if (batNowPct < 20 && !charging) text("CHARGE ME!", 2, 4, 2);
  else if (!drawBm(BM_TITLE, 2, 0, 160, 18, GxEPD_BLACK)) text("TRUMP-GOCHI", 2, 4, 2);
  bonusShow = false;
  char b[8]; snprintf(b, sizeof b, "%d%%", batNowPct);
  int bx = 196 - (int)strlen(b) * 6;
  text(b, 1, bx, 6);
  if (charging) {                                      // lightning bolt left of the percent
    int x = bx - 9, y = 3;
    display.fillTriangle(x + 5, y, x + 1, y + 7, x + 4, y + 7, GxEPD_BLACK);
    display.fillTriangle(x + 3, y + 6, x + 7, y + 6, x + 2, y + 13, GxEPD_BLACK);
  }
  if (!drawBm(BM_MOOD0 + mood(), 60, 19, 80, 80, GxEPD_BLACK)) drawFace();
  for (int i = 0; i < NSTAT; i++) {                                       // bars 102..165
    int y = 102 + i * 16;
    if (!drawBm(BM_STAT0 + i, 0, y, 40, 11, GxEPD_BLACK)) text(STAT_NAME[i], 1, 2, y + 2);
    display.drawRect(42, y, 126, 11, GxEPD_BLACK);
    int w = (int)(st.v[i] * 1.22f + 0.5f);
    if (w > 0) display.fillRect(44, y + 2, w, 7, GxEPD_BLACK);
    if (i == ENERGY && energyCap() < 100) {            // battery cap: Energy can't pass this line
      int cx = 44 + (int)(energyCap() * 1.22f + 0.5f);
      for (int k = y - 2; k <= y + 12; k += 2) display.drawPixel(cx, k, GxEPD_BLACK);
      display.drawFastVLine(cx + 1, y - 2, 15, GxEPD_BLACK);
    }
    char n[5]; snprintf(n, sizeof n, "%d", (int)(st.v[i] + 0.5f));
    text(n, 1, 172, y + 2);
  }
  for (int i = 0; i < NACT; i++) {                                        // menu 174..199
    int x = i * 50;
    bool on = i == sel;
    if (on) display.fillRect(x + 1, 174, 48, 25, GxEPD_BLACK);
    else display.drawRect(x + 1, 174, 48, 25, GxEPD_BLACK);
    uint16_t c = on ? GxEPD_WHITE : GxEPD_BLACK;
    if (!drawBm(BM_ACT0 + i, x + 2, 175, 46, 23, c)) text(ACT_NAME[i], 1, x + 25 - (int)strlen(ACT_NAME[i]) * 3, 183, c);
  }
  if (cardTag[0]) { text(cardTag, 2, 4, 40); text("press:", 1, 148, 40); text("skip", 1, 148, 50); }   // slide marks beside the face
  full = full || forceFull || (refreshes % FULL_REFRESH_EVERY) == 0;
  refreshes++;
  forceFull = false;
  display.display(!full);
  msgIdx = -1;
}

static void center(const char *t, int size, int y) { text(t, size, (200 - (int)strlen(t) * 6 * size) / 2, y); }

// Age question before the splash: big YES / NO buttons, short = switch, long = confirm. NO -> "SORRY".
static void drawGate(bool full) {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  if (nope) {
    center("SORRY", 4, 50);
    center("Gochi is 21+ only.", 1, 100);
    center("Come back later.", 1, 112);
    center("press: back", 1, 186);
  } else {
    center("ARE YOU", 3, 14);
    center("21+?", 6, 44);
    display.fillRect(40, 100, 120, 2, GxEPD_BLACK);
    center("Satire for adults", 1, 110);
    center("For entertainment only", 1, 122);
    const char *lb[2] = { "YES", "NO" };
    for (int i = 0; i < 2; i++) {
      int x = 4 + i * 98;
      bool on = i == gateSel;
      if (on) display.fillRect(x, 142, 94, 54, GxEPD_BLACK); else { display.drawRect(x, 142, 94, 54, GxEPD_BLACK); display.drawRect(x + 1, 143, 92, 52, GxEPD_BLACK); }
      text(lb[i], 3, x + 47 - (int)strlen(lb[i]) * 9, 158, on ? GxEPD_WHITE : GxEPD_BLACK);
    }
  }
  full = full || forceFull || (refreshes % FULL_REFRESH_EVERY) == 0;
  refreshes++;
  forceFull = false;
  display.display(!full);
}

// Splash before the slides: "Welcome to GOCHI"; after 3 s (or a press) the slides.
static void drawSplash() {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  center("Welcome to", 2, 52);
  center("GOCHI", 5, 74);
  display.fillRect(40, 122, 120, 2, GxEPD_BLACK);
  center("Little Pets With", 1, 134);
  center("Big Ego Game", 1, 146);
  center("press to start", 1, 188);
  refreshes++;
  display.display(false);
}

// Mood bitmap i drawn 2x (160 x 160 for an 80 x 80 mood) centred at cx, top y.
static void drawBm2(int i, int cx, int y) {
  const Bm &b = bms[i];
  int bpr = (b.w + 7) / 8, x0 = cx - b.w;
  for (int j = 0; j < b.h; j++) for (int k = 0; k < b.w; k++)
    if (b.d[j * bpr + (k >> 3)] & (0x80 >> (k & 7))) display.fillRect(x0 + 2 * k, y + 2 * j, 2, 2, GxEPD_BLACK);
}

// Country flag 32 x 20 in the corner of the intro card, by pack id (custom ids: none). 1-bit: blue = black,
// red = 50 % dither, white = white. Same drawing in gochi-web and on the GitHub page.
static const char *flagFor(const char *id) {
  static const char *M[][2] = { {"trump", "US"}, {"biden", "US"}, {"aoc", "US"}, {"zeman", "CZ"}, {"babis", "CZ"},
                                {"putin", "RU"}, {"kim", "KP"}, {"xi", "CN"} };
  for (auto &m : M) if (!strcasecmp(id, m[0])) return m[1];
  return nullptr;
}
static void dither(int x, int y, int w, int h) {
  for (int j = 0; j < h; j++) for (int i = j & 1; i < w; i += 2) display.drawPixel(x + i, y + j, GxEPD_BLACK);
}
static void star(int cx, int cy, float r, uint16_t c) {
  int px[10], py[10];
  for (int i = 0; i < 10; i++) { float rr = (i & 1) ? r * 0.42f : r, a = -PI / 2 + i * PI / 5;
    px[i] = cx + lroundf(cosf(a) * rr); py[i] = cy + lroundf(sinf(a) * rr); }
  for (int i = 0; i < 10; i++) display.fillTriangle(cx, cy, px[i], py[i], px[(i + 1) % 10], py[(i + 1) % 10], c);
}
static void drawFlag(const char *f, int x, int y) {
  const int W = 32, H = 20;
  const uint16_t B = GxEPD_BLACK, Wh = GxEPD_WHITE;
  display.fillRect(x, y, W, H, Wh);
  if (!strcmp(f, "US")) {
    for (int j = 0; j < H; j += 4) display.fillRect(x, y + j, W, 2, B);
    display.fillRect(x, y, 15, 10, B);
    for (int r = 0; r < 2; r++) for (int c = 0; c < 3; c++) display.fillRect(x + 2 + c * 4 + (r ? 2 : 0), y + 2 + r * 4, 2, 2, Wh);
  } else if (!strcmp(f, "CZ")) {
    dither(x, y + 10, W, 10);
    display.fillTriangle(x, y, x + 16, y + 10, x, y + H, B);
  } else if (!strcmp(f, "RU")) {
    display.fillRect(x, y + 7, W, 6, B);
    dither(x, y + 13, W, 7);
  } else if (!strcmp(f, "CN")) {
    dither(x, y, W, H);
    star(x + 7, y + 7, 5, Wh);
    const int sx[4] = { 13, 15, 15, 13 }, sy[4] = { 3, 6, 10, 13 };
    for (int i = 0; i < 4; i++) display.fillRect(x + sx[i], y + sy[i], 2, 2, Wh);
  } else if (!strcmp(f, "KP")) {
    display.fillRect(x, y, W, 3, B); display.fillRect(x, y + H - 3, W, 3, B);
    dither(x, y + 5, W, 10);
    display.fillCircle(x + 10, y + 10, 4, Wh);
    star(x + 10, y + 10, 3.5f, B);
  }
  display.drawRect(x, y, W, H, B);
}

// Slide, two per character: big card (title + big face), then the game screen at its starting values (traits, actions).
// Short = tiles, long = choose.
static void drawWelcome(bool full) {
  int n = 0, k = 0;
  for (int i = 0; i < NSLOT; i++) if (slotExists(i)) { n++; if (i <= cand) k = n; }
  if (!demoGame) {
    display.setFullWindow();
    display.fillScreen(GxEPD_WHITE);
    if (!drawBm(BM_TITLE, 0, 0, 200, 18, GxEPD_BLACK)) center(packId, 2, 2);
    if (bms[BM_MOOD0].w) drawBm2(BM_MOOD0, 100, 20); else drawFace();
    if (const char *f = flagFor(packId)) drawFlag(f, 164, 22);   // top right corner, above the face's hair/hat
    char t[24]; snprintf(t, sizeof t, "%d/%d   press: skip", k, n);
    center(t, 1, 188);
    display.display(!(full || forceFull || (refreshes++ % FULL_REFRESH_EVERY) == 0));
    forceFull = false;
    return;
  }
  State keep = st; int ks = sel; bool kb = bonusShow;
  for (int i = 0; i < NSTAT; i++) st.v[i] = START[i];
  st.zeroSince = 0; sel = -1; msgIdx = -1; bonusShow = false;
  snprintf(cardTag, sizeof cardTag, "%d/%d", k, n);
  draw(full);
  cardTag[0] = 0; st = keep; sel = ks; bonusShow = kb;
}

// Bitmap i at half size (a pixel is black when any of its 2 x 2 source pixels is), top left x,y.
static void drawBmHalf(int i, int x, int y) {
  const Bm &b = bms[i];
  int bpr = (b.w + 7) / 8;
  for (int j = 0; j < b.h; j++) for (int k = 0; k < b.w; k++)
    if (b.d[j * bpr + (k >> 3)] & (0x80 >> (k & 7))) display.drawPixel(x + k / 2, y + j / 2, GxEPD_BLACK);
}

// Welcome: every installed character as a tile (3 per row): small happy face + pack id; the candidate has a thick frame.
static void drawTiles(bool full) {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  int n = 0;
  for (int i = 0; i < NSLOT; i++) {
    if (!slotExists(i) || !loadSlot(i)) continue;
    int x = n % 3 * 66 + 1, y = n / 3 * 62;
    n++;
    for (int k = 0; k < (i == cand ? 3 : 0); k++) display.drawRect(x + k, y + k, 66 - 2 * k, 62 - 2 * k, GxEPD_BLACK);
    if (bms[BM_MOOD0].w) drawBmHalf(BM_MOOD0, x + 33 - bms[BM_MOOD0].w / 4, y + 5);
    char t[11]; int l = 0;
    for (; l < 10 && packId[l]; l++) t[l] = toupper((unsigned char)packId[l]);
    t[l] = 0;
    text(t, 1, x + 33 - l * 3, y + 48);
  }
  loadSlot(cand);                                      // the candidate stays loaded: a long press starts it
  center("short: next  long: choose", 1, 190);
  full = full || forceFull || (refreshes % FULL_REFRESH_EVERY) == 0;
  refreshes++;
  forceFull = false;
  display.display(!full);
}

// Game over: angry face, days survived; long press -> welcome.
static void drawDead(uint32_t now) {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  center("GAME OVER", 3, 4);
  int m = 3;
  if (bms[BM_MOOD0 + m].w) drawBm(BM_MOOD0 + m, 60, 34, 80, 80, GxEPD_BLACK);
  else { State keep = st; st.v[ENERGY] = 50; st.v[BURGERS] = 50; st.v[EGO] = 0; drawFace(); st = keep; }
  char t[32];
  uint32_t days = (st.zeroSince > st.born ? st.zeroSince + DEATH_S - st.born : 0) / 86400;
  snprintf(t, sizeof t, "%lu day%s", (unsigned long)days, days == 1 ? "" : "s");
  if (!drawBm(BM_TITLE, 0, 122, 200, 18, GxEPD_BLACK)) center(packId, 2, 124);
  center(t, 2, 150);
  center("long press: new gochi", 1, 188);
  display.display(false);
  refreshes = 1;
}

static void drawNotice(const char *a, const char *b) {
  display.setFullWindow();
  display.fillScreen(GxEPD_WHITE);
  text(a, 2, 8, 60);
  text(b, 1, 8, 100);
  display.display(false);
  refreshes = 1;
}

// ---------- actions ----------
static void doAction(int a, uint32_t now) {
  for (int i = 0; i < NSTAT; i++) addStat(i, ACT[a][i]);
  if (PAUSE_H[a]) st.sleepUntil = now + PAUSE_H[a] * 3600UL;   // e.g. Sleep: no decay for a while
  msgIdx = a;
  save();
}

// ---------- sleep ----------
RTC_DATA_ATTR static uint32_t sleepAtMs = 0, sleepIdleMs = 0;   // "?" shows why the last sleep happened
static void deepSleep(uint32_t s) {
  display.hibernate();
  for (int pin : { VBAT_PWR, EPD_PWR, EPD_RST, EPD_CS }) gpio_hold_en((gpio_num_t)pin);
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_timer_wakeup(s * 1000000ULL);
  rtc_gpio_pullup_en((gpio_num_t)BTN);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN, 0);
  Serial.flush();
  esp_deep_sleep_start();
}

// ---------- serial ----------
// Replies stay under 64 bytes a line: longer lines come out garbled over HWCDC on this board (seen with core 3.3.12).
static void status(uint32_t now) {
  Serial.printf("OK gochi %s slot %d packs %d dead %d rot %u\n", VERSION, st.slot, packMask(), st.dead, rotDeg);
}
static void details(uint32_t now) {                    // "?S": test details, then "OK end"
  Serial.printf("pack %s\n", packId);
  Serial.printf("stats %.1f %.1f %.1f %.1f\n", st.v[0], st.v[1], st.v[2], st.v[3]);
  Serial.printf("last %lu sleep %lu\n", (unsigned long)st.last, (unsigned long)st.sleepUntil);
  Serial.printf("zero %lu born %lu\n", (unsigned long)st.zeroSince, (unsigned long)st.born);
  Serial.printf("now %lu vbat %d plug %d\n", (unsigned long)now, batMv(), (int)HWCDC::isPlugged());
  Serial.printf("bat %d%% chg %d since %lu ref %d\n", batNowPct, (int)charging, (unsigned long)chgSince, refMv);
  Serial.printf("up %lu lastSleep %lu/%lu\n", (unsigned long)millis(), (unsigned long)sleepAtMs, (unsigned long)sleepIdleMs);
  Serial.printf("demo %d cand %d\n", (int)demo, cand);
  Serial.println("OK end");
}

static bool redraw = false;
static File upFile;
static uint32_t upLen = 0, upGot = 0, upCrc = 0;
static int upSlot = 0;
static void enterWelcome(uint32_t now);
static int hexNib(char c) { return isdigit((unsigned char)c) ? c - '0' : isxdigit((unsigned char)c) ? tolower(c) - 'a' + 10 : -1; }

// PB / PW / PE / PX: character pack upload (see header).
static void handlePack(const char *line, size_t len, uint32_t now) {
  if (line[1] == 'B') {
    char *e;
    long sl = strtol(line + 2, &e, 10);
    if (*e != '.' || sl < 0 || sl >= NSLOT) { Serial.println("ERR use PB<slot 0-7>.<bytes>"); return; }
    long n = strtol(e + 1, nullptr, 10);
    if (n < 46 || n > 32768) { Serial.println("ERR bad pack size"); return; }
    if (upFile) upFile.close();
    upFile = LittleFS.open("/pack.tmp", "w");
    if (!upFile) { Serial.println("ERR flash file"); return; }
    upLen = n; upGot = 0; upCrc = 0xFFFFFFFFUL; upSlot = sl;
    Serial.println("OK pb");
  } else if (line[1] == 'W') {
    static uint8_t buf[512];
    size_t hx = len - 2, k = hx / 2;
    if (!upFile) { Serial.println("ERR no PB"); return; }
    if (hx % 2 || k == 0 || k > sizeof buf || upGot + k > upLen) { Serial.println("ERR bad chunk"); return; }
    for (size_t i = 0; i < k; i++) {
      int a = hexNib(line[2 + 2 * i]), b = hexNib(line[3 + 2 * i]);
      if (a < 0 || b < 0) { Serial.println("ERR bad hex"); return; }
      buf[i] = a << 4 | b;
    }
    if (upFile.write(buf, k) != k) { Serial.println("ERR flash full"); return; }
    upCrc = crc32Upd(upCrc, buf, k);
    upGot += k;
    Serial.printf("OK pw %lu\n", (unsigned long)upGot);
  } else if (line[1] == 'E') {
    if (!upFile) { Serial.println("ERR no PB"); return; }
    upFile.close();
    uint32_t want = strtoul(line + 2, nullptr, 16);
    if (upGot != upLen) { Serial.println("ERR pack incomplete"); return; }
    if ((~upCrc) != want) { Serial.println("ERR crc"); return; }
    const char *e = loadPackFile("/pack.tmp");      // check it (and show it)
    if (e) { Serial.printf("ERR %s\n", e); return; }   // a bad pack leaves the character as it was
    char p[16]; slotPath(upSlot, p);
    LittleFS.remove(p);
    LittleFS.rename("/pack.tmp", p);
    Serial.printf("OK pack %d %s %lu bytes\n", upSlot, packId, (unsigned long)upLen);
    if (st.slot == SLOT_NONE) cand = upSlot;           // welcome: show the new one
    else if (st.slot != upSlot) { loadSlot(st.slot); return; }   // another character (or built-in) is playing: keep it
    forceFull = true; redraw = true;                    // welcome, or the playing gochi got a new look (same game)
  } else if (line[1] == 'X') {
    long sl = strtol(line + 2, nullptr, 10);
    if (sl < 0 || sl >= NSLOT || len < 3) { Serial.println("ERR use PX<slot 0-7>"); return; }
    char p[16]; slotPath(sl, p);
    LittleFS.remove(p);
    Serial.printf("OK pack %ld removed\n", sl);
    if (st.slot == sl || st.slot == SLOT_NONE) enterWelcome(now);
  } else Serial.println("ERR unknown");
}

static void handleSerial(uint32_t now) {
  static char line[1100]; static size_t len = 0;       // a PW line: 2 + 1024 hex
  while (Serial.available()) {
    char c = Serial.read();
    lastSerialMs = millis();
    if (c != '\n' && c != '\r') { if (len < sizeof line - 1) line[len++] = c; continue; }
    line[len] = 0;
    if (!len) continue;
    if (line[0] == 'P' && len >= 2) handlePack(line, len, now);
    else if (!strcmp(line, "?")) status(now);
    else if (!strcmp(line, "?S")) details(now);
    else if (!strcmp(line, "?C")) rtcDump();
    else if (!strcmp(line, "V")) Serial.printf("OK vbat %d mV %d%%\n", batMv(), batPct(batMv()));
    else if (line[0] == 'T') {
      uint32_t t = strtoul(line + 1, nullptr, 10);
      if (t < 1700000000UL) Serial.println("ERR bad time");
      else if (!rtcWrite(t) && (i2cRestart(), !rtcWrite(t))) Serial.println("ERR clock chip not answering");
      else {
        if (st.last > t || t - st.last > 30 * 86400UL) { st.last = t; st.sleepUntil = 0; if (st.zeroSince) st.zeroSince = t; save(); }   // clock was wrong
        Serial.println("OK time set");
      }
    } else if (line[0] == 'S' && len >= 4 && line[2] == '=') {
      static const char *K = "bern";
      const char *k = strchr(K, line[1]);
      int i = k ? (int)(k - K) : -1;
      long v = strtol(line + 3, nullptr, 10);
      if (i < 0 || v < 0 || v > 100) Serial.println("ERR use S<b|e|r|n>=0..100");
      else { st.v[i] = v; catchUp(now); save(); redraw = true; Serial.println("OK stat set"); }
    } else if (line[0] == 'F') {
      float h = atof(line + 1);
      if (h <= 0 || h > 24 * 30) Serial.println("ERR use F<hours>");
      else {                                           // the gochi lived h hours longer: shift its clock back
        uint32_t d = (uint32_t)(h * 3600);
        st.last -= d; st.born -= d;
        if (st.sleepUntil) st.sleepUntil -= d;
        if (st.zeroSince) st.zeroSince -= d;
        catchUp(now); save(); redraw = true; details(now);
      }
    } else if (line[0] == 'R' && len >= 2) {
      long d = strtol(line + 1, nullptr, 10);
      if (d != 0 && d != 90 && d != 180 && d != 270) Serial.println("ERR use R0, R90, R180 or R270");
      else { rotDeg = d; prefs.putUShort("rot", rotDeg); display.setRotation(gfxRotation(rotDeg)); forceFull = true; redraw = true;
             Serial.printf("OK rotation %u\n", rotDeg); }
    } else if (!strcmp(line, "N")) { enterWelcome(now); Serial.println("OK new game"); }
    else Serial.println("ERR unknown");
    len = 0;
  }
}

// ---------- main ----------
static bool clockOk = false;
static uint32_t lastInput = 0, idleLimit = IDLE_MS, lastTickMs = 0, lastPluggedMs = 0;
static bool timerWake = false;

static int nextSlot(int from) {                        // next installed slot after `from` (wraps), -1 = none
  for (int k = 1; k <= NSLOT; k++) { int i = (from + k + NSLOT) % NSLOT; if (slotExists(i)) return i; }
  return -1;
}

// No living gochi: welcome screen with packs, else a new built-in Trump.
static void enterWelcome(uint32_t now) {
  cand = nextSlot(-1);
  if (cand < 0) { loadSlot(SLOT_BUILTIN); newGame(SLOT_BUILTIN, now); addStat(ENERGY, 0); sel = 0; }
  else { loadSlot(cand); st.slot = SLOT_NONE; st.dead = 0; st.ver = 2; }
  demo = splash = gate = cand >= 0; gateSel = 0; nope = false; demoGame = false; demoStartMs = millis();
  save(); forceFull = true; redraw = true;
}

static void show(uint32_t now, bool full) {
  if (st.dead) drawDead(now);
  else if (st.slot == SLOT_NONE) { if (gate) drawGate(full); else if (!demo) drawTiles(full); else if (splash) drawSplash(); else drawWelcome(full); }
  else draw(full);
}

static void onButton(int b, uint32_t now) {
  if (st.dead) { if (b == B_LONG) enterWelcome(now); return; }
  if (st.slot == SLOT_NONE) {
    if (gate) {                                        // 21+: short = switch YES/NO, long = confirm
      if (nope) { nope = false; gateSel = 0; forceFull = true; }
      else if (b == B_SHORT || b == B_DOUBLE) gateSel ^= 1;
      else if (b == B_LONG && gateSel == 1) { nope = true; forceFull = true; }
      else if (b == B_LONG) { gate = false; demo = splash = true; demoGame = false; demoStartMs = millis(); forceFull = true; }
      redraw = true;
      return;
    }
    if (splash) { splash = false; forceFull = true; redraw = true; return; }   // splash: any press starts the slides
    bool slides = demo;
    demo = false;                                      // any press ends the slides; a short one only shows the tiles
    if (slides) forceFull = true;
    if (b == B_SHORT && cand >= 0 && !slides) { int n = nextSlot(cand); if (n >= 0 && n != cand) { cand = n; loadSlot(cand); } }
    else if (b == B_LONG && cand >= 0) { newGame(cand, now); addStat(ENERGY, 0); save(); sel = 0; forceFull = true; }
    redraw = true;
    return;
  }
  catchUp(now);
  if (sel < 0) sel = 0;                                // first press after a timer wake: show the menu
  else if (b == B_SHORT) sel = (sel + 1) % NACT;
  else if (b == B_LONG) doAction(sel, now);
  else if (b == B_DOUBLE) sel = 0;
  if (st.dead) forceFull = true;
  redraw = true;
}

void setup() {
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool woke = cause == ESP_SLEEP_WAKEUP_TIMER || cause == ESP_SLEEP_WAKEUP_EXT0;
  setCpuFrequencyMhz(80);
  pinMode(VBAT_PWR, OUTPUT);
  digitalWrite(VBAT_PWR, HIGH);                        // first: latch battery power
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, LOW);
  pinMode(AUDIO_PWR, OUTPUT);
  digitalWrite(AUDIO_PWR, LOW);                        // before I2C, as in Waveshare's examples
  for (int pin : { VBAT_PWR, EPD_PWR, EPD_RST, EPD_CS }) gpio_hold_dis((gpio_num_t)pin);
  if (cause == ESP_SLEEP_WAKEUP_EXT0) rtc_gpio_deinit((gpio_num_t)BTN);
  pinMode(BTN, INPUT_PULLUP);
  isDown = ignoreHeld = digitalRead(BTN) == LOW;       // the wake press only wakes
  attachInterrupt(BTN, btnIsr, CHANGE);
  Serial.setRxBufferSize(4096);                        // pack lines arrive while the screen refreshes
  Serial.begin(115200);
  if (!woke) delay(100);                               // power-on: let the rails settle before touching the RTC (as booth_qr)
  i2cRecover();
  Wire.begin(I2C_SDA, I2C_SCL);
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  prefs.begin("gochi", false);
  if (!LittleFS.begin(true)) Serial.println("LittleFS failed");   // first start formats the (unused) spiffs partition
  useBuiltin();
  display.init(0, !woke);
  rotDeg = prefs.getUShort("rot", 0);
  if (rotDeg % 90 || rotDeg > 270) rotDeg = 0;
  display.setRotation(gfxRotation(rotDeg));

  uint32_t now = 0;
  clockOk = rtcRead(now);
  if (!clockOk) { drawNotice("SET CLOCK", "USB serial: T<unix time>"); }
  else if (batMv() < BAT_EMPTY_MV && batMv() > 1000) { drawNotice("BATTERY EMPTY", "Connect the USB cable"); deepSleep(WAKE_EVERY_S); }
  else {
    if (!load() || (st.slot >= 0 && !slotExists(st.slot))) enterWelcome(now);
    else if (st.slot == SLOT_NONE) { cand = nextSlot(-1); if (cand < 0) enterWelcome(now); else loadSlot(cand); }
    else loadSlot(st.slot);
    bool wasDead = st.dead;
    batSample(now);                                    // before the display draws (refresh current sags the cell)
    catchUp(now); chargeBonus(now);
    if (st.v[ENERGY] > energyCap()) st.v[ENERGY] = energyCap();
    save();
    if (cause == ESP_SLEEP_WAKEUP_TIMER) {             // redraw only what changed while asleep
      idleLimit = TIMER_WAKE_MS;
      timerWake = true;
      demo = false;
      if (st.slot >= 0 || st.slot == SLOT_BUILTIN) { if (st.dead && !wasDead) drawDead(now); else if (!st.dead) draw(false); }
    } else { sel = 0; show(now, true); }               // button wake / power-on: full refresh
    redraw = false;
  }
  lastInput = lastTickMs = lastPluggedMs = demoAtMs = demoStartMs = millis();
  if (timerWake) lastPluggedMs -= UNPLUGGED_MS - 1000;  // timer wake: only ~1 s to spot a computer
}

void loop() {
  static uint32_t now = 0, lastRtcMs = 0;
  if (!now || millis() - lastRtcMs > 1000) {            // RTC once a second
    lastRtcMs = millis();
    uint32_t t;
    if (rtcRead(t)) now = t;
  }
  if (now && !clockOk) {
    clockOk = true;
    if (!load() || st.slot == SLOT_NONE) enterWelcome(now); else loadSlot(st.slot);
    sel = 0; redraw = true; forceFull = true;
  }
  handleSerial(now);
  if (clockOk) {
    int b = readBtn();
    if (b != B_NONE) { lastInput = millis(); idleLimit = IDLE_MS; onButton(b, now); }
    if (millis() - lastTickMs > 60000) {               // awake: decay once a minute, redraw if a number changed
      lastTickMs = millis();
      int before[NSTAT]; for (int i = 0; i < NSTAT; i++) before[i] = (int)(st.v[i] + 0.5f);
      bool wasDead = st.dead, wasChg = charging; int wasPct = batNowPct;
      batSample(now);
      catchUp(now); chargeBonus(now);
      if (st.v[ENERGY] > energyCap()) st.v[ENERGY] = energyCap();
      for (int i = 0; i < NSTAT; i++) if (before[i] != (int)(st.v[i] + 0.5f)) redraw = true;
      if (wasChg != charging || wasPct != batNowPct || bonusShow || st.zeroSince) redraw = true;   // dying: countdown each minute
      if (st.dead != wasDead) { save(); redraw = true; }
    }
    if (splash && !gate && st.slot == SLOT_NONE && millis() - demoAtMs >= SPLASH_MS && millis() - lastSerialMs > 2000) { splash = false; redraw = true; }   // first slide
    else if (demo && !splash && st.slot == SLOT_NONE && millis() - demoAtMs >= DEMO_STEP_MS && millis() - lastSerialMs > 2000) {   // not during an upload
      if (!demoGame) demoGame = true;                  // big card -> game screen of the same character
      else {
        demoGame = false;
        int n = nextSlot(cand);                        // next character's big card; after the last one: tiles
        if (n <= cand) { demo = false; forceFull = true; }
        if (n >= 0 && n != cand) { cand = n; loadSlot(cand); }
      }
      redraw = true;
    }
    if (redraw) { redraw = false; show(now, false); demoAtMs = millis(); }
  }
  if (HWCDC::isPlugged()) lastPluggedMs = millis();
  if (millis() - lastPluggedMs > UNPLUGGED_MS && !isDown && millis() - lastInput > idleLimit &&
      (!lastSerialMs || millis() - lastSerialMs > SERIAL_AWAKE_MS) && !(demo && millis() - demoStartMs < DEMO_BATT_MS)) {
    if (clockOk) { catchUp(now); save(); }
    if (demo && !gate && clockOk && st.slot == SLOT_NONE) { demo = splash = false; drawTiles(false); }   // slides cut short: sleep on the tiles (the 21+ question stays)
    if (sel >= 0 && clockOk && st.slot != SLOT_NONE && !st.dead) { sel = -1; draw(false); }  // sleep without a highlighted item
    sleepAtMs = millis(); sleepIdleMs = millis() - lastInput;
    deepSleep(st.zeroSince && !st.dead && st.slot != SLOT_NONE ? DYING_WAKE_S : WAKE_EVERY_S);
  }
  delay(5);
}
