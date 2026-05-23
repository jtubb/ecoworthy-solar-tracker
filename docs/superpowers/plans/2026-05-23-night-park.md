# Night-Park Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Park the panel to a configurable east-facing, flat-tilt position when the sun has been below an ADC threshold for a configurable number of minutes, so the first morning light triggers immediate tracking instead of waiting for the panel to slew across half its stroke.

**Architecture:** STC firmware adds a new state `ST_NIGHT_PARK` with autonomous entry (sun-sum-below-threshold timer) and autonomous exit (sun-sum-above-threshold hysteresis). The existing goto framework drives the move to configurable az/el targets. Five new EEPROM-persisted config fields are exposed over the existing `!cfg get/set` UART protocol; the ESP bridge wires them to HA sliders + a switch. A debug `!sim_dark` UART command lets the bench harness force the dark state without physical sensor occlusion.

**Tech Stack:** SDCC + Make + stcgal (STC firmware), ESPHome external component on ESP8266 Arduino (ESP bridge), HA frontend (sliders + switch). No new mesh frame types; everything rides the existing CFG protocol.

**Design decisions locked in by 2026-05-23 conversation:**
- Layer: STC firmware (autonomous) — survives ESP/WiFi/HA outages.
- Position: full east + flat tilt — best for catching dawn light.
- Tunability: operator-tunable via HA sliders + LCD menu items.

---

## File Structure

- `EcoWorthyFirmware/include/board.h` — add `ST_NIGHT_PARK` to the state enum (currently in `main.c`, see "Refactor note" below).
- `EcoWorthyFirmware/src/main.c` — the bulk of the firmware work. Five new EEPROM fields, five new `CFG_F_*` IDs, new state body, modified state transition logic, new LCD menu page, new `!sim_dark` debug command.
- `esphome/components/tracker_bridge/tracker_bridge.h` — five new constants for the cfg field IDs, handler entries in `handle_payload_` for the cfg-reply path, new setter calls in the GET_REQ/SET_REQ dispatcher, four new `number::Number*` pointers + one `switch::Switch*` pointer for the local entities, broadcast_cfg_get_resp_ extension.
- `esphome/components/tracker_bridge/number.py` — four new platform schema entries (night_park_az_pct, night_park_el_pct, night_park_dark_min, night_park_dark_thresh).
- `esphome/components/tracker_bridge/switch.py` — new platform schema entry for night_park_enable. (If a `switch.py` file doesn't exist yet, this is the right time to create it; the test_offline switch in the YAMLs is currently a template-platform switch, not a tracker_bridge native switch — we'll mirror its pattern.)
- `EcoWorthyFirmware/esphome/solar-tracker-{1,2}.yaml` — add the four number sliders + one switch entity to each tracker's YAML, mirroring the existing pattern for `wind_storm_mps` etc.
- `tools/bench_test.py` — add `test_night_park_engages_after_dark_timer` and `test_night_park_releases_on_light_return`. Both gated on the `!sim_dark` debug hook landing in the STC firmware.

**Refactor note:** The state enum `ST_IDLE / ST_MENU / ST_TRACK / ST_STORM / ST_CAL` currently lives inline in `src/main.c` (~line 1343). Per the writing-plans skill, "files that change together should live together" and "responsibility, not technical layer." The state enum is logically *the* state machine of the firmware — moving it to `include/board.h` would let other translation units (none today, but the firmware is approaching the point of file-splitting) reference it cleanly. **Out of scope for this plan** — flagged so the future-you can decide. Keep the enum in `main.c` for now and just add `ST_NIGHT_PARK` next to its siblings.

---

## EEPROM Layout

Current layout (`main.c` ~line 561-570):

| Offset | Field |
|---|---|
| 0-1 | CONFIG_MAGIC_0, CONFIG_MAGIC_1 |
| 2-3 | ns_stroke_ms (u16) |
| 4-5 | ew_stroke_ms (u16) |
| 6 | horiz_ns_pct |
| 7 | horiz_ew_pct |
| 8 | wind_storm_mps |
| 9 | wind_release_mps |
| 10 | storm_dwell_min |
| 11 | track_thresh |
| 12 | role |
| 13 | wind_source |

New fields appended at 14-18:

| Offset | Field | Default | Range |
|---|---|---|---|
| 14 | night_park_enable | 1 | 0 or 1 |
| 15 | night_park_az_pct | 0 (full east) | 0-100 |
| 16 | night_park_el_pct | 50 (flat) | 0-100 |
| 17 | night_park_dark_min | 30 (minutes) | 1-120 |
| 18 | night_park_dark_thr | 30 (ADC avg) | 5-200 |

