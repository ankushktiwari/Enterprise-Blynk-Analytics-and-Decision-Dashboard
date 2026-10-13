# Report: Enterprise Blynk Analytics and Decision Dashboard

## 1. Objective

Provide an operator-facing analytics layer on top of the battery protection stack: historical trends, a calculated risk score, structured fault history, human-readable recommendations and an executive summary, with severity visuals that match the Task 4 state machine, and with **every backend state transition reflected on the dashboard**.

## 2. Architecture

```
 core 1 (Arduino loop)                              core 0 (net task)
 ---------------------                              -----------------
 ADC read (10 Hz) -> fault eval (debounce)          Wi-Fi/Blynk state machine
        -> Task 4 state machine -> relay/LED/buzzer       (blocking connect is harmless here)
        -> 1 Hz analytics: trend, risk, advice            |
        -> Snapshot (spinlock copy) ----------------->    sendSnapshot: deadband + heartbeat
        -> EventRec queue, 64 slots, 3 priority tiers --> sendRecord: oldest first, paced 8/s
        LCD (changed lines only)                          Blynk.run(), BLYNK_WRITE(V16) = ack
```

The shared state is one snapshot struct (copied under a spinlock, a few hundred bytes) and the event queue. The protection core never calls a Blynk function and never waits on the network.

Memory (measured with `sizeof`): queue 64 × 128 B = 8,192 B; the dashboard snapshot is 672 B and the sketch keeps four static copies (~2.7 KB). No dynamic allocation.

## 3. Risk model

Five factors, each 0..1, combined with fixed weights (see README for the full table): SoC 0.20, imbalance vs the adaptive limit 0.25, imbalance trend 0.15, recent fault frequency 0.25, load stress 0.15. Weights are my judgement, not calibrated against real pack data. Design rules that were tested:

- Each factor, taken alone, increases risk monotonically (unit tests); a *falling* imbalance adds no risk.
- Worst case on every factor gives exactly 100; a healthy pack scores < 10.
- The state machine puts a **floor** under the score (35 / 70 / 90 for DEGRADED / FAILSAFE / SHUTDOWN), checked every 100 ms in every scenario, so the gauge can never read "low" while the banner says FAILSAFE.
- Level changes use 3-point hysteresis; the score rises fast and falls slowly to avoid flicker.

## 4. Dashboard accuracy

Each transition becomes a tier-2 record carrying a sequence number, written to the journal (V12), the sequence gauge (V14) and the state code (V8), and optionally a Blynk event. Records are sent oldest-first and removed only after a send while still connected.

Overflow policy: lowest tier first, oldest first; a new low-priority record is dropped before an old transition is evicted. Any loss produces a `[GAP]` line with record and transition counts. Link-down and link-up markers are tier 1, so outage boundaries are kept.

The test harness compares the dashboard's journal against the firmware's own JSON log on the serial port: same count, same sequence numbers, same order, same from/to states, and the V14/V8 gauges end equal to the backend. When the queue overflows, the test requires `delivered + reported-lost = produced`, so a silent loss fails.

## 5. Message budget (measured)

Healthy, idle system over one simulated hour, steady state:

| Profile | Writes / hour | Per 30 days |
|---------|---------------|-------------|
| `PROFILE_DEMO 1` (60 s heartbeat, 30 s text refresh) | 720 | ~518,000 |
| `PROFILE_DEMO 0` (30 min heartbeat and refresh) | 20 | ~14,400 |

A transition costs roughly ten messages (about four for the journal record and six from the snapshot refresh); that figure is from reading the code, not a separate measurement. The demo profile is for demonstrations; do not leave it running against a metered plan.

Write rate never exceeded 8 per second in any scenario (pacing is 125 ms).

## 6. Test results

Harness: `sketch.ino` compiled unchanged with g++ against stubs; simulated time; `-Wall -Wextra` clean.

