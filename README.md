# Enterprise Blynk Analytics and Decision Dashboard

An ESP32 firmware that turns a battery-protection system into an **operator dashboard**: historical trends, a composite **risk score**, a structured **fault journal**, plain-language **maintenance advice**, and an **executive summary** (health, uptime, fault count, current state). Severity colours and icons follow the Task 4 state machine (NORMAL / DEGRADED / FAILSAFE / SHUTDOWN), and every state transition in the backend is delivered to the dashboard, in order, even across a cloud outage.

It continues the earlier projects: adaptive imbalance limit from the BMS repo, the transition table and staged recovery from the Fault State Machine repo, and the queue-and-replay idea from the telemetry repo. The one deliberate change is that **networking runs on its own core**, so a blocking Blynk connect can never delay protection.

> **Status.** The firmware compiles and passes the PC test suite in this folder (details below). It has **not** been run on Wokwi or against a live Blynk account, and `diagram.json` is hand-written. See [Limitations](#limitations).
> demo video <https://lnkd.in/p/gRZie87n>
> live procect <https://wokwi.com/projects/477764909826222081> <https://blynk.cloud/dashboard/773467/global/devices/308601/organization/773467/devices/2320698/dashboard>

## Files

| File | Purpose |
|------|---------|
| `sketch.ino` | firmware (protection + analytics on core 1, network task on core 0) |
| `diagram.json`, `libraries.txt` | Wokwi project |
| `BLYNK_SETUP.md` | template, 17 datastreams, events, widget layout |
| `REPORT.md` | design decisions, measurements, test results, issues found |


## Hardware (simulated)

| Part | Pin |
|------|-----|
| Cell 1 / 2 / 3 potentiometers (2.8 – 4.4 V) | GPIO 34 / 35 / 32 |
| Load current potentiometer (0 – 10 A) | GPIO 33 |
| LEDs green / yellow / red | GPIO 4 / 5 / 2 |
| Buzzer, relay (active LOW) | GPIO 18, 19 |
| LCD 16x2 I2C | GPIO 21 (SDA), 22 (SCL) |
| Buttons INJECT / RESET / REPORT / OUTAGE (to GND) | GPIO 25 / 26 / 27 / 14 |

All analog inputs are on ADC1, so they keep working while Wi-Fi is active.

## What the dashboard shows

| Requirement | Where |
|-------------|-------|
| Time-series graphs | V0–V2 cells, V3 SoC, V4 imbalance, V5 trend, V6 current, V7 risk, V8 state |
| Composite risk score | V7 (0–100), colour by level |
| Structured fault history | V12 journal, V13 fault count, Blynk events |
| Operator recommendations | V11, up to 3 prioritised lines from live data |
| Executive summary | V10: health %, grade, risk, uptime, availability, faults, SoC, weakest cell |
| Severity colours/icons | 🟢 NORMAL · 🟡 DEGRADED · 🔴 FAILSAFE · ⛔ SHUTDOWN (V8/V9 colour property) |
| Every transition reflected | ordered queue + sequence number V14 (see below) |

Real output captured from the simulation:

```
🟡 DEGRADED | Health 57% (C) | Risk 43 MODERATE 🟡
⏱ Up 0d 00:01:36 | Availability 100.0%
Faults 1 (recent 1) | Last: IMBAL
SoC 53% | Weakest C3 3.58V | Imb 223mV ↗

⚖ Cell spread 223mV is above the 193mV limit. Run a balancing charge.
```

```
🔴 FAILSAFE | Health 30% (E) | Risk 70 HIGH 🟠
...
🔴 Load disconnected: OVERCURRENT. Recovery attempt 1 of 3. Do not force the relay closed.
🔌 Load current 9.2A is above the 8A limit. Reduce the load or look for a short.
🧰 Risk is 70 (HIGH). Book a maintenance inspection.
```

Journal (Terminal widget):

```
[LIVE] 00:01:25 FAULT SET F4 IMBAL (MINOR) | spread=191mV limit=190mV
[LIVE] 00:01:25 #001 🟡 NORMAL->DEGRADED | F4 IMBAL (MINOR) | minor fault | I=2.0A SoC=55% risk=40
[LIVE] 00:01:44 #002 🟢 DEGRADED->NORMAL | no active fault | clean hold complete | I=2.0A SoC=63% risk=35
[LIVE] 00:01:46 #003 🔴 NORMAL->FAILSAFE | F5 OVERCURRENT (MAJOR) | major fault | I=9.2A SoC=63% risk=70
[LIVE] 00:02:00 #006 ⛔ NORMAL->SHUTDOWN | F3 CELL_DEEP (FATAL) | fatal fault | I=2.0A SoC=24% risk=90
[LIVE] 00:02:07 #007 🔴 SHUTDOWN->FAILSAFE | no active fault | operator reset (button) | ...
```

Lines are tagged `[LIVE]` if sent within 3 s of happening, otherwise `[Q -42s]` (replayed after an outage, with its true age).

## State machine (Task 4)

`next = TRANS[state][event]`, same table as the Fault State Machine repo.

| From \ event | CLEAR | MINOR | MAJOR | FATAL | VERIFIED | EXHAUSTED | RESET |
|---|---|---|---|---|---|---|---|
| NORMAL | NORMAL | DEGRADED | FAILSAFE | SHUTDOWN | NORMAL | NORMAL | NORMAL |
| DEGRADED | NORMAL | DEGRADED | FAILSAFE | SHUTDOWN | DEGRADED | DEGRADED | DEGRADED |
| FAILSAFE | FAILSAFE | FAILSAFE | FAILSAFE | SHUTDOWN | DEGRADED | SHUTDOWN | FAILSAFE |
| SHUTDOWN | SHUTDOWN | SHUTDOWN | SHUTDOWN | SHUTDOWN | SHUTDOWN | SHUTDOWN | FAILSAFE |

Recovery is staged: FAILSAFE needs 3 s with no fault (VERIFIED) → DEGRADED, then 5 s clean → NORMAL. Three failed recoveries, or 45 s in FAILSAFE, ends in SHUTDOWN. Only RESET (button or dashboard) leaves SHUTDOWN, and it goes to FAILSAFE, never straight to NORMAL. The relay is open in FAILSAFE and SHUTDOWN. Faults are debounced (3 samples to set, 10 to clear) with hysteresis.

| Fault | Severity | Trips when |
|-------|----------|------------|
| CELL_LOW | minor | any cell < 3.30 V (clears > 3.40) |
| IMBAL | minor | spread > adaptive limit, 0.10 V at full → 0.30 V at empty |
| CELL_CRIT | major | cell < 3.00 V or > 4.25 V |
| OVERCURRENT | major | > 8.0 A (clears < 7.0) |
| ADC_FRZ | major | 20 identical reads on a cell channel; that channel is excluded from metrics |
| CELL_DEEP | fatal | cell < 2.85 V |

## Risk score

`risk = 100 × (0.20·SoC + 0.25·imbalance + 0.15·trend + 0.25·faults + 0.15·load)`, each factor 0..1:

| Factor | 0 when | 1 when |
|--------|--------|--------|
| SoC | ≥ 50 % | ≤ 5 % |
| Imbalance | spread ≤ 30 % of the adaptive limit | spread at the limit |
| Trend | spread not rising | rising ≥ 120 mV/min (30 s regression), only counted once spread ≥ 40 % of limit |
| Faults | none in the last 5 min | severity-weighted count ≥ 8 (minor 1, major 2, fatal 4) |
| Load | ≤ 4 A | at the 8 A trip current |

The result is smoothed (rises fast, falls slowly), then **floored by the state machine** (DEGRADED ≥ 35, FAILSAFE ≥ 70, SHUTDOWN ≥ 90) so the score can never contradict the state shown. Levels: LOW < 30 ≤ MODERATE < 55 ≤ HIGH < 80 ≤ CRITICAL, with 3-point hysteresis. Health % is `100 − risk`.

The 5-minute fault window and 120 mV/min trend scale are **demo-sized**; they are constants at the top of the sketch.

## Recommendations

Rule-based, in priority order, at most three lines: state first (SHUTDOWN / FAILSAFE), then active faults (critical cell, over-voltage, overcurrent, frozen sensor, imbalance, low cell), then early warnings (rising imbalance before it faults, same fault repeating 3+ times, low SoC, HIGH/CRITICAL risk), otherwise "No action needed".

## How every transition reaches the dashboard

1. The protection core records each transition into a queue (priority tier 2) with a sequence number.
2. The network core sends the oldest record first, and removes it only after a successful send (so a drop mid-send re-sends rather than loses).
3. During an outage the queue holds up to 64 records (8 KB). When full, the lowest tier goes first (info, then fault/network markers, then transitions), oldest first. A new low-priority record is dropped before an old transition is.
4. If anything is lost, a `[GAP]` line says how many records and **how many transitions**. V14 (sequence) lets you see a jump either way.
5. Each replayed record also writes V8 (state code) and V14 (sequence), so after a replay both gauges show the latest transition; on reconnect the banner, colours and all gauges are re-sent from the live snapshot.

Writes are paced to 8/s. Chart streams use deadbands plus a heartbeat; texts are re-sent on meaningful change only (not on ADC noise).

**Message cost** (measured in the harness, healthy and idle): `PROFILE_DEMO 1` ≈ 720 writes/hour, `PROFILE_DEMO 0` ≈ 20 writes/hour (≈ 14,400 per 30 days). Use `0` for anything long-running.

## Demo script

| Do | Expect |
|----|--------|
| Start, leave pots at their defaults | 🟢 NORMAL, risk 0 LOW, "No action needed" |
| INJECT until `IMBAL_RAMP` (selection commits after 1.2 s idle) | after ~25–35 s: advice "Imbalance is rising +N mV/min…" (before any fault) while risk climbs; about a minute after the ramp starts: 🟡 DEGRADED |
| INJECT → `NONE` | clears, 5 s hold, back to 🟢 |
| INJECT → `OVERLOAD` | 🔴 FAILSAFE, relay opens, buzzer pulses, advice "Load disconnected…" |
| INJECT → `NONE` | 3 s → 🟡 → 5 s → 🟢 |
| INJECT → `DEEP_DISCHARGE` | ⛔ SHUTDOWN, risk 90 CRITICAL |
| INJECT → `NONE`, then RESET (button or dashboard Acknowledge) | 🔴 FAILSAFE → 🟡 → 🟢 |
| INJECT → `ADC_FREEZE` | 🔴 FAILSAFE (frozen sensor), channel isolated |
| OUTAGE, trigger a few faults, OUTAGE again | LCD shows `OFF`; on reconnect the journal replays with `[Q -Ns]` tags in order, then `NET UP after Ns (N queued)` |
| REPORT | executive summary + fault registry on Serial, registry lines in the journal |

LCD: line 1 `STATE R<risk> LVL`, line 2 `S<soc> F<faults> Q<queue> ON/OFF/...`.

## Tests

```
sh test/run_tests.sh        # builds with g++, runs every scenario
g++ -DPROFILE_DEMO=0 ...    # same suite with service timings
```

The harness compiles `sketch.ino` unchanged against stubs for Arduino, Wi-Fi, Blynk and the LCD, and replays scenarios in simulated time. Checks include: transition table properties; full walkthrough of every fault and recovery path; dashboard journal vs the backend's own JSON log (same sequence, same states, same order); outage replay; queue overflow; transition overflow; retry-limit and timeout shutdowns; reaction time while the network task is stuck connecting; write rate ≤ 8/s; no dashboard writes while the link is down; relay always matches state; risk never below its state floor; and 20-simulated-minute random soaks (random pots, buttons, outages, acknowledgements).

Results: all scenarios pass, in both timing profiles, plus 17 soak seeds. `REPORT.md` lists what testing found and what was changed.

## Limitations

- **Not verified on Wokwi or Blynk.** The Blynk calls `setProperty`, `logEvent` and writing to a Terminal widget follow the library API but were exercised only against stubs. `diagram.json` was generated, not drawn in the Wokwi editor.
- **The stub models the two cores as one timeline.** It checks logic and ordering, not real FreeRTOS scheduling, spinlock timing, or stack sizes. The 12 KB network task stack is an estimate.
- **Chart timestamps are arrival time.** Blynk stamps a point when it arrives, so a replayed transition appears at replay time on the timeline chart. The journal line carries the true age.
- **At-least-once delivery.** A link drop mid-send can re-send a record, so a journal line may appear twice.
- **Queue is RAM only.** A reset or brown-out loses undelivered records.
- **SoC is estimated from loaded cell voltage** (lookup table). It is a proxy, not coulomb counting.
- **Demo-scale analytics windows** (5-minute fault window, 30 s trend, 120 mV/min full scale).
- **Single pack, simulated sensing.** No temperature, no real current sensor, ADC noise is simulated (`SIM_ADC_NOISE`). A real design needs a hardware watchdog and an independent backup disconnect.
- **Free-tier limits** (datastreams, messages, event rate) differ by plan; check yours.
- Blynk's credentials are placeholders; never commit a real token.
