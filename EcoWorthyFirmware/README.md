# EcoWorthyFirmware

Custom firmware for the STC15F2K60S2 on the Ecoworthy dual-axis solar tracker
board. See `../CLAUDE.md` for hardware context and reverse-engineered pinout.

## Layout

```
EcoWorthyFirmware/
├── Makefile               build + flash recipes
├── README.md              this file
├── include/
│   ├── stc15f2k60s2.h     SFR / sbit declarations
│   └── board.h            board-specific pin map (BUZZER, RELAY_*, LCD_*)
├── src/
│   └── main.c             firmware entry point
└── build/                 SDCC output (created by `make`)
```

## Toolchain

- **SDCC** ≥ 4.4 (`sdcc --version` should print `4.x.y`)
- **GNU Make** ≥ 4.0
- **stcgal** 1.x (Python — `pip install stcgal`)
- A USB-TTL adapter wired to the board's programming header.

## Build

```sh
make            # produce build/ecoworthy.ihx
make clean      # remove build/
```

The Makefile builds with `--stack-auto` (reentrant calling convention).
This is **required**, not optional: the state-machine + helper call graph
exceeds SDCC's default 128-byte internal-RAM overlay budget, and the
reentrant model relocates function locals to the internal stack. The
Makefile also passes an explicit `--lib-path` to the bundled
`small-stack-auto` libraries because SDCC didn't auto-resolve that
model's lib directory on the development Windows install — adjust the
path if your SDCC lives elsewhere. Removing `--stack-auto` resurrects
the `?ASlink-Error-Could not get NNN consecutive bytes ... DSEG` link
failure.

## Flash

stcgal speaks the STC ISP protocol over UART. The chip's bootloader only listens
during a short window right after power-on, so the sequence is:

1. **Power the STC OFF.** Wait for the 5 V rail to fully drain.
2. Start stcgal — it will print `Waiting for MCU, please cycle power:` and begin
   polling.
3. **Power the STC ON.** The bootloader sees stcgal's handshake bytes and
   stays in ISP mode.
4. stcgal proceeds with trim → erase → write → verify → option-byte setup,
   then disconnects. Total time: ~2 seconds.
5. The chip resets and runs the new firmware.

```sh
stcgal -P stc15 -p COM5 -t 22118 build/ecoworthy.ihx
```

Flags:
- `-P stc15` — protocol family.
- `-p COM5` — serial port for the USB-TTL adapter (adjust for your machine).
- `-t 22118` — set the chip's internal RC oscillator trim to 22.1184 MHz
  (baud-rate-friendly).

## Wiring for ISP (USB-TTL → board programming header)

| USB-TTL | STC pin | Function |
|---------|---------|----------|
| TX      | 15 (P3.0 / RxD) | host → MCU |
| RX      | 16 (P3.1 / TxD) | MCU → host |
| GND     | 14              | shared ground |

If the USB-TTL adapter outputs 3.3 V logic and the STC runs at 5 V, you need a
**bidirectional level shifter** (BSS138-style is fine) between them — 3.3 V is
below the STC's 3.5 V V_IH worst case.

⚠ **The STC's UART pins (P3.0, P3.1) are also relay drives**
(RELAY_E and RELAY_S). The ISP bootloader configures them as UART, but once
stock firmware takes over, they revert to relay outputs and the chip will
fight the USB-TTL adapter on the shared wire. This is why fast power-cycling
and an immediately-ready stcgal matter: miss the bootloader window and the
contention starts.

## What the firmware does (Phases 0–2C)

`main.c` boots relay-safe (all relays + buzzer forced LOW), configures
ports, inits the HD44780 LCD, starts a 1 kHz Timer 0 `millis()` tick,
loads EEPROM config, and runs boot auto-zero if calibrated. Then a
50 ms state-machine main loop:

- **Idle** — live sun/wind readout; SET enters the menu.
- **Menu** — scrolling no-wrap list: Track / Jog / Calibrate / Backlash
  / Settings / Version. Buttons are a debounced analog resistor ladder.
- **Calibrate** — per axis: bump-off, retract-to-stall (zero), then
  timed extend + timed retract; stores `max(extend, retract)` with the
  extend stall-delay subtracted. Stall = dI signature on the
  soft-Zener rail-droop sense (see `../CLAUDE.md` quirk 6).
- **Jog** — manual axis control; SET saves current position as the
  horizontal reference (% of stroke) to EEPROM.
- **Track** — differential sun-sensor pulse-tracker (8-sample averaged,
  configurable threshold), per-axis 2-min/18-min duty limiting.
- **Storm** — wind ≥ threshold forces park-to-horizontal, then holds
  with a resettable dwell timer before auto-returning to Track.
- **Settings** — EEPROM-persisted: wind storm/release, storm dwell,
  track threshold. Press-and-hold auto-repeats value edits.

Position is tracked by integrating relay on-time, saturated to the
calibrated stroke. Duty + storm interlocks run every loop iteration
regardless of mode.

## Debugging notes

- The SDCC busy-wait `delay_us(N)` is currently ~2-3× slower than naive math
  suggests (~3 µs per "us" at 22.1184 MHz). Timings in the LCD init are
  generous enough to absorb this. When real-time accuracy is needed
  (software UART), switch to Timer 0 / PCA hardware timing.
- See `../CLAUDE.md` "Firmware gotchas" for the STC15 port-mode SFR
  ordering trap that bit Phase 0.
