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

## What Phase 0 firmware does

`main.c` performs:
1. Relay-safe boot — every relay pin and the buzzer forced LOW before
   anything else runs.
2. Configures relays, buzzer, and LCD pins as push-pull outputs.
3. Initializes the HD44780 LCD with the standard 8-bit wake-up sequence.
4. Displays `EcoWorthy` on line 1 and `Phase 0 OK` on line 2.
5. Idles in an infinite empty loop.

No relays activate. No buzzer sounds. The LCD message confirms the chip is
running custom firmware.

## Debugging notes

- The SDCC busy-wait `delay_us(N)` is currently ~2-3× slower than naive math
  suggests (~3 µs per "us" at 22.1184 MHz). Timings in the LCD init are
  generous enough to absorb this. When real-time accuracy is needed
  (software UART), switch to Timer 0 / PCA hardware timing.
- See `../CLAUDE.md` "Firmware gotchas" for the STC15 port-mode SFR
  ordering trap that bit Phase 0.
