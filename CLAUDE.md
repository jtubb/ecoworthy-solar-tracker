# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository status

Custom-firmware project for an off-the-shelf "Ecoworthy" dual-axis solar tracker board, plus an ESPHome bridge to Home Assistant via an ESP-01S. **Phases 0–3 are complete:** the STC15F2K60S2 runs a full custom tracker firmware — boot auto-zero, stall-to-stall calibration (max of extend/retract stroke, with extend stall-delay correction), relay-on-time position tracking, a menu UI (Track / Jog / Calibrate / Backlash / Settings / Version) with debounced resistor-ladder buttons and press-and-hold auto-repeat, EEPROM-persisted settings, differential sun-sensor pulse-tracking with per-axis duty-cycle limiting, a wind storm interlock (park-to-horizontal + resettable dwell), and a duty-cycle interlock with a bottom-right padlock indicator (forced cooldown after calibration). Phase 3 adds the **half-duplex single-wire bridge** to an ESP-01S running ESPHome (`tracker_bridge` external_component at `esphome/components/`): the ESP polls every 2 s, the STC replies with a `\xAA\x55`-framed CRC-8/SMBUS status packet, and az/el/wind/mode are published to Home Assistant. Read-only in v1. Firmware lives in `EcoWorthyFirmware/`; the ESP-side component lives at `esphome/components/tracker_bridge/` (canonical ESPHome lookup path) with a reference YAML at `EcoWorthyFirmware/esphome/solar-tracker-1.yaml`. **Phase 4** (multi-tracker ESP-NOW mesh — designated primary broadcasts wind/storm to secondaries that omit their own wind sensors) is sketched in the Phase 3 plan doc, not yet built.

**Build note:** the project now requires `--stack-auto` (set in the Makefile). The state-machine + helper call graph exceeds SDCC's default 128-byte IRAM overlay budget; the reentrant model puts locals on the internal stack instead. Don't remove this flag without re-checking the DSEG overlay error.

## Layout

- `EcoWorthyFirmware/` — STC15F2K60S2 firmware (SDCC + GNU Make + stcgal).
- `SolarTracker/Ecoworthy Board Description.txt` — reverse-engineered pinout. Authoritative for *function* wiring (which pin connects to what on the board). Port assignments are now resolved against the datasheet — see "Verified pin-to-port map" below.
- `docs/superpowers/` — phase specs and implementation plans.
- `*.pdf` at repo root — vendor manuals for the surrounding solar install (Growatt SPH inverters, SG48100 battery). Reference material; not related to the tracker firmware.

## Target hardware

- **MCU:** STC 15F2K60S2 (SOP28), 8051-derivative. Toolchain: SDCC. Flasher: stcgal or STC-ISP via pins 15/16 (P3.0/P3.1) during reset.
- **5V regulator:** LM2596S buck, 3 A capable — plenty of headroom for the ESP bridge.
- **Relays:** Hongfa **HF165FD-5** (5 V coil, ~55 Ω, ~91 mA). ULN2003A's COM pin is on the 5 V rail. The 12 V the original notes mention is on relay *contacts*, switching the actuators — not on the coil side.
- **Outputs:** 1602A parallel LCD, piezo buzzer, 4 relays via ULN2003A driving **two** 12 V linear actuators (one per axis, N/S and E/W). Relays are configured as a 2-relay H-bridge per actuator — one relay extends, the other retracts.
- **Inputs:** 6 buttons (analog-multiplexed, see quirks), IR receiver, wind-speed pulse, 4 sun-intensity sensors, limit switches (see quirks).

## Non-obvious hardware quirks

