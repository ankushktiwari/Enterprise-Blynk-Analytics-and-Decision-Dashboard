/*
  Enterprise Blynk Analytics and Decision Dashboard  (ESP32 + Wokwi)

  Core 1 (Arduino loop):  sensing, fault detection, Task 4 state machine,
                          analytics (trend, risk score), recommendations, LCD/LED/buzzer.
  Core 0 (net task):      Wi-Fi + Blynk state machine, ordered event queue, dashboard writes.
  A blocking Blynk connect() can therefore never delay protection.

  Fill in the three BLYNK_* defines below (see BLYNK_SETUP.md).
  Without credentials the protection and analytics still run; only the dashboard is offline.
*/

#define BLYNK_TEMPLATE_ID "TMPL3vBvZVzrI"
#define BLYNK_TEMPLATE_NAME "eventdrivenelemetry"
#define BLYNK_AUTH_TOKEN "kX2uKNgg_FDGI_TcVZQztB3wSyuSJeVt"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <LiquidCrystal_I2C.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// ============================== configuration ==============================
#define SIM_ADC_NOISE 1                      // +-2 counts, so a frozen ADC is detectable (set 0 on real hardware)
static const char*   WIFI_SSID = "Wokwi-GUEST";
static const char*   WIFI_PASS = "";

// pins
static const uint8_t PIN_CELL[3] = {34, 35, 32};
static const uint8_t PIN_CURR = 33;
static const uint8_t PIN_LED_G = 4, PIN_LED_Y = 5, PIN_LED_R = 2;
static const uint8_t PIN_BUZ = 18, PIN_RELAY = 19;                 // relay is active LOW
static const uint8_t PIN_BTN_INJECT = 25, PIN_BTN_RESET = 26, PIN_BTN_REPORT = 27, PIN_BTN_OUTAGE = 14;

// timing
static const uint32_t SAMPLE_MS = 100;       // protection tick
static const uint32_t ANALYTICS_MS = 1000;   // trend + risk tick
static const uint32_t HOLD_MS = 5000;        // DEGRADED clean hold before NORMAL
static const uint32_t STABLE_MS = 3000;      // FAILSAFE clean time before verification passes
static const uint32_t FAILSAFE_TIMEOUT_MS = 45000;
static const uint8_t  MAX_ATTEMPTS = 3;
static const uint8_t  SET_N = 3, CLR_N = 10; // debounce: 3 bad samples to set, 10 good to clear

// sensing
static const float V_PHYS_MIN = 2.8f, V_PHYS_MAX = 4.4f, I_PHYS_MAX = 10.0f;
static const float CELL_LOW_SET = 3.30f, CELL_LOW_CLR = 3.40f;
static const float CELL_CRIT_LO = 3.00f, CELL_CRIT_LO_CLR = 3.10f;
static const float CELL_CRIT_HI = 4.25f, CELL_CRIT_HI_CLR = 4.20f;
static const float CELL_DEEP_SET = 2.85f, CELL_DEEP_CLR = 2.95f;
static const float I_TRIP = 8.0f, I_CLR = 7.0f;
static const uint8_t FREEZE_N = 20;

// analytics
static const uint32_t FAULT_WINDOW_MS = 300000UL;   // 5 min "recent" window (demo scale; use 24 h in service)
static const float TREND_FULL_MV_PER_MIN = 120.0f;  // demo scale
static const float W_SOC = 0.20f, W_IMB = 0.25f, W_TREND = 0.15f, W_FAULT = 0.25f, W_LOAD = 0.15f;
static const float RAMP_V_PER_S = 0.003f, RAMP_MAX_V = 0.35f;   // IMBAL_RAMP injection

// network / dashboard
static const uint8_t  QCAP = 64;            // 64 x 128 B = 8 KB
static const uint32_t PACE_MS = 125;          // <= 8 writes/s (Blynk guideline is 10)
static const uint32_t CHART_MIN_MS = 2000;
#ifndef PROFILE_DEMO
#define PROFILE_DEMO 1                        // 1 = demo timings (lively charts); 0 = service timings (fits a small message quota)
#endif
#if PROFILE_DEMO
static const uint32_t HEARTBEAT_MS = 60000UL, TEXT_REFRESH_MS = 30000UL;
#else
static const uint32_t HEARTBEAT_MS = 1800000UL, TEXT_REFRESH_MS = 1800000UL;
#endif
static const uint32_t TEXT_MIN_MS = 5000;
static const uint32_t LIVE_WINDOW_MS = 3000;
static const uint32_t INJ_COMMIT_MS = 1200;   // INJECT selection applies after this idle time, so cycling never triggers the modes it passes

// Blynk datastream pins
enum {
  DS_CELL1 = 0, DS_CELL2, DS_CELL3, DS_SOC, DS_IMB, DS_SLOPE, DS_CURRENT, DS_RISK,
  DS_STATE, DS_BANNER, DS_SUMMARY, DS_RECO, DS_JOURNAL, DS_FAULTS, DS_SEQ, DS_QUEUE, DS_CMD
};

// ================================ types ===================================
enum State : uint8_t { S_NORMAL = 0, S_DEGRADED, S_FAILSAFE, S_SHUTDOWN };
enum Ev : uint8_t { E_CLEAR = 0, E_MINOR, E_MAJOR, E_FATAL, E_VERIFIED, E_EXHAUSTED, E_RESET, E_COUNT };
enum Sev : uint8_t { SEV_NONE = 0, SEV_MINOR, SEV_MAJOR, SEV_FATAL };
enum FaultId : uint8_t { F_NONE = 0, F_CELL_LOW, F_CELL_CRIT, F_CELL_DEEP, F_IMBAL, F_OVERCURRENT, F_ADC_FRZ, F_COUNT };
enum Inject : uint8_t { INJ_NONE = 0, INJ_CELL_DROP, INJ_DEEP, INJ_IMBAL, INJ_OVERLOAD, INJ_FREEZE, INJ_COUNT };
enum EvKind : uint8_t { K_TRANS = 0, K_FAULT_SET, K_FAULT_CLR, K_RISK, K_INFO };

static const char* const STATE_NAME[4]  = {"NORMAL", "DEGRADED", "FAILSAFE", "SHUTDOWN"};
static const char* const STATE_ICON[4]  = {"🟢", "🟡", "🔴", "⛔"};
static const char* const STATE_COLOR[4] = {"#23C48E", "#ED9D00", "#D3435C", "#7A0019"};
static const char* const LEVEL_NAME[4]  = {"LOW", "MODERATE", "HIGH", "CRITICAL"};
static const char* const LEVEL_ICON[4]  = {"🟢", "🟡", "🟠", "🔴"};
static const char* const LEVEL_COLOR[4] = {"#23C48E", "#ED9D00", "#FF7A00", "#D3435C"};
static const char* const SEV_NAME[4]    = {"-", "MINOR", "MAJOR", "FATAL"};
static const char* const INJ_NAME[INJ_COUNT] = {"NONE", "CELL_DROP", "DEEP_DISCHARGE", "IMBAL_RAMP", "OVERLOAD", "ADC_FREEZE"};
static const char* const EVT_CODE[4]    = {"degraded", "failsafe", "shutdown", "recovered"};