The `night_park_dark_thr` is the per-sensor *average* ADC reading below which all four sun sensors must read for the dark timer to accumulate. With 10-bit ADC and ambient indoor scattered light typically reading 30-80, default 30 is a sensible "true dark" threshold. The 5-200 range bounds it away from "always trigger" (0) and "never trigger" (255).

---

## CFG Field IDs (UART protocol)

Current IDs (`main.c` ~line 537-546):

| ID | Name | RW |
|---|---|---|
| 0x01 | CFG_F_WIND_STORM | RW |
| 0x02 | CFG_F_WIND_RELEASE | RW |
| 0x03 | CFG_F_STORM_DWELL | RW |
| 0x04 | CFG_F_TRACK_THRESH | RW |
| 0x10-0x13 | strokes + horiz | RO (calibration-set) |
| 0x20 | CFG_F_ROLE | RW |
| 0x21 | CFG_F_WIND_SOURCE | RW |

New IDs assigned in the next contiguous range starting at 0x05:

| ID | Name | RW |
|---|---|---|
| 0x05 | CFG_F_NIGHT_PARK_EN | RW |
| 0x06 | CFG_F_NIGHT_PARK_AZ | RW |
| 0x07 | CFG_F_NIGHT_PARK_EL | RW |
| 0x08 | CFG_F_NIGHT_PARK_MIN | RW |
| 0x09 | CFG_F_NIGHT_PARK_THR | RW |

The ESP bridge's `cfg_poll` lambda currently reads only `{1,2,3,4}` — it will be extended to read `{1,2,3,4,5,6,7,8,9}` in Step 13.

---

## State Machine Integration

Transitions to add (existing transitions remain unchanged):

- **ST_TRACK → ST_NIGHT_PARK**: when `night_park_enable && all four sun ADC < night_park_dark_thr` for `night_park_dark_min` consecutive minutes.
- **ST_NIGHT_PARK → ST_TRACK**: when `any sun ADC > (night_park_dark_thr + HYSTERESIS)` for 6 consecutive samples (~60 s at the existing main-loop tick rate). Hysteresis = 20 ADC counts is hardcoded — prevents flapping during dawn light rise.
- **ST_NIGHT_PARK → ST_STORM**: storm always wins, same as ST_TRACK. The existing `storm_check()` already runs unconditionally and forces `*state = ST_STORM`.
- **ST_IDLE / ST_CAL / ST_MENU**: do NOT enter ST_NIGHT_PARK — only ST_TRACK can transition to night park (analog to how only ST_TRACK can be storm-displaced gracefully).

The "during ST_NIGHT_PARK, drive the panel to the target" body reuses the existing `goto_active = 1` framework: set `goto_target_az_pct = night_park_az_pct`, `goto_target_el_pct = night_park_el_pct`, and let the existing per-axis seek logic do the move. Once both axes are within tolerance, the panel sits still; the wake-up sun check continues every loop tick.

---

## Task 1: Add EEPROM fields and CFG IDs

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (~lines 499-545 for comments + globals + CFG_F_*, ~lines 561-608 for load/save)

**Goal:** Persist the five new fields. No state-machine wiring yet — just the data layer.

- [ ] **Step 1: Add the five global variables** alongside existing config globals (~line 510-515):

```c
static unsigned char night_park_enable   = 1;
static unsigned char night_park_az_pct   = 0;   /* full east */
static unsigned char night_park_el_pct   = 50;  /* flat */
static unsigned char night_park_dark_min = 30;
static unsigned char night_park_dark_thr = 30;
```

- [ ] **Step 2: Add the EEPROM layout comment lines** in the block that documents the layout (~line 499-505, immediately before `#define CONFIG_MAGIC_0`):

```c
 *   14    : night_park_enable (0 or 1)
 *   15    : night_park_az_pct (0..100; 0 = full east)
 *   16    : night_park_el_pct (0..100; 50 = flat)
 *   17    : night_park_dark_min (1..120)
 *   18    : night_park_dark_thr (5..200; per-sensor ADC threshold for "dark")
```

- [ ] **Step 3: Add the CFG_F_* IDs** to the constants block (~line 537-546):

```c
#define CFG_F_NIGHT_PARK_EN    0x05
#define CFG_F_NIGHT_PARK_AZ    0x06
#define CFG_F_NIGHT_PARK_EL    0x07
#define CFG_F_NIGHT_PARK_MIN   0x08
#define CFG_F_NIGHT_PARK_THR   0x09
```

- [ ] **Step 4: Extend `config_load()`** (~line 561) to read the five new bytes:

```c
night_park_enable   = iap_read_byte(EEPROM_BASE + 14);
night_park_az_pct   = iap_read_byte(EEPROM_BASE + 15);
night_park_el_pct   = iap_read_byte(EEPROM_BASE + 16);
night_park_dark_min = iap_read_byte(EEPROM_BASE + 17);
night_park_dark_thr = iap_read_byte(EEPROM_BASE + 18);
```

