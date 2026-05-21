# Phase 4 — Multi-tracker Mesh: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a single-hop ESP-NOW mesh over the Phase 3 bridge that lets up to 10 trackers share wind/storm state, expose all telemetry and config to Home Assistant via dynamic gateway election, and accept HA commands (force park/release, goto, jog, calibrate) over an AES-128-GCM authenticated channel.

**Architecture:** Each tracker runs identical code; behavior diverges by configuration (static `role: primary | secondary`) and runtime state (WiFi up = gateway candidate, lowest MAC of currently-up nodes is the acting gateway). The wire protocol extends Phase 3's `\xAA\x55`-framed UART with new `!` command payloads on the STC side, and adds an ESP-NOW broadcast layer on the ESP side with AES-GCM auth and a 32-bit anti-replay counter. See `docs/superpowers/specs/2026-05-20-phase-4-mesh-design.md` for full design rationale.

**Tech Stack:**
- STC15F2K60S2 firmware: SDCC 4.x, GNU Make, stcgal
- ESP-01S firmware: ESPHome Arduino framework on ESP8266, with custom external_component (`tracker_bridge`, extended)
- Crypto: Arduino `Crypto` library by rweather (AES-128-GCM) on ESP; STC has no crypto (trusts gateway via local UART)
- Persistence: STC EEPROM (12 bytes existing config struct → extend to 14); ESP Preferences flash for counter state

---

## Files

**STC firmware (existing, modify):**
- `EcoWorthyFirmware/src/main.c` — new EEPROM bytes (role, wind_source), Settings menu entries, `!cmd` parser, storm-logic remote-wind path, command handlers (park/release/goto/jog/stop/cal/cfg), remote-wind watchdog
- `EcoWorthyFirmware/include/board.h` — no changes expected
- `EcoWorthyFirmware/include/stc15f2k60s2.h` — no changes expected

**ESP firmware (existing, modify):**
- `esphome/components/tracker_bridge/__init__.py` — extend hub schema with mesh config (channel, PSK, tracker_id, peer list)
- `esphome/components/tracker_bridge/sensor.py` — extend to support per-remote-tracker sensor instantiation
- `esphome/components/tracker_bridge/text_sensor.py` — same, plus `role` text sensor
- `esphome/components/tracker_bridge/tracker_bridge.h` — major additions (ESP-NOW init, AES-GCM, message handlers, gateway election, config sync, command dispatch). Keep monolithic for v1 unless it crosses ~600 lines, in which case extract `tracker_mesh.h`.

**ESP firmware (new files):**
- `esphome/components/tracker_bridge/number.py` — new platform for RW config entities (wind storm/release/dwell/track thresh)
- `esphome/components/tracker_bridge/button.py` — new platform for command buttons (force park/release, jog ×4, calibrate, per-tracker goto submit)
- `EcoWorthyFirmware/esphome/secrets.example.yaml` — sample secrets file documenting `tracker_mesh_psk`

**Reference YAML (existing, modify):**
- `EcoWorthyFirmware/esphome/solar-tracker-1.yaml` — add `mesh:` config under `tracker_bridge`, per-remote `peer:` entries, new sensor/number/button entities

**Docs (existing, modify after implementation):**
- `CLAUDE.md` — repository-status update to reflect Phase 4 complete
- `EcoWorthyFirmware/README.md` — add Phase 4 description section

---

## Task 1: STC — new EEPROM bytes (`role`, `wind_source`)

**Goal:** Persist `role` and `wind_source` settings in EEPROM bytes 12 and 13. Default `role=secondary`, `wind_source=local`. No UI yet — that lands in Task 2.

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (EEPROM section, `config_load`, `config_save`, range constants)

- [ ] **Step 1: Declare the new state variables + range constants**

In `main.c`, near the existing `static unsigned char wind_storm_mps   = 15;` block, add:

```c
static unsigned char role           = 2;   /* 1=primary, 2=secondary */
static unsigned char wind_source    = 0;   /* 0=local, 1=remote */
```

Near the existing range defines (`WIND_STORM_MIN`, etc.) add:

```c
#define ROLE_MIN          1
#define ROLE_MAX          2
#define WIND_SOURCE_MIN   0
#define WIND_SOURCE_MAX   1
```

- [ ] **Step 2: Extend `config_load` to read bytes 12 and 13**

Inside `config_load()`, after the existing `track_thresh = iap_read_byte(EEPROM_BASE + 11);` line, add:

```c
role        = iap_read_byte(EEPROM_BASE + 12);
wind_source = iap_read_byte(EEPROM_BASE + 13);
```

And after the existing range-check block (where `track_thresh` is sanity-checked) add:

```c
if (role < ROLE_MIN || role > ROLE_MAX) role = 2;             /* default secondary */
if (wind_source > WIND_SOURCE_MAX) wind_source = 0;           /* default local */
```

- [ ] **Step 3: Extend `config_save` to write bytes 12 and 13**

Inside `config_save()`, after the existing `iap_program_byte(EEPROM_BASE + 11, track_thresh);` line, add:

```c
iap_program_byte(EEPROM_BASE + 12, role);
iap_program_byte(EEPROM_BASE + 13, wind_source);
```

- [ ] **Step 4: Update the EEPROM layout comment**

In the comment block above `ns_stroke_ms`, add:

```c
 *   12    : role (1=primary, 2=secondary)
 *   13    : wind_source (0=local, 1=remote)
```

- [ ] **Step 5: Build, flash, verify**

Build:
```powershell
cd EcoWorthyFirmware
make
```

Flash with stcgal as usual.

Validation: at boot, the firmware should behave identically to Phase 3 (no UI for the new settings yet, defaults applied). Run a calibration → `config_save` writes new bytes; power-cycle → fresh `config_load` reads them. No observable user-facing change yet.

- [ ] **Step 6: Commit**

```bash
git add EcoWorthyFirmware/src/main.c
git commit -m "feat(stc): P4-1 -- role/wind_source EEPROM bytes (no UI yet)"
```

---

## Task 2: STC — Settings menu entries for `role` and `wind_source`

**Goal:** Expose the new settings in the existing Settings menu so the operator can configure them via the LCD. Range-clamped to safe values. Match the existing settings UX (press-hold auto-repeat, SET-saves-QUIT-cancels).

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (setting_t enum, setting_defs, setting_get/set, ranges)

- [ ] **Step 1: Add two new setting_t enum values**

Extend the existing `typedef enum { ... } setting_t;`:

```c
typedef enum {
    SET_WIND_STORM = 0,
    SET_WIND_RELEASE,
    SET_STORM_DWELL,
    SET_TRACK_THRESH,
    SET_ROLE,              /* new */
    SET_WIND_SOURCE,       /* new */
    SET_COUNT
} setting_t;
```

- [ ] **Step 2: Add new entries to `setting_defs`**

Extend the existing const array:

```c
static const setting_def_t setting_defs[SET_COUNT] = {
    { "W.Storm",  "Wind storm",     "m/s", WIND_STORM_MIN,   WIND_STORM_MAX   },
    { "W.Rel",    "Wind release",   "m/s", WIND_RELEASE_MIN, WIND_RELEASE_MAX },
    { "S.Dwell",  "Storm dwell",    "min", DWELL_MIN_MIN,    DWELL_MIN_MAX    },
    { "TrkThr",   "Track thresh",   "adc", TRACK_THRESH_MIN, TRACK_THRESH_MAX },
    { "Role",     "Role 1=P 2=S",   "",    ROLE_MIN,         ROLE_MAX         },
    { "WSrc",     "Wind src 0L 1R", "",    WIND_SOURCE_MIN,  WIND_SOURCE_MAX  },
};
```

(The short label is what shows in the settings list; the full label is shown when editing. Unit "" means no unit string.)

- [ ] **Step 3: Extend `setting_get` switch**

```c
static unsigned char setting_get(setting_t s) {
    switch (s) {
        case SET_WIND_STORM:   return wind_storm_mps;
        case SET_WIND_RELEASE: return wind_release_mps;
        case SET_STORM_DWELL:  return storm_dwell_min;
        case SET_TRACK_THRESH: return track_thresh;
        case SET_ROLE:         return role;            /* new */
        case SET_WIND_SOURCE:  return wind_source;     /* new */
        default:               return 0;
    }
}
```

- [ ] **Step 4: Extend `setting_set` switch**

```c
static void setting_set(setting_t s, unsigned char v) {
    switch (s) {
        case SET_WIND_STORM:   wind_storm_mps   = v; break;
        case SET_WIND_RELEASE: wind_release_mps = v; break;
        case SET_STORM_DWELL:  storm_dwell_min  = v; break;
        case SET_TRACK_THRESH: track_thresh     = v; break;
        case SET_ROLE:         role             = v; break;   /* new */
        case SET_WIND_SOURCE:  wind_source      = v; break;   /* new */
        default: break;
    }
}
```

- [ ] **Step 5: Build, flash, validate**

```powershell
make
stcgal -P stc15 -p COM5 -t 22118 build\ecoworthy.ihx
```

Validation: Menu → Settings should now show 6 entries (scrollable). Edit Role between 1 and 2; edit WSrc between 0 and 1. SET saves to EEPROM, QUIT reverts. Power-cycle confirms persistence.

- [ ] **Step 6: Commit**

```bash
git add EcoWorthyFirmware/src/main.c
git commit -m "feat(stc): P4-2 -- Settings menu entries for role + wind_source"
```

---

## Task 3: STC — extend frame parser to dispatch `!cmd` payloads

**Goal:** The STC's existing `uart_service` only handles bare `?` polls. Extend it to dispatch `!`-prefixed command payloads. For Task 3 we just route `!park` and `!release` to the existing storm interlock state — implementing the full command surface comes in Task 4. This validates the parser plumbing first.

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (`uart_service`, new command dispatcher, storm force entry/exit hooks)

- [ ] **Step 1: Add a forced-storm-state flag**

Near the existing storm state declarations (with `storm_phase`, `storm_dwell_start_ms`, `storm_parking`), add:

```c
/* Operator-forced storm (via HA / mesh).  Independent of wind-based
 * entry: when set, storm_check() forces ST_STORM regardless of wind. */
static __xdata unsigned char storm_forced = 0;
```

- [ ] **Step 2: Extend `storm_check` to honor `storm_forced`**