struct FaultDef { const char* name; const char* src; Sev sev; };
static const FaultDef FDEF[F_COUNT] = {
  {"NONE", "-", SEV_NONE},          {"CELL_LOW", "CELL", SEV_MINOR}, {"CELL_CRIT", "CELL", SEV_MAJOR},
  {"CELL_DEEP", "CELL", SEV_FATAL}, {"IMBAL", "CELL", SEV_MINOR},    {"OVERCURRENT", "LOAD", SEV_MAJOR},
  {"ADC_FRZ", "ADC", SEV_MAJOR}
};

// Task 4 transition table: next state = TRANS[state][event]
static const State TRANS[4][E_COUNT] = {
  /* NORMAL   */ {S_NORMAL,   S_DEGRADED, S_FAILSAFE, S_SHUTDOWN, S_NORMAL,   S_NORMAL,   S_NORMAL},
  /* DEGRADED */ {S_NORMAL,   S_DEGRADED, S_FAILSAFE, S_SHUTDOWN, S_DEGRADED, S_DEGRADED, S_DEGRADED},
  /* FAILSAFE */ {S_FAILSAFE, S_FAILSAFE, S_FAILSAFE, S_SHUTDOWN, S_DEGRADED, S_SHUTDOWN, S_FAILSAFE},
  /* SHUTDOWN */ {S_SHUTDOWN, S_SHUTDOWN, S_SHUTDOWN, S_SHUTDOWN, S_SHUTDOWN, S_SHUTDOWN, S_FAILSAFE}
};

struct Metrics {
  float cell[3]; bool valid[3]; int nValid;
  float minV, maxV, avgV, spreadV, thrV, soc, current;
  int weakest, strongest;
};

struct FaultRt { bool active; uint8_t bad, good; uint16_t count; uint32_t firstMs, lastMs; };
struct FHist { uint32_t t; uint8_t id; };

struct RiskIn { float soc, spreadV, thrV, slopeMvMin, current, faultWeighted; };
struct RiskOut { float rSoc, rImb, rTrend, rFault, rLoad; };

struct EventRec {
  uint32_t tMs, seq;
  uint8_t kind, tier, toState;          // tier: 2 = state transition, 1 = fault set / network boundary, 0 = other
  int8_t evtCode;
  char text[116];
};

struct Snapshot {
  float cell[3], soc, imbMv, slope, current, availPct;
  uint8_t risk, level, state, relay, health, attempts;
  uint16_t faultTotal, faultRecent;
  uint32_t transSeq, sumKey;            // sumKey: quantised digest of the summary, so ADC noise does not cause text writes
  char banner[80], sumHead[96], sumTail[128], reco[320];
};

// ============================== shared state ==============================
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&g_mux)
#define UNLOCK() portEXIT_CRITICAL(&g_mux)

static EventRec g_q[QCAP];
static uint8_t  g_qn = 0;
static volatile uint32_t g_dropped = 0, g_droppedReported = 0, g_droppedTrans = 0, g_droppedTransReported = 0;
static Snapshot g_snap;
static volatile bool g_simOutage = false, g_ackReq = false;
static volatile uint8_t g_netMode = 0;       // 0 OFFLINE, 1 CONNECTING, 2 ONLINE

// ---- protection / analytics state (core 1 only) ----
static State    g_state = S_NORMAL;
static uint32_t g_stateSince = 0, g_transSeq = 0, g_cleanMs = 0, g_stateMs[4] = {0, 0, 0, 0};
static uint8_t  g_attempts = 0;
static Inject   g_inj = INJ_NONE, g_injSel = INJ_NONE;
static uint32_t g_injPressMs = 0;
static bool     g_injPending = false;
static Metrics  g_m;
static FaultRt  g_f[F_COUNT];
static bool     g_condSet[F_COUNT], g_condClr[F_COUNT];
static FHist    g_hist[24];
static uint8_t  g_histN = 0, g_histHead = 0;
static uint16_t g_faultTotal = 0;
static uint8_t  g_lastFault = F_NONE;
static uint32_t g_lastFaultMs = 0;
static int      g_lastRaw[3] = {-1, -1, -1};
static uint8_t  g_same[3] = {0, 0, 0};
static int      g_frozenRaw = -1;
static float    g_rampOff = 0;
static float    g_imbHist[30];
static uint8_t  g_imbN = 0, g_imbHead = 0;
static float    g_slope = 0, g_riskEma = 0;
static uint8_t  g_risk = 0, g_level = 0;
static bool     g_snapDirty = true;

static LiquidCrystal_I2C lcd(0x27, 16, 2);