- [ ] **Step 5: Extend the range-check block in `config_load()`** (~line 571-582) with defaults-on-out-of-range guards:

```c
if (night_park_enable > 1) night_park_enable = 1;
if (night_park_az_pct > 100) night_park_az_pct = 0;
if (night_park_el_pct > 100) night_park_el_pct = 50;
if (night_park_dark_min < 1 || night_park_dark_min > 120) night_park_dark_min = 30;
if (night_park_dark_thr < 5 || night_park_dark_thr > 200) night_park_dark_thr = 30;
```

- [ ] **Step 6: Extend `config_save()`** (~line 596-608) to write the five new bytes:

```c
iap_program_byte(EEPROM_BASE + 14, night_park_enable);
iap_program_byte(EEPROM_BASE + 15, night_park_az_pct);
iap_program_byte(EEPROM_BASE + 16, night_park_el_pct);
iap_program_byte(EEPROM_BASE + 17, night_park_dark_min);
iap_program_byte(EEPROM_BASE + 18, night_park_dark_thr);
```

- [ ] **Step 7: Extend the factory-defaults reset block** (~line 1304-1305, where horiz_ns_pct = 50 etc. is set) with the same defaults from Step 1.

- [ ] **Step 8: Build the firmware**: `cd EcoWorthyFirmware && make`. Expect a clean build. If DSEG overlay error pops up, you've exceeded SDCC's IRAM budget — see CLAUDE.md note about `--stack-auto` (it should already be set).

- [ ] **Step 9: Commit**: `feat(firmware): night-park EEPROM fields and CFG IDs`

---

## Task 2: Extend !cfg get/set handler for new IDs

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (~lines 1916-1980, the `!cfg get` and `!cfg set` UART command handlers)

**Goal:** Make the five new fields settable and gettable over the UART protocol. No state-machine wiring yet — just the protocol layer.

- [ ] **Step 1: Extend the `!cfg get` switch** (~line 1921-1933):

```c
case CFG_F_NIGHT_PARK_EN:  uart_cfg_send_value(id, night_park_enable);   return;
case CFG_F_NIGHT_PARK_AZ:  uart_cfg_send_value(id, night_park_az_pct);   return;
case CFG_F_NIGHT_PARK_EL:  uart_cfg_send_value(id, night_park_el_pct);   return;
case CFG_F_NIGHT_PARK_MIN: uart_cfg_send_value(id, night_park_dark_min); return;
case CFG_F_NIGHT_PARK_THR: uart_cfg_send_value(id, night_park_dark_thr); return;
```

- [ ] **Step 2: Extend the `!cfg set` switch** (~line 1948-1980):

```c
case CFG_F_NIGHT_PARK_EN:
    if (val > 1) status = CFG_STATUS_RANGE;
    else { night_park_enable = (unsigned char)val; config_save(); }
    break;
case CFG_F_NIGHT_PARK_AZ:
    if (val > 100) status = CFG_STATUS_RANGE;
    else { night_park_az_pct = (unsigned char)val; config_save(); }
    break;
case CFG_F_NIGHT_PARK_EL:
    if (val > 100) status = CFG_STATUS_RANGE;
    else { night_park_el_pct = (unsigned char)val; config_save(); }
    break;
case CFG_F_NIGHT_PARK_MIN:
    if (val < 1 || val > 120) status = CFG_STATUS_RANGE;
    else { night_park_dark_min = (unsigned char)val; config_save(); }
    break;
case CFG_F_NIGHT_PARK_THR:
    if (val < 5 || val > 200) status = CFG_STATUS_RANGE;
    else { night_park_dark_thr = (unsigned char)val; config_save(); }
    break;
```

- [ ] **Step 3: Flash + verify via direct UART** (bench setup with no ESP needed):
  ```
  python tools/scripts/send-cmd.py "!cfg get id=5"
  ```
  Expected reply: `cfg id=5 val=1` (default enable = on).
  ```
  python tools/scripts/send-cmd.py "!cfg set id=6 val=10"
  python tools/scripts/send-cmd.py "!cfg get id=6"
  ```
  Expected reply: `cfg id=6 val=10`.

- [ ] **Step 4: Power-cycle the STC, re-read id=6**. Expect `val=10` (EEPROM persistence works).

- [ ] **Step 5: Commit**: `feat(firmware): !cfg get/set support for night-park fields`

---

## Task 3: Add ST_NIGHT_PARK state + transition logic

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (~line 1343-1349 for enum, ~line 1473+ for ST_TRACK body, new section for ST_NIGHT_PARK body, ~line 1717 for state_to_string)

