# Phase 2C — Auto-tracking with Menu UI

## Goal

Closed-loop solar tracking on the existing Ecoworthy hardware, with
safety interlocks (wind, duty cycle, stall detection) and a
button-driven LCD menu for user calibration and configuration.

After Phase 2C, the rig autonomously tracks the sun when conditions
allow, parks horizontal in high wind, and respects the actuator
manufacturer's thermal duty-cycle limits.

## High-level state machine

```
                 ┌─────────┐   SET held
   power-on ──→  │ NO_CAL  │ ───────────┐
                 └─────────┘             │
                       ↑                 ↓
                       │            ┌─────────┐
                       │  (failure) │ CAL_RUN │
                       │            └─────────┘
                       │                 │ stroke times saved to EEPROM
                       │                 ↓
                       │            ┌─────────┐
                       └────────────│  IDLE   │←─────┐
                                    └─────────┘      │
                                       │             │
                  ┌──────┬──────┬──────┴──────┐      │
                SET│   QUIT  wind>=15  duty hit│     │
                  ↓     ↑     ↓        ↓      │     │
              ┌──────┐  │  ┌──────┐ ┌──────────┐    │
              │ MENU │  │  │STORM │ │DUTY_HOLD │    │
              └──────┘  │  └──────┘ └──────────┘    │
                 (exits to selected mode below)     │
                                                    │
              Menu modes:                           │
                Jog        ────── QUIT ─────────────┘
                Calibrate  ────── done/QUIT ────────┘
                Settings   ────── QUIT ─────────────┘
                Version    ────── QUIT ─────────────┘
                Track      ────── (default mode) ───┐
                                                    ↓
                                              ┌──────────┐
                                              │TRACKING  │
                                              └──────────┘
                                                 │  ↑
                                  wind>=15 ──────┘  │
                                  STORM exit ───────┘
```

**Key transitions:**
- **First boot, no EEPROM**: NO_CAL → user holds SET → CAL_RUN → IDLE (after calibration completes)
- **Normal boot, EEPROM valid**: NO_CAL skipped → IDLE
- **From IDLE**: press SET → MENU. Or auto-enter TRACKING after timeout (configurable, default off — start with explicit user choice)
- **STORM** is a forced state — overrides menu/tracking/jog whenever wind ≥ 15 m/s
- **DUTY_HOLD** is per-axis. Tracking continues for axes with budget remaining

## Menu system

### Top-level menu (entered from IDLE via SET)

Items in order, with N (up) / S (down) for scroll, SET for select, QUIT to exit menu:

1. **Track** — start auto-tracking. Exits menu to TRACKING state.
2. **Jog** — manual button-drive mode. Same as our current Phase 2A/2D main screen. SET while in Jog saves current position as the horizontal reference. QUIT returns to IDLE.
3. **Calibrate** — run the stall-to-stall calibration sequence. After completion, returns to IDLE. Updates EEPROM.
4. **Settings** — enter Settings sub-menu.
5. **Version** — display firmware version + build info. QUIT to return.

### Settings sub-menu

Editable parameters, each on its own screen. N/S adjusts value, SET commits, QUIT cancels (revert to previous value).

1. **Storm enter** (default 15, m/s, range 5–25)
2. **Storm exit** (default 10, m/s, range 0–storm_enter−1)
3. **Storm dwell** (default 10, minutes, range 1–60)
4. **Track dead-band** (default 5, ADC counts, range 1–50)
5. **Move pulse** (default 1000, ms, range 100–5000)
6. **Save** — commit changes to EEPROM and exit Settings

If user exits Settings via QUIT without choosing Save, changes are discarded.

### LCD layout — menu screens

Two-row 1602A. Convention: a `>` cursor on the left marks the currently-selected line; the line below shows the next item (or context).

**Top-level menu** (example, currently on "Calibrate"):
```
  Track
> Calibrate
```

**Settings item** (example, editing storm enter):
```
Storm enter: 15
N/S=adj  SET=ok
```

Or for items with units:
```
Move pulse: 1000ms
N/S=adj  SET=ok
```

**Version screen**:
```
EcoWorthy v0.2c
2026-05-12 build
```

### Navigation summary

**Universal button roles** (apply everywhere except Jog mode):
- **N / S / E / W** = directional arrows for navigation and value adjustment. In menus, N/S scroll; E/W reserved for future page-scroll or coarse adjust (unused for now).
- **SET** = select menu item, commit edited value.
- **QUIT** = exit current mode, back one menu level.

**In Jog mode only**, N/S/E/W revert to their Phase 2A behavior (drive actuators directly).