// ============================== small helpers =============================
static void fmtClock(uint32_t ms, char* b, size_t n) {
  unsigned long s = ms / 1000;
  snprintf(b, n, "%02lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60);
}
static void fmtUptime(uint32_t ms, char* b, size_t n) {
  unsigned long s = ms / 1000;
  snprintf(b, n, "%lud %02lu:%02lu:%02lu", s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
}
static float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static int   iround(float x) { return (int)lroundf(x); }

// ============================ event queue (ordered) =======================
static void qPush(const EventRec& r) {
  LOCK();
  if (g_qn == QCAP) {                       // full: sacrifice the lowest tier, oldest first (never a transition for a lesser record)
    uint8_t minTier = 2;
    for (int i = 0; i < g_qn; i++) if (g_q[i].tier < minTier) minTier = g_q[i].tier;
    if (r.tier < minTier) { g_dropped++; UNLOCK(); return; }       // the new record is the least important: drop it
    int victim = 0;
    for (int i = 0; i < g_qn; i++) if (g_q[i].tier == minTier) { victim = i; break; }
    if (g_q[victim].tier == 2) g_droppedTrans++;
    for (int i = victim; i < g_qn - 1; i++) g_q[i] = g_q[i + 1];
    g_qn--; g_dropped++;
  }
  g_q[g_qn++] = r;
  UNLOCK();
}
static bool qPeek(EventRec* out) {
  bool ok = false;
  LOCK(); if (g_qn) { *out = g_q[0]; ok = true; } UNLOCK();
  return ok;
}
static void qPopFront() {
  LOCK();
  if (g_qn) { for (int i = 0; i < g_qn - 1; i++) g_q[i] = g_q[i + 1]; g_qn--; }
  UNLOCK();
}
static uint8_t qDepth() { uint8_t n; LOCK(); n = g_qn; UNLOCK(); return n; }

static void pushEvent(uint8_t kind, bool critical, int8_t evtCode, uint8_t toState, uint32_t seq, const char* fmt, ...) {
  EventRec r; memset(&r, 0, sizeof r);
  r.tMs = millis(); r.kind = kind; r.tier = critical ? (kind == K_TRANS ? 2 : 1) : 0; r.evtCode = evtCode; r.toState = toState; r.seq = seq;
  va_list ap; va_start(ap, fmt); vsnprintf(r.text, sizeof r.text, fmt, ap); va_end(ap);
  qPush(r);
}

// ================================ analytics ===============================
static float imbThreshold(float soc) { return 0.10f + 0.20f * (1.0f - soc / 100.0f); }   // 0.10 V full .. 0.30 V empty

static float socFromVoltage(float v) {
  static const float V[] = {3.00f, 3.30f, 3.60f, 3.70f, 3.85f, 4.00f, 4.20f};
  static const float P[] = {0, 10, 35, 50, 70, 85, 100};
  if (v <= V[0]) return 0;
  if (v >= V[6]) return 100;
  for (int i = 1; i < 7; i++) if (v <= V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (v - V[i - 1]) / (V[i] - V[i - 1]);
  return 100;
}

// Composite risk, 0..100. Pure function so it can be tested alone.
static float riskCompute(const RiskIn& in, RiskOut* o) {
  RiskOut t;
  t.rSoc   = clamp01((50.0f - in.soc) / 45.0f);                       // 0 at >=50 %, 1 at <=5 %
  float lv = in.thrV > 0 ? in.spreadV / in.thrV : 0;
  t.rImb   = clamp01((lv - 0.3f) / 0.7f);                             // 0 at 30 % of limit, 1 at limit
  t.rTrend = clamp01(in.slopeMvMin / TREND_FULL_MV_PER_MIN)           // rising imbalance only,
           * clamp01(lv / 0.4f);                                      // and only once the spread is meaningful (>= 40 % of the limit)
  t.rFault = clamp01(in.faultWeighted / 8.0f);                        // severity-weighted fault count
  t.rLoad  = clamp01((in.current - 4.0f) / (I_TRIP - 4.0f));          // 0 at 4 A, 1 at trip current
  if (o) *o = t;
  return 100.0f * (W_SOC * t.rSoc + W_IMB * t.rImb + W_TREND * t.rTrend + W_FAULT * t.rFault + W_LOAD * t.rLoad);
}
static float riskFloor(State s) { static const float F[4] = {0, 35, 70, 90}; return F[s]; }   // never contradict the state machine
static uint8_t levelFor(float risk, uint8_t cur) {
  static const float T[3] = {30, 55, 80};
  uint8_t l = cur;
  while (l < 3 && risk >= T[l]) l++;
  while (l > 0 && risk < T[l - 1] - 3.0f) l--;                        // 3-point hysteresis
  return l;
}
static const char* gradeFor(int health) { return health >= 85 ? "A" : health >= 70 ? "B" : health >= 55 ? "C" : health >= 35 ? "D" : "E"; }

static float faultWeighted(uint32_t now) {
  float w = 0;
  for (int i = 0; i < g_histN; i++) {
    if (now - g_hist[i].t > FAULT_WINDOW_MS) continue;
    Sev s = FDEF[g_hist[i].id].sev;
    w += (s == SEV_FATAL ? 4 : s == SEV_MAJOR ? 2 : 1);
  }
  return w;
}
static uint16_t faultWindowCount(int id, uint32_t now) {      // id<0: all faults
  uint16_t n = 0;
  for (int i = 0; i < g_histN; i++) if (now - g_hist[i].t <= FAULT_WINDOW_MS && (id < 0 || g_hist[i].id == id)) n++;
  return n;
}
static void histPush(uint32_t now, uint8_t id) {
  g_hist[g_histHead].t = now; g_hist[g_histHead].id = id;
  g_histHead = (g_histHead + 1) % 24; if (g_histN < 24) g_histN++;
}
static void imbPush(float mv) {
  g_imbHist[g_imbHead] = mv; g_imbHead = (g_imbHead + 1) % 30; if (g_imbN < 30) g_imbN++;
}
static float imbSlopeMvPerMin() {       // least-squares slope over the last <=30 s
  if (g_imbN < 10) return 0;
  float sx = 0, sy = 0, sxy = 0, sxx = 0; int n = g_imbN;
  for (int i = 0; i < n; i++) {
    int idx = (g_imbHead - n + i + 60) % 30;
    float y = g_imbHist[idx];
    sx += i; sy += y; sxy += i * y; sxx += (float)i * i;
  }
  float den = n * sxx - sx * sx;
  return den == 0 ? 0 : 60.0f * (n * sxy - sx * sy) / den;
}
static Sev worstActive() {
  Sev w = SEV_NONE;
  for (int i = 1; i < F_COUNT; i++) if (g_f[i].active && FDEF[i].sev > w) w = FDEF[i].sev;
  return w;
}
static int primaryFault() {
  int best = F_NONE;
  for (int i = 1; i < F_COUNT; i++)
    if (g_f[i].active && (best == F_NONE || FDEF[i].sev > FDEF[best].sev)) best = i;
  return best;
}

// ------------------------ operator recommendations ------------------------
static void addReco(char* out, size_t n, int* cnt, const char* fmt, ...) {
  if (*cnt >= 3) return;
  size_t len = strlen(out);
  if (len + 8 >= n) return;
  va_list ap; va_start(ap, fmt);
  vsnprintf(out + len, n - len, fmt, ap);
  va_end(ap);
  size_t l2 = strlen(out);
  if (l2 + 2 < n) { out[l2] = '\n'; out[l2 + 1] = 0; }
  (*cnt)++;
}

static void buildReco(char* out, size_t n, uint32_t now) {
  out[0] = 0; int cnt = 0;
  const Metrics& m = g_m;
  int pf = primaryFault();
  int w = m.weakest + 1;
  int thrMv = iround(m.thrV * 1000), spreadMv = iround(m.spreadV * 1000);
  if (g_state == S_SHUTDOWN)
    addReco(out, n, &cnt, "⛔ SHUTDOWN%s%s. Load is off. Fix the cause, then press Acknowledge.", pf ? ": " : "", pf ? FDEF[pf].name : "");
  else if (g_state == S_FAILSAFE)
    addReco(out, n, &cnt, "🔴 Load disconnected%s%s. Recovery attempt %u of %u. Do not force the relay closed.", pf ? ": " : "", pf ? FDEF[pf].name : "", g_attempts, MAX_ATTEMPTS);
  if (g_f[F_CELL_DEEP].active || g_f[F_CELL_CRIT].active) {
    if (m.maxV > CELL_CRIT_HI) addReco(out, n, &cnt, "⚡ A cell is at %.2fV (over-voltage). Stop charging and check the charger.", m.maxV);
    else addReco(out, n, &cnt, "🔋 Cell %d is at %.2fV, critically low. Stop discharging and charge now.", w, m.minV);
  }
  if (g_f[F_OVERCURRENT].active) addReco(out, n, &cnt, "🔌 Load current %.1fA is above the %.0fA limit. Reduce the load or look for a short.", m.current, I_TRIP);
  if (g_f[F_ADC_FRZ].active) addReco(out, n, &cnt, "🛠 A cell sensor reading is frozen. Check its wiring before trusting the data.");
  if (g_f[F_IMBAL].active) addReco(out, n, &cnt, "⚖ Cell spread %dmV is above the %dmV limit. Run a balancing charge.", spreadMv, thrMv);
  if (g_f[F_CELL_LOW].active && !g_f[F_CELL_CRIT].active) addReco(out, n, &cnt, "🔋 Cell %d is at %.2fV and low. Charge soon.", w, m.minV);
  if (g_slope > 30.0f && m.spreadV >= 0.4f * m.thrV && !g_f[F_IMBAL].active)
    addReco(out, n, &cnt, "📈 Imbalance is rising +%d mV/min. Schedule balancing before it reaches %dmV.", iround(g_slope), thrMv);
  for (int i = 1; i < F_COUNT; i++) {
    uint16_t c = faultWindowCount(i, now);
    if (c >= 3) { addReco(out, n, &cnt, "🔁 %s tripped %u times recently. Inspect connectors, cell %d and wiring.", FDEF[i].name, c, w); break; }
  }
  if (m.soc < 20.0f && !g_f[F_CELL_LOW].active && !g_f[F_CELL_CRIT].active)
    addReco(out, n, &cnt, "🔋 State of charge is %d%%. Charge soon to avoid deep discharge.", iround(m.soc));
  if (g_level >= 2) addReco(out, n, &cnt, "🧰 Risk is %d (%s). Book a maintenance inspection.", g_risk, LEVEL_NAME[g_level]);
  if (cnt == 0) addReco(out, n, &cnt, "✅ No action needed. Battery is healthy; keep monitoring.");
}

// Rebuild the dashboard snapshot. step=true also advances the 1 Hz trend history.
static void analyticsTick(uint32_t now, bool step) {
  const Metrics& m = g_m;
  if (step) { imbPush(m.spreadV * 1000.0f); g_slope = imbSlopeMvPerMin(); }
  RiskIn in = {m.soc, m.spreadV, m.thrV, g_slope, m.current, faultWeighted(now)};
  RiskOut ro;
  float raw = riskCompute(in, &ro);
  if (step) { float a = raw > g_riskEma ? 0.5f : 0.2f; g_riskEma += a * (raw - g_riskEma); }
  float r = fmaxf(g_riskEma, riskFloor(g_state));
  int ri = iround(r); if (ri < 0) ri = 0; if (ri > 100) ri = 100;
  g_risk = (uint8_t)ri;
  uint8_t lv = levelFor(r, g_level);
  if (lv != g_level) {
    pushEvent(K_RISK, false, -1, g_state, 0, "%s RISK %s->%s (%d) | soc=%d%% imb=%dmV trend=%+dmV/min",
              LEVEL_ICON[lv], LEVEL_NAME[g_level], LEVEL_NAME[lv], g_risk, iround(m.soc), iround(m.spreadV * 1000), iround(g_slope));
    g_level = lv;
  }
  int health = 100 - g_risk;
  uint32_t total = g_stateMs[0] + g_stateMs[1] + g_stateMs[2] + g_stateMs[3];
  float avail = total ? 100.0f * (g_stateMs[S_NORMAL] + g_stateMs[S_DEGRADED]) / total : 100.0f;

  static Snapshot s;                    // static: keeps the stack small
  memset(&s, 0, sizeof s);
  for (int i = 0; i < 3; i++) s.cell[i] = m.cell[i];
  s.soc = m.soc; s.imbMv = m.spreadV * 1000.0f; s.slope = g_slope; s.current = m.current; s.availPct = avail;
  s.risk = g_risk; s.level = g_level; s.state = g_state; s.relay = (g_state <= S_DEGRADED); s.health = (uint8_t)health;
  s.attempts = g_attempts; s.faultTotal = g_faultTotal; s.faultRecent = faultWindowCount(-1, now); s.transSeq = g_transSeq;
  snprintf(s.banner, sizeof s.banner, "%s %s | Relay %s | Load %d%%", STATE_ICON[g_state], STATE_NAME[g_state],
           s.relay ? "ON" : "OFF", g_state == S_NORMAL ? 100 : g_state == S_DEGRADED ? 50 : 0);
  snprintf(s.sumHead, sizeof s.sumHead, "%s %s | Health %d%% (%s) | Risk %d %s %s",
           STATE_ICON[g_state], STATE_NAME[g_state], health, gradeFor(health), g_risk, LEVEL_NAME[g_level], LEVEL_ICON[g_level]);
  const char* arrow = g_slope > 10 ? "↗" : g_slope < -10 ? "↘" : "→";
  snprintf(s.sumTail, sizeof s.sumTail, "Faults %u (recent %u) | Last: %s\nSoC %d%% | Weakest C%d %.2fV | Imb %dmV %s",
           g_faultTotal, s.faultRecent, FDEF[g_lastFault].name, iround(m.soc), m.weakest + 1, m.minV, iround(s.imbMv), arrow);
  buildReco(s.reco, sizeof s.reco, now);
  {                                             // quantised digest: changes only on meaningful differences
    uint32_t h = 2166136261u;
    uint32_t weak = m.spreadV >= 0.2f * m.thrV ? (uint32_t)m.weakest : 3;      // weakest cell is noise until the spread is real
    uint32_t trend = g_slope > 10 ? 2 : g_slope < -10 ? 0 : 1;
    uint32_t parts[7] = {g_state, g_level, (uint32_t)(g_risk / 10), weak, trend, g_faultTotal, (uint32_t)(m.soc / 10.0f)};
    for (int i = 0; i < 7; i++) h = (h ^ parts[i]) * 16777619u;
    s.sumKey = h;
  }
  LOCK(); g_snap = s; UNLOCK();
  g_snapDirty = false;
}

// =============================== state machine ============================
static void setRelay(bool closed) { digitalWrite(PIN_RELAY, closed ? LOW : HIGH); }

static void doTransition(State to, const char* reason, uint32_t now) {
  State from = g_state;
  if (to == S_FAILSAFE) g_attempts++;
  if (to == S_NORMAL) g_attempts = 0;
  if (to >= S_FAILSAFE) setRelay(false);           // open first when failing
  g_state = to; g_stateSince = now; g_cleanMs = 0; g_transSeq++;
  if (to <= S_DEGRADED) setRelay(true);
  int pf = primaryFault();
  int riskNow = g_risk > (int)riskFloor(to) ? g_risk : (int)riskFloor(to);   // risk as it will read after this transition
  char fault[48];
  if (pf) snprintf(fault, sizeof fault, "F%d %s (%s)", pf, FDEF[pf].name, SEV_NAME[FDEF[pf].sev]);
  else snprintf(fault, sizeof fault, "no active fault");
  int8_t code = (to == S_DEGRADED && from == S_NORMAL) ? 0 : (to == S_DEGRADED) ? -1 : (to == S_FAILSAFE) ? 1 : (to == S_SHUTDOWN) ? 2 : 3;
  if (to == S_DEGRADED && from == S_FAILSAFE) code = -1;           // recovery step, not an alert
  pushEvent(K_TRANS, true, code, to, g_transSeq, "%s %s->%s | %s | %s | I=%.1fA SoC=%d%% risk=%d",
            STATE_ICON[to], STATE_NAME[from], STATE_NAME[to], fault, reason, g_m.current, iround(g_m.soc), riskNow);
  char line[220];
  snprintf(line, sizeof line,
           "{\"t_ms\":%lu,\"ev\":\"STATE\",\"seq\":%lu,\"from\":\"%s\",\"to\":\"%s\",\"fault_id\":%d,\"fault\":\"%s\",\"reason\":\"%s\",\"risk\":%d}",
           (unsigned long)now, (unsigned long)g_transSeq, STATE_NAME[from], STATE_NAME[to], pf, FDEF[pf].name, reason, riskNow);
  Serial.println(line);
  g_snapDirty = true;
}

static void applyEvent(Ev ev, const char* reason, uint32_t now) {
  if (ev == E_RESET) g_attempts = 0;               // operator intervention restarts the retry budget
  State to = TRANS[g_state][ev];
  if (to != g_state) doTransition(to, reason, now);
}

static void operatorAck(const char* src, uint32_t now) {
  if (g_state == S_SHUTDOWN) {
    char r[40]; snprintf(r, sizeof r, "operator reset (%s)", src);
    applyEvent(E_RESET, r, now);
  } else {
    pushEvent(K_INFO, false, -1, g_state, 0, "ACK from %s ignored: state is %s", src, STATE_NAME[g_state]);
  }
}

// ================================= faults =================================
static void describeFault(int id, char* b, size_t n) {
  const Metrics& m = g_m;
  switch (id) {
    case F_CELL_LOW: case F_CELL_CRIT: case F_CELL_DEEP: snprintf(b, n, "cell%d=%.2fV max=%.2fV", m.weakest + 1, m.minV, m.maxV); break;
    case F_IMBAL: snprintf(b, n, "spread=%dmV limit=%dmV", iround(m.spreadV * 1000), iround(m.thrV * 1000)); break;
    case F_OVERCURRENT: snprintf(b, n, "I=%.1fA limit=%.0fA", m.current, I_TRIP); break;
    case F_ADC_FRZ: snprintf(b, n, "same reading x%u/%u/%u", g_same[0], g_same[1], g_same[2]); break;
    default: snprintf(b, n, "-");
  }
}

static void evalFaults(uint32_t now) {
  const Metrics& m = g_m;
  bool any = m.nValid > 0;
  g_condSet[F_CELL_LOW]  = any && m.minV < CELL_LOW_SET;
  g_condClr[F_CELL_LOW]  = !any || m.minV > CELL_LOW_CLR;
  g_condSet[F_CELL_CRIT] = any && (m.minV < CELL_CRIT_LO || m.maxV > CELL_CRIT_HI);
  g_condClr[F_CELL_CRIT] = !any || (m.minV > CELL_CRIT_LO_CLR && m.maxV < CELL_CRIT_HI_CLR);
  g_condSet[F_CELL_DEEP] = any && m.minV < CELL_DEEP_SET;
  g_condClr[F_CELL_DEEP] = !any || m.minV > CELL_DEEP_CLR;
  g_condSet[F_IMBAL]     = m.nValid >= 2 && m.spreadV > m.thrV;
  g_condClr[F_IMBAL]     = m.nValid < 2 || m.spreadV < 0.8f * m.thrV;
  g_condSet[F_OVERCURRENT] = m.current > I_TRIP;
  g_condClr[F_OVERCURRENT] = m.current < I_CLR;
  bool fz = false, fzClr = true;
  for (int i = 0; i < 3; i++) { if (g_same[i] >= FREEZE_N) fz = true; if (g_same[i] >= 2) fzClr = false; }
  g_condSet[F_ADC_FRZ] = fz; g_condClr[F_ADC_FRZ] = fzClr;

  for (int i = 1; i < F_COUNT; i++) {
    FaultRt& f = g_f[i];
    if (!f.active) {
      if (g_condSet[i]) { if (++f.bad >= SET_N) {
        f.active = true; f.bad = 0; f.good = 0; f.count++; if (f.count == 1) f.firstMs = now; f.lastMs = now;
        g_faultTotal++; histPush(now, i); g_snapDirty = true;
        // "last fault" = the most severe fault of a burst, so a deep discharge is not masked by the imbalance it causes
        if (g_lastFault == F_NONE || FDEF[i].sev >= FDEF[g_lastFault].sev || now - g_lastFaultMs > 2000) g_lastFault = i;
        g_lastFaultMs = now;
        char d[48]; describeFault(i, d, sizeof d);
        pushEvent(K_FAULT_SET, true, -1, g_state, 0, "FAULT SET F%d %s (%s) | %s", i, FDEF[i].name, SEV_NAME[FDEF[i].sev], d);
        char line[140];
        snprintf(line, sizeof line, "{\"t_ms\":%lu,\"ev\":\"FAULT\",\"op\":\"set\",\"fault_id\":%d,\"fault\":\"%s\",\"source\":\"%s\",\"sev\":\"%s\"}",
                 (unsigned long)now, i, FDEF[i].name, FDEF[i].src, SEV_NAME[FDEF[i].sev]);
        Serial.println(line);
      } } else f.bad = 0;
    } else {
      if (g_condClr[i]) { if (++f.good >= CLR_N) {
        f.active = false; f.good = 0; f.bad = 0; g_snapDirty = true;
        pushEvent(K_FAULT_CLR, false, -1, g_state, 0, "FAULT CLR F%d %s after %lus", i, FDEF[i].name, (unsigned long)((now - f.lastMs) / 1000));
      } } else f.good = 0;
    }
  }
}

// ================================ sensing =================================
static int adcRead(uint8_t pin) {
  int r = analogRead(pin);
#if SIM_ADC_NOISE
  r += (int)random(-2, 3);
#endif
  return r < 0 ? 0 : (r > 4095 ? 4095 : r);
}

static void readSensors(uint32_t dt) {
  Metrics& m = g_m;
  for (int i = 0; i < 3; i++) {
    int raw = adcRead(PIN_CELL[i]);
    if (g_inj == INJ_FREEZE && i == 0) { if (g_frozenRaw < 0) g_frozenRaw = raw; raw = g_frozenRaw; }
    if (raw == g_lastRaw[i]) { if (g_same[i] < 250) g_same[i]++; } else g_same[i] = 0;
    g_lastRaw[i] = raw;
    float v = V_PHYS_MIN + raw * (V_PHYS_MAX - V_PHYS_MIN) / 4095.0f;
    if (g_inj == INJ_CELL_DROP && i == 1) v = 2.95f;
    if (g_inj == INJ_DEEP && i == 1) v = 2.80f;
    if (g_inj == INJ_IMBAL && i == 2) v = fmaxf(2.9f, v - g_rampOff);
    m.cell[i] = v;
  }
  if (g_inj != INJ_FREEZE) g_frozenRaw = -1;
  if (g_inj == INJ_IMBAL) g_rampOff = fminf(RAMP_MAX_V, g_rampOff + RAMP_V_PER_S * dt / 1000.0f); else g_rampOff = 0;

  int rawI = adcRead(PIN_CURR);
  m.current = rawI * I_PHYS_MAX / 4095.0f;
  if (g_inj == INJ_OVERLOAD) m.current = 9.2f;

  m.nValid = 0; m.minV = 99; m.maxV = -99; m.avgV = 0; m.weakest = 0; m.strongest = 0;
  for (int i = 0; i < 3; i++) {                    // a frozen channel is isolated: its stale value is not used
    m.valid[i] = g_same[i] < FREEZE_N;
    if (!m.valid[i]) continue;
    m.nValid++; m.avgV += m.cell[i];
    if (m.cell[i] < m.minV) { m.minV = m.cell[i]; m.weakest = i; }
    if (m.cell[i] > m.maxV) { m.maxV = m.cell[i]; m.strongest = i; }
  }
  if (m.nValid == 0) { m.minV = m.maxV = m.avgV = m.cell[0]; m.spreadV = 0; }
  else { m.avgV /= m.nValid; m.spreadV = m.maxV - m.minV; }
  m.soc = socFromVoltage(m.avgV);
  m.thrV = imbThreshold(m.soc);
}

static void sampleTick(uint32_t now, uint32_t dt) {
  readSensors(dt);
  evalFaults(now);
  g_stateMs[g_state] += dt;
  if (g_ackReq) { g_ackReq = false; operatorAck("dashboard", now); }

  Sev worst = worstActive();
  if (worst == SEV_NONE) g_cleanMs += dt; else g_cleanMs = 0;
  int ev = -1; const char* why = "";
  if (worst == SEV_FATAL) { ev = E_FATAL; why = "fatal fault"; }
  else if (worst == SEV_MAJOR) { ev = E_MAJOR; why = "major fault"; }
  else if (worst == SEV_MINOR) { ev = E_MINOR; why = "minor fault"; }
  else if (g_state == S_DEGRADED && g_cleanMs >= HOLD_MS) { ev = E_CLEAR; why = "clean hold complete"; }
  else if (g_state == S_FAILSAFE && g_cleanMs >= STABLE_MS) { ev = E_VERIFIED; why = "stable, verification passed"; }
  if (g_state == S_FAILSAFE && ev != E_FATAL) {
    if (g_attempts > MAX_ATTEMPTS) { ev = E_EXHAUSTED; why = "retry limit reached"; }
    else if (now - g_stateSince >= FAILSAFE_TIMEOUT_MS) { ev = E_EXHAUSTED; why = "failsafe timeout"; }
  }
  if (ev >= 0) applyEvent((Ev)ev, why, now);
}

// =============================== UI / buttons =============================
struct Btn { uint8_t pin; bool last; uint32_t t; };
static Btn g_btn[4] = {{PIN_BTN_INJECT, true, 0}, {PIN_BTN_RESET, true, 0}, {PIN_BTN_REPORT, true, 0}, {PIN_BTN_OUTAGE, true, 0}};

static void printReport(uint32_t now) {
  static Snapshot s; LOCK(); s = g_snap; UNLOCK();
  char up[24]; fmtUptime(now, up, sizeof up);
  Serial.println("---- EXECUTIVE SUMMARY ----");
  Serial.println(s.sumHead);
  char l[96]; snprintf(l, sizeof l, "Uptime %s | Availability %.1f%%", up, s.availPct); Serial.println(l);
  Serial.println(s.sumTail);
  Serial.println("---- RECOMMENDATIONS ----");
  Serial.println(s.reco);
  Serial.println("---- FAULT REGISTRY ----");
  for (int i = 1; i < F_COUNT; i++) if (g_f[i].count) {
    char a[24], b[24]; fmtClock(g_f[i].firstMs, a, sizeof a); fmtClock(g_f[i].lastMs, b, sizeof b);
    snprintf(l, sizeof l, "F%d %s x%u first %s last %s", i, FDEF[i].name, g_f[i].count, a, b);
    Serial.println(l);
    pushEvent(K_INFO, false, -1, g_state, 0, "REGISTRY %s", l);
  }
  pushEvent(K_INFO, false, -1, g_state, 0, "REPORT requested | faults=%u transitions=%lu", g_faultTotal, (unsigned long)g_transSeq);
}

static void pollButtons(uint32_t now) {
  for (int i = 0; i < 4; i++) {
    bool lvl = digitalRead(g_btn[i].pin);
    if (lvl != g_btn[i].last && now - g_btn[i].t > 30) {
      g_btn[i].t = now; g_btn[i].last = lvl;
      if (lvl) continue;                           // act on press only (active LOW)
      if (i == 0) { g_injSel = (Inject)((g_injSel + 1) % INJ_COUNT); g_injPressMs = now; g_injPending = true; }
      if (i == 1) operatorAck("button", now);
      if (i == 2) printReport(now);
      if (i == 3) { g_simOutage = !g_simOutage; pushEvent(K_INFO, false, -1, g_state, 0, "SIM OUTAGE %s", g_simOutage ? "ON" : "OFF"); }
    }
  }
}

static void injectTick(uint32_t now) {            // commit a selected fault injection once the button has been idle
  if (!g_injPending || now - g_injPressMs < INJ_COMMIT_MS) return;
  g_injPending = false;
  if (g_injSel != g_inj) { g_inj = g_injSel; pushEvent(K_INFO, false, -1, g_state, 0, "INJECT -> %s", INJ_NAME[g_inj]); }
}

static void outputsTick(uint32_t now) {
  bool blink = (now / 250) % 2, slow = (now / 500) % 2;
  bool g = g_state == S_NORMAL, y = g_state == S_DEGRADED;
  bool r = g_state == S_SHUTDOWN || (g_state == S_FAILSAFE && blink);
  digitalWrite(PIN_LED_G, g); digitalWrite(PIN_LED_Y, y); digitalWrite(PIN_LED_R, r);
  bool buz = (g_state == S_FAILSAFE && (now / 1000) % 2 == 0 && slow) || (g_state == S_SHUTDOWN && blink);
  static bool lastBuz = false;
  if (buz != lastBuz) { if (buz) tone(PIN_BUZ, 2000); else noTone(PIN_BUZ); lastBuz = buz; }
}

static char g_lcdLast[2][17];
static void lcdLine(uint8_t row, const char* txt) {      // write only when a line changed (no lcd.clear())
  char b[17]; snprintf(b, sizeof b, "%-16.16s", txt);
  if (strcmp(b, g_lcdLast[row]) != 0) { lcd.setCursor(0, row); lcd.print(b); strcpy(g_lcdLast[row], b); }
}
static void lcdTick(uint32_t now) {
  static uint32_t t = 0; if (now - t < 250) return; t = now;
  char a[24], b[24];
  snprintf(a, sizeof a, "%-8s R%-3d %-3.3s", STATE_NAME[g_state], g_risk, LEVEL_NAME[g_level]);
  static const char* NET[3] = {"OFF", "...", "ON"};
  snprintf(b, sizeof b, "S%-3d F%-2u Q%-2u %-3s", iround(g_m.soc), g_faultTotal, qDepth(), NET[g_netMode > 2 ? 0 : g_netMode]);
  lcdLine(0, a); lcdLine(1, b);
}

// ============================ network task (core 0) =======================
enum NetState { NET_IDLE, NET_WIFI, NET_BLYNK, NET_ONLINE };
static NetState g_netState = NET_IDLE;
static uint32_t g_netNextTry = 0, g_backoff = 1000, g_wifiStart = 0, g_downSince = 0, g_lastWrite = 0;
static bool g_blynkCfg = false, g_everOnline = false;

struct NumStream { uint8_t pin; float db; bool hb; bool sent; float last; uint32_t lastMs; };
static const int NS_SEQ = 10;
static NumStream g_ns[11] = {
  {DS_CELL1, 0.02f, true, false, 0, 0}, {DS_CELL2, 0.02f, true, false, 0, 0}, {DS_CELL3, 0.02f, true, false, 0, 0},
  {DS_SOC, 1.0f, true, false, 0, 0}, {DS_IMB, 5.0f, true, false, 0, 0}, {DS_SLOPE, 3.0f, true, false, 0, 0},
  {DS_CURRENT, 0.3f, true, false, 0, 0}, {DS_RISK, 2.0f, true, false, 0, 0},
  {DS_FAULTS, 0.5f, false, false, 0, 0}, {DS_QUEUE, 0.5f, false, false, 0, 0}, {DS_SEQ, 0.5f, false, false, 0, 0}
};
static int  g_lastState = -1, g_lastLevel = -1;
static char g_lastBanner[80], g_lastReco[320];
static uint32_t g_lastSumKey = 0;
static uint32_t g_lastSumMs = 0, g_lastRecoMs = 0;

static void invalidateLast() {
  for (auto& s : g_ns) s.sent = false;
  g_lastState = -1; g_lastLevel = -1; g_lastBanner[0] = g_lastReco[0] = 0; g_lastSumKey = 0; g_lastSumMs = g_lastRecoMs = 0;
}
static void paceWait() { while (millis() - g_lastWrite < PACE_MS) vTaskDelay(pdMS_TO_TICKS(5)); g_lastWrite = millis(); }
static void bwF(int pin, float v) { paceWait(); Blynk.virtualWrite(pin, v); }
static void bwI(int pin, int v) { paceWait(); Blynk.virtualWrite(pin, v); }
static void bwS(int pin, const char* v) { paceWait(); Blynk.virtualWrite(pin, v); }
static void bwP(int pin, const char* prop, const char* v) { paceWait(); Blynk.setProperty(pin, prop, v); }

static void sendRecord(const EventRec& r, uint32_t now) {
  static char line[220];
  uint32_t dropped = g_dropped, droppedT = g_droppedTrans;
  if (dropped > g_droppedReported) {
    snprintf(line, sizeof line, "[GAP] ⚠ %lu record(s) dropped while the queue was full (%lu state transition(s) lost)\n",
             (unsigned long)(dropped - g_droppedReported), (unsigned long)(droppedT - g_droppedTransReported));
    bwS(DS_JOURNAL, line); g_droppedReported = dropped; g_droppedTransReported = droppedT;
  }
  char tag[16], clk[16];
  uint32_t age = now - r.tMs;
  if (age <= LIVE_WINDOW_MS) snprintf(tag, sizeof tag, "[LIVE]"); else snprintf(tag, sizeof tag, "[Q -%lus]", (unsigned long)(age / 1000));
  fmtClock(r.tMs, clk, sizeof clk);
  if (r.kind == K_TRANS) snprintf(line, sizeof line, "%s %s #%03lu %s\n", tag, clk, (unsigned long)r.seq, r.text);
  else snprintf(line, sizeof line, "%s %s %s\n", tag, clk, r.text);
  bwS(DS_JOURNAL, line);
  if (r.kind == K_TRANS) {
    bwI(DS_SEQ, (int)r.seq);
    g_ns[NS_SEQ].last = (float)r.seq; g_ns[NS_SEQ].sent = true; g_ns[NS_SEQ].lastMs = millis();
    bwI(DS_STATE, r.toState);
    if (r.evtCode >= 0) { paceWait(); Blynk.logEvent(EVT_CODE[(int)r.evtCode], r.text); }
  }
}

static bool sameShape(const char* a, const char* b) {      // equal ignoring digits
  for (;;) {
    while (*a >= '0' && *a <= '9') a++;
    while (*b >= '0' && *b <= '9') b++;
    if (*a != *b) return false;
    if (!*a) return true;
    a++; b++;
  }
}

static void sendSnapshot(uint32_t now) {
  static Snapshot s; LOCK(); s = g_snap; UNLOCK();
  bool stateChanged = (int)s.state != g_lastState;
  float v[11] = {s.cell[0], s.cell[1], s.cell[2], s.soc, s.imbMv, s.slope, s.current, (float)s.risk, (float)s.faultTotal, (float)qDepth(), (float)s.transSeq};
  // 1) state first: banner, code, colour
  if (stateChanged) {
    bwI(DS_STATE, s.state); if (qDepth()) return;
    bwS(DS_BANNER, s.banner); strcpy(g_lastBanner, s.banner); if (qDepth()) return;
    bwP(DS_STATE, "color", STATE_COLOR[s.state]); if (qDepth()) return;
    bwP(DS_BANNER, "color", STATE_COLOR[s.state]); if (qDepth()) return;
    g_lastState = s.state;
  }
  if ((int)s.level != g_lastLevel) {
    bwP(DS_RISK, "color", LEVEL_COLOR[s.level]); if (qDepth()) return;
    g_lastLevel = s.level;
  }
  // 2) numeric streams: deadband + heartbeat
  for (int i = 0; i < 11; i++) {
    NumStream& n = g_ns[i];
    bool due = !n.sent;
    if (!due && fabsf(v[i] - n.last) >= n.db && now - n.lastMs >= CHART_MIN_MS) due = true;
    if (!due && n.hb && now - n.lastMs >= HEARTBEAT_MS) due = true;
    if (!due) continue;
    if (n.pin == DS_FAULTS || n.pin == DS_RISK || n.pin == DS_QUEUE || n.pin == DS_SEQ) bwI(n.pin, iround(v[i])); else bwF(n.pin, v[i]);
    n.sent = true; n.last = v[i]; n.lastMs = millis();
    if (qDepth()) return;
  }
  // 3) text: executive summary and recommendations
  char uptime[24]; fmtUptime(now, uptime, sizeof uptime);
  bool sumChanged = s.sumKey != g_lastSumKey;
  if ((sumChanged && (stateChanged || now - g_lastSumMs >= TEXT_MIN_MS)) || now - g_lastSumMs >= TEXT_REFRESH_MS) {
    static char full[400];
    snprintf(full, sizeof full, "%s\n⏱ Up %s | Availability %.1f%%\n%s", s.sumHead, uptime, s.availPct, s.sumTail);
    bwS(DS_SUMMARY, full); g_lastSumKey = s.sumKey; g_lastSumMs = millis();
    if (qDepth()) return;
  }
  bool recoChanged = !sameShape(s.reco, g_lastReco);   // digits drift with noise; numbers refresh on the slow interval
  if ((recoChanged && (stateChanged || now - g_lastRecoMs >= TEXT_MIN_MS)) || now - g_lastRecoMs >= TEXT_REFRESH_MS) {
    bwS(DS_RECO, s.reco); strcpy(g_lastReco, s.reco); g_lastRecoMs = millis();
  }
}

static void netFail(uint32_t now) {
  g_netState = NET_IDLE; g_netNextTry = now + g_backoff;
  g_backoff = g_backoff * 2 > 30000 ? 30000 : g_backoff * 2;
}

static void netGoDown(uint32_t now) {
  g_netState = NET_IDLE; g_netMode = 0;
  if (!g_downSince) {                              // critical record: outage boundaries are never dropped
    g_downSince = now ? now : 1;
    pushEvent(K_INFO, true, -1, g_state, 0, "NET DOWN");
  }
}

static void netStep(uint32_t now) {
  if (g_simOutage) {                               // simulated outage: drop the cloud link, do not retry
    if (Blynk.connected()) Blynk.disconnect();
    if (g_netState != NET_IDLE || !g_downSince) netGoDown(now);
    g_backoff = 1000; g_netNextTry = now;
    return;
  }
  switch (g_netState) {
    case NET_IDLE:
      g_netMode = 0;
      if (now < g_netNextTry) return;
      if (WiFi.status() == WL_CONNECTED) g_netState = NET_BLYNK;
      else { WiFi.begin(WIFI_SSID, WIFI_PASS, 6); g_wifiStart = now; g_netState = NET_WIFI; }
      break;
    case NET_WIFI:
      g_netMode = 1;
      if (WiFi.status() == WL_CONNECTED) g_netState = NET_BLYNK;
      else if (now - g_wifiStart > 15000) { WiFi.disconnect(); netFail(now); }
      break;
    case NET_BLYNK:
      g_netMode = 1;
      if (!g_blynkCfg) { Blynk.config(BLYNK_AUTH_TOKEN); g_blynkCfg = true; }
      if (Blynk.connect(4000)) {                   // may block up to 4 s - harmless, this is the net task
        g_netState = NET_ONLINE; g_netMode = 2; g_backoff = 1000;
        if (g_downSince) pushEvent(K_INFO, true, -1, g_state, 0, "NET UP after %lus (%u queued)", (unsigned long)((millis() - g_downSince) / 1000), qDepth());
        else pushEvent(K_INFO, false, -1, g_state, 0, "NET ONLINE");
        g_downSince = 0; g_everOnline = true; invalidateLast();
      } else netFail(millis());
      break;
    case NET_ONLINE:
      g_netMode = 2;
      if (!Blynk.connected()) { netGoDown(now); netFail(now); }
      break;
  }
}

static void netIteration() {
  uint32_t now = millis();
  netStep(now);
  if (g_netState != NET_ONLINE) return;
  Blynk.run();
  EventRec r;
  if (qPeek(&r)) {                                 // oldest first; pop only after a successful send
    sendRecord(r, millis());
    if (Blynk.connected()) qPopFront();
  } else sendSnapshot(millis());
}

BLYNK_WRITE(V16) { if (param.asInt() == 1) g_ackReq = true; }     // operator Acknowledge button (DS_CMD)

static void netTaskEntry(void*) { for (;;) { netIteration(); vTaskDelay(pdMS_TO_TICKS(10)); } }

// ================================ setup/loop ==============================
void setup() {
  Serial.begin(115200);
  for (int i = 0; i < 3; i++) pinMode(PIN_CELL[i], INPUT);
  pinMode(PIN_CURR, INPUT);
  pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_Y, OUTPUT); pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_BUZ, OUTPUT); pinMode(PIN_RELAY, OUTPUT);
  for (int i = 0; i < 4; i++) pinMode(g_btn[i].pin, INPUT_PULLUP);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  setRelay(true);
  lcd.init(); lcd.backlight();
  lcdLine(0, "BMS Analytics"); lcdLine(1, "starting...");
  uint32_t now = millis();
  g_stateSince = now;
  readSensors(SAMPLE_MS);
  g_m.soc = socFromVoltage(g_m.avgV);
  pushEvent(K_INFO, true, -1, g_state, 0, "BOOT | NORMAL | Task 4 state machine, risk model v1");
  analyticsTick(now, true);
  Serial.println("{\"ev\":\"BOOT\",\"fw\":\"bms-analytics-1.0\"}");
  xTaskCreatePinnedToCore(netTaskEntry, "net", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  static uint32_t tS = 0, tA = 0;
  uint32_t now = millis();
  pollButtons(now);
  injectTick(now);
  if (now - tS >= SAMPLE_MS) { uint32_t dt = now - tS; tS = now; sampleTick(now, dt); }
  if (now - tA >= ANALYTICS_MS) { tA = now; analyticsTick(now, true); }
  else if (g_snapDirty) analyticsTick(now, false);
  outputsTick(now);
  lcdTick(now);
}