**Goal:** Add the new state and its entry/exit conditions. Storm + calibration paths must still preempt night park.

- [ ] **Step 1: Add `ST_NIGHT_PARK` to the state enum** (~line 1343-1349):

```c
enum state {
    ST_IDLE,
    ST_MENU,
    ST_TRACK,
    ST_STORM,
    ST_NIGHT_PARK,
    ST_CAL,
};
```

- [ ] **Step 2: Add the `state_to_string` case** (~line 1717):

```c
case ST_NIGHT_PARK:    return "night";
```

- [ ] **Step 3: Add a sun-sum helper function** alongside the existing sun-reading helpers (find them by grepping for `read_sun` or by looking at where `track_dN` / `track_dE` are computed):

```c
/* Returns the average ADC value across the 4 sun sensors.  Used by the
 * night-park detector: when this stays below night_park_dark_thr for
 * night_park_dark_min consecutive minutes, transition to ST_NIGHT_PARK. */
static unsigned char read_sun_avg_(void) {
    unsigned int sum = 0;
    sum += adc_read(0);   /* East  -- P1.0 / ADC0 */
    sum += adc_read(1);   /* West  -- P1.1 / ADC1 */
    sum += adc_read(2);   /* South -- P1.2 / ADC2 */
    sum += adc_read(3);   /* North -- P1.3 / ADC3 */
    return (unsigned char)(sum >> 2);  /* divide by 4 */
}
```

Verify the `adc_read()` function and channel numbers match what the existing track code uses. If the existing track code uses a different ADC accessor (e.g. cached values from the main loop's sampling), use that instead.

- [ ] **Step 4: Add the dark-timer state variables** in the same block where `storm_dwell_start_ms` lives (~line 792):

```c
static __xdata unsigned long night_dark_start_ms = 0;
static __xdata unsigned char night_dark_armed = 0;  /* 1 once timer is accumulating */
static __xdata unsigned char night_light_streak = 0;  /* consecutive bright samples in ST_NIGHT_PARK */
```

- [ ] **Step 5: Add the entry-condition check at the end of the ST_TRACK body** (~line 1472-1530, just before the `break` that ends ST_TRACK):

```c
/* Night-park entry check.  Only ST_TRACK can transition to ST_NIGHT_PARK
 * (the panel must already be in a normal tracking pose; calibration / menu /
 * idle are not auto-displaced). */
if (night_park_enable) {
    unsigned char avg = read_sun_avg_();
    if (avg < night_park_dark_thr) {
        if (!night_dark_armed) {
            night_dark_start_ms = now;
            night_dark_armed = 1;
        }
        unsigned long need = (unsigned long)night_park_dark_min * 60000UL;
        if (now - night_dark_start_ms >= need) {
            goto_active = 1;
            goto_target_az_pct = night_park_az_pct;
            goto_target_el_pct = night_park_el_pct;
            night_light_streak = 0;
            *state = ST_NIGHT_PARK;
            night_dark_armed = 0;  /* reset for next cycle */
        }
    } else {
        night_dark_armed = 0;  /* any bright sample resets the timer */
        night_dark_start_ms = 0;
    }
}
```

- [ ] **Step 6: Add the ST_NIGHT_PARK body** right after the ST_STORM body (~line 1622-1690):

```c
case ST_NIGHT_PARK: {
    /* Body: the existing goto framework drives the move.  Once goto_active
     * goes back to 0 (both axes within tolerance), the panel sits at the
     * configured east+flat position waiting for dawn.
     *
     * Wake-up: any sun sensor brighter than (dark_thr + 20) for 6
     * consecutive ~10s samples returns to ST_TRACK.  The 20-count
     * hysteresis prevents flapping during dawn light rise.  6 samples
     * (~60s) prevents headlight beams / lightning flashes from waking. */
    if (now - last_light_sample_ms >= 10000UL) {
        last_light_sample_ms = now;
        if (read_sun_avg_() > (unsigned char)(night_park_dark_thr + 20)) {
            night_light_streak++;
            if (night_light_streak >= 6) {
                goto_active = 0;  /* cancel any in-flight goto */
                night_light_streak = 0;
                *state = ST_TRACK;
            }
        } else {
            night_light_streak = 0;
        }
    }
    break;
}
```

Add the `last_light_sample_ms` global in the same block as the other `__xdata` timer vars (~line 792):

```c
static __xdata unsigned long last_light_sample_ms = 0;
```

- [ ] **Step 7: Update the menu state-list** so manual menu navigation can't transition to ST_NIGHT_PARK directly (it's an automatic-only state). Find the menu transition table (~line 2510, look for `case MENU_TRACK`) and confirm there's no entry that selects ST_NIGHT_PARK. Add a code comment noting "ST_NIGHT_PARK is automatic-only — not menu-selectable."

