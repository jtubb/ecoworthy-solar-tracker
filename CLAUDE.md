# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository status

There is no code yet. This directory is currently a **hardware reverse-engineering notebook** for a planned custom-firmware project targeting an off-the-shelf "Ecoworthy" dual-axis solar tracker board, plus an ESPHome bridge to Home Assistant via an ESP-01S. No build system, toolchain, tests, or VCS are set up.

## Layout

- `SolarTracker/Ecoworthy Board Description.txt` — reverse-engineered pinout. Authoritative for hardware wiring until firmware code exists.
- `*.pdf` at repo root — vendor manuals for the surrounding solar install (Growatt SPH inverters, SG48100 battery). Reference material; not related to the tracker firmware.

## Target hardware

- **MCU:** STC 15F2K60S2 (SOP28), 8051-derivative. Toolchain: SDCC. Flasher: stcgal or STC-ISP via pins 15/16 (P3.0/P3.1) during reset.
- **5V regulator:** LM2596S buck, 3 A capable — plenty of headroom for the ESP bridge.
- **Relays:** Hongfa **HF165FD-5** (5 V coil, ~55 Ω, ~91 mA). ULN2003A's COM pin is on the 5 V rail. The 12 V the original notes mention is on relay *contacts*, switching the actuators — not on the coil side.
- **Outputs:** 1602A parallel LCD, piezo buzzer, 4 relays via ULN2003A driving **two** 12 V linear actuators (one per axis, N/S and E/W). Relays are configured as a 2-relay H-bridge per actuator — one relay extends, the other retracts.
- **Inputs:** 6 buttons (analog-multiplexed, see quirks), IR receiver, wind-speed pulse, 4 sun-intensity sensors, limit switches (see quirks).

## Non-obvious hardware quirks

1. **Buttons are read as a single analog/current-sensed input on MCU pin 9** — not 6 GPIOs. Series resistors: B1=12 kΩ, B2=51 kΩ, B3=27 kΩ, B4=15 kΩ, B5=62 kΩ, B6=none/0 Ω. ADC-sample and threshold; debounce *after* classification.
2. **Limit switches are not on dedicated MCU pins.** They feed the ULN2003A inputs via diodes, pulling relay-driver inputs low at endstops. Vendor kit may ship without physical limit switches; pin 8 (2.7 kΩ to V_in, 10 kΩ + cap + diode to GND) is probably a **soft current-sensing** network for stall detection.
3. **Pin 13 = P5.5, pin 19 = P3.4** (per STC15F2K60S2 SOP28 datasheet). Both NC on this PCB. Neither is a UART hardware alt-pin, but P3.4 has timer/PCA capability — preserve it for future use (e.g., wind-speed input capture).
4. **Relay-to-direction mapping** (via ULN2003A's input-to-output pin pairing — input N pairs with output 17−N):
   - MCU pin 11 → ULN in 2 → ULN out 15 → Relay 3 → "North" (nominal)
   - MCU pin 15 → ULN in 5 → ULN out 12 → Relay 2 → "East" (nominal)
   - MCU pin 16 → ULN in 4 → ULN out 13 → Relay 4 → "South" (nominal)
   - MCU pin 18 → ULN in 1 → ULN out 16 → Relay 1 → "West" (nominal)

   **Cardinal-direction labels are the vendor's nominal intent, not ground truth.** Actuator mounting orientation and wire polarity are installer-dependent. Firmware must treat these as axis-relative directions (`+axis`/`-axis`) and resolve true compass mapping via a calibration step. See "H-bridge structure" below.

5. **H-bridge structure.** Two physical actuators (one per axis), four relays, two relays per axis forming an H-bridge that selects forward/reverse polarity:
   - **E/W axis**: Relay 1 (pin 18) and Relay 2 (pin 15). Activating exactly one extends/retracts the actuator.
   - **N/S axis**: Relay 3 (pin 11) and Relay 4 (pin 16). Same pattern.
   - **Both relays on the same axis must never be active simultaneously** — depending on the H-bridge wiring this can short the 12 V supply or open-circuit the motor. Firmware MUST enforce mutual exclusion per axis as a hard invariant, not a best-effort.
6. **IR receiver (pin 17) is vestigial** — the vendor never shipped a remote. **Repurposed as the Home Assistant bridge RX** (see below).
7. **Pins 15/16 are committed to relay drive AND are P3.0/P3.1 (UART1 default).** Pin 15 drives Relay 2 (East), pin 16 drives Relay 4 (South). Cannot use UART1 in default-pin configuration during normal operation without driving the E/S relays.

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
                          └──── single wire ──── IR receiver signal leg ──── STC pin 17 (P3.5)
                          
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
// Configure P3.5 (pin 17) as open-drain
P3M0 |= (1 << 5);   // 11 = open-drain mode
P3M1 |= (1 << 5);
P3   |= (1 << 5);   // release line (pulled HIGH by breakout's pull-up)
```

- **Software UART** on P3.5 at **9600 baud** (push to 38400 if needed; 5 kΩ effective pull-up + trace capacitance limits the practical ceiling well below 115200).
- TX a "0": drive P3.5 LOW for one bit time. TX a "1": release (write 1) for one bit time. RX: sample mid-bit.

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
2. **Pin 17 idle state with IR receiver in place.** Some IR receiver modules actively pull the signal line high or low at idle. Verify with a multimeter that pin 17 is high-impedance enough for the breakout's pull-up to dominate.
3. **ESP-01S flash size.** Run `esptool.py flash_id` on the specific module before committing to the OTA path.
4. **Breakout's TX/RX pull-up parallel resistance.** Assumed 10 kΩ each → 5 kΩ effective when tied. Measure with a multimeter on the actual breakout to confirm the values aren't 4.7 kΩ or 22 kΩ (some clones vary).

## When extending this project

- Re-read `Ecoworthy Board Description.txt` end-to-end before any firmware work. Several pins still have "unknown function" notes that should be resolved with a multimeter, not assumed.
- If adding code, create `SolarTracker/firmware/` for the STC side and `SolarTracker/esphome/` for the ESP-01S YAML. Document build/flash commands here once they exist (SDCC + stcgal for the STC; ESPHome CLI for the ESP).
- The bridge design is finalized — don't redesign without new constraints. If the constraint changes (e.g., relays replaced, enclosure changes), the rejected-alternatives section above gives you the tree of options that were considered.