| Context | N | S | E | W | SET | QUIT |
|---|---|---|---|---|---|---|
| IDLE | — | — | — | — | Enter MENU | — |
| MENU top | scroll up | scroll down | — | — | enter selected | exit to IDLE |
| Settings list | scroll up | scroll down | — | — | edit selected | back to MENU |
| Settings edit | value + | value − | — | — | commit value | discard, back |
| **Jog** | extend tilt | retract tilt | extend rotate | retract rotate | save horizontal | exit to IDLE |
| TRACKING | — | — | — | — | enter MENU (pauses tracking) | force-stop, → IDLE |
| STORM | — | — | — | — | — | (locked until wind drops) |

**Menu inactivity timeout**: if no button is pressed for **60 seconds** while in any MENU or Settings screen, auto-exit to IDLE. Prevents the rig from being stuck in a menu if the user wanders off.

## Calibration sequence (CAL_RUN state)

Triggered: user picks "Calibrate" from menu (or holds SET at NO_CAL screen on first boot).

Algorithm, per axis:

1. **Retract until stall.** Drive REV (retract) relay, sample dI continuously. When dI stays in [-4, -1] for 200 ms continuously → stalled. De-assert relay. This is "position 0".
2. **Wait 1 s** for actuator + dI to fully settle.
3. **Extend until stall.** Drive FWD (extend) relay, sample dI. Record total elapsed time. When stalled → de-assert. Total elapsed = `stroke_ms`.
4. **Wait 1 s** to settle.

LCD progress display during cal:
```
Calibrating N/S
Step 2/4 t=15.3s
```

After both axes complete, save to EEPROM:
- `cal_ns_stroke_ms`
- `cal_ew_stroke_ms`
- `horiz_ns_pct = 50` (default mid-stroke — user can override later via Jog → Save Horizontal)
- `horiz_ew_pct = 50`

Total cal time: roughly 2 × (full retract + full extend) per axis ≈ 4 × stroke_ms. For typical 12V linear actuators, ~30 s per direction, so calibration takes ~4 min total. Display countdown so user knows it's not stuck.

**Stall detection — 0.2 s window**: maintain a `stall_count` per axis, incremented every main-loop iteration when dI is in [-4,-1] and reset when dI is out of range. At 50 ms loop period that's 4 consecutive samples = stalled. (Note: if main-loop period drifts, recalibrate this constant.)

## Position tracking (runtime integration)

After calibration we know `stroke_ms` per axis. Position is tracked as:

```
ns_pos_ms += +(actuator_ns_on_time)  for FWD direction
ns_pos_ms -= +(actuator_ns_on_time)  for REV direction
ns_pos_pct = (ns_pos_ms * 100) / stroke_ns_ms
```

Clamp `ns_pos_ms` to `[0, stroke_ns_ms]` to handle drift.

**Re-zero opportunistically**: any time the actuator hits a stall (whether commanded or unexpected), reset that axis's position estimate to 0 (if retract direction) or `stroke_ms` (if extend direction). This bounds drift to a single move's worth of error.

Position estimate is **not** saved to EEPROM during normal operation (would wear EEPROM). It's reconstructed at boot by retracting to stall — taking ~30 s of motion at startup. This is acceptable given the alternative (re-running full calibration) is much worse.

**Alternate boot strategy**: save position estimate to EEPROM only on power-down (using a brown-out or a controlled shutdown). Skip until later phase.

## Auto-tracking algorithm

Run every main loop iteration when in TRACKING state:

```
imbalance_ns = sun_n - sun_s          // signed, +N means N is brighter
imbalance_ew = sun_e - sun_w

if |imbalance_ns| > dead_band:
    target_ns = (imbalance_ns > 0) ? AXIS_FWD : AXIS_REV
else:
    target_ns = AXIS_OFF
// same for EW

apply_duty_cycle_limit(target_ns, target_ew)  // may force OFF
apply_wind_interlock(target_ns, target_ew)    // may divert to horizontal
set_axis_ns(target_ns)
set_axis_ew(target_ew)
```

**Move pulse**: when starting a movement, run for at least `move_pulse_ms` (default 1000) before re-evaluating. Prevents oscillation when sun is near dead-band edge.

**Implementation**: when a movement is commanded, set `pulse_until_ms = now + move_pulse_ms`. Until that time, keep driving regardless of new readings. Only re-evaluate after the pulse completes.

## Wind interlock with dwell timer

State variables:
- `wind_state` ∈ {NORMAL, STORM}
- `storm_exit_dwell_start_ms`: timestamp when wind first dropped below exit threshold
- `last_peak_above_exit_ms`: timestamp of most recent reading at-or-above exit threshold

Algorithm, each main loop iteration:

```
if wind_state == NORMAL:
    if wind >= storm_enter_mps (default 15):
        wind_state = STORM
        storm_exit_dwell_start_ms = now
        last_peak_above_exit_ms = now
        // start driving to horizontal
elif wind_state == STORM:
    if wind >= storm_exit_mps (default 10):
        last_peak_above_exit_ms = now  // reset the dwell
    if (now - last_peak_above_exit_ms) >= storm_dwell_minutes * 60_000:
        wind_state = NORMAL  // release
```