- [ ] **Step 8: Verify storm preemption** still works. The existing `storm_check()` runs in the main loop and forces `*state = ST_STORM` whenever wind exceeds the storm threshold, regardless of current state. With ST_NIGHT_PARK added, that should still pre-empt correctly — but verify by reading the storm_check body and confirming it doesn't have any "only from ST_TRACK" guard.

- [ ] **Step 9: Build and bench-test by lowering thresholds** so the timer fires fast:
  ```
  python tools/scripts/send-cmd.py "!cfg set id=8 val=1"    # dark_min = 1
  python tools/scripts/send-cmd.py "!cfg set id=9 val=200"  # dark_thr = 200 (effectively "always dark")
  ```
  Wait ~70 s. Observe LCD: should switch to "night" mode and panel should slew to east+flat.
  Then:
  ```
  python tools/scripts/send-cmd.py "!cfg set id=9 val=5"    # dark_thr = 5 (effectively "always bright")
  ```
  Wait ~60 s. LCD should switch back to "track."

- [ ] **Step 10: Restore safe defaults**:
  ```
  python tools/scripts/send-cmd.py "!cfg set id=8 val=30"
  python tools/scripts/send-cmd.py "!cfg set id=9 val=30"
  ```

- [ ] **Step 11: Commit**: `feat(firmware): ST_NIGHT_PARK state with autonomous entry/exit`

---

## Task 4: Add !sim_dark debug command for bench testing

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (~the UART command dispatcher, near the `!park` / `!release` / `!cal` / `!cfg` handlers)

**Goal:** Let the bench harness force the "all sensors dark" state without physically covering the sensors. The command flips a flag that `read_sun_avg_()` consults; when set, the function returns 0 (full dark) regardless of actual ADC readings.

- [ ] **Step 1: Add a sim flag** in the test-hook block:

```c
static __xdata unsigned char sim_dark_active = 0;
```

- [ ] **Step 2: Modify `read_sun_avg_()`** to consult the flag:

```c
static unsigned char read_sun_avg_(void) {
    if (sim_dark_active) return 0;
    unsigned int sum = 0;
    sum += adc_read(0);
    sum += adc_read(1);
    sum += adc_read(2);
    sum += adc_read(3);
    return (unsigned char)(sum >> 2);
}
```

- [ ] **Step 3: Add the `!sim_dark` UART command handler** alongside the other `!`-prefixed commands:

```c
/* !sim_dark on/off -- bench-test hook: force read_sun_avg_() to return 0
 * regardless of actual ADC.  Lets bench_test.py drive the night-park
 * detector without physically occluding the sensors. */
if (strncmp(p + 1, "sim_dark ", 9) == 0) {
    const char *q = p + 10;
    if (strncmp(q, "on", 2) == 0)  sim_dark_active = 1;
    else if (strncmp(q, "off", 3) == 0) sim_dark_active = 0;
    return;
}
```

- [ ] **Step 4: Bench-verify**:
  ```
  python tools/scripts/send-cmd.py "!cfg set id=8 val=1"   # dark_min = 1
  python tools/scripts/send-cmd.py "!sim_dark on"
  ```
  Wait ~70 s. LCD should switch to "night."
  ```
  python tools/scripts/send-cmd.py "!sim_dark off"
  ```
  Wait ~60 s. LCD should switch back to "track."

- [ ] **Step 5: Commit**: `feat(firmware): !sim_dark debug command for bench testing`

---

## Task 5: Add LCD menu page for night-park config

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (~the menu definitions and rendering)

**Goal:** Operator can view/edit the five night-park fields from the LCD without HA. Adds a sub-menu under Settings.

This task is medium-effort and adds five new menu items. If the LCD UX feels cluttered, consider grouping into a single "Night" page that scrolls through the five fields with button-decoder navigation. **Implementer's discretion** — the firmware-side menu code already has the pattern; mirror it.

- [ ] **Step 1: Find the menu definition** (grep for `MENU_SETTINGS` or `MENU_TRACK`). Read it end-to-end so you understand the pattern.

- [ ] **Step 2: Add a `MENU_NIGHT_PARK` sub-menu entry** under Settings.

- [ ] **Step 3: For each of the five fields, add a menu page** with the same edit-mode pattern used for wind_storm_mps / track_thresh.

- [ ] **Step 4: Hand-test on the LCD**:
  - Navigate to Settings → Night Park.
  - Toggle enable on/off — verify next status poll reports the change.
  - Bump dark_min to 60 — verify next reboot persists it.

- [ ] **Step 5: Commit**: `feat(firmware): LCD menu pages for night-park config`

---

