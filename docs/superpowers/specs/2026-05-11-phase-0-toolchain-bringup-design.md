# Phase 0: Toolchain Bring-Up — Design Spec

**Date:** 2026-05-11
**Project:** EcoWorthy solar tracker custom firmware
**Phase:** 0 of 5 (Toolchain bring-up)

## Goal

Prove the full firmware development loop end-to-end on the actual Ecoworthy tracker board, before any application code is written. By the end of Phase 0:

- Code can be edited, built, and flashed in a single `make flash` command
- The board responds visibly to flashed code (audible buzzer beep, then text on the LCD)
- No relays fire and no actuator motion occurs during boot
- Subsequent phases inherit a working dev loop, an LCD usable as a debug output, and a known-correct internal clock frequency

Phase 0 is **the only phase where a failure can totally block forward progress** (e.g., SDCC produces unflashable HEX, stcgal doesn't recognize the chip, LCD parallel timing doesn't match the module on this board). De-risk these now, before there's application logic to debug alongside.

## Success criteria

After flashing the Phase 0 binary and power-cycling the board:

1. Audible 200 ms beep from the piezo on MCU pin 10 within ~100 ms of power-up.
2. ~500 ms of silence.
3. LCD displays:
   ```
   EcoWorthy
   Phase 0 OK
   ```
4. **No relay click, no actuator motion, no LED change on any unused circuit.** The tracker sits motionless and silent after the beep + LCD display.

If any of (1)–(3) fails, Phase 0 is not complete. (4) is a safety requirement at every boot.

## Toolchain decisions

| Component | Choice | Rationale |
|---|---|---|
| Compiler | SDCC (Small Device C Compiler) | Free, open-source, only viable C compiler for STC15. Installed via SDCC Windows MSI from sdcc.sourceforge.net. |
| Flasher | stcgal (Python CLI, installed via `pip install stcgal`) | Scriptable, integrates with `make`. Open source. |
| Programmer hardware | Prolific PL2303GT USB-TTL cable (sealed form factor, 6 wires: Black=GND, Red=+5V, Green=TXD, plus Blue/Yellow/White unidentified — to be characterized later) | User already owns. Newer-generation PL2303 chip without the driver issues of older PL2303HXA/TA chips. |
| Build system | GNU Make | Embedded-project standard. Provides `make`, `make flash`, `make clean`, `make monitor` targets. Available on Windows via Git for Windows or MSYS2. |
| Internal clock | **22.1184 MHz** (set by stcgal `--trim 22118` at flash time) | Baud-rate-friendly: divides cleanly to 9600/38400/115200 for Phase 4's software UART. STC15F2K60S2 internal RC oscillator is factory-trimmed for this frequency. |
| Programming voltage | 5 V (matches existing board rail) | LM2596S regulator already provides this. |
| Auto-reset | **Deferred** for Phase 0 — manual power-cycle | Identifying which of the unlabeled PL2303 wires is DTR vs RTS vs RXD requires bench characterization. Manual cycling is mildly annoying but unblocks coding immediately. Auto-reset hardware will be installed in parallel with Phase 1. |

## Project structure

Firmware source lives at the repo root in `EcoWorthyFirmware/`:

```
Solarsystem/
├── CLAUDE.md
├── SolarTracker/
│   └── Ecoworthy Board Description.txt
└── EcoWorthyFirmware/
    ├── Makefile
    ├── README.md
    ├── include/
    │   └── stc15f2k60s2.h    # Community-maintained SFR header (specific URL TBD in implementation plan)
    └── src/
        └── main.c            # Phase 0: beep + LCD hello
```

Phase 0 deliberately uses a single source file. Modular splitting (`lcd.c`, `buzzer.c`, `buttons.c`, `relay.c`, etc.) happens in Phase 1 when multiple callers need shared modules — YAGNI for Phase 0.

## Build flow

```
src/main.c ──[ sdcc -mmcs51 --model-small -I include ]──> build/main.ihx
                                                                │
                                                       [ stcgal -P stc15
                                                          -p COMx
                                                          --trim 22118
                                                          build/main.ihx ]
                                                                │
                                                                ▼
                                                       STC15F2K60S2 flash
```

### Makefile targets

| Target | Action |
|---|---|
| `make` (default: `make build`) | Compile `src/main.c` → `build/main.ihx` |
| `make flash` | Build if stale, then run stcgal. Prompts user to power-cycle the board. |
| `make clean` | Remove `build/` |
| `make monitor` | Open the PL2303 as a serial terminal at 9600 baud (placeholder for Phase 4 use) |

COM port is a variable at the top of the Makefile (`COM_PORT ?= COM5`) so users can override it without editing the body.

## Code shape: what `main.c` does

The first executable instructions, in order:

1. **Force all ULN2003A-driver pins LOW.** Before anything else, write to port 3 to ensure MCU pins 11, 15, 16, 18 (which drive the 4 relays through the ULN2003A) are at logic 0. This is the most safety-critical instruction in Phase 0 — without it, quasi-bidirectional default-high pins would energize all four relays at boot and slam the actuators against their endstops.

2. **100 ms settle delay.** Let supply rails stabilize and the LCD's internal power-up reset complete.

3. **Beep the piezo on MCU pin 10 for 200 ms.** Square-wave drive at ~2 kHz (piezo resonant range). Verifies: MCU is clocked, code is running, pin 10 wiring is intact, the toolchain produces working binaries.

4. **500 ms quiet.**

5. **Initialize the 1602A LCD** via the documented HD44780 8-bit init sequence (function set → display on → entry mode → clear → home). All eight data pins (D0–D7 on MCU pins 23–28, 1, 2) are driven; RS on pin 20, RW on pin 21, E on pin 22.

6. **Print two lines** ("EcoWorthy" / "Phase 0 OK").

7. **Idle forever** in a `while(1)` loop. No further activity. A future polish step might add a periodic "heartbeat" beep, but Phase 0 doesn't require it.

### Pin mappings (hardcoded inline for Phase 0)

```
Buzzer        : MCU pin 10
LCD D0–D7     : MCU pins 23–28, 1, 2
LCD RS        : MCU pin 20
LCD RW        : MCU pin 21
LCD E         : MCU pin 22
ULN2003A drives (MUST be LOW at boot):
  pin 11 (Relay 3 / nominally "North")
  pin 15 (Relay 2 / nominally "East")
  pin 16 (Relay 4 / nominally "South")
  pin 18 (Relay 1 / nominally "West")
```

