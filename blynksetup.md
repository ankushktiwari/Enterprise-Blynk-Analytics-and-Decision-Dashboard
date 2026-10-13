# Blynk setup

About 10 minutes. You need a Blynk.Console account (blynk.cloud).

> Status: this guide matches the firmware's datastream numbers exactly, but I could not test it against a live Blynk account. If a widget option is named differently in your console version, the datastream table below is the part that must stay exact.

## 1. Create the template

1. Blynk.Console → **Templates → New Template**
   - Name: `BMS Analytics`
   - Hardware: `ESP32`, Connection: `WiFi`
2. **Datastreams → New → Virtual Pin**. Create all 17 exactly as below. The pin number is what the firmware writes to.

| Pin | Name | Type | Range | Unit | Used for |
|-----|------|------|-------|------|----------|
| V0 | Cell 1 | Double | 2.0 – 4.5 | V | chart |
| V1 | Cell 2 | Double | 2.0 – 4.5 | V | chart |
| V2 | Cell 3 | Double | 2.0 – 4.5 | V | chart |
| V3 | SoC | Double | 0 – 100 | % | gauge + chart |
| V4 | Imbalance | Double | 0 – 1000 | mV | chart |
| V5 | Imbalance trend | Double | -500 – 500 | mV/min | chart |
| V6 | Load current | Double | 0 – 10 | A | chart |
| V7 | Risk score | Integer | 0 – 100 | – | gauge + chart |
| V8 | State code | Integer | 0 – 3 | – | state timeline |
| V9 | Status banner | String | – | – | label (colour follows state) |
| V10 | Executive summary | String | – | – | label |
| V11 | Recommendations | String | – | – | label |
| V12 | Fault & event journal | String | – | – | terminal |
| V13 | Fault count | Integer | 0 – 999 | – | label |
| V14 | Transition seq | Integer | 0 – 1000000 | – | label |
| V15 | Queue depth | Integer | 0 – 64 | – | label |
| V16 | Operator command | Integer | 0 – 1 | – | button (Acknowledge) |

If your plan limits datastreams, drop these first, in this order: V15, V5, V6, V13. The firmware keeps working; those widgets just stay empty.

## 2. Events (optional but recommended)

**Events → New Event**, with these exact codes (the firmware calls `Blynk.logEvent` with them):

| Code | Suggested type |
|------|----------------|
| `degraded` | Warning |
| `failsafe` | Critical |
| `shutdown` | Critical |
| `recovered` | Info |

Blynk may throttle repeated events of the same code. The journal (V12) is the authoritative record; events are only the notification layer.

## 3. Web dashboard

Suggested layout (each widget bound to the datastream shown):

| Area | Widget | Datastream |
|------|--------|------------|
| Top banner | Label | V9 status banner |
| Executive summary | Label (large) | V10 |
| Risk | Gauge (0–100) | V7 |
| Health trend | SuperChart | V7 risk + V4 imbalance |
| Cells | SuperChart (3 lines) | V0, V1, V2 |
| State timeline | SuperChart (step) | V8 |
| SoC / load | Gauge + Chart | V3, V6 |
| Operator advice | Label | V11 |
| Fault history | Terminal | V12 |
| Counters | Label (one per datastream) | V13, V14, V15 |
| Acknowledge | Button, mode **Push**, ON value `1` | V16 |

Widget names vary slightly between Blynk console/app versions; where this guide says Label, use whichever widget simply shows a datastream's current value.

For the **state timeline** chart, set V8 to show a step line. For the labels, enlarge the widget if long text is cut off.

**Colours.** The firmware sets the colour property of V8/V9 on every state change (green `#23C48E`, yellow `#ED9D00`, red `#D3435C`, dark red `#7A0019`) and of V7 on every risk-level change. Do not hard-code a colour on those widgets, or it will override the firmware.

## 4. Create the device and put the credentials in the sketch

1. **Devices → New Device → From template** → `BMS Analytics`.
2. Open the device → **Device Info**. Copy `BLYNK_TEMPLATE_ID`, `BLYNK_TEMPLATE_NAME`, `BLYNK_AUTH_TOKEN`.
3. Paste them over the three `#define`s at the top of `sketch.ino`.

**Do not commit your token.** Keep the placeholder values in the repository, or keep the real ones in an uncommitted copy.

## 5. Run in Wokwi

1. New ESP32 project; replace `sketch.ino`, `diagram.json`, `libraries.txt` with the files from this folder.
2. Start the simulation. The sketch connects to Wi-Fi `Wokwi-GUEST` and then to Blynk. The LCD shows `ON` in the bottom-right when the dashboard link is up.
3. Press the on-screen buttons (see the README for the demo script).

## Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| LCD shows `...` forever | Wi-Fi up but Blynk refused: check the three credentials and that the device is created from the template |
| Dashboard empty but LCD shows `ON` | datastream pin numbers don't match the table |
| Terminal shows nothing | V12 must be a **String** datastream |
| Colours never change | a fixed colour is set on the widget, remove it |
| `OFF` after pressing OUTAGE | expected: simulated outage. Press OUTAGE again; queued events replay with `[Q -Ns]` tags |