## Task 6: ESP bridge handler for the five new cfg fields

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h` (~line 388-440 for the cfg-reply handler, ~line 706+ for cfg_poll, dispatcher for SET_REQ)

**Goal:** The ESP bridge already polls `{1,2,3,4}` every 30 s. Extend to `{1..9}` and route the new reads to the new local entity pointers + the broadcast GET_RESP frame.

- [ ] **Step 1: Add the five new CFG_F_* constants** in `tracker_bridge.h` (look for where `CFG_F_WIND_STORM = 1` etc. are defined, or just hardcode the IDs in the switch like main.c does):

```cpp
static constexpr uint8_t CFG_F_NIGHT_PARK_EN  = 5;
static constexpr uint8_t CFG_F_NIGHT_PARK_AZ  = 6;
static constexpr uint8_t CFG_F_NIGHT_PARK_EL  = 7;
static constexpr uint8_t CFG_F_NIGHT_PARK_MIN = 8;
static constexpr uint8_t CFG_F_NIGHT_PARK_THR = 9;
```

- [ ] **Step 2: Add the five new local entity pointers** alongside `local_wind_storm_num_` etc.:

```cpp
#ifdef USE_NUMBER
number::Number *local_night_park_az_num_{nullptr};
number::Number *local_night_park_el_num_{nullptr};
number::Number *local_night_park_min_num_{nullptr};
number::Number *local_night_park_thr_num_{nullptr};
#endif
#ifdef USE_SWITCH
switch_::Switch *local_night_park_en_switch_{nullptr};
#endif
```

- [ ] **Step 3: Extend the `handle_payload_` cfg-reply path** (~line 414, the `bool known_field = (cfg_id >= 1 && cfg_id <= 4);` check) to accept ids 1-9:

```cpp
bool known_field = (cfg_id >= 1 && cfg_id <= 9);
```

And extend the inner switch (~line 427) to dispatch to the new local entities:

```cpp
case 5: /* CFG_F_NIGHT_PARK_EN — switch path, handled below */ break;
case 6: local_num = local_night_park_az_num_;  break;
case 7: local_num = local_night_park_el_num_;  break;
case 8: local_num = local_night_park_min_num_; break;
case 9: local_num = local_night_park_thr_num_; break;
```

Add the special case for the switch (fid == 5) just after the number-update block:

```cpp
#ifdef USE_SWITCH
if (fid == CFG_F_NIGHT_PARK_EN && local_night_park_en_switch_) {
    local_night_park_en_switch_->publish_state(fval != 0);
}
#endif
```

- [ ] **Step 4: Extend the `cfg_poll` lambda** (~line 727) to read all 9 IDs. With P5-11's stagger, this means 9 × 150 ms = 1.35 s total burst time, still well under the 30 s cadence:

```cpp
for (uint8_t i = 0; i < 9; i++) {
    uint8_t fid = uint8_t(i + 1);
    this->set_timeout(uint32_t(i) * 150u, [this, fid]() {
      char buf[20];
      int sn = snprintf(buf, sizeof(buf), "!cfg get id=%u", (unsigned)fid);
      if (sn > 0 && (size_t)sn < sizeof(buf)) this->write_str_frame_(buf);
    });
}
```

- [ ] **Step 5: Add setter methods** for the platform schema:

```cpp
#ifdef USE_NUMBER
void set_night_park_az_number(number::Number *n)  { local_night_park_az_num_ = n; }
void set_night_park_el_number(number::Number *n)  { local_night_park_el_num_ = n; }
void set_night_park_min_number(number::Number *n) { local_night_park_min_num_ = n; }
void set_night_park_thr_number(number::Number *n) { local_night_park_thr_num_ = n; }
#endif
#ifdef USE_SWITCH
void set_night_park_en_switch(switch_::Switch *s) { local_night_park_en_switch_ = s; }
#endif
```

- [ ] **Step 6: Build the ESP firmware via the dashboard**. Expect a clean build with no new warnings.

- [ ] **Step 7: Commit**: `feat(esp): night-park cfg field handling in tracker_bridge`

---

## Task 7: Number platform schema for the four new sliders

**Files:**
- Modify: `esphome/components/tracker_bridge/number.py`

**Goal:** Add `night_park_az_pct`, `night_park_el_pct`, `night_park_dark_min`, `night_park_dark_thresh` to the platform schema. The pattern is identical to `wind_storm_mps` etc. already in the file.

- [ ] **Step 1: Read the existing schema** to understand the pattern. Look for how `wind_storm_mps` is declared and wired through.

- [ ] **Step 2: Add the four schema entries** with appropriate min/max/step:

```python
"night_park_az_pct":   slider_schema(min_value=0, max_value=100, step=1),
"night_park_el_pct":   slider_schema(min_value=0, max_value=100, step=1),
"night_park_dark_min": slider_schema(min_value=1, max_value=120, step=1),
"night_park_dark_thr": slider_schema(min_value=5, max_value=200, step=1),
```

(Names match what the YAML will use.)

- [ ] **Step 3: Wire each slider** to call the matching `set_night_park_*_number` setter on TrackerBridge AND to send a `!cfg set id=N val=V` when the user moves it. The existing code for `wind_storm_mps` shows the pattern — copy it.

- [ ] **Step 4: Commit**: `feat(esp): night-park number platform schema`

---

## Task 8: Switch platform schema for night_park_enable

**Files:**
- Create or Modify: `esphome/components/tracker_bridge/switch.py`

**Goal:** Add a `night_park_enable` switch to the platform schema. ESPHome's switch.py pattern is straightforward — declares an HA boolean switch backed by a tracker_bridge setter + a `!cfg set id=5 val=0|1` send.

- [ ] **Step 1: Check whether `switch.py` exists yet** under `esphome/components/tracker_bridge/`. If not, create it.

- [ ] **Step 2: Implement the switch schema** mirroring number.py's pattern. The switch's `write_state` lambda calls `id(bridge).night_park_set_enable_(state)` which sends `!cfg set id=5 val=N`.

- [ ] **Step 3: Add `night_park_set_enable_(bool)` method to tracker_bridge.h** that constructs the UART command:

```cpp
void night_park_set_enable(bool en) {
    char buf[20];
    int sn = snprintf(buf, sizeof(buf), "!cfg set id=5 val=%u", en ? 1u : 0u);
    if (sn > 0 && (size_t)sn < sizeof(buf)) this->write_str_frame_(buf);
}
```

- [ ] **Step 4: Commit**: `feat(esp): night-park enable switch platform`

---

## Task 9: Wire night-park entities into both YAMLs

**Files:**
- Modify: `EcoWorthyFirmware/esphome/solar-tracker-1.yaml`
- Modify: `EcoWorthyFirmware/esphome/solar-tracker-2.yaml`

**Goal:** Expose the four sliders + one switch on each tracker's HA device.

- [ ] **Step 1: Add the four number entries** to each tracker's existing `number:` block (alongside `wind_storm_mps` etc.):

```yaml
night_park_az_pct:
  name: "$friendly_name Night Park East"
  min_value: 0
  max_value: 100
  step: 1