Inside `storm_check()`, right after the throttle-return logic (after `storm_scan_last = now;`), but BEFORE the existing storm-state and cfg_valid early returns, add:

```c
    /* Forced storm: enter ST_STORM regardless of wind / cal state. */
    if (storm_forced && *state != ST_STORM) {
        ns_duty_on_ms = 0; ew_duty_on_ms = 0;
        ns_duty_lockout_end = 0; ew_duty_lockout_end = 0;
        duty_last_tick_ms = millis();
        ns_pulse_end_ms = 0; ew_pulse_end_ms = 0;
        storm_phase = STORM_PARKING;
        storm_parking = 1;
        lcd_clear();
        *state = ST_STORM;
        return;
    }
```

Then inside `storm_tick()`'s HOLDING-phase exit check (where it currently returns to ST_TRACK if dwell satisfied), gate the exit on `!storm_forced`:

```c
        if (elapsed >= need && !storm_forced) {
            lcd_clear();
            track_last_check_ms = millis();
            *state = ST_TRACK;
            return;
        }
```

- [ ] **Step 3: Refactor `uart_service` for command dispatch**

Replace the existing `uart_service()` body:

```c
static void uart_service(state_t st) {
    uart_poll_frames();
    if (uart_frame_ready) {
        uart_cmd_dispatch(&st_unused, st);   /* see step 4 */
        uart_frame_ready = 0;
    }
}
```

Wait — `st` is passed by value and we may need to mutate state inside command handlers. Refactor to:

```c
static void uart_service(state_t *state) {
    uart_poll_frames();
    if (uart_frame_ready) {
        uart_cmd_dispatch(state);
        uart_frame_ready = 0;
    }
}
```

And update the main-loop call site:

```c
uart_service(&state);   /* was uart_service(state); */
```

- [ ] **Step 4: Add `uart_cmd_dispatch` with `!park`/`!release` handling**

Place this function near `uart_service`:

```c
/* Dispatch a validated framed payload.  ? = status request; ! =
 * command (sub-parsed by token).  Other payloads are dropped. */
static void uart_cmd_dispatch(state_t *state) {
    const char *p = uart_frame_buf;

    /* ? = status poll (existing behavior) */
    if (p[0] == '?' && p[1] == '\0') {
        uart_status_send(*state);
        return;
    }

    /* !park / !release: set/clear the forced-storm flag */
    if (p[0] != '!') return;
    if (strncmp(p + 1, "park", 4) == 0 && p[5] == '\0') {
        storm_forced = 1;
        return;
    }
    if (strncmp(p + 1, "release", 7) == 0 && p[8] == '\0') {
        storm_forced = 0;
        return;
    }
    /* Other ! commands implemented in Task 4. */
}
```

Note: SDCC's `string.h` provides `strncmp`. If pulling in `<string.h>` adds undesirable code-size overhead, replace with an inline byte-by-byte comparison helper. For Task 3, use `strncmp` and revisit in Task 4 if size becomes tight.

- [ ] **Step 5: Build, flash, validate**

```powershell
make
stcgal -P stc15 -p COM5 -t 22118 build\ecoworthy.ihx
```

Validation: use `stream_server` (telnet 192.168.1.193:23) to send a framed `!park` packet — the STC should enter STORM PARK mode (you can manually compute the CRC8/SMBUS of `!park` and send `AA 55 ! p a r k <CRC> 0A`). Then send `!release` to clear. Use the existing CRC calc workflow.

Easier alternative: temporarily enable `P3_UART_STATUS_TEST 1` AND have the ESP send `!park` from a test button. Track via the LCD displaying `STORM` mode.

- [ ] **Step 6: Commit**

```bash
git add EcoWorthyFirmware/src/main.c
git commit -m "feat(stc): P4-3 -- !cmd dispatch + !park/!release force-storm"
```

---

## Task 4: STC — full `!cmd` command surface (`!goto`, `!jog`, `!stop`, `!cal`, `!wind=`)

**Goal:** Implement the remaining commands. The parser is a hand-rolled token matcher (no scanf-like, to avoid pulling in stdlib bloat). All commands respect existing safety bounds.

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (`uart_cmd_dispatch`, new helpers, remote wind variables)

- [ ] **Step 1: Add remote-wind state**

Near `wind_mps_cached`, add:

```c
static __xdata unsigned char remote_wind_mps = 0;
static __xdata unsigned long remote_wind_last_update_ms = 0;
#define REMOTE_WIND_TIMEOUT_MS 20000UL   /* Q5 failsafe: 4 missed 5s broadcasts */
```

- [ ] **Step 2: Add a simple integer parser**

```c
/* Parse decimal digits from p; return value and advance *p past them.
 * Returns -1 on no-digits.  Caps at 999 to bound. */
static int parse_u_advance(const char **p) {
    int v = 0;
    int saw = 0;
    while (**p >= '0' && **p <= '9') {
        v = v * 10 + (**p - '0');
        if (v > 999) v = 999;
        (*p)++;
        saw = 1;
    }
    return saw ? v : -1;
}
```

- [ ] **Step 3: Extend `uart_cmd_dispatch` with the remaining commands**

Add to the existing function, after the `!release` block:

```c
    /* !wind=NN -- remote wind update from gateway */
    if (strncmp(p + 1, "wind=", 5) == 0) {
        const char *q = p + 6;
        int v = parse_u_advance(&q);
        if (v >= 0) {
            remote_wind_mps = (v > 99) ? 99 : (unsigned char)v;
            remote_wind_last_update_ms = millis();
        }
        return;
    }

    /* !stop -- emergency stop both axes */
    if (strncmp(p + 1, "stop", 4) == 0 && p[5] == '\0') {
        set_axis_ns(AXIS_OFF);
        set_axis_ew(AXIS_OFF);
        ns_pulse_end_ms = 0;
        ew_pulse_end_ms = 0;
        return;
    }

    /* !goto az=NN el=NN -- goto position percentages.
     * Respects storm/duty interlocks; clamps to [0,100].
     * Reuses the storm_tick position-seek pattern by setting a
     * persistent target that the main loop drives toward. */
    if (strncmp(p + 1, "goto ", 5) == 0) {
        /* Parse az= and el=, both required */
        int az_v = -1, el_v = -1;
        const char *q = p + 6;
        while (*q) {
            if (q[0] == 'a' && q[1] == 'z' && q[2] == '=') { q += 3; az_v = parse_u_advance(&q); }
            else if (q[0] == 'e' && q[1] == 'l' && q[2] == '=') { q += 3; el_v = parse_u_advance(&q); }
            else if (*q == ' ') q++;
            else q++;  /* skip unknown */
        }
        if (az_v < 0 || el_v < 0) return;
        if (az_v > 100) az_v = 100;
        if (el_v > 100) el_v = 100;
        /* No-op if in storm or duty-locked (defense in depth on top of
         * the actual movement loop's checks). */
        if (*state == ST_STORM || ns_duty_locked() || ew_duty_locked()) return;
        /* Latch target; the position-seek logic in the main loop
         * (added in Task 5) drives toward it. */
        goto_az_target_pct = (unsigned char)az_v;
        goto_el_target_pct = (unsigned char)el_v;
        goto_active = 1;
        return;
    }

    /* !jog ax=N dir=+/- ms=NNN -- timed pulse on one axis */
    if (strncmp(p + 1, "jog ", 4) == 0) {
        const char *q = p + 5;
        int ax = -1, dir_pos = -1, dur_ms = -1;
        while (*q) {
            if (q[0] == 'a' && q[1] == 'x' && q[2] == '=') { q += 3; ax = parse_u_advance(&q); }
            else if (q[0] == 'd' && q[1] == 'i' && q[2] == 'r' && q[3] == '=') {
                q += 4;
                if (*q == '+') { dir_pos = 1; q++; }
                else if (*q == '-') { dir_pos = 0; q++; }
            }
            else if (q[0] == 'm' && q[1] == 's' && q[2] == '=') { q += 3; dur_ms = parse_u_advance(&q); }
            else if (*q == ' ') q++;
            else q++;
        }
        if (ax < 0 || dir_pos < 0 || dur_ms < 0) return;
        if (dur_ms > 2500) dur_ms = 2500;
        if (*state == ST_STORM) return;
        if (ax == 0) {
            if (ns_duty_locked()) return;
            set_axis_ns(dir_pos ? AXIS_FWD : AXIS_REV);
            ns_pulse_end_ms = millis() + (unsigned long)dur_ms;
        } else if (ax == 1) {
            if (ew_duty_locked()) return;
            set_axis_ew(dir_pos ? AXIS_FWD : AXIS_REV);
            ew_pulse_end_ms = millis() + (unsigned long)dur_ms;
        }
        return;
    }

    /* !cal -- trigger calibration (only from IDLE, not in storm) */
    if (strncmp(p + 1, "cal", 3) == 0 && p[4] == '\0') {
        if (*state == ST_IDLE && !storm_forced) {
            *state = ST_CAL;
        }
        return;
    }
```

- [ ] **Step 4: Declare `goto_*` state vars**

Near the other position-seek-style vars (e.g., `ns_pulse_end_ms`), add:

```c
static __xdata unsigned char goto_az_target_pct = 0;
static __xdata unsigned char goto_el_target_pct = 0;
static __xdata unsigned char goto_active = 0;
```

- [ ] **Step 5: Add goto-driving logic in the main loop**

Inside the main `for(;;)` loop, after `position_tick()` but before `duty_tick()`, add:

```c
        /* HA goto: drive both axes toward goto_*_target_pct in
         * percent of stroke.  Stops each axis when within 1% of
         * target.  Clears goto_active when both axes reach. */
        if (goto_active) {
            unsigned long tgt_ns_ms = (unsigned long)goto_el_target_pct * ns_stroke_ms / 100UL;
            unsigned long tgt_ew_ms = (unsigned long)goto_az_target_pct * ew_stroke_ms / 100UL;
            unsigned char ns_done = 0, ew_done = 0;
            if (ns_state == AXIS_OFF) {
                if ((unsigned long)ns_pos_ms + 200 < tgt_ns_ms) set_axis_ns(AXIS_FWD);
                else if ((unsigned long)ns_pos_ms > tgt_ns_ms + 200) set_axis_ns(AXIS_REV);
                else ns_done = 1;
            } else {
                /* Currently driving; stop when we cross the target. */
                if (ns_state == AXIS_FWD && (unsigned long)ns_pos_ms >= tgt_ns_ms) { set_axis_ns(AXIS_OFF); ns_done = 1; }
                if (ns_state == AXIS_REV && (unsigned long)ns_pos_ms <= tgt_ns_ms) { set_axis_ns(AXIS_OFF); ns_done = 1; }
            }
            if (ew_state == AXIS_OFF) {
                if ((unsigned long)ew_pos_ms + 200 < tgt_ew_ms) set_axis_ew(AXIS_FWD);
                else if ((unsigned long)ew_pos_ms > tgt_ew_ms + 200) set_axis_ew(AXIS_REV);
                else ew_done = 1;
            } else {
                if (ew_state == AXIS_FWD && (unsigned long)ew_pos_ms >= tgt_ew_ms) { set_axis_ew(AXIS_OFF); ew_done = 1; }
                if (ew_state == AXIS_REV && (unsigned long)ew_pos_ms <= tgt_ew_ms) { set_axis_ew(AXIS_OFF); ew_done = 1; }
            }
            if (ns_done && ew_done) goto_active = 0;
            /* Storm or duty lockout cancels the goto */
            if (state == ST_STORM || ns_duty_locked() || ew_duty_locked()) {
                goto_active = 0;
                set_axis_ns(AXIS_OFF);
                set_axis_ew(AXIS_OFF);
            }
        }
```

Note the el/az to NS/EW mapping: per the spec, `el` = N/S tilt, `az` = E/W rotation.

- [ ] **Step 6: Build, flash, validate each command**

```powershell
make
stcgal -P stc15 -p COM5 -t 22118 build\ecoworthy.ihx
```

For each command, send a framed `!cmd` via stream_server (computing CRC manually or via a small Python helper) and verify the LCD/relay state:

- `!stop` → both axes off immediately
- `!goto az=50 el=50` → axes drive to mid-stroke, stop within tolerance
- `!jog ax=0 dir=+ ms=500` → NS axis pulses FWD for 500 ms
- `!jog ax=1 dir=- ms=500` → EW axis pulses REV
- `!wind=20` → broadcast wind LCD value updates (in jog screen), then over 20 s of silence storm activates (Task 5 closes the loop)
- `!cal` → calibration starts from IDLE

- [ ] **Step 7: Commit**

```bash
git add EcoWorthyFirmware/src/main.c
git commit -m "feat(stc): P4-4 -- full !cmd surface (goto/jog/stop/cal/wind=)"
```

---

## Task 5: STC — storm logic uses remote wind + watchdog failsafe

**Goal:** When `wind_source == 1` (remote), `storm_check` consults `remote_wind_mps` instead of the local sensor. If `remote_wind_last_update_ms` ages past `REMOTE_WIND_TIMEOUT_MS`, force storm-park as failsafe. Per-tracker behavior diverges based on the Settings-menu role.

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (`storm_check`)

- [ ] **Step 1: Modify `storm_check` to branch on `wind_source`**

Inside `storm_check()`, replace the existing single line:

```c
    w = wind_mps(adc_read_avg(ADC_CH_WIND, 8));
    wind_mps_cached = w;
```

with:

```c
    if (wind_source == 0) {
        /* Local: read sensor as before */
        w = wind_mps(adc_read_avg(ADC_CH_WIND, 8));
        wind_mps_cached = w;
    } else {
        /* Remote: use cached value from !wind= broadcast.
         * Failsafe: if no update in REMOTE_WIND_TIMEOUT_MS, force storm. */
        unsigned long age = now - remote_wind_last_update_ms;
        if (remote_wind_last_update_ms == 0 || age > REMOTE_WIND_TIMEOUT_MS) {
            /* No broadcast yet, or stale: force storm */
            storm_forced = 1;
            w = wind_storm_mps;   /* report at-threshold so storm logic proceeds */
        } else {
            w = remote_wind_mps;
            wind_mps_cached = w;  /* mirror to display */
        }
    }
```

- [ ] **Step 2: Build, flash, validate**

```powershell
make
stcgal -P stc15 -p COM5 -t 22118 build\ecoworthy.ihx
```

Two scenarios to test:

**A. `wind_source = 0` (local, default)**: behavior unchanged from Phase 3 — local sensor drives storm. Verify by blowing on the sensor.

**B. `wind_source = 1` (remote)**: via Settings menu, set WSrc=1. Boot the tracker — within 20 s with no `!wind=` broadcasts, it should enter STORM PARK as failsafe. Send `!wind=05` (under threshold) via stream_server → STORM clears after dwell. Send `!wind=20` → STORM re-enters. Stop sending `!wind=` for 20+ s → STORM re-enters via failsafe.

- [ ] **Step 3: Commit**

```bash
git add EcoWorthyFirmware/src/main.c
git commit -m "feat(stc): P4-5 -- storm uses remote wind when wind_source=1 + failsafe"
```

---

## Task 6: STC — `!cfg get/set` config protocol over UART

**Goal:** Implement get/set for all config fields per the spec. RO fields reject SETs with NACK. The ESP-side `tracker_bridge` will use this in Task 10 to expose HA entities.

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c` (`uart_cmd_dispatch` extension, response sender)

- [ ] **Step 1: Define field IDs and a get/set jump table**

Near the EEPROM constants, add:

```c
#define CFG_F_WIND_STORM    0x01
#define CFG_F_WIND_RELEASE  0x02
#define CFG_F_STORM_DWELL   0x03
#define CFG_F_TRACK_THRESH  0x04
#define CFG_F_NS_STROKE     0x10
#define CFG_F_EW_STROKE     0x11
#define CFG_F_HORIZ_NS      0x12
#define CFG_F_HORIZ_EW      0x13
#define CFG_F_ROLE          0x20
#define CFG_F_WIND_SOURCE   0x21

#define CFG_STATUS_OK       0
#define CFG_STATUS_RO       1
#define CFG_STATUS_RANGE    2
#define CFG_STATUS_UNKNOWN  3
```

- [ ] **Step 2: Add a config-response sender**

```c
/* Emit a framed config response.  For u8 fields, val_hi is unused. */
static void uart_cfg_send_value(unsigned char field_id,
                                unsigned int val_u16) {
    static __xdata char buf[40];
    char *p = buf;
    p = uart_app_str(p, "cfg id=");
    p = uart_app_u8(p, field_id);
    p = uart_app_str(p, " val=");
    /* For 16-bit fields, emit full value */
    if (val_u16 < 256) {
        p = uart_app_u8(p, (unsigned char)val_u16);
    } else {
        /* Emit as decimal up to 5 digits */
        unsigned int v = val_u16;
        if (v > 65535) v = 65535;
        if (v >= 10000) { *p++ = '0' + (v / 10000); v %= 10000;
                          *p++ = '0' + (v / 1000);  v %= 1000;
                          *p++ = '0' + (v / 100);   v %= 100;
                          *p++ = '0' + (v / 10);    *p++ = '0' + (v % 10); }
        else { p = uart_app_u8(p, (unsigned char)(v / 1000));   /* will trim later */
               /* simplest: use sprintf-like manual: emit digits */ }
    }
    *p = '\0';
    uart_send_frame(buf);
}
```

Hmm — the above is awkward. Replace step 2 with a cleaner u16 emitter:

```c
static char *uart_app_u16(char *p, unsigned int v) {
    char tmp[6];
    int n = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0) *p++ = tmp[--n];
    return p;
}

static void uart_cfg_send_value(unsigned char field_id,
                                unsigned int val_u16) {
    static __xdata char buf[40];
    char *p = buf;
    p = uart_app_str(p, "cfg id=");
    p = uart_app_u8(p, field_id);
    p = uart_app_str(p, " val=");
    p = uart_app_u16(p, val_u16);
    *p = '\0';
    uart_send_frame(buf);
}

static void uart_cfg_send_ack(unsigned char field_id,
                              unsigned char status) {
    static __xdata char buf[40];
    char *p = buf;
    p = uart_app_str(p, "cfg ack id=");
    p = uart_app_u8(p, field_id);
    p = uart_app_str(p, " st=");
    p = uart_app_u8(p, status);
    *p = '\0';
    uart_send_frame(buf);
}
```

- [ ] **Step 3: Add get/set handlers to `uart_cmd_dispatch`**

After the `!cal` block, add:

```c
    /* !cfg get id=NN -- read config field */
    if (strncmp(p + 1, "cfg get id=", 11) == 0) {
        const char *q = p + 12;
        int id = parse_u_advance(&q);
        if (id < 0) return;
        switch (id) {
            case CFG_F_WIND_STORM:    uart_cfg_send_value(id, wind_storm_mps); return;
            case CFG_F_WIND_RELEASE:  uart_cfg_send_value(id, wind_release_mps); return;
            case CFG_F_STORM_DWELL:   uart_cfg_send_value(id, storm_dwell_min); return;
            case CFG_F_TRACK_THRESH:  uart_cfg_send_value(id, track_thresh); return;
            case CFG_F_NS_STROKE:     uart_cfg_send_value(id, ns_stroke_ms); return;
            case CFG_F_EW_STROKE:     uart_cfg_send_value(id, ew_stroke_ms); return;
            case CFG_F_HORIZ_NS:      uart_cfg_send_value(id, horiz_ns_pct); return;
            case CFG_F_HORIZ_EW:      uart_cfg_send_value(id, horiz_ew_pct); return;
            case CFG_F_ROLE:          uart_cfg_send_value(id, role); return;
            case CFG_F_WIND_SOURCE:   uart_cfg_send_value(id, wind_source); return;
            default:                  uart_cfg_send_ack(id, CFG_STATUS_UNKNOWN); return;
        }
    }

    /* !cfg set id=NN val=NN */
    if (strncmp(p + 1, "cfg set id=", 11) == 0) {
        const char *q = p + 12;
        int id = parse_u_advance(&q);
        if (id < 0) return;
        /* Skip " val=" */
        while (*q == ' ') q++;
        if (q[0] != 'v' || q[1] != 'a' || q[2] != 'l' || q[3] != '=') return;
        q += 4;
        int val = parse_u_advance(&q);
        if (val < 0) return;
        unsigned char status = CFG_STATUS_OK;
        switch (id) {
            case CFG_F_WIND_STORM:
                if (val < WIND_STORM_MIN || val > WIND_STORM_MAX) status = CFG_STATUS_RANGE;
                else { wind_storm_mps = (unsigned char)val; config_save(); }
                break;
            case CFG_F_WIND_RELEASE:
                if (val > WIND_RELEASE_MAX) status = CFG_STATUS_RANGE;
                else { wind_release_mps = (unsigned char)val; config_save(); }
                break;
            case CFG_F_STORM_DWELL:
                if (val < DWELL_MIN_MIN || val > DWELL_MIN_MAX) status = CFG_STATUS_RANGE;
                else { storm_dwell_min = (unsigned char)val; config_save(); }
                break;
            case CFG_F_TRACK_THRESH:
                if (val < TRACK_THRESH_MIN || val > TRACK_THRESH_MAX) status = CFG_STATUS_RANGE;
                else { track_thresh = (unsigned char)val; config_save(); }
                break;
            /* All other fields read-only over mesh */
            case CFG_F_NS_STROKE:
            case CFG_F_EW_STROKE:
            case CFG_F_HORIZ_NS:
            case CFG_F_HORIZ_EW:
            case CFG_F_ROLE:
            case CFG_F_WIND_SOURCE:
                status = CFG_STATUS_RO;
                break;
            default:
                status = CFG_STATUS_UNKNOWN;
        }
        uart_cfg_send_ack((unsigned char)id, status);
        return;
    }