| Scenario | What it proves |
|----------|----------------|
| units (34 checks) | weights sum to 1, monotonic factors, level hysteresis, adaptive limit, SoC table, no illegal jump to NORMAL from FAILSAFE/SHUTDOWN |
| demo (97) | imbalance trend advice appears *before* the fault; every fault and recovery path; reset and dashboard acknowledge; frozen-ADC isolation; summary and colours reach the dashboard |
| outage (137) | transitions during an outage replay in order with `[Q -Ns]` tags; NET DOWN/UP recorded; no writes while down |
| flood (130) | info flood overflows the queue; transitions survive; GAP shown |
| txflood (553) | more transitions than slots: newest 64 kept, 27 lost transitions reported |
| exhaust (41), timeout (17) | retry-limit and 45 s timeout both end in SHUTDOWN with the right reason |
| linkdrop (21) | a stuck connect does not delay FAILSAFE (reaction < 800 ms) |
| idle (8–9) | message cost, above |
| soak × 17 seeds | 20 simulated minutes each of random pots, buttons, outages, acknowledgements: 75–100 transitions per run, tens of thousands of invariant checks per run, no failures |

The same suite also passes with `-DPROFILE_DEMO=0`.

**What this does not prove:** real FreeRTOS scheduling, the Blynk cloud's behaviour (rate limits, event throttling, Terminal rendering), Wokwi wiring, or anything about real cells. The stub runs both cores on one timeline.

## 7. Issues found by testing (and fixed)

| # | Found | Fix |
|---|-------|-----|
| 1 | Under a long outage with random faults, transitions #58–#63 never reached the dashboard (queue full of equal-priority "critical" records) | three priority tiers, queue 32 → 64, new low-tier records dropped first, GAP line counts lost transitions |
| 2 | NET DOWN was the first record evicted, so the outage start vanished from the journal | link-down/up records are tier 1 |
| 3 | Cycling INJECT through 5 modes passed through CELL_DROP and DEEP_DISCHARGE and could trip real faults or SHUTDOWN | selection commits only after 1.2 s without a press |
| 4 | Executive summary was re-sent about every 5 s while idle (693 of 711 writes per hour) because noisy digits changed the text | change detection uses a quantised digest; numbers refresh on the slow interval. Idle cost dropped from 711 to 20 writes/h |
| 5 | The "weakest cell" index flipped between equal cells on ADC noise and kept triggering that resend | ignored until the spread is ≥ 20 % of the limit |
| 6 | V14 (sequence) was never written if no transition had happened, so a fresh dashboard showed it empty | included in the full refresh |
| 7 | Summary said `Last: IMBAL` during a CELL_DEEP shutdown (last *set* is not the cause) | "last fault" keeps the most severe of a burst |
| 8 | A transition line showed the risk from before the transition (`NORMAL->FAILSAFE … risk=10`) | shows the risk as it will read after the floor |
| 9 | Trend risk fired on 1 mV → 5 mV wobble | trend only counts once the spread is ≥ 40 % of the limit |

Also fixed in the **test harness** (not the firmware): button presses were being stretched by the net task's pacing waits, and outage windows were recorded late. Both made early runs report false failures; the harness now schedules button edges in absolute simulated time.

## 8. Known limits and next steps

- Verify on Wokwi and a real Blynk account (my highest-priority gap), including `setProperty` colours, `logEvent` throttling and Terminal behaviour.
- Persist tier-2 records to NVS so a reset during an outage doesn't lose them; add a hardware/task watchdog.
- Duplicates are possible after a mid-send drop (at-least-once). A per-record sequence could let the reader de-duplicate.
- Replace the voltage-lookup SoC with coulomb counting plus periodic OCV correction.
- Calibrate the risk weights and windows against real pack data, and move the demo-scale windows (5 min faults, 30 s trend) to service scale.
- Merge into one firmware with the earlier BMS repo's per-cell logic and the relay repo's sensor validation; add temperature.
- Integrate timestamps (NTP) so a replayed point can carry its true time instead of its arrival time on charts.