night_park_el_pct:
  name: "$friendly_name Night Park Tilt"
  min_value: 0
  max_value: 100
  step: 1
night_park_dark_min:
  name: "$friendly_name Night Park Dark Minutes"
  min_value: 1
  max_value: 120
  step: 1
night_park_dark_thr:
  name: "$friendly_name Night Park Dark Threshold"
  min_value: 5
  max_value: 200
  step: 1
```

- [ ] **Step 2: Add the switch entry** to each tracker's YAML (a NEW switch block under tracker_bridge, separate from the template Test Offline switch):

```yaml
switch:
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    night_park_enable:
      name: "$friendly_name Night Park Enable"
  - platform: template
    name: "$friendly_name Test Offline"
    # ... existing test_offline switch block ...
```

- [ ] **Step 3: Reflash both nodes via the dashboard**.

- [ ] **Step 4: HA-side check** — verify the four sliders + switch appear on both tracker devices with values matching what `!cfg get` reports from a direct UART query.

- [ ] **Step 5: Commit**: `feat(yaml): expose night-park sliders + switch in both tracker YAMLs`

---

## Task 10: Bench tests via !sim_dark

**Files:**
- Modify: `tools/bench_test.py`

**Goal:** Two new automated tests that exercise the night-park entry and wake-up paths without physical sensor occlusion. Uses the `!sim_dark` debug hook from Task 4 — these tests are gated on that hook being live in the firmware.

- [ ] **Step 1: Add `test_night_park_engages_after_dark_timer`** to the test list:

```python
async def test_night_park_engages_after_dark_timer(h: TestHarness) -> None:
    """
    Set dark_min to 1 minute, sim_dark on, verify mode transitions to
    "night" within ~80 s.  Restore original dark_min and sim_dark off in
    finally.
    """
    t1 = "solar-tracker-1"
    orig_dark_min = h._entity_state[t1].get("night_park_dark_min")
    await h.set_number(t1, "night_park_dark_min", 1.0)
    await asyncio.sleep(2)
    since = time.monotonic()
    try:
        # NOTE: !sim_dark goes over the UART, not via an HA entity.
        # Add a harness helper or use the existing send_uart_cmd if
        # we re-enable a UART injection path post-P5-12.  Alternative:
        # add an HA switch for sim_dark mirroring the test_offline pattern.
        await h.send_uart_cmd(t1, "!sim_dark on")
        await h.expect_entity(t1, "mode", "night", timeout=80)
    finally:
        await h.send_uart_cmd(t1, "!sim_dark off")
        if orig_dark_min is not None:
            await h.set_number(t1, "night_park_dark_min", float(orig_dark_min))