1. **Buttons are read as a single analog input on MCU pin 9 (P1.6 / ADC6)** — not 6 GPIOs. The bus is pulled to GND through ~27 kΩ; each button switches the bus to Vcc through its own series resistor. Pressing produces a voltage divider; the resulting ADC value identifies the button.

   Verified series resistors (4-digit SMD codes from the board):

   | Button | Code | Series resistor | Empirical ADC reading |
   |---|---|---|---|
   | (idle) | — | — | 0 |
   | B1 SET | 1203 | 120 kΩ | 188 |
   | B2 QUIT | 5102 | 51 kΩ | 356 |
   | B3 WEST | 2702 | 27 kΩ | 517 |
   | B4 EAST | 1502 | 15 kΩ | 662 |
   | B5 NORTH | 6201 | 6.2 kΩ | 835 |
   | B6 SOUTH | — | 0 Ω | 1023 |

   Gap between adjacent button readings is ~145–188 ADC counts — well above noise floor. Classify with midpoint thresholds; debounce *after* classification. The vendor's 120 kΩ and 6.2 kΩ values came from precision 4-digit SMD codes (3 significant digits + 1 exponent), which earlier reverse-engineering misread as 12 kΩ / 62 kΩ — fixed in Phase 1.
2. **Limit switches are not on dedicated MCU pins — they're a hardware safety net that bypasses firmware entirely.** The unused endstop contacts on the PCB feed ULN2003A inputs via diodes. If physical switches are wired to them, closing a switch pulls the corresponding relay-driver input LOW *regardless of what the MCU asserts*, mechanically cutting motor power at the limit. Vendor kit ships without populated switches because the supplied actuators have internal limit switches that perform the same job (open the motor circuit at endstop). For extra safety on a real install, wiring external switches to the unused PCB pads is essentially free — the firmware doesn't need to know about them. The pin 8 analog network (see quirk 6) is a **secondary** signal that detects motor activity / soft stall, not the primary endstop mechanism.
3. **Verified pin-to-port map (STC15F2K60S2 SOP28).** Authoritative — derived from the STC datasheet (pages 31–35) cross-referenced with `SolarTracker/Ecoworthy Board Description.txt`. **Note:** The description file says pin 7 is a "Pulsed input" wind sensor; the actual hardware delivered is an **analog** sensor with a 0–2 V output (verified by reading the part's label in Phase 2D). Description-file labels for ambiguous pins should be treated as guesses, not ground truth — confirm against physical hardware.

   | Pin | Port | Function on this board |
   |---|---|---|
   | 1 | P2.7 | LCD D7 |
   | 2 | P2.6 | LCD D6 |
   | 3 | P1.0 / ADC0 | Sun sensor — East |
   | 4 | P1.1 / ADC1 | Sun sensor — West |
   | 5 | P1.2 / ADC2 | Sun sensor — South |
   | 6 | P1.3 / ADC3 | Sun sensor — North |
   | 7 | P1.4 / ADC4 | Wind-speed analog (0–2 V, 25 m/s per volt) |
   | 8 | P1.5 / ADC5 | Stall current sense (soft) |
   | 9 | P1.6 / ADC6 | Button analog bus |
   | 10 | P1.7 / ADC7 | Piezo buzzer |
   | 11 | **P5.4 / RST / MCLKO** | Relay 3 (North) — see quirk 8 |
   | 12 | VCC | — |
   | 13 | P5.5 | NC |
   | 14 | GND | — |
   | 15 | P3.0 / RxD / INT4 | Relay 2 (East) |
   | 16 | P3.1 / TxD | Relay 4 (South) |
   | 17 | **P3.2 / INT0** | IR receiver → repurposed as ESP bridge |
   | 18 | P3.3 / INT1 | Relay 1 (West) |
   | 19 | P3.4 / T0 / T1CLKO | LCD backlight (active-HIGH, push-pull) |
   | 20 | P3.5 / T1 / T0CLKO | LCD RS |
   | 21 | P3.6 / INT2 / RxD_2 | LCD RW |
   | 22 | P3.7 / INT3 / TxD_2 | LCD E |
   | 23 | P2.0 | LCD D0 |
   | 24 | P2.1 | LCD D1 |
   | 25 | P2.2 | LCD D2 |
   | 26 | P2.3 | LCD D3 |
   | 27 | P2.4 | LCD D4 |
   | 28 | P2.5 | LCD D5 |

   **Port 0 is not exposed on SOP28** — every P0.x pin shows "—" in the datasheet's SOP28 column. Don't write to P0; it's a no-op.

   **Port 3 is split-purpose on this board** — P3.0/P3.1 are relays E/S, P3.2 is the ESP bridge, P3.3 is relay W, P3.4 is NC, and P3.5–P3.7 are the LCD control bus (RS/RW/E). Never do byte-level writes to P3 except during relay-safe-boot before the LCD is initialized — use bit-level (sbit) ops everywhere else.
4. **Relay-to-direction mapping** (via ULN2003A's input-to-output pin pairing — input N pairs with output 17−N):
   - MCU pin 11 → ULN in 2 → ULN out 15 → Relay 3 → "North" (nominal)
   - MCU pin 15 → ULN in 5 → ULN out 12 → Relay 2 → "East" (nominal)
   - MCU pin 16 → ULN in 4 → ULN out 13 → Relay 4 → "South" (nominal)
   - MCU pin 18 → ULN in 1 → ULN out 16 → Relay 1 → "West" (nominal)

   **Cardinal-direction labels are the vendor's nominal intent, not ground truth.** Actuator mounting orientation and wire polarity are installer-dependent. Firmware must treat these as axis-relative directions (`+axis`/`-axis`) and resolve true compass mapping via a calibration step. See "H-bridge structure" below.

5. **H-bridge structure — empirically determined in Phase 2A.** Two physical actuators, four relays, two relays per axis. **The pairing does NOT follow the intuitive cardinal grouping.** Verified by manual button testing:
   - **N/S axis (tilts the panel)**: Relay 3 = `RELAY_N` (extends) + Relay 1 = `RELAY_W` (retracts).
   - **E/W axis (rotates the panel)**: Relay 4 = `RELAY_S` (extends) + Relay 2 = `RELAY_E` (retracts).

   The cardinal relay *labels* are misleading — `N+W` are H-bridge mates, and `S+E` are H-bridge mates. Either the vendor's labeling reflects panel-facing direction (not axis pairing), or the installer wired the actuators in a non-standard way. The empirical pairing is what matters.

   **Mutex pairs (hard invariant)**: `{RELAY_N, RELAY_W}` and `{RELAY_S, RELAY_E}`. Activating both relays of the same pair shorts the 12 V supply through both contacts. Firmware MUST enforce this — see `set_axis_ns()` and `set_axis_ew()` in `main.c`.

   **Button-to-relay mapping** (firmware re-routes for cardinal UX) — verified in Phase 2A bench testing:

   | Button | Axis | Actuator action | Panel motion | Relay |
   |---|---|---|---|---|
   | N | N/S (tilt) | extend | tilt north | `RELAY_N` |
   | S | N/S (tilt) | retract | tilt south | `RELAY_W` |
   | E | E/W (rotate) | extend | rotate east | `RELAY_S` |
   | W | E/W (rotate) | retract | rotate west | `RELAY_E` |

   So pressing S routes through to `RELAY_W` and pressing E routes through to `RELAY_S` — the button label and relay label don't align, but the panel motion matches the button label. **Convention:** extending an actuator always moves the panel toward N or E; retracting always moves toward S or W. This is consistent across both axes on this installation.
6. **IR receiver (pin 17 / P3.2) is vestigial** — the vendor never shipped a remote. **Repurposed as the Home Assistant bridge** (see below). P3.2 is **INT0**, which is a hardware bonus: the start-bit falling edge of incoming UART traffic can fire the external interrupt, so software-UART RX doesn't need to poll.
7. **Pins 15/16 are committed to relay drive AND are P3.0/P3.1 (UART1 default).** Pin 15 drives Relay 2 (East), pin 16 drives Relay 4 (South). Cannot use UART1 in default-pin configuration during normal operation without driving the E/S relays.
8. **RELAY_N (pin 11) is on P5.4, which is the external RESET pin by default.** The board only works if the **ENRST** config bit is *disabled* — that demotes P5.4 from RST to a normal GPIO. Stock firmware obviously runs with ENRST off; our flashes must preserve that. Every flash with stcgal must explicitly set the option (CLI flag varies by stcgal version — typically `-o reset_pin=0` or similar; verify in `stcgal --help`). If the chip ever boots with ENRST on, the first relay coil-pickup pulse will reset the MCU mid-tracking — symptom is "STC reboots every time it tries to move north."

## Home Assistant bridge: ESP-01S over pin 17 (single wire)

### Topology

```
                          ┌──── 5V (breakout VIN, from LM2596 rail)
                          │
ESP GPIO1 (TX) ─[BSS138 M1]┤── 10kΩ ╮
ESP GPIO3 (RX) ─[BSS138 M2]┤── 10kΩ ╯ pull-ups (in breakout's R-pak)
                          │
                          │ tied together on 5V side of breakout
                          │
                          └──── single wire ──── IR receiver signal leg ──── STC pin 17 (P3.2 / INT0)
                          
                          GND ── breakout GND ── tracker GND
```

The ESP-01S breakout has a **bidirectional BSS138-style level shifter** (2 MOSFETs + 4-resistor network). The breakout's 5V side is naturally an open-drain bus with built-in pull-ups — no external components needed.

**Wiring**: TX and RX on the breakout's 5V side are tied together (solder bridge on the breakout, or twist at the wire) to make the ESP's hardware UART half-duplex on a single wire. Single signal wire goes to the IR receiver's through-hole signal leg (0.1″ pitch — no SMD soldering required).

### Why this pin / topology (rejected alternatives)

Each was considered and ruled out. Don't re-litigate without new information:

- **Pin 15/16 as bidirectional UART**: pins 15 and 16 also drive Relay 2 (East) and Relay 4 (South) via the ULN2003A. UART idle state is HIGH → continuous relay energize. Even with "park low between bursts" firmware tricks on TX (pin 16), the RX direction (pin 15) is owned by the ESP, which can't natively park-low between transmissions.
- **Pins 13/19 for UART**: P5.5 and P3.4 — neither is a UART hardware alt-pin. Would require software UART, and they're SMD-only access (no convenient through-hole component on the trace). Pin 17 wins on access via the IR receiver leg.
- **Pin 15 with PCB trace cut**: viable but requires cutting one trace and rerouting Relay 3 to another GPIO. More invasive than necessary given pin 17 is free electrically.
- **Two-wire (separate TX/RX)**: works, but the breakout's bidirectional shifter makes a single wire trivially safe — saving one wire is genuinely useful for the cramped enclosure.
- **Dallas 1-Wire protocol**: ESPHome has native support, but requires the STC to be a 1-Wire slave with tight (15–60 µs) timing slots that fight with LCD writes. Half-duplex UART is simpler to implement on the STC side.
- **Direct connection without level shifter**: 5V STC vs 3.3V ESP push-pull contention is destructive. BSS138 breakout makes both sides effectively open-drain, which is safe.

### STC firmware setup

```c
// Configure P3.2 (pin 17) as open-drain
P3M0 |= (1 << 2);   // 11 = open-drain mode
P3M1 |= (1 << 2);
P3   |= (1 << 2);   // release line (pulled HIGH by breakout's pull-up)
```

- **Software UART** on P3.2 at **9600 baud** (push to 38400 if needed; 5 kΩ effective pull-up + trace capacitance limits the practical ceiling well below 115200).
- TX a "0": drive P3.2 LOW for one bit time. TX a "1": release (write 1) for one bit time. RX: sample mid-bit.
- **RX architecture**: P3.2 is INT0. Enable INT0 on falling edge (IT0=1, EX0=1); the ISR detects the start bit and either kicks off a bit-sampling timer or runs the receive state machine directly. This is cheaper than polling and lets the main loop continue LCD/sensor work.

### Protocol: magic-prefix framed half-duplex, ESP as master

```
ESP→STC:  ?\n                                    (poll every ~2s)
STC→ESP:  \xAA\x55 az=178 el=42 wind=12 mode=track <CRC8>\n

ESP→STC:  !goto az=180\n
STC→ESP:  \xAA\x55 ok <CRC8>\n
```

Rules:
- **ESP discards any incoming bytes that don't start with `\xAA\x55`** — those are the echoes of its own transmissions on the shared wire. Implement in ESPHome via a custom `uart_text_sensor` or a small custom component.
- **STC ignores anything received before a valid framing prefix.** This also drops the ESP-01S boot debug spam (74880 baud bursts on power-up).
- **Append a 1-byte CRC8 or XOR checksum** to every framed message. The wire is short and reliable, but the bus is shared open-drain — occasional bit errors are realistic and HA reporting `az=999999` triggering a panic stop is the failure mode to prevent.

### ESP-01S requirements (not original ESP-01)

- **Flash ≥ 1 MB** — ESPHome with OTA needs it. The original 512 KB ESP-01 fits ESPHome serially but blocks OTA, forcing the enclosure to be reopened on every update.
- **Local decoupling**: 470 µF electrolytic + 100 nF ceramic across the breakout's 5V VIN to handle WiFi TX current spikes (~300 mA in <10 µs). The LM2596 at 150 kHz can't slew that fast; missing this cap causes spurious resets and WiFi disconnects.
- **Antenna placement**: PCB trace antenna is at the end opposite the pin header. Keep it away from the relay coils and out of any metal enclosure walls.
- **Disable serial logging in ESPHome** (`logger: baud_rate: 0`) — the hardware UART is fully dedicated to the STC bridge. Use WiFi-based ESPHome logging instead.

### Timing constraints derived from HF165FD-5

These matter for any firmware that drives the relays (pins 11, 15, 16, 18 via ULN2003A) and may matter again if pins 15/16 are ever reconsidered for UART:

- **Operate (pickup) time**: 10 ms max, ~7 ms typical
- **Release time**: 5 ms max
- **Coil L/R time constant**: ~0.7–1 ms
- **Pickup current threshold**: ~64 mA (~70% of nominal 91 mA)

Rule of thumb: a sub-millisecond pulse on a relay drive line is well below the mechanical pickup threshold and won't move the armature. Multi-millisecond pulses will. After de-asserting a relay, wait ≥8 ms (release + margin) before any operation that could re-energize it.

## Open questions to verify on bench

Before committing firmware to hardware:

1. **LM2596 ripple on analog inputs.** 150 kHz switching may show up on sun-sensor ADC readings as ~10–30 mV fuzz. Add a 1 kΩ + 100 nF RC filter on each sensor line if differential readings need precision.
2. **Pin 17 (P3.2) idle state with IR receiver in place.** Some IR receiver modules actively pull the signal line high or low at idle. Verify with a multimeter that pin 17 is high-impedance enough for the breakout's pull-up to dominate. Decide whether the IR receiver stays populated (then its output is on the line) or gets desoldered.
3. **ESP-01S flash size.** Run `esptool.py flash_id` on the specific module before committing to the OTA path.
4. **Breakout's TX/RX pull-up parallel resistance.** Assumed 10 kΩ each → 5 kΩ effective when tied. Measure with a multimeter on the actual breakout to confirm the values aren't 4.7 kΩ or 22 kΩ (some clones vary).
5. **P5 bit-addressability on STC15F2K60S2.** Classic 8051 makes any SFR at an address-mod-8 boundary bit-addressable, and P5 is at 0xC8 (boundary). But STC datasheets are not uniformly explicit about extending bit-addressability to P4/P5 on every variant. If `RELAY_N = 1` (an sbit write on P5.4) doesn't physically wiggle pin 11, fall back to `P5 |= 0x10` byte-write macros in `board.h`. Verified working in Task 6 — sbit on P5.4 drives correctly.
6. **Stall-current sense (pin 8 / P1.5 / ADC5) — fully characterized in Phase 2C.** Verified topology (measured on the board, supersedes earlier guesses):

   ```
   12V_in ── 2.7kΩ ── pin 8 ── 3.0kΩ ── GND
                       │
                       ├── cap (~45 nF) ── GND
                       │
                       └── soft-knee Zener ── GND  (cathode on pin, V_z ≈ 1.2V, Z_z ≈ 77Ω)
   ```

   It's a **rail-droop monitor**, not a current shunt. The divider's open-circuit voltage is 12 × 3.0/5.7 = 6.32V, but the Zener clamps the pin to ~1.2V (baseline ADC ~250). The Zener is intentionally **soft-knee** (Z_z ≈ 77Ω, vs. <20Ω for a hard Zener) so that bus-voltage droop is transmitted to the pin at the ratio Z_z / R1 ≈ 77/2700 ≈ **2.85% of bus droop**. The cap is a noise filter (~2.5 kHz corner) that rejects motor commutation noise; the Zener doubles as ADC over-voltage protection.

   Firmware displays the channel as `dI` = `current_idle - sample`, 8-sample averaged. Empirical three-band classifier (`di_classify()` in `main.c`):

   | `dI` value | Band | Meaning |
   |---|---|---|
   | 0..1 | IDLE | no motor current (or below the ~2-count ADC noise floor) |
   | 2..4 | STALLED | residual current; motor stopped or limit switch open |
   | ≥ 5 | MOVING | motor pulling load current, panel in motion |

   **Bench vs. install signal magnitude.** On a stiff CV bench supply with short leads, motor-induced bus droop is small (~few hundred mV transient) and the signal is marginal (5–10 ADC counts during motion). On a real install with battery + lossy DC wiring (50–300 mΩ round-trip impedance), 1–3 V of droop is realistic under motor load → 30–85 ADC counts at the pin. **The same algorithm that's marginal on bench will be robust on the install.**

   **Why stall reads near idle, not as the largest droop.** The supplied actuators have *internal* limit switches that mechanically **open** the motor circuit at the endstop. Current drops to zero → rail recovers → pin returns to (or near) baseline. At the *retract* endstop a small residual current path through the limit-switch-bypass diode produces the −1 to −4 count signature; at the *extend* endstop the circuit opens cleanly and dI returns to 0. The classifier uses a `saw_motion` gate so that "back to idle after seeing MOVING" is also accepted as stall.

## Firmware gotchas

- **STC15 port-mode SFR ordering is M1-then-M0, not M0-then-M1.** The lower-numbered address is P*M1, the higher is P*M0 (e.g. P3M1=0xB1, P3M0=0xB2). Easy mistake: write P*M0 in the natural "M0 comes first" order and silently configure every push-pull output as a high-impedance input (mode 10 instead of mode 01). Symptom is "code looks fine, asm looks fine, but pin won't drive" — incidental external loads (piezo capacitance, ULN2003A internal pull-downs) make the chip *look* OK while not actually driving anything. The `include/stc15f2k60s2.h` header now has these in datasheet order with a comment.
- **The piezo buzzer is active-LOW.** `BUZZER = 0` (pin 10 / P1.7 LOW) energizes the buzzer; `BUZZER = 1` silences it. The board wires the piezo between the 5 V rail and pin 10, so the MCU is the sink-side switch. The original reverse-engineering notes only said "Piezo buzzer" without polarity — verified during Phase 0 bring-up.

## When extending this project

- The verified pin-to-port map (quirk 3 above) is the single source of truth for SOP28 pin assignments. `Ecoworthy Board Description.txt` is still authoritative for what each pin connects to *on the PCB*, but for STC15 port identity, trust the table.
- STC firmware lives in `EcoWorthyFirmware/` (SDCC + stcgal). The ESP-01S YAML will live in `EcoWorthyFirmware/esphome/` when Phase 2+ starts. Build/flash commands are documented in `EcoWorthyFirmware/README.md`.
- The bridge design is finalized — don't redesign without new constraints. If the constraint changes (e.g., relays replaced, enclosure changes), the rejected-alternatives section above gives you the tree of options that were considered.