The cardinal-direction labels are the vendor's nominal intent. Until the actuators are physically installed and calibrated, the true compass direction of any given relay activation is unknown — actuator mounting orientation and motor wire polarity are installer-dependent. For Phase 0 this doesn't matter (we don't activate any relay), but Phase 2+ firmware will treat the relays as axis-relative (`+axis` / `−axis`) with a runtime-configurable polarity map.

These `#define`s live inline in `main.c` for now and migrate to a shared `board.h` in Phase 1.

### H-bridge structure (context for later phases)

Four relays drive **two** linear actuators (one per axis), configured as a 2-relay H-bridge per actuator. Each relay pair selects forward/reverse current polarity for its actuator:

- **E/W axis actuator** ↔ Relay 1 (MCU pin 18) and Relay 2 (MCU pin 15)
- **N/S axis actuator** ↔ Relay 3 (MCU pin 11) and Relay 4 (MCU pin 16)

Activating exactly one relay of a pair drives the actuator in one direction. Activating both relays of a pair simultaneously is **forbidden** — depending on the H-bridge wiring it either shorts the 12 V supply through both relay contacts or open-circuits the motor. Phase 2's relay driver MUST enforce per-axis mutual exclusion as a hard invariant. For Phase 0, the boot-time "force all four LOW" satisfies this trivially.

### Timing constants

All delay loops are busy-wait, calibrated for 22.1184 MHz on the STC15's 1T 8051 core (one machine cycle per clock tick — so ~45 ns per cycle, ~22 cycles per microsecond). The LCD HD44780 init sequence requires:

- ≥15 ms wait after power-on before first command
- ≥4.1 ms after first `0x30` function-set
- ≥100 µs after second `0x30`
- ≥40 µs after each subsequent command
- ≥1.64 ms after `Clear Display`

These constants are derived from the HD44780 datasheet and well-documented; the implementation plan will codify them as named `#define`s.

## Auto-reset circuit (parts to order, install in parallel with Phase 1)

Decoupled from Phase 0 flash workflow but worth ordering now so the hardware is on hand when Phase 0 milestones are hit. Manual power-cycle is fine for Phase 0; this circuit eliminates the manual step for Phases 1+.

### Topology

P-channel MOSFET high-side switch with NPN inverter, controlled by DTR (or RTS) from the PL2303:

```
+5V (PL2303 VCC, always on)
       │
       │ source
   ┌─[ AO3401 P-MOSFET ]─┐
   │                      │
   │ drain                │ gate
   │                     ┌┴── 10 kΩ pull-up to +5V
   │                     │
   │                     │ collector
   │              ┌──[ 2N3904 NPN ]──┐
   │              │                  │
   │            emitter             base
   │              │                  │
STC's VCC pin   GND        10 kΩ ──[ DTR or RTS from PL2303 ]
   (existing
    trace cut
    here)

(Optional) 100 nF ceramic across STC VCC to GND, smooths the brief power-off transient.

Normal state: DTR high (3.3 V) → NPN on → gate low → MOSFET on → STC powered.
ISP trigger:  DTR low (0 V)    → NPN off → gate high → MOSFET off → STC unpowered briefly.
```

### BOM

| Part | Qty | Notes |
|---|---|---|
| AO3401 P-channel MOSFET, SOT-23 | 1 | Logic-level, -30 V, -4 A, RDS(on) 35 mΩ at Vgs=-4.5 V |
| 2N3904 NPN BJT, TO-92 | 1 | Standard small-signal NPN |
| 10 kΩ resistor, 1/4 W, 5% | 2 | Gate pull-up; base current limit |
| 100 nF ceramic capacitor (optional) | 1 | Polish; smooths transient |

User chose the discrete-transistor design over a single-IC PhotoMOS (e.g., AQY210EH) based on parts availability — discrete transistors are more universally stocked.

### Activation (Phase 1+)

Once the unlabeled PL2303 wires are characterized (Python + multimeter test against `s.dtr = True/False`), the DTR/RTS wire connects to the NPN base via 10 kΩ. stcgal then runs with `--autoreset`, which pulses DTR in a known sequence that lands the STC in its ISP bootloader window without user intervention.

## Flash workflow (Phase 0, manual power-cycle)

1. From a shell in `EcoWorthyFirmware/`, run `make flash`.
2. The Makefile builds if needed, then invokes:
   ```
   stcgal -P stc15 -p COM5 --trim 22118 build/main.ihx
   ```
3. stcgal prints "Waiting for MCU, please power on..."
4. User toggles the tracker board's power switch off, then back on, within stcgal's timeout (~30 s).
5. stcgal detects the chip, programs flash, sets the IRTRIM byte for 22.1184 MHz, and exits.
6. On the next power-up (or this same one), the code runs.

Expected flash time at full speed: ~1 second once stcgal connects.

## Verification procedure

After `make flash` reports success:

1. **Power on the board** (if not already on).
2. **Listen for the beep** within ~100 ms. If silent, the first thing to check is pin 10 connectivity and that `main()` actually ran (vs. crashed in startup).
3. **Look at the LCD** ~500 ms after the beep. Both lines should display the expected text. Blank LCD → init timing problem. Garbled text → wrong pin mapping or RW polarity error.
4. **Confirm no relay click and no actuator motion.** If any relay fires, immediately power off and investigate — the relay-safe-boot step is wrong or didn't execute.
5. **Reboot the board** by power-cycling and confirm reproducibility. Phase 0 is complete when steps 2–4 succeed reliably across at least 5 consecutive cycles.

## What Phase 0 explicitly does NOT do

To keep scope tight:

- **No sensor reads.** Buttons, sun sensors, wind speed are Phase 1.
- **No relay or actuator drive.** Relay-driver pins are explicitly held LOW. Active relay control is Phase 2.
- **No timers or interrupts.** All delays are busy-wait. Interrupt-driven timing is Phase 1.
- **No UART (hardware or software).** ESP bridge is Phase 4.
- **No EEPROM persistence.** Calibration storage is Phase 3.
- **No assembly code.** Pure C only.
- **No autoreset wiring.** Manual power-cycle is the Phase 0 workflow; autoreset is a parallel-track hardware project.

## Risks and unknowns

| Risk | Mitigation |
|---|---|
| LCD parallel-interface timing doesn't match this specific module | HD44780 init constants come from the datasheet with safety margins (e.g., 5 ms instead of 4.1 ms after first function-set). If the LCD still doesn't initialize, double the delays and re-flash. |
| Community `stc15f2k60s2.h` header has typos or missing SFRs | Implementation plan will pin a specific GitHub URL/commit and document why it was chosen. Fallback: hand-define needed SFRs locally. |
| stcgal doesn't recognize the chip on first try | Common cause: COM port number or USB-TTL TX/RX swapped. Implementation plan includes verification steps before the first flash. |
| `--trim 22118` produces a clock that's significantly off | STC15 internal RC accuracy is ±0.3% at room temp. Verify post-flash by measuring beep frequency or buzzer pulse width. If off by more than a few percent, try `--trim 22117` or `--trim 22119`. |
| Pin 10 not actually wired to the piezo on this specific board revision | Check with a multimeter before flashing. If the piezo is elsewhere, update the `BUZZER_PIN` define before building. |

## Open questions (to resolve before implementation plan)

1. **Specific `stc15f2k60s2.h` source URL.** Will pick a maintained header during the implementation plan.
2. **Buzzer drive frequency.** Most piezos resonate around 2–4 kHz; pick a frequency that's audible without being annoying. Decision in the implementation plan.
3. **PL2303 wire identification.** Deferred. Phase 0 doesn't need it; Phase 1 hardware work will resolve it.

## Future work (not Phase 0)

- Phase 0 polish: install auto-reset hardware once DTR/RTS wire is identified.
- Phase 1: sensor/input drivers (sun sensors, buttons, wind pulse).
- Phase 2: actuator drivers + soft-limit safety.
- Phase 3: tracking algorithm + state machine.
- Phase 4: ESP-01S bridge + ESPHome integration.