```

**Important:** the harness currently has no UART injection path (stream_server was dropped in P5-12). Two options for sending `!sim_dark`:
1. Expose `sim_dark` as an HA switch (mirror `test_offline`'s template-switch pattern, wired to `night_park_set_sim_dark(bool)` on the bridge — same idiom we used for the test hook). Recommended.
2. Re-add stream_server pinned to a known-good SHA. Carries the risk we hit on 2026-05-22.

Pick option 1 for this plan. Add the harness via `set_switch(t1, "sim_dark", True)` instead of `send_uart_cmd`.

- [ ] **Step 2: Add the `sim_dark` template switch** to both YAMLs (in the same block as `test_offline`):

```yaml
- platform: template
  name: "$friendly_name Sim Dark"
  id: sim_dark_switch
  optimistic: true
  entity_category: diagnostic
  icon: mdi:weather-night
  turn_on_action:
    - lambda: id(bridge).night_park_set_sim_dark(true);
  turn_off_action:
    - lambda: id(bridge).night_park_set_sim_dark(false);
```

And add `night_park_set_sim_dark(bool)` to tracker_bridge.h:

```cpp
void night_park_set_sim_dark(bool on) {
    char buf[20];
    int sn = snprintf(buf, sizeof(buf), "!sim_dark %s", on ? "on" : "off");
    if (sn > 0 && (size_t)sn < sizeof(buf)) this->write_str_frame_(buf);
}
```

- [ ] **Step 3: Add `test_night_park_releases_on_light_return`**:

```python
async def test_night_park_releases_on_light_return(h: TestHarness) -> None:
    """
    Force the tracker into night park (sim_dark on + dark_min=1, wait 80s),
    then turn sim_dark off and verify mode returns to "track" within ~90s
    (6 consecutive ~10s bright samples + margin).
    """
    t1 = "solar-tracker-1"
    orig_dark_min = h._entity_state[t1].get("night_park_dark_min")
    await h.set_number(t1, "night_park_dark_min", 1.0)
    await asyncio.sleep(2)
    try:
        await h.set_switch(t1, "sim_dark", True)
        await h.expect_entity(t1, "mode", "night", timeout=80)
        await h.set_switch(t1, "sim_dark", False)
        await h.expect_entity(t1, "mode", "track", timeout=90)
    finally:
        await h.set_switch(t1, "sim_dark", False)
        if orig_dark_min is not None:
            await h.set_number(t1, "night_park_dark_min", float(orig_dark_min))
```

- [ ] **Step 4: Add both tests to the `ALL_TESTS` list** in bench_test.py.

- [ ] **Step 5: Run the new tests** individually first:
  ```
  python tools/bench_test.py --test test_night_park_engages_after_dark_timer
  python tools/bench_test.py --test test_night_park_releases_on_light_return
  ```
  Both should pass.

- [ ] **Step 6: Run the full suite** and confirm 7-pass-2-skip.

- [ ] **Step 7: Commit**: `test: night-park engage + release bench tests`

---

## Task 11: Documentation pass

**Files:**
- Modify: `CLAUDE.md` (top-of-file "Repository status" paragraph)
- Modify: `EcoWorthyFirmware/README.md` (if it exists; add a "Night-park" section)

**Goal:** Future-you reading CLAUDE.md fresh should see night-park called out as a feature, with the operator-tunable knobs listed. The CLAUDE.md "Phases 0–4 are complete" paragraph should be updated to mention the night-park addition.

- [ ] **Step 1: Update CLAUDE.md** with a sentence like: "A **night-park** mode automatically moves the panel to a configured east-facing flat-tilt position when all four sun sensors read below the operator-set threshold for a configurable number of minutes, so the first morning sun is detected immediately; wakes back to ST_TRACK when light returns."

- [ ] **Step 2: Add the five new CFG field IDs (0x05-0x09) to any cfg-protocol documentation** in CLAUDE.md or a doc/file that lists them.

- [ ] **Step 3: Commit**: `docs: document night-park feature in CLAUDE.md`

---

## Validation summary

After all tasks ship:
- LCD shows "night" mode at night and "track" during day.
- HA shows five new entities per tracker device (4 sliders + 1 switch).
- Operator can disable night-park entirely via the switch.
- Bench harness passes 7 automated tests + 2 manual skips.
- Panel reliably catches the first morning sun within minutes of dawn (manual verification — observe one full day cycle after install).