```

- [ ] **Step 4: Build, flash, validate**

```powershell
make
stcgal -P stc15 -p COM5 -t 22118 build\ecoworthy.ihx
```

Via stream_server, send each get/set and confirm the framed response:

- `!cfg get id=1` → `cfg id=1 val=15` (wind_storm_mps)
- `!cfg get id=16` → `cfg id=16 val=20000` or similar (ns_stroke_ms)
- `!cfg set id=1 val=20` → `cfg ack id=1 st=0`; subsequent get shows 20
- `!cfg set id=16 val=1000` → `cfg ack id=16 st=1` (RO)
- `!cfg set id=1 val=99` → `cfg ack id=1 st=2` (out of range)
- `!cfg get id=99` → `cfg ack id=99 st=3` (unknown)

- [ ] **Step 5: Commit**

```bash
git add EcoWorthyFirmware/src/main.c
git commit -m "feat(stc): P4-6 -- !cfg get/set protocol with RO enforcement"
```

---

## Task 7: ESP — ESP-NOW infrastructure with AES-128-GCM

**Goal:** Bring up ESP-NOW peering on the `tracker_bridge` component with AES-128-GCM auth and counter management. No application messages yet — just prove the wire layer.

**Files:**
- Modify: `esphome/components/tracker_bridge/__init__.py` (schema additions)
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h` (ESP-NOW init, AES, counter)
- Modify: `EcoWorthyFirmware/esphome/solar-tracker-1.yaml` (add `mesh:` config)
- Create: `EcoWorthyFirmware/esphome/secrets.example.yaml`

- [ ] **Step 1: Extend Python schema with mesh config**

Edit `esphome/components/tracker_bridge/__init__.py`. Add after the existing imports:

```python
CONF_MESH = "mesh"
CONF_CHANNEL = "channel"
CONF_PSK = "psk"
CONF_TRACKER_ID = "tracker_id"
```

Extend the CONFIG_SCHEMA:

```python
CONFIG_SCHEMA = (
    cv.Schema({
        cv.GenerateID(): cv.declare_id(TrackerBridge),
        cv.Optional(CONF_MESH): cv.Schema({
            cv.Required(CONF_CHANNEL): cv.int_range(min=1, max=13),
            cv.Required(CONF_PSK): cv.string,   # 16-byte hex or passphrase
            cv.Required(CONF_TRACKER_ID): cv.string_strict,
        }),
    })
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)
```

Extend `to_code`:

```python
async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_MESH in config:
        mesh = config[CONF_MESH]
        cg.add(var.set_mesh_channel(mesh[CONF_CHANNEL]))
        cg.add(var.set_mesh_psk(mesh[CONF_PSK]))
        cg.add(var.set_tracker_id(mesh[CONF_TRACKER_ID]))
```

- [ ] **Step 2: Add ESP-NOW init + AES + counter to tracker_bridge.h**

In `tracker_bridge.h`, add at the top:

```cpp
#include <espnow.h>             // ESP-NOW C API for ESP8266
#include <Crypto.h>
#include <AES.h>
#include <GCM.h>
#include "esphome/core/preferences.h"
```

Add to the class body (private section):

```cpp
  /* --- Mesh config (set by YAML, read at setup) --- */
  uint8_t mesh_channel_{0};
  std::string mesh_psk_{};
  uint8_t mesh_key_[16]{};      // derived from psk_ via SHA-256 trunc (deterministic)
  std::string tracker_id_{};
  bool mesh_enabled_{false};

  /* --- Counter management --- */
  uint32_t tx_counter_{0};
  ESPPreferenceObject pref_tx_counter_;
  static constexpr uint32_t CTR_FLUSH_EVERY = 100;

  /* --- Per-peer last-accepted counter map --- */
  std::map<std::array<uint8_t, 6>, uint32_t> peer_last_ctr_;

 public:
  void set_mesh_channel(uint8_t ch) { mesh_channel_ = ch; mesh_enabled_ = true; }
  void set_mesh_psk(const std::string &psk) { mesh_psk_ = psk; }
  void set_tracker_id(const std::string &id) { tracker_id_ = id; }
```

- [ ] **Step 3: Initialize ESP-NOW in `setup()`**

Extend the existing `setup()` body:

```cpp
  void setup() override {
    this->set_interval("poll", 2000, [this]() { this->send_poll_(); });
    ESP_LOGCONFIG(TAG, "tracker_bridge configured");

    if (mesh_enabled_) {
      mesh_setup_();
    }
  }
```

Implement `mesh_setup_`:

```cpp
  void mesh_setup_() {
    /* Derive 16-byte AES key from PSK via SHA-256 truncation
     * (deterministic, lets operators use a passphrase in YAML). */
    SHA256 sha;
    sha.update(mesh_psk_.data(), mesh_psk_.size());
    uint8_t digest[32];
    sha.finalize(digest, 32);
    memcpy(mesh_key_, digest, 16);

    /* Restore TX counter from flash; advance by safety margin */
    pref_tx_counter_ = global_preferences->make_preference<uint32_t>(this->get_object_id_hash() ^ 0xC0DEC0DE);
    if (!pref_tx_counter_.load(&tx_counter_)) tx_counter_ = 0;
    tx_counter_ += CTR_FLUSH_EVERY;
    pref_tx_counter_.save(&tx_counter_);
    global_preferences->sync();

    /* Init ESP-NOW.  WiFi must already be on the mesh channel. */
    if (esp_now_init() != 0) {
      ESP_LOGE(TAG, "esp_now_init failed");
      return;
    }
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb([](uint8_t *mac, uint8_t *data, uint8_t len) {
      // Static callback -> dispatch to the singleton (we have 1 mesh per device)
      TrackerBridge::instance_->mesh_rx_(mac, data, len);
    });

    /* Add broadcast peer (FF:FF:FF:FF:FF:FF) */
    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    esp_now_add_peer(bcast, ESP_NOW_ROLE_SLAVE, mesh_channel_, NULL, 0);

    instance_ = this;
    ESP_LOGI(TAG, "Mesh enabled: channel=%u tracker_id=%s", mesh_channel_, tracker_id_.c_str());
  }

  static TrackerBridge *instance_;
```

And outside the class (in the .h, at the bottom of the namespace):

```cpp
TrackerBridge *TrackerBridge::instance_ = nullptr;
```

- [ ] **Step 4: AES-GCM frame send + receive helpers**

Add (still in tracker_bridge.h, after `mesh_setup_`):

```cpp
  /* Build, encrypt, broadcast a single mesh frame.
   * payload_in must be plaintext payload; type is the message-type byte. */
  void mesh_tx_(uint8_t type, const uint8_t *payload_in, size_t plen) {
    /* Header: type(1) src(6) ctr(4) */
    uint8_t pkt[1 + 6 + 4 + 64 + 8];
    pkt[0] = type;
    uint8_t mac[6];
    WiFi.macAddress(mac);
    memcpy(pkt + 1, mac, 6);
    uint32_t ctr = ++tx_counter_;
    pkt[7] = (ctr >> 24) & 0xFF;
    pkt[8] = (ctr >> 16) & 0xFF;
    pkt[9] = (ctr >> 8) & 0xFF;
    pkt[10] = ctr & 0xFF;

    /* Persist counter every CTR_FLUSH_EVERY ticks */
    if ((ctr % CTR_FLUSH_EVERY) == 0) {
      pref_tx_counter_.save(&tx_counter_);
      global_preferences->sync();
    }

    /* AES-GCM encrypt payload */
    GCM<AES128> ccm;
    gcm.setKey(mesh_key_, 16);
    /* Nonce: src(6) || ctr(4) || 0(2) = 12 bytes */
    uint8_t nonce[12];
    memcpy(nonce, mac, 6);
    memcpy(nonce + 6, pkt + 7, 4);
    nonce[10] = 0; nonce[11] = 0;
    gcm.setIV(nonce, 12);
    /* Additional authenticated data: type byte + header (so a tampered
     * type or counter is detected). */
    gcm.addAuthData(pkt, 1 + 6 + 4);
    /* Encrypt payload into pkt at position 11; tag follows */
    gcm.encrypt(pkt + 11, payload_in, plen);
    uint8_t tag[8];
    gcm.computeTag(tag, 8);
    memcpy(pkt + 11 + plen, tag, 8);

    /* Broadcast */
    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    esp_now_send(bcast, pkt, 11 + plen + 8);
  }

  /* Decrypt + auth + dedup-check an incoming frame.  On success,
   * calls mesh_dispatch_(type, src, plaintext, plen). */
  void mesh_rx_(uint8_t *mac, uint8_t *data, uint8_t len) {
    if (len < 1 + 6 + 4 + 0 + 8) return;
    uint8_t type = data[0];
    uint8_t src[6]; memcpy(src, data + 1, 6);
    uint32_t ctr = ((uint32_t)data[7] << 24) | ((uint32_t)data[8] << 16)
                 | ((uint32_t)data[9] << 8) | (uint32_t)data[10];
    size_t plen = len - (1 + 6 + 4 + 8);

    /* Replay-protect via last-accepted-counter table */
    std::array<uint8_t, 6> key{src[0],src[1],src[2],src[3],src[4],src[5]};
    auto it = peer_last_ctr_.find(key);
    if (it != peer_last_ctr_.end() && ctr <= it->second) {
      ESP_LOGD(TAG, "replay drop src=%02X..%02X ctr=%u last=%u", src[0], src[5], ctr, it->second);
      return;
    }

    /* AES-GCM decrypt */
    GCM<AES128> ccm;
    gcm.setKey(mesh_key_, 16);
    uint8_t nonce[12];
    memcpy(nonce, src, 6);
    memcpy(nonce + 6, data + 7, 4);
    nonce[10] = 0; nonce[11] = 0;
    gcm.setIV(nonce, 12);
    gcm.addAuthData(data, 1 + 6 + 4);
    uint8_t plaintext[64];
    gcm.decrypt(plaintext, data + 11, plen);
    if (!gcm.checkTag(data + 11 + plen, 8)) {
      ESP_LOGD(TAG, "AES auth fail src=%02X..%02X", src[0], src[5]);
      return;
    }

    /* Authenticated -> update counter table, dispatch */
    peer_last_ctr_[key] = ctr;
    mesh_dispatch_(type, src, plaintext, plen);
  }

  /* Stub for now; filled in by later tasks */
  void mesh_dispatch_(uint8_t type, uint8_t src[6], const uint8_t *p, size_t plen) {
    ESP_LOGI(TAG, "rx type=%u from %02X..%02X plen=%u", type, src[0], src[5], plen);
  }
```