So: storm enters at ≥15 m/s. Releases only after 10 continuous minutes during which wind has stayed below 10 m/s (any single reading ≥10 resets the timer). This matches the user's stated requirement.

### Storm park behavior

When `wind_state == STORM`:
- Compute target position (the saved horizontal: `horiz_ns_pct`, `horiz_ew_pct`)
- Compute error = target − current per axis
- Drive whichever direction reduces |error|, until |error| ≤ 1% (close enough)
- Once at horizontal, hold (no relay activation)
- Refuse to leave STORM until dwell expires

The duty-cycle limit still applies in STORM — if an axis exceeds budget while moving to horizontal, it will pause and resume after cooldown. This is acceptable because the panel reaches horizontal eventually.

## Duty cycle — leaky bucket

Per axis:
- `duty_credit_ns` (range 0–120 seconds)
- Increment by 1 each second when relay is energized
- Decrement by 1 every 9 seconds when relay is off (giving the 1:9 thermal ratio)
- If `duty_credit_ns >= 120`: force this axis OFF and enter DUTY_HOLD for it
- Stay in DUTY_HOLD for this axis until `duty_credit_ns <= 60`

The 1-Hz cadence is handled by a timer (could be a software counter, or Timer 0 ISR at 1 kHz dividing down).

## EEPROM layout (STC15 IAP)

```
struct config {
    uint16_t magic;             // 0xC0DE; if absent, NO_CAL state
    uint8_t  version;           // bumps when struct changes
    uint16_t cal_ns_stroke_ms;
    uint16_t cal_ew_stroke_ms;
    uint8_t  horiz_ns_pct;
    uint8_t  horiz_ew_pct;
    uint8_t  storm_enter_mps;
    uint8_t  storm_exit_mps;
    uint8_t  storm_dwell_min;
    uint8_t  track_deadband_adc;
    uint16_t move_pulse_ms;
    // total: 14 bytes; well under STC15 IAP page size (512 bytes)
};
```

Written only on:
- Calibration completion
- "Save Horizontal" (in Jog mode, press SET)
- Settings menu → "Save" item

EEPROM cell endurance ~100k writes. With save-only-on-user-action, we're nowhere near it.

## Sub-phase decomposition

| Sub | Scope | Verification |
|---|---|---|
| **2C-1** | EEPROM read/write helpers, config struct | Write known struct, power-cycle, read back unchanged |
| **2C-2** | Menu framework: top-level navigation, item rendering, button mapping | Buttons scroll/select; each item lights up; QUIT exits |
| **2C-3** | Version screen (simplest menu item) | "Version" item shows build string |
| **2C-4** | Jog mode as menu item (re-use existing manual-drive code) | Manual control works from menu entry; QUIT returns to IDLE |
| **2C-5** | Calibration sequence with stall detection | "Calibrate" runs full retract→extend per axis, saves stroke times |
| **2C-6** | Position tracking + Save Horizontal | Position estimate updates with movement; SET in Jog saves horizontal |
| **2C-7** | Settings menu (storm parameters + dead-band + pulse) | Each setting editable; Save commits to EEPROM |
| **2C-8** | Auto-tracking algorithm + duty-cycle | "Track" item enters TRACKING; panel follows light; duty hold engages |
| **2C-9** | Wind interlock + storm park + dwell | Simulated wind ≥15 → drive to horizontal; release after 10 min below 10 |
| **2C-10** | Final integration + 5-cycle bench test + tag `phase-2c-complete` | Boot reliably to IDLE; full menu navigation; safe tracking + park |

## Resolved decisions

- **Storm park**: per-axis percentage stored in EEPROM (matches calibration; flexible).
- **Auto-resume after storm dwell**: yes, return to TRACKING automatically. The rig was tracking before the storm; resume that mode unless user explicitly enters menu.
- **Manual override during TRACKING**: N/S/E/W are inert during TRACKING (and IDLE, and STORM). User must enter Jog via the menu to drive actuators manually. Keeps modes unambiguous.
- **Menu inactivity timeout**: 60 seconds. If no button press while in MENU/Settings, auto-exit to IDLE.

## Future work (not Phase 2C)

1. **Power-down position save**: brownout-triggered save would skip the boot-time retract-to-stall recalibration. Phase 3+ candidate.
2. **Time of day / RTC**: no RTC on this board. Tracking is sensor-driven. Implication: at night, dead-band must absorb sensor noise. Phase 3 could add an "all-sensors-below-floor" cutoff to explicitly idle at night.
3. **Calibration drift monitoring**: periodic re-cal reminders if drift is observed. Out of scope for 2C.
4. **Configurable menu inactivity timeout**: keeping it a constant for now. Promote to a Settings item only if 60 s proves wrong.
5. **E/W as page-scroll or coarse-adjust in menus**: unused for now. Add if menus grow long.