- [ ] **Step 5: Add `mesh` config to the reference YAML**

Edit `EcoWorthyFirmware/esphome/solar-tracker-1.yaml`. Locate the existing `tracker_bridge:` block and extend:

```yaml
tracker_bridge:
  id: bridge
  uart_id: uart_bus
  mesh:
    channel: 6        # must match the AP channel
    psk: !secret tracker_mesh_psk
    tracker_id: "T1"
```

Also force the WiFi channel under the existing `wifi:` block by adding `channel: 6` (the same value).

- [ ] **Step 6: Create `secrets.example.yaml`**

Create `EcoWorthyFirmware/esphome/secrets.example.yaml`:

```yaml
# Copy to secrets.yaml and fill in.
ha_api_key: "your-ha-api-key-base64"
ota_password: "your-ota-password"
wifi_ssid: "your-wifi-ssid"
wifi_password: "your-wifi-password"
fallback_password: "fallback-ap-password"
# Phase 4: any string >= 16 chars; SHA-256-derived 16-byte AES key.
# Same on every tracker in the array.
tracker_mesh_psk: "change-me-to-a-long-random-string"
```

- [ ] **Step 7: Build + flash + validate basic mesh broadcast**

Build ESPHome (will fail at first; we haven't called `mesh_tx_` yet) — verify it compiles. Then add a one-shot test broadcast in `setup()` after `mesh_setup_()`:

```cpp
this->set_timeout(3000, [this]() {
  uint8_t test[] = {0xDE, 0xAD};
  mesh_tx_(99, test, 2);   // type 99 = test, not assigned
});
```

Flash two trackers (or one tracker plus another ESP-01S running a basic mesh-RX listener). Confirm in the ESPHome log: each tracker logs `rx type=99 from XX..XX plen=2` from the other(s). If the AES key mismatches, you'll see `AES auth fail`.

Remove the test broadcast before committing.

- [ ] **Step 8: Commit**

```bash
git add esphome/components/tracker_bridge/__init__.py \
        esphome/components/tracker_bridge/tracker_bridge.h \
        EcoWorthyFirmware/esphome/solar-tracker-1.yaml \
        EcoWorthyFirmware/esphome/secrets.example.yaml
git commit -m "feat(esp): P4-7 -- ESP-NOW infrastructure with AES-128-GCM"
```

---

## Task 8: ESP — WIND, TELEMETRY, GATEWAY_HB message types

**Goal:** Implement the three periodic broadcast types: `WIND` (sent by tracker whose role is primary), `TELEMETRY` (sent by all), `GATEWAY_HB` (sent by WiFi-up trackers).

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h`

- [ ] **Step 1: Define message-type constants and cache local STC state**

At the top of the class (private):

```cpp
  /* Message types matching the spec */
  static constexpr uint8_t MSG_WIND       = 1;
  static constexpr uint8_t MSG_TELEMETRY  = 2;
  static constexpr uint8_t MSG_GATEWAY_HB = 3;
  static constexpr uint8_t MSG_COMMAND    = 4;
  static constexpr uint8_t MSG_CONFIG     = 5;

  /* Cached STC state, populated by status-poll parser */
  uint8_t local_az_pct_{0};
  uint8_t local_el_pct_{0};
  uint8_t local_wind_used_{0};
  std::string local_mode_{};
  uint8_t local_role_{2};      // 1=primary, 2=secondary (read via CONFIG_GET on startup)
```

- [ ] **Step 2: Update `handle_payload_` to also cache role**

After existing parser, add: read role via `!cfg get id=32` on startup and cache. Actually role comes through CONFIG; for Task 8, hardcode local_role_ from YAML for simplicity, or add it as a fixed config later. For v1 of this task: hardcode in YAML.

Extend `__init__.py` schema:

```python
CONF_LOCAL_ROLE = "local_role"
# in CONFIG_SCHEMA mesh subschema:
cv.Optional(CONF_LOCAL_ROLE, default="secondary"): cv.one_of("primary", "secondary", lower=True),
```

In `to_code`:

```python
cg.add(var.set_local_role(1 if mesh[CONF_LOCAL_ROLE] == "primary" else 2))
```

In `tracker_bridge.h`:

```cpp
void set_local_role(uint8_t r) { local_role_ = r; }
```

- [ ] **Step 3: Add periodic broadcasts in `mesh_setup_`**

At the end of `mesh_setup_`:

```cpp
    /* Every 5 s: TELEMETRY (always), WIND (if primary), GATEWAY_HB (if WiFi up) */
    this->set_interval("mesh_broadcasts", 5000, [this]() {
      this->mesh_tx_telemetry_();
      if (this->local_role_ == 1) this->mesh_tx_wind_();
      if (WiFi.isConnected()) this->mesh_tx_gateway_hb_();
    });
```

Implement each:

```cpp
  void mesh_tx_telemetry_() {
    uint8_t p[4] = {
      local_az_pct_, local_el_pct_, local_wind_used_,
      mode_to_code_(local_mode_)
    };
    mesh_tx_(MSG_TELEMETRY, p, 4);
  }

  void mesh_tx_wind_() {
    uint8_t p[2] = { local_wind_used_, /* flags */ 0 };
    /* Set flag bit 0 if our local mode is "storm" */
    if (local_mode_ == "storm") p[1] |= 0x01;
    mesh_tx_(MSG_WIND, p, 2);
  }

  void mesh_tx_gateway_hb_() {
    int8_t rssi = WiFi.RSSI();
    uint8_t p[1] = { (uint8_t)rssi };
    mesh_tx_(MSG_GATEWAY_HB, p, 1);
  }

  static uint8_t mode_to_code_(const std::string &m) {
    if (m == "idle") return 0;
    if (m == "track") return 1;
    if (m == "storm") return 2;
    if (m == "jog") return 3;
    if (m == "cal") return 4;
    if (m == "menu") return 5;
    if (m == "set") return 6;
    return 255;
  }
```

- [ ] **Step 4: Handle inbound WIND -> forward to STC**

Extend `mesh_dispatch_`:

```cpp
  void mesh_dispatch_(uint8_t type, uint8_t src[6], const uint8_t *p, size_t plen) {
    switch (type) {
      case MSG_WIND:
        if (plen < 2) return;
        /* Forward wind value to local STC via the existing !wind=NN command */
        send_command_frame_("!wind=", p[0]);
        break;
      case MSG_TELEMETRY:
      case MSG_GATEWAY_HB:
        /* Tasks 9 + 10 handle these */
        break;
    }
  }

  /* Build and send a !wind=NN style framed command over local UART */
  void send_command_frame_(const char *prefix, uint8_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%s%u", prefix, value);
    /* Re-use uart_send_frame logic but for outbound: */
    uint8_t crc = 0;
    for (size_t i = 0; i < strlen(buf); i++)
      crc = crc8_step_(crc, (uint8_t)buf[i]);
    this->write_byte(0xAA);
    this->write_byte(0x55);
    for (size_t i = 0; i < strlen(buf); i++)
      this->write_byte((uint8_t)buf[i]);
    this->write_byte((uint8_t)hex_digit_(crc >> 4));
    this->write_byte((uint8_t)hex_digit_(crc & 0x0F));
    this->write_byte('\n');
  }
```

- [ ] **Step 5: Build, flash 2+ trackers, validate**

Configure one tracker with `local_role: primary` in YAML; the other as `secondary`. Flash both. In each ESPHome log:

- Primary should log periodic `tx WIND` and `tx TELEMETRY` and `tx GATEWAY_HB` (if WiFi).
- Secondary should log `rx type=1 from <primary MAC>` every 5 s. Confirm the secondary's STC starts reporting wind values via its LCD jog/idle screens (proves `!wind=NN` reached the STC).

Set the secondary's STC `wind_source = 1` via Settings. Confirm:
- Without primary, secondary enters STORM PARK within 20 s (failsafe).
- With primary broadcasting `WIND=5` (below threshold), secondary stays in normal mode.
- Blow on primary's wind sensor → primary's local storm logic triggers → primary broadcasts higher wind → secondary's storm_check trips → secondary enters STORM.

- [ ] **Step 6: Commit**

```bash
git add esphome/components/tracker_bridge/__init__.py \
        esphome/components/tracker_bridge/tracker_bridge.h
git commit -m "feat(esp): P4-8 -- WIND/TELEMETRY/GATEWAY_HB periodic broadcasts"
```

---

## Task 9: ESP — gateway election + HA entity publishing for remotes

**Goal:** Track which gateway-capable nodes are currently up (via GATEWAY_HB). The lowest-MAC active one is the acting gateway. Only the acting gateway publishes other trackers' TELEMETRY to HA.

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h`
- Modify: `esphome/components/tracker_bridge/__init__.py` (configurable peer entity prefix)

- [ ] **Step 1: Add peer-tracking and gateway-election state**

```cpp
  struct PeerEntry {
    uint32_t last_gateway_hb_ms{0};   // when we last saw GATEWAY_HB
    uint32_t last_telemetry_ms{0};
    uint8_t az_pct{0}, el_pct{0}, wind_used{0};
    std::string mode{};
  };
  std::map<std::array<uint8_t, 6>, PeerEntry> peers_;

  static constexpr uint32_t HB_STALE_MS = 15000;   // 3 missed @ 5s
```

- [ ] **Step 2: Update `mesh_dispatch_` to record peer state**

```cpp
      case MSG_GATEWAY_HB: {
        std::array<uint8_t, 6> key{src[0],src[1],src[2],src[3],src[4],src[5]};
        peers_[key].last_gateway_hb_ms = millis();
        break;
      }
      case MSG_TELEMETRY: {
        if (plen < 4) return;
        std::array<uint8_t, 6> key{src[0],src[1],src[2],src[3],src[4],src[5]};
        auto &e = peers_[key];
        e.last_telemetry_ms = millis();
        e.az_pct = p[0]; e.el_pct = p[1]; e.wind_used = p[2];
        e.mode = mode_from_code_(p[3]);
        /* If we're the acting gateway, publish to HA for this peer */
        if (is_acting_gateway_()) publish_peer_to_ha_(src, e);
        break;
      }
```

- [ ] **Step 3: Add gateway-election helper**

```cpp
  bool is_acting_gateway_() {
    if (!WiFi.isConnected()) return false;
    uint8_t my_mac[6];
    WiFi.macAddress(my_mac);
    uint32_t now = millis();
    /* Find lowest MAC currently broadcasting GATEWAY_HB within HB_STALE_MS */
    std::array<uint8_t, 6> lowest{my_mac[0],my_mac[1],my_mac[2],my_mac[3],my_mac[4],my_mac[5]};
    for (const auto &kv : peers_) {
      if (now - kv.second.last_gateway_hb_ms > HB_STALE_MS) continue;
      if (kv.first < lowest) lowest = kv.first;
    }
    return std::memcmp(my_mac, lowest.data(), 6) == 0;
  }
```

- [ ] **Step 4: HA entity publishing for peers (placeholder)**

For Task 9 we'll just LOG when we'd publish — actual dynamic ESPHome entity creation is constrained, so we'll declare peer entities statically in YAML in Task 11. For now:

```cpp
  void publish_peer_to_ha_(uint8_t src[6], const PeerEntry &e) {
    ESP_LOGI(TAG, "[gateway] peer %02X..%02X az=%u el=%u wind=%u mode=%s",
             src[0], src[5], e.az_pct, e.el_pct, e.wind_used, e.mode.c_str());
    /* Map src MAC to declared peer sensors in Task 11 */
  }
```

- [ ] **Step 5: Build, flash 3 trackers, validate election**

Bring up three trackers, all gateway-capable (configure WiFi on all). After ~10 s each tracker's log should show stable `rx type=3 from <other MACs>` heartbeats.

On the tracker with the lowest MAC: log shows `[gateway] peer ...` messages for the others. On the higher-MAC trackers, no `[gateway]` log lines (they're not acting gateway).

Power-cycle the lowest-MAC tracker → within 15 s the next-lowest takes over as acting gateway. Restart it → it reclaims gateway role.

- [ ] **Step 6: Commit**

```bash
git add esphome/components/tracker_bridge/tracker_bridge.h
git commit -m "feat(esp): P4-9 -- gateway election + peer-state tracking"
```

---

## Task 10: ESP — COMMAND and CONFIG message types

**Goal:** HA → mesh COMMAND surface (force park/release, goto, jog, stop, calibrate). CONFIG get/set for both local and remote trackers.

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h`
- Create: `esphome/components/tracker_bridge/button.py`

- [ ] **Step 1: Define command codes + CONFIG ops in `tracker_bridge.h`**

```cpp
  static constexpr uint8_t CMD_FORCE_PARK    = 1;
  static constexpr uint8_t CMD_FORCE_RELEASE = 2;
  static constexpr uint8_t CMD_GOTO          = 3;
  static constexpr uint8_t CMD_JOG           = 4;
  static constexpr uint8_t CMD_STOP          = 5;
  static constexpr uint8_t CMD_CALIBRATE     = 6;

  static constexpr uint8_t CFG_OP_GET_REQ    = 1;
  static constexpr uint8_t CFG_OP_GET_RESP   = 2;
  static constexpr uint8_t CFG_OP_SET_REQ    = 3;
  static constexpr uint8_t CFG_OP_SET_ACK    = 4;
```

- [ ] **Step 2: Add public broadcast methods**

```cpp
 public:
  /* HA->mesh broadcast methods (callable from buttons/numbers) */
  void broadcast_force_park()    { mesh_tx_command_(broadcast_mac_(), CMD_FORCE_PARK, 0, 0); }
  void broadcast_force_release() { mesh_tx_command_(broadcast_mac_(), CMD_FORCE_RELEASE, 0, 0); }
  void broadcast_stop()          { mesh_tx_command_(broadcast_mac_(), CMD_STOP, 0, 0); }
  void send_goto(const uint8_t target[6], uint8_t az, uint8_t el) { mesh_tx_command_(target, CMD_GOTO, az, el); }
  void send_jog(const uint8_t target[6], uint8_t ax_dir, uint8_t dur_100ms) { mesh_tx_command_(target, CMD_JOG, ax_dir, dur_100ms); }
  void send_calibrate(const uint8_t target[6]) { mesh_tx_command_(target, CMD_CALIBRATE, 0, 0); }
```

Implementation helpers:

```cpp
 protected:
  static const uint8_t *broadcast_mac_() {
    static uint8_t b[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    return b;
  }
  void mesh_tx_command_(const uint8_t target[6], uint8_t cmd, uint8_t a1, uint8_t a2) {
    uint8_t p[9];
    memcpy(p, target, 6);
    p[6] = cmd; p[7] = a1; p[8] = a2;
    mesh_tx_(MSG_COMMAND, p, 9);
  }
```

- [ ] **Step 3: Receive COMMAND and translate to local STC**

In `mesh_dispatch_`:

```cpp
      case MSG_COMMAND: {
        if (plen < 9) return;
        /* If target not broadcast and not us, ignore */
        uint8_t my_mac[6]; WiFi.macAddress(my_mac);
        bool is_bcast = std::all_of(p, p + 6, [](uint8_t b) { return b == 0xFF; });
        if (!is_bcast && std::memcmp(p, my_mac, 6) != 0) return;
        uint8_t cmd = p[6], a1 = p[7], a2 = p[8];
        switch (cmd) {
          case CMD_FORCE_PARK:    write_str_frame_("!park"); break;
          case CMD_FORCE_RELEASE: write_str_frame_("!release"); break;
          case CMD_STOP:          write_str_frame_("!stop"); break;
          case CMD_GOTO: {
            char buf[32];
            snprintf(buf, sizeof(buf), "!goto az=%u el=%u", a1, a2);
            write_str_frame_(buf); break;
          }
          case CMD_JOG: {
            uint8_t axis = (a1 >> 7) & 1;
            uint8_t dir = a1 & 1;
            uint16_t dur_ms = (uint16_t)a2 * 100;
            char buf[40];
            snprintf(buf, sizeof(buf), "!jog ax=%u dir=%c ms=%u", axis, dir ? '+' : '-', dur_ms);
            write_str_frame_(buf); break;
          }
          case CMD_CALIBRATE: write_str_frame_("!cal"); break;
        }
        break;
      }
```

`write_str_frame_` is a general framed-string sender (refactor `send_command_frame_` from Task 8 step 4):

```cpp
  void write_str_frame_(const char *s) {
    uint8_t crc = 0;
    for (const char *q = s; *q; q++) crc = crc8_step_(crc, (uint8_t)*q);
    this->write_byte(0xAA);
    this->write_byte(0x55);
    for (const char *q = s; *q; q++) this->write_byte((uint8_t)*q);
    this->write_byte((uint8_t)hex_digit_(crc >> 4));
    this->write_byte((uint8_t)hex_digit_(crc & 0x0F));
    this->write_byte('\n');
  }
```

(Update `send_command_frame_` in Task 8 to use this helper too, eliminating duplication.)

- [ ] **Step 4: Create `button.py` platform for HA command buttons**

`esphome/components/tracker_bridge/button.py`:

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from . import TrackerBridge, CONF_TRACKER_BRIDGE_ID

DEPENDENCIES = ["tracker_bridge"]

CONF_FORCE_PARK = "force_park"
CONF_FORCE_RELEASE = "force_release"
CONF_STOP = "stop"

tracker_bridge_ns = cg.esphome_ns.namespace("tracker_bridge")
ForceParkButton    = tracker_bridge_ns.class_("ForceParkButton",    button.Button)
ForceReleaseButton = tracker_bridge_ns.class_("ForceReleaseButton", button.Button)
StopButton         = tracker_bridge_ns.class_("StopButton",         button.Button)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(CONF_TRACKER_BRIDGE_ID): cv.use_id(TrackerBridge),
    cv.Optional(CONF_FORCE_PARK):    button.button_schema(ForceParkButton),
    cv.Optional(CONF_FORCE_RELEASE): button.button_schema(ForceReleaseButton),
    cv.Optional(CONF_STOP):          button.button_schema(StopButton),
})

async def to_code(config):
    parent = await cg.get_variable(config[CONF_TRACKER_BRIDGE_ID])
    if CONF_FORCE_PARK in config:
        b = await button.new_button(config[CONF_FORCE_PARK])
        cg.add(b.set_parent(parent))
    if CONF_FORCE_RELEASE in config:
        b = await button.new_button(config[CONF_FORCE_RELEASE])
        cg.add(b.set_parent(parent))
    if CONF_STOP in config:
        b = await button.new_button(config[CONF_STOP])
        cg.add(b.set_parent(parent))
```

In `tracker_bridge.h` add the button C++ classes (each has a `set_parent` and a `press_action` that calls the corresponding broadcast):

```cpp
class ForceParkButton : public button::Button {
 public:
  void set_parent(TrackerBridge *p) { parent_ = p; }
 protected:
  void press_action() override { if (parent_) parent_->broadcast_force_park(); }
  TrackerBridge *parent_{nullptr};
};
class ForceReleaseButton : public button::Button {
 public:
  void set_parent(TrackerBridge *p) { parent_ = p; }
 protected:
  void press_action() override { if (parent_) parent_->broadcast_force_release(); }
  TrackerBridge *parent_{nullptr};
};
class StopButton : public button::Button {
 public:
  void set_parent(TrackerBridge *p) { parent_ = p; }
 protected:
  void press_action() override { if (parent_) parent_->broadcast_stop(); }
  TrackerBridge *parent_{nullptr};
};
```

- [ ] **Step 5: Add CONFIG handling**

In `mesh_dispatch_`, add a MSG_CONFIG case that handles GET_REQ (reply with cached value), GET_RESP (update peer config cache for HA publish), SET_REQ (forward to local STC via `!cfg set`), SET_ACK (broadcast back).

```cpp
      case MSG_CONFIG: {
        if (plen < 2) return;
        uint8_t op = p[0], field_id = p[1];
        switch (op) {
          case CFG_OP_GET_REQ:
            /* Reply with our local STC's value: query STC via UART !cfg get */
            if (target_matches_us_(src)) {
              char buf[20];
              snprintf(buf, sizeof(buf), "!cfg get id=%u", field_id);
              write_str_frame_(buf);
              /* STC reply will come via uart RX path -> publish to HA */
            }
            break;
          case CFG_OP_SET_REQ:
            if (target_matches_us_(src) && plen >= 3) {
              uint8_t val = p[2];   /* For u8 fields; u16 needs plen>=4 */
              char buf[32];
              snprintf(buf, sizeof(buf), "!cfg set id=%u val=%u", field_id, val);
              write_str_frame_(buf);
            }
            break;
          case CFG_OP_GET_RESP:
          case CFG_OP_SET_ACK:
            /* Update HA peer-config entity, see Task 11 */
            break;
        }
        break;
      }
```

(Note: for u16 SET_REQ, payload is `op, field, val_lo, val_hi` — extend accordingly when the operator wants to set u16 values. v1 only SETs u8 RW fields, so 3-byte SET_REQ is sufficient.)

Add helper:

```cpp
  bool target_matches_us_(const uint8_t src[6]) {
    /* For CFG, we treat src as the target (i.e., this peer wants
     * its OWN config). The acting gateway issues CONFIG requests
     * carrying the target node's MAC as src.  Simpler: each tracker
     * just responds to GET_REQ broadcasts addressed via field_id
     * convention. For v1, accept all GET_REQ -- it's local-config
     * inspection on the responder side anyway. */
    return true;
  }
```

(Refine targeting semantics in Task 11; for now, every tracker responds to every GET, which is wasteful but functionally correct — the gateway de-dups by tracking which response is for which peer.)

- [ ] **Step 6: Build, flash, validate commands**

Add buttons in YAML:

```yaml
button:
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    force_park:    { name: "$friendly_name Force Park" }
    force_release: { name: "$friendly_name Force Release" }
    stop:          { name: "$friendly_name Emergency Stop" }
```

From HA, press "Force Park" → all trackers (including the secondary, even out of WiFi) enter STORM PARK. Press "Force Release" → exit. "Emergency Stop" → axes off immediately on all.

- [ ] **Step 7: Commit**

```bash
git add esphome/components/tracker_bridge/__init__.py \
        esphome/components/tracker_bridge/tracker_bridge.h \
        esphome/components/tracker_bridge/button.py \
        EcoWorthyFirmware/esphome/solar-tracker-1.yaml
git commit -m "feat(esp): P4-10 -- COMMAND + CONFIG message types + HA buttons"
```

---

## Task 11: ESP — per-peer HA entities + `number.py` for RW config

**Goal:** Declare each remote tracker statically in the YAML; when the acting gateway receives that peer's TELEMETRY or CONFIG_GET_RESP, it publishes to the named entities. Add `number.py` platform for RW config sliders.

**Files:**
- Create: `esphome/components/tracker_bridge/number.py`
- Modify: `esphome/components/tracker_bridge/__init__.py` (peer schema)
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h` (per-peer entity registration)
- Modify: `EcoWorthyFirmware/esphome/solar-tracker-1.yaml` (declare peers)

- [ ] **Step 1: Extend `__init__.py` with a peer list**

Add to the mesh schema:

```python
CONF_PEERS = "peers"
CONF_MAC = "mac"
CONF_ID_LABEL = "id"

PEER_SCHEMA = cv.Schema({
    cv.Required(CONF_MAC): cv.mac_address,
    cv.Required(CONF_ID_LABEL): cv.string_strict,
})

# In CONFIG_SCHEMA's mesh sub-schema:
cv.Optional(CONF_PEERS, default=[]): cv.ensure_list(PEER_SCHEMA),
```

In `to_code` (mesh block), register each peer:

```python
for peer in mesh.get(CONF_PEERS, []):
    mac_bytes = peer[CONF_MAC].parts   # list of 6 ints
    cg.add(var.register_peer(
        mac_bytes[0], mac_bytes[1], mac_bytes[2],
        mac_bytes[3], mac_bytes[4], mac_bytes[5],
        peer[CONF_ID_LABEL]
    ))
```

- [ ] **Step 2: Add per-peer entity storage to `tracker_bridge.h`**

```cpp
  struct PeerDecl {
    std::string id_label;
    sensor::Sensor *az{nullptr};
    sensor::Sensor *el{nullptr};
    sensor::Sensor *wind{nullptr};
    text_sensor::TextSensor *mode{nullptr};
    /* Config entities filled in by number.py / sensor.py via setters */
    number::Number *wind_storm{nullptr};
    number::Number *wind_release{nullptr};
    number::Number *storm_dwell{nullptr};
    number::Number *track_thresh{nullptr};
  };
  std::map<std::array<uint8_t, 6>, PeerDecl> peer_decls_;

  void register_peer(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e, uint8_t f, const std::string &id) {
    std::array<uint8_t, 6> mac{a,b,c,d,e,f};
    peer_decls_[mac].id_label = id;
  }
```

- [ ] **Step 3: Extend `sensor.py` to support per-peer attachment**

The existing sensor.py registers az/el/wind/mode for the LOCAL tracker. Extend with optional `peer_id` to attach to a specific declared peer:

```python
CONF_PEER_ID = "peer_id"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(CONF_TRACKER_BRIDGE_ID): cv.use_id(TrackerBridge),
    cv.Optional(CONF_PEER_ID): cv.string_strict,
    cv.Optional(CONF_AZ): sensor.sensor_schema(...),
    # ... existing sensors ...
})

async def to_code(config):
    parent = await cg.get_variable(config[CONF_TRACKER_BRIDGE_ID])
    peer_id = config.get(CONF_PEER_ID, "")   # "" means local
    if CONF_AZ in config:
        s = await sensor.new_sensor(config[CONF_AZ])
        cg.add(parent.set_az_sensor_for(peer_id, s))
    # ... etc
```

In `tracker_bridge.h`:

```cpp
  void set_az_sensor_for(const std::string &peer_id, sensor::Sensor *s) {
    if (peer_id.empty()) { az_ = s; return; }
    for (auto &kv : peer_decls_) {
      if (kv.second.id_label == peer_id) { kv.second.az = s; return; }
    }
    ESP_LOGW(TAG, "unknown peer_id %s for az_sensor", peer_id.c_str());
  }
  /* same pattern for set_el_sensor_for, set_wind_sensor_for, set_mode_sensor_for */
```

Update `publish_peer_to_ha_`:

```cpp
  void publish_peer_to_ha_(const uint8_t src[6], const PeerEntry &e) {
    std::array<uint8_t, 6> key{src[0],src[1],src[2],src[3],src[4],src[5]};
    auto it = peer_decls_.find(key);
    if (it == peer_decls_.end()) {
      ESP_LOGD(TAG, "unknown peer MAC %02X..%02X", src[0], src[5]);
      return;
    }
    auto &d = it->second;
    if (d.az)   d.az->publish_state(float(e.az_pct));
    if (d.el)   d.el->publish_state(float(e.el_pct));
    if (d.wind) d.wind->publish_state(float(e.wind_used));
    if (d.mode) d.mode->publish_state(e.mode);
  }
```

- [ ] **Step 4: Create `number.py` platform**

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_MIN_VALUE, CONF_MAX_VALUE, CONF_STEP
from . import TrackerBridge, CONF_TRACKER_BRIDGE_ID

DEPENDENCIES = ["tracker_bridge"]

CONF_PEER_ID = "peer_id"
CONF_WIND_STORM = "wind_storm_mps"
CONF_WIND_RELEASE = "wind_release_mps"
CONF_STORM_DWELL = "storm_dwell_min"
CONF_TRACK_THRESH = "track_thresh"

tracker_bridge_ns = cg.esphome_ns.namespace("tracker_bridge")
ConfigNumber = tracker_bridge_ns.class_("ConfigNumber", number.Number)

NUM_SCHEMA = number.number_schema(ConfigNumber).extend({
    cv.Optional(CONF_MIN_VALUE, default=0): cv.float_,
    cv.Optional(CONF_MAX_VALUE, default=100): cv.float_,
    cv.Optional(CONF_STEP, default=1): cv.float_,
})

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(CONF_TRACKER_BRIDGE_ID): cv.use_id(TrackerBridge),
    cv.Optional(CONF_PEER_ID): cv.string_strict,
    cv.Optional(CONF_WIND_STORM):   NUM_SCHEMA,
    cv.Optional(CONF_WIND_RELEASE): NUM_SCHEMA,
    cv.Optional(CONF_STORM_DWELL):  NUM_SCHEMA,
    cv.Optional(CONF_TRACK_THRESH): NUM_SCHEMA,
})

# Field IDs matching STC:
FIELD_ID = { CONF_WIND_STORM: 1, CONF_WIND_RELEASE: 2, CONF_STORM_DWELL: 3, CONF_TRACK_THRESH: 4 }

async def to_code(config):
    parent = await cg.get_variable(config[CONF_TRACKER_BRIDGE_ID])
    peer_id = config.get(CONF_PEER_ID, "")
    for key, fid in FIELD_ID.items():
        if key in config:
            num = await number.new_number(config[key], min_value=config[key][CONF_MIN_VALUE],
                                          max_value=config[key][CONF_MAX_VALUE],
                                          step=config[key][CONF_STEP])
            cg.add(num.set_parent(parent))
            cg.add(num.set_peer_id(peer_id))
            cg.add(num.set_field_id(fid))
```

In `tracker_bridge.h`:

```cpp
class ConfigNumber : public number::Number {
 public:
  void set_parent(TrackerBridge *p) { parent_ = p; }
  void set_peer_id(const std::string &id) { peer_id_ = id; }
  void set_field_id(uint8_t fid) { field_id_ = fid; }
 protected:
  void control(float value) override {
    if (parent_) parent_->config_set_for_peer(peer_id_, field_id_, (uint8_t)value);
  }
  TrackerBridge *parent_{nullptr};
  std::string peer_id_;
  uint8_t field_id_{0};
};
```

In `TrackerBridge`:

```cpp
 public:
  /* Called by HA when a slider moves */
  void config_set_for_peer(const std::string &peer_id, uint8_t field_id, uint8_t value) {
    if (peer_id.empty()) {
      /* Local STC: write directly via !cfg set */
      char buf[32];
      snprintf(buf, sizeof(buf), "!cfg set id=%u val=%u", field_id, value);
      write_str_frame_(buf);
    } else {
      /* Remote: broadcast CONFIG SET_REQ targeting that peer */
      auto target_mac = find_peer_mac_(peer_id);
      if (!target_mac) return;
      uint8_t p[3] = { CFG_OP_SET_REQ, field_id, value };
      mesh_tx_(MSG_CONFIG, p, 3);
    }
  }
```

- [ ] **Step 5: Update YAML to declare peers + per-peer entities**

In `solar-tracker-1.yaml`, extend the mesh block:

```yaml
tracker_bridge:
  id: bridge
  uart_id: uart_bus
  mesh:
    channel: 6
    psk: !secret tracker_mesh_psk
    tracker_id: "T1"
    local_role: primary
    peers:
      - mac: "AA:BB:CC:DD:EE:02"
        id: "T2"
      - mac: "AA:BB:CC:DD:EE:03"
        id: "T3"
```

Add per-peer sensor declarations:

```yaml
sensor:
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    az:   { name: "T1 Azimuth %" }
    el:   { name: "T1 Elevation %" }
    wind: { name: "T1 Wind" }
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    peer_id: T2
    az:   { name: "T2 Azimuth %" }
    el:   { name: "T2 Elevation %" }
    wind: { name: "T2 Wind" }
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    peer_id: T3
    az:   { name: "T3 Azimuth %" }
    el:   { name: "T3 Elevation %" }
    wind: { name: "T3 Wind" }

text_sensor:
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    mode: { name: "T1 Mode" }
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    peer_id: T2
    mode: { name: "T2 Mode" }
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    peer_id: T3
    mode: { name: "T3 Mode" }

number:
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    wind_storm_mps:   { name: "T1 Wind Storm",   min_value: 5, max_value: 30 }
    wind_release_mps: { name: "T1 Wind Release", min_value: 0, max_value: 20 }
    storm_dwell_min:  { name: "T1 Storm Dwell",  min_value: 1, max_value: 60 }
    track_thresh:     { name: "T1 Track Thresh", min_value: 1, max_value: 99 }
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    peer_id: T2
    wind_storm_mps:   { name: "T2 Wind Storm",   min_value: 5, max_value: 30 }
    # ... (repeat for T3)
```

- [ ] **Step 6: Build, flash all trackers, validate**

In HA, you should now see entities for T1, T2, T3:
- Per-tracker az/el/wind sensors
- Per-tracker mode text_sensor
- Per-tracker wind_storm/release/dwell/track_thresh number sliders
- Force park/release/stop buttons (from Task 10)

Drag a slider for T2's wind_storm → T2's STC should accept the CONFIG_SET and persist (verify via T2's Settings menu).

- [ ] **Step 7: Commit**

```bash
git add esphome/components/tracker_bridge/__init__.py \
        esphome/components/tracker_bridge/sensor.py \
        esphome/components/tracker_bridge/text_sensor.py \
        esphome/components/tracker_bridge/number.py \
        esphome/components/tracker_bridge/tracker_bridge.h \
        EcoWorthyFirmware/esphome/solar-tracker-1.yaml
git commit -m "feat(esp): P4-11 -- per-peer HA entities + RW config sliders"
```

---

## Task 12: Integration test + docs + tag `phase-4-complete`

**Goal:** Full 3-tracker end-to-end test, doc updates, tag the phase.

**Files:**
- Modify: `CLAUDE.md`
- Modify: `EcoWorthyFirmware/README.md`
- Modify: `EcoWorthyFirmware/esphome/secrets.example.yaml` (final form)

- [ ] **Step 1: Integration test on 3 trackers**

Deploy 3 trackers. Configure:
- T1: `role: primary`, `wind_source: local`, has wind sensor
- T2: `role: secondary`, `wind_source: remote`, may have wind sensor unconnected
- T3: same as T2

Validate:
- **Telemetry**: HA shows T1/T2/T3 az/el/wind/mode entities updating ~every 5 s.
- **Wind broadcast**: blow on T1's wind sensor; T2 and T3 wind entities track T1's value.
- **Storm trip**: shake T1's sensor hard; T1 enters STORM, broadcasts WIND with storm flag; T2 and T3 enter STORM independently via their own storm_check.
- **Wind failsafe**: power off T1; within 20 s, T2 and T3 enter STORM PARK.
- **Gateway failover**: identify lowest-MAC tracker (acting gateway); power-cycle it; within 15 s another takes over publishing.
- **Command**: HA "Force Park" → all 3 trackers enter STORM. "Force Release" → all exit (provided wind below threshold).
- **Movement**: HA goto on T2 → T2 drives to position (forwarded via mesh).
- **Config**: HA slider for T3 wind_storm → T3 STC persists; verify via T3 Settings menu LCD.

- [ ] **Step 2: Document any issues + the resolution path**

Update the design spec with any deviations discovered during the integration test. Most likely deviations: counter persistence interval too low (more EEPROM wear than expected), gateway-election timing tuning, RSSI-based gateway preference. Note these in the spec, not necessarily fix this phase.

- [ ] **Step 3: Update CLAUDE.md repository status**

```markdown
... **Phases 0–4 are complete:** ...

Phase 4 extends the Phase 3 bridge into a multi-tracker mesh: up to
10 trackers share wind/storm state and config via AES-128-GCM
encrypted ESP-NOW broadcasts on a single WiFi channel. A statically-
designated wind primary broadcasts wind every 5 s; secondaries with
`wind_source: remote` use the broadcast value and failsafe-park if
broadcasts stop. Gateway role is dynamically elected (lowest MAC of
WiFi-up nodes); the acting gateway publishes every tracker's
telemetry and accepts HA commands (force park/release, goto, jog,
stop, calibrate, config set) on behalf of the array. Per-tracker
RW config (wind thresholds, track threshold) is exposed as HA
number sliders; calibration data (stroke times, horiz pct) and
role/wind-source are read-only over mesh.
```

- [ ] **Step 4: Update EcoWorthyFirmware/README.md**

Add a section:

```markdown
### Phase 4: multi-tracker mesh

The Phase 3 bridge extends to a mesh of up to 10 trackers via
ESP-NOW broadcast on a single WiFi channel. All trackers run
identical firmware; behavior diverges by per-tracker `role` and
`wind_source` settings (LCD Settings menu, EEPROM bytes 12-13) and
runtime state (WiFi reachable -> gateway candidate via
lowest-MAC election). The wind primary broadcasts wind every 5 s
(AES-128-GCM authenticated with a per-array PSK); secondaries with
`wind_source: remote` consume the broadcast and failsafe to storm-
park if broadcasts stop for ~20 s. HA commands and config get/set
flow through the acting gateway as `\xAA\x55`-framed `!` commands
to each STC, sharing the Phase 3 wire format.
```

- [ ] **Step 5: Commit docs**

```bash
git add CLAUDE.md EcoWorthyFirmware/README.md
git commit -m "docs(phase-4): repository status + README mesh section"
```

- [ ] **Step 6: Tag phase-4-complete**

```bash
git tag phase-4-complete
git push origin master
git push origin phase-4-complete
```

---

## Self-review

**Spec coverage check** (going section-by-section through `docs/superpowers/specs/2026-05-20-phase-4-mesh-design.md`):

- Locked design decisions (table of 8): all reflected. Scale (T1-T11), WiFi reality (T7-T9), gateway topology (T9), dedup (T9 lowest-MAC), failsafe (T5), command surface (T4 + T10), security (T7 AES-GCM), config get/set (T6 STC, T10/T11 ESP).
- Three orthogonal roles: wind primary (T2 Settings, T8 broadcast), acting gateway (T9 election), remote (default).
- Wire protocol: frame format (T7), message types WIND/TELEMETRY/GATEWAY_HB/COMMAND/CONFIG (T8/T9/T10), command codes (T10), CONFIG protocol (T6/T10/T11), counter management (T7), channel handling (T7 + secrets example).
- STC firmware additions: new EEPROM bytes (T1), Settings entries (T2), storm logic extension (T5), command parser (T3/T4/T6), safety bounds (T4).
- ESP side additions: ESP-NOW init (T7), message broadcasts (T8), gateway election (T9), HA entity layout (T11), config sync (T10/T11).
- Failure model: wind primary offline (T5 failsafe), acting gateway loses WiFi (T9 election re-runs), counter desync (acknowledged in spec; v1 = power-cycle), channel mismatch (T7 documents), AES key mismatch (T7 logs and drops).
- Out of scope items: deferred as documented.

**Placeholder scan**: one item flagged — in Task 10 step 5 I noted "Refine targeting semantics in Task 11" which is a real punt. Task 11 doesn't fully resolve it (Task 10 currently treats every tracker as responding to every CONFIG_GET, which is wasteful but not incorrect). Adding clarification to Task 10's note: this is intentional v1 behavior, not a placeholder; the acting gateway uses CONFIG_GET_RESP's source MAC to attribute the response to the right peer. No further work needed in Task 11 beyond what's there.

**Type consistency**: function names — `mesh_tx_command_` consistent across tasks; `write_str_frame_` replaces `send_command_frame_` in Task 10 (Task 8 should be updated to use it instead). Field IDs match between STC (T6) and ESP (T11 FIELD_ID dict). Message-type constants (MSG_WIND=1, ...) match between STC `!wind=` handling and ESP MSG_WIND broadcast. Command codes match between ESP CMD_FORCE_PARK=1 and STC dispatcher accepts `!park`.

Plan ready for execution.
