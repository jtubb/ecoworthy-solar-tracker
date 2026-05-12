# Phase 0: Toolchain Bring-Up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get an end-to-end firmware development loop working on the EcoWorthy tracker board — ending with an audible buzzer beep and "Phase 0 OK" text on the LCD, proving the toolchain, flasher, and target hardware all work together.

**Architecture:** SDCC (the Small Device C Compiler) builds `src/main.c` against a minimal STC15F2K60S2 SFR header, producing an Intel HEX file. stcgal (a Python tool) flashes the HEX over a Prolific PL2303GT USB-TTL adapter, with the user manually power-cycling the tracker board to enter the STC bootloader's ISP window. GNU Make wraps build, flash, and clean into one-word commands.

**Tech Stack:** SDCC (C compiler), stcgal (Python flasher), GNU Make (build), Git (version control), Prolific PL2303GT (USB-TTL bridge).

---

## File Structure

By the end of this plan, the repository looks like:

```
Solarsystem/                                  # repo root
├── .git/                                     # initialized in Task 1
├── .gitignore                                # ignore build artifacts
├── CLAUDE.md                                 # updated in Task 9
├── SolarTracker/
│   └── Ecoworthy Board Description.txt       # untouched
├── docs/
│   └── superpowers/
│       ├── specs/2026-05-11-phase-0-...md    # untouched
│       └── plans/2026-05-11-phase-0-...md    # this file
└── EcoWorthyFirmware/                        # all firmware lives here
    ├── README.md                             # written in Task 8
    ├── Makefile                              # written in Tasks 4, 5
    ├── include/
    │   ├── stc15f2k60s2.h                    # minimal SFR header (Task 3)
    │   └── board.h                           # pin mappings (Task 3)
    └── src/
        └── main.c                            # grown incrementally
                                              #  Task 4: relay-safe-boot only
                                              #  Task 6: + buzzer beep
                                              #  Task 7: + LCD "Hello"
```

**File responsibilities:**

- `include/stc15f2k60s2.h` — SFR definitions for the STC15F2K60S2. Minimal: only what Phase 0 needs (standard 8052 SFRs + the few STC-specific ones we touch). Expanded in later phases.
- `include/board.h` — Maps every physical MCU pin used by this board to its STC15 port.bit name (e.g., `BUZZER_PIN = P3_5`). This is the *one* place that has to be right before any code reads or writes pins; everything else uses the symbolic names.
- `src/main.c` — Application entry. For Phase 0, a single file containing `main()`, busy-wait delays, buzzer drive, and LCD init/print. Splits into modules in Phase 1.
- `Makefile` — `make`, `make flash`, `make clean`, `make monitor` targets.

---

## Task 1: Initialize Git Repository and Project Skeleton

**Files:**
- Create: `Solarsystem/.gitignore`
- Create: `Solarsystem/EcoWorthyFirmware/` (empty directories: `include/`, `src/`, `build/`)

- [ ] **Step 1: Initialize git from the repo root**

Run from `C:\Users\jtubb.SOLUTIONS\Documents\Solarsystem`:

```bash
git init
git add CLAUDE.md SolarTracker/ docs/
git commit -m "chore: initial commit of design notes and Phase 0 spec/plan"
```

Expected: git reports an initial commit with the existing notes and docs files.

- [ ] **Step 2: Create the EcoWorthyFirmware directory tree**

```bash
mkdir -p EcoWorthyFirmware/include EcoWorthyFirmware/src EcoWorthyFirmware/build
```

Expected: three empty directories created.

- [ ] **Step 3: Create `.gitignore`**

Create `Solarsystem/.gitignore` with these exact contents:

```gitignore
# Build artifacts
EcoWorthyFirmware/build/
*.ihx
*.hex
*.lst
*.lk
*.map
*.mem
*.rel
*.rst
*.sym
*.asm
*.adb

# Editor/OS noise
.vscode/
.idea/
*.swp
*.swo
Thumbs.db
.DS_Store
```

- [ ] **Step 4: Commit the skeleton**

```bash
git add .gitignore EcoWorthyFirmware/
git commit -m "chore: scaffold EcoWorthyFirmware project"
```

Note: `git add EcoWorthyFirmware/` will add the directory tree even though it's empty if you `touch EcoWorthyFirmware/include/.gitkeep` first; alternatively, add it once any task creates real files.

---

## Task 2: Install and Verify Toolchain

**Files:** None. This task only installs and verifies software on the developer machine.

- [ ] **Step 1: Install SDCC**

Download the SDCC Windows installer from https://sourceforge.net/projects/sdcc/files/sdcc-win64/ (pick the latest stable release — 4.4.0 or newer). Run the MSI. **Check "Add SDCC to PATH"** during installation.

Verify in a new PowerShell window:

```powershell
sdcc --version
```

Expected output: `SDCC : mcs51/z80/... 4.x.x ... (Windows)` with no "command not found" error.

- [ ] **Step 2: Install Python 3 and stcgal**

Open PowerShell. Check if Python is installed:

```powershell
python --version
```

If not present, install Python 3.11 or newer from https://www.python.org/downloads/ — **check "Add Python to PATH"** during install.

Then install stcgal:

```powershell
pip install stcgal
```

Verify:

```powershell
stcgal --version
```

Expected output: `stcgal 1.x` with no error.

- [ ] **Step 3: Install GNU Make**

The simplest path on Windows: install via `choco` if you have Chocolatey, or via `scoop`. From an admin PowerShell:

```powershell
choco install make -y
```

(Alternatives: `scoop install make`, or use MSYS2's `pacman -S make`.)

Verify:

```powershell
make --version
```

Expected output: `GNU Make 4.x ...`

- [ ] **Step 4: Document tooling versions**

In a scratch note (do not commit yet), record the versions of SDCC, stcgal, Python, and make you have. This goes into the README in Task 8.

---

## Task 3: Create SFR Header and Board Pin Map

**Files:**
- Create: `EcoWorthyFirmware/include/stc15f2k60s2.h`
- Create: `EcoWorthyFirmware/include/board.h`

The minimal STC15-specific SFR set we need for Phase 0 is small: just port mode registers in case we want them later. SDCC's stock `8052.h` covers `P0`/`P1`/`P2`/`P3`. We define `P4`/`P5` and the mode registers ourselves because Phase 0 may use pins on those extra ports.

- [ ] **Step 1: Open the STC15F2K60S2 datasheet, SOP28 pinout section**

The user has confirmed: pin 13 = P5.5, pin 19 = P3.4, pin 17 = P3.5. With those anchors, work through the rest of the SOP28 pin → port.bit mapping from the datasheet (search Google for "STC15F2K60S2 datasheet" — the PDF from STC Microelectronics is the canonical source; the SOP28 pinout is typically on page 8–12 of the English datasheet).

Record the mapping for the pins we use in Phase 0:

| Phys pin | Port.bit | Used as |
|---|---|---|
| 1 | (verify from datasheet) | LCD D7 |
| 2 | (verify) | LCD D6 |
| 10 | (verify) | Buzzer |
| 11 | (verify) | Relay 3 (N), MUST be LOW at boot |
| 15 | P3.0 | Relay 2 (E), MUST be LOW at boot |
| 16 | P3.1 | Relay 4 (S), MUST be LOW at boot |
| 17 | P3.5 | (Phase 4: ESP bridge — not used in Phase 0) |
| 18 | (verify) | Relay 1 (W), MUST be LOW at boot |
| 20 | (verify) | LCD RS |
| 21 | (verify) | LCD RW |
| 22 | (verify) | LCD E |
| 23–28 | (verify) | LCD D0–D5 |

Anchoring on `pin 15 = P3.0` (the STC15 programming pin), `pin 16 = P3.1`, `pin 17 = P3.5`, `pin 19 = P3.4` constrains the rest. The remaining pins are likely distributed across P1, P2, P4, P5.

- [ ] **Step 2: Write `include/stc15f2k60s2.h`**

Create with this content. This is a minimal header — additional SFRs are added in later phases as needed.

```c
#ifndef STC15F2K60S2_H
#define STC15F2K60S2_H

/* Pull in SDCC's standard 8052 SFR definitions for P0/P1/P2/P3/SP/DPL/DPH/PSW/etc. */
#include <8052.h>

/* STC15-specific extra ports (not in standard 8052) */
__sfr __at (0xC0) P4;
__sfr __at (0xC8) P5;

/* STC15 port mode registers (00 = quasi-bi, 01 = push-pull, 10 = input-only, 11 = open-drain) */
__sfr __at (0x93) P0M1;
__sfr __at (0x94) P0M0;
__sfr __at (0x91) P1M1;
__sfr __at (0x92) P1M0;
__sfr __at (0x95) P2M1;
__sfr __at (0x96) P2M0;
__sfr __at (0xB1) P3M1;
__sfr __at (0xB2) P3M0;
__sfr __at (0xB3) P4M1;
__sfr __at (0xB4) P4M0;
__sfr __at (0xC9) P5M1;
__sfr __at (0xCA) P5M0;

/* STC15 auxiliary register (extra RAM enable, BRT prescaler, etc.) */
__sfr __at (0x8E) AUXR;

#endif /* STC15F2K60S2_H */
```

- [ ] **Step 3: Write `include/board.h`**

Use the verified mapping from Step 1. The following is a starting point — **edit each `BUZZER_PIN`, `LCD_*`, etc. to match what the datasheet actually says for your SOP28 pins.**

```c
#ifndef BOARD_H
#define BOARD_H

#include "stc15f2k60s2.h"

/*
 * Ecoworthy tracker board pin map.
 * Edit these sbit declarations to match the verified SOP28 pinout from the
 * STC15F2K60S2 datasheet. The values shown below are starting points that
 * MUST be verified before the first flash.
 */

/* Buzzer: phys pin 10. Replace P3_7 with the actual port.bit name. */
__sbit __at (0xB7) BUZZER_PIN;   /* P3.7 — VERIFY */

/* LCD parallel interface */
__sbit __at (0xA0) LCD_RS;       /* phys pin 20 — VERIFY */
__sbit __at (0xA1) LCD_RW;       /* phys pin 21 — VERIFY */
__sbit __at (0xA2) LCD_E;        /* phys pin 22 — VERIFY */
/* LCD D0–D7 are read/written as a full byte; the data port is defined below. */
#define LCD_DATA_PORT  P2        /* the port containing LCD D0–D7 — VERIFY */

/*
 * Relay control pins (driven LOW at boot, never simultaneously per-axis later).
 *   pin 11 → Relay 3 ("North" nominal)
 *   pin 15 = P3.0 → Relay 2 ("East" nominal)
 *   pin 16 = P3.1 → Relay 4 ("South" nominal)
 *   pin 18 → Relay 1 ("West" nominal)
 */
/* Anchored P3 pins are known: */
__sbit __at (0xB0) RELAY_E;      /* P3.0, phys pin 15 */
__sbit __at (0xB1) RELAY_S;      /* P3.1, phys pin 16 */
/* Pins 11 and 18 — VERIFY their port.bit assignments from the datasheet: */
__sbit __at (0xB6) RELAY_N;      /* P3.6 — VERIFY */
__sbit __at (0xB3) RELAY_W;      /* P3.3 — VERIFY */

#endif /* BOARD_H */
```

If a pin you're trying to use turns out not to be on P3, the `__sbit` syntax doesn't work directly (SDCC's `__sbit` only addresses bit-addressable SFRs, which are P0/P1/P2/P3 and a few others). For pins on P4 or P5, you'll write/read via the port byte and a bitmask. Update `board.h` accordingly once the datasheet is open.

- [ ] **Step 4: Commit**

```bash
git add EcoWorthyFirmware/include/
git commit -m "feat: add minimal STC15 SFR header and board pin map"
```

---

## Task 4: Write Minimal `main.c` and Build-Only Makefile

**Files:**
- Create: `EcoWorthyFirmware/src/main.c`
- Create: `EcoWorthyFirmware/Makefile`

This task produces a buildable HEX file containing nothing but "force all relay pins LOW and idle forever." That gives us the safest possible first flash.

- [ ] **Step 1: Write `src/main.c` (minimal version)**

```c
#include "board.h"

/*
 * Phase 0 — Task 4 minimal version.
 *
 * Single most important safety property: relay-driver pins (which feed the
 * ULN2003A inputs) must be LOW at boot. If they default to quasi-bidirectional
 * weak-high, the relays energize and the actuators slam against their endstops.
 *
 * The very first instructions of main() write zero to every port to guarantee
 * a safe initial state. SDCC's crt0 runs before main() but does not initialize
 * port states; the chip's hardware default is what we override here.
 */
void main(void)
{
    P0 = 0x00;
    P1 = 0x00;
    P2 = 0x00;
    P3 = 0x00;
    P4 = 0x00;
    P5 = 0x00;

    /* Idle forever. */
    while (1) {
        /* Phase 0 Task 4: do nothing. */
    }
}
```

- [ ] **Step 2: Write the build-only Makefile**

Create `EcoWorthyFirmware/Makefile`:

```makefile
# EcoWorthy tracker firmware — Phase 0
#
# Default target: build the Intel HEX file ready for stcgal.
# Override COM_PORT on the command line, e.g. `make flash COM_PORT=COM7`.

# Tooling
SDCC      := sdcc
STCGAL    := stcgal

# Project layout
SRC_DIR   := src
INC_DIR   := include
BUILD_DIR := build
TARGET    := main

# Sources
SOURCES   := $(SRC_DIR)/main.c

# SDCC flags
CFLAGS    := -mmcs51 --model-small --std-c99 -I $(INC_DIR)

# Default goal
.PHONY: all
all: $(BUILD_DIR)/$(TARGET).ihx

$(BUILD_DIR)/$(TARGET).ihx: $(SOURCES) | $(BUILD_DIR)
	$(SDCC) $(CFLAGS) -o $(BUILD_DIR)/ $(SOURCES)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)

.PHONY: flash
flash:
	@echo "flash target will be added in Task 5"
	@exit 1

.PHONY: monitor
monitor:
	@echo "monitor target will be added in Phase 4 (UART bridge)"
	@exit 1
```

- [ ] **Step 3: Build**

From `EcoWorthyFirmware/`:

```bash
make
```

Expected output: SDCC compiles `main.c` and reports memory usage (something like "code size: 12 bytes, ROM: ..., RAM: ..."). No errors.

The build directory now contains `main.ihx` (Intel HEX format).

- [ ] **Step 4: Inspect the HEX file**

```bash
head -5 build/main.ihx
```

Expected: lines starting with `:` followed by hex characters. The first line typically begins `:03 0000 00 ...` representing the program start. If the file is empty or contains errors, the build silently failed — re-run `make` and check stderr.

- [ ] **Step 5: Commit**

```bash
git add EcoWorthyFirmware/Makefile EcoWorthyFirmware/src/main.c
git commit -m "feat: build-only Makefile and minimal relay-safe-boot main.c"
```

---

## Task 5: First Flash with Manual Power-Cycle

**Files:**
- Modify: `EcoWorthyFirmware/Makefile` (add `flash` target)

This is the highest-risk moment of Phase 0 — the first time real code touches the chip. If the relay-safe-boot logic is wrong, the actuators move. Wire everything up, then walk through the flash step-by-step.

- [ ] **Step 1: Wire the PL2303GT to the tracker's programming header**

| PL2303 wire | Tracker pin |
|---|---|
| Black (GND) | Board GND |
| Red (+5 V) | Leave disconnected if the board is independently powered, OR connect to board VCC if powering from USB |
| Green (TXD out from PC) | MCU pin 15 (P3.0, the STC's RxD) |
| White (RXD into PC) | MCU pin 16 (P3.1, the STC's TxD) |

Blue and Yellow are unused for now (these are the unidentified DTR/RTS — irrelevant since we're doing manual power-cycle).

**Double-check before powering on:** that Green is NOT touching VCC or any other rail, and that GND is solidly connected. A reversed TX/RX is harmless (stcgal just won't see the chip); a shorted VCC can damage things.

- [ ] **Step 2: Identify your COM port**

In Windows Device Manager, expand "Ports (COM & LPT)". Plug in the PL2303 — a new "Prolific USB-to-Serial Comm Port (COMx)" entry appears. Note the COM number (e.g., `COM5`).

- [ ] **Step 3: Run stcgal manually (first time, no Makefile yet)**

From `EcoWorthyFirmware/`:

```bash
stcgal -P stc15 -p COM5 --trim 22118 build/main.ihx
```

(Replace `COM5` with your actual COM port.)

Expected: stcgal prints "Waiting for MCU, please cycle power..."

- [ ] **Step 4: Power-cycle the tracker board**

Turn the tracker's power switch off, wait 2 seconds, then back on.

Within ~5 seconds, stcgal should detect the chip and begin programming. Expected output:

```
Target model:
  Name: STC15F2K60S2
  Magic: ...
  Code flash: 60.0 KB
  EEPROM flash: ...
Target frequency: ...
Target BSL version: ...
Loading flash: 12 bytes
Trimming frequency to 22118 kHz...
Switching to new baudrate ...
Erasing 1 blocks: .
Writing 1 blocks: W
Setting options
Disconnected!
```

If stcgal reports "Protocol error" or "Connection timeout," see the troubleshooting list at the bottom of this task.

- [ ] **Step 5: Verify hardware safety**

After flashing completes:

- **Listen** for any relay click. Expected: total silence.
- **Watch** the actuators. Expected: no motion at all.
- **Look** at the LCD. Expected: blank or whatever it shows at random power-up.

If any relay clicks or the actuators move: **power off the board immediately**, disconnect the actuator wires from the relay outputs, and investigate. The most likely cause is that `board.h` has the wrong port.bit for one of the relay pins, so writing `P3 = 0` didn't actually clear that pin.

- [ ] **Step 6: Add `flash` target to the Makefile**

Replace the placeholder `flash` target in `EcoWorthyFirmware/Makefile`:

```makefile
COM_PORT ?= COM5
TRIM_KHZ  ?= 22118

.PHONY: flash
flash: $(BUILD_DIR)/$(TARGET).ihx
	@echo ">>> Power-cycle the tracker board within 5 seconds <<<"
	$(STCGAL) -P stc15 -p $(COM_PORT) --trim $(TRIM_KHZ) $(BUILD_DIR)/$(TARGET).ihx
```

Now `make flash` works as a one-shot command.

- [ ] **Step 7: Test `make flash`**

```bash
make flash COM_PORT=COM5
```

Power-cycle when prompted. Same result as Step 4.

- [ ] **Step 8: Commit**

```bash
git add EcoWorthyFirmware/Makefile
git commit -m "feat: add stcgal flash target with manual power-cycle"
```

**Troubleshooting (Step 4 fallbacks):**

- "Connection timeout" → COM port wrong, or TX/RX swapped, or power-cycle too late. Try again with the actual COM port and cycle faster.
- "Protocol error" → baud rate negotiation failed. Try `stcgal ... -b 9600` (force slow baud).
- "Chip type mismatch" → wrong `-P stc15` value; check the chip marking matches STC15 family.
- LCD shows random pixels after flash → normal at this stage. We initialize it in Task 7.

---

## Task 6: Add Buzzer Beep

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c`

Add a 200 ms beep at the start of `main()`, after the relay-safe-boot. This proves the toolchain end-to-end on a single pin toggle.

- [ ] **Step 1: Calibrate the delay loop count empirically**

At 22.1184 MHz on a 1T 8051, one machine cycle is ~45 ns. A simple decrement-and-branch loop is roughly 5–8 cycles per iteration in SDCC's default optimization, so ~200 iterations ≈ 1 µs. We'll start with that and tune by ear in Step 4.

- [ ] **Step 2: Update `src/main.c` to add the beep**

Replace the body of `src/main.c` with:

```c
#include "board.h"

/*
 * Phase 0 — Task 6: relay-safe-boot + buzzer beep.
 */

/* Approximate microsecond delay. Tuned empirically for 22.1184 MHz / 1T core.
 * Each loop iteration is ~5-8 cycles in SDCC's default optimization, so
 * INNER_LOOPS_PER_US controls the calibration. Adjust if the audible beep
 * pitch is far from 2 kHz.
 */
#define INNER_LOOPS_PER_US  4

static void delay_us(unsigned int us)
{
    unsigned int i;
    while (us--) {
        for (i = 0; i < INNER_LOOPS_PER_US; i++) {
            /* no-op; prevents the compiler from optimizing the loop away when
             * volatile pin writes are not involved
             */
            __asm
              nop
            __endasm;
        }
    }
}

static void delay_ms(unsigned int ms)
{
    while (ms--) {
        delay_us(1000);
    }
}

/* Square-wave drive of the piezo at ~2 kHz for the requested duration. */
static void buzzer_beep(unsigned int duration_ms)
{
    unsigned int half_cycles = duration_ms * 4;  /* 4 half-cycles per ms = 2 kHz */
    while (half_cycles--) {
        BUZZER_PIN = 1;
        delay_us(250);
        BUZZER_PIN = 0;
        delay_us(250);
    }
}

void main(void)
{
    /* Safety: relay-safe boot. */
    P0 = 0x00;
    P1 = 0x00;
    P2 = 0x00;
    P3 = 0x00;
    P4 = 0x00;
    P5 = 0x00;

    /* Let supplies settle before doing anything else. */
    delay_ms(100);

    /* Phase 0 milestone 1: prove the dev loop. */
    buzzer_beep(200);

    /* Idle forever. */
    while (1) {
        /* Phase 0 Task 6: nothing. */
    }
}
```

- [ ] **Step 3: Build and flash**

```bash
make clean && make flash COM_PORT=COM5
```

Power-cycle on prompt.

- [ ] **Step 4: Verify beep**

After flashing completes and the board comes back up:

- **Hear** a brief beep within ~100 ms of power-up.
- The pitch should sound like a typical piezo "blip" — anywhere from 1 kHz to 4 kHz is audible. If it sounds very low and slow ("brrrt") or chirpy and high-pitched, the delay calibration is off — adjust `INNER_LOOPS_PER_US` (smaller = lower pitch, larger = higher pitch) and reflash.
- Still no relay click, no actuator motion.

If no beep at all:

- Verify `BUZZER_PIN` in `board.h` matches the actual physical pin 10 (multimeter beep test from the buzzer's signal lead back to the MCU).
- Verify the piezo is wired between the pin and GND, not pin and VCC.
- Test with an LED + 1 kΩ in parallel with the buzzer to confirm the pin is toggling.

- [ ] **Step 5: Commit**

```bash
git add EcoWorthyFirmware/src/main.c
git commit -m "feat: add buzzer beep to prove dev loop"
```

---

## Task 7: Add LCD "Hello"

**Files:**
- Modify: `EcoWorthyFirmware/src/main.c`

Initialize the 1602A in 8-bit parallel mode and print two lines.

- [ ] **Step 1: Understand the LCD command timing**

The HD44780 controller in the 1602A has these timing requirements (from the datasheet):
- Power-on stabilize: ≥15 ms
- After first `0x30`: ≥4.1 ms
- After second `0x30`: ≥100 µs
- After every subsequent command except clear/home: ≥40 µs
- After Clear Display or Return Home: ≥1.64 ms

E-pulse width: ≥230 ns high. Easily satisfied by any explicit operation between our writes.

- [ ] **Step 2: Replace `src/main.c` with the version including LCD**

```c
#include "board.h"

/* === Timing primitives (same as Task 6) === */

#define INNER_LOOPS_PER_US  4

static void delay_us(unsigned int us)
{
    unsigned int i;
    while (us--) {
        for (i = 0; i < INNER_LOOPS_PER_US; i++) {
            __asm
              nop
            __endasm;
        }
    }
}

static void delay_ms(unsigned int ms)
{
    while (ms--) {
        delay_us(1000);
    }
}

/* === Buzzer (same as Task 6) === */

static void buzzer_beep(unsigned int duration_ms)
{
    unsigned int half_cycles = duration_ms * 4;
    while (half_cycles--) {
        BUZZER_PIN = 1;
        delay_us(250);
        BUZZER_PIN = 0;
        delay_us(250);
    }
}

/* === LCD driver (1602A in 8-bit parallel mode) === */

static void lcd_pulse_e(void)
{
    LCD_E = 1;
    delay_us(1);    /* well above 230 ns minimum */
    LCD_E = 0;
    delay_us(1);
}

static void lcd_write(unsigned char data, unsigned char is_data)
{
    LCD_DATA_PORT = data;
    LCD_RS = is_data;  /* 0 for command, 1 for data */
    LCD_RW = 0;        /* always write */
    lcd_pulse_e();
    delay_us(50);      /* ≥40 µs settle after each write */
}

static void lcd_cmd(unsigned char cmd)
{
    lcd_write(cmd, 0);
}

static void lcd_char(unsigned char c)
{
    lcd_write(c, 1);
}

static void lcd_init(void)
{
    delay_ms(50);          /* power-on stabilize, well above 15 ms minimum */
    lcd_cmd(0x30);
    delay_ms(5);           /* well above 4.1 ms */
    lcd_cmd(0x30);
    delay_us(150);         /* well above 100 µs */
    lcd_cmd(0x30);
    delay_us(50);
    lcd_cmd(0x38);         /* function set: 8-bit, 2-line, 5x8 font */
    delay_us(50);
    lcd_cmd(0x0C);         /* display on, cursor off, blink off */
    delay_us(50);
    lcd_cmd(0x01);         /* clear display */
    delay_ms(2);           /* clear is slow */
    lcd_cmd(0x06);         /* entry mode: increment, no shift */
    delay_us(50);
}

static void lcd_goto(unsigned char row, unsigned char col)
{
    /* DDRAM addresses: row 0 starts at 0x00, row 1 starts at 0x40. */
    unsigned char addr = (row == 0) ? col : (0x40 + col);
    lcd_cmd(0x80 | addr);
    delay_us(50);
}

static void lcd_print(const char *s)
{
    while (*s) {
        lcd_char((unsigned char)*s);
        s++;
    }
}

/* === Application entry === */

void main(void)
{
    /* Safety: relay-safe boot. */
    P0 = 0x00;
    P1 = 0x00;
    P2 = 0x00;
    P3 = 0x00;
    P4 = 0x00;
    P5 = 0x00;

    /* Let supplies settle. */
    delay_ms(100);

    /* Phase 0 milestone 1: prove the dev loop. */
    buzzer_beep(200);

    delay_ms(500);

    /* Phase 0 milestone 2: prove the LCD parallel interface. */
    lcd_init();
    lcd_goto(0, 0);
    lcd_print("EcoWorthy");
    lcd_goto(1, 0);
    lcd_print("Phase 0 OK");

    /* Idle forever. */
    while (1) {
        /* Phase 0 complete. */
    }
}
```

- [ ] **Step 3: Build and flash**

```bash
make clean && make flash COM_PORT=COM5
```

Power-cycle on prompt.

- [ ] **Step 4: Verify the LCD displays correctly**

After flashing and the board comes up:

- 200 ms beep within the first ~100 ms after power-up
- ~500 ms silence
- LCD shows:
  ```
  EcoWorthy
  Phase 0 OK
  ```
- No relay click, no actuator motion

If the LCD shows nothing:

- LCD contrast pot might need adjustment (the small trim resistor next to the LCD usually controls V0).
- Verify `LCD_DATA_PORT`, `LCD_RS`, `LCD_RW`, `LCD_E` all match the actual board wiring in `board.h`.
- Try doubling every delay in `lcd_init()` — some LCD modules are slower than the HD44780 spec.

If the LCD shows garbled characters or only some characters render:

- Likely a data pin is swapped. Verify D0–D7 mapping in `board.h`.
- The `LCD_DATA_PORT` byte must have D0 in bit 0, D1 in bit 1, etc. If the board wires them out of order, you'll need a small lookup function instead of writing the byte directly.

If only the first row appears:

- Likely `lcd_goto(1, 0)` is computing the wrong DDRAM address. Verify the LCD is a 2-line variant (it should be, per the design notes — pin 22 = E line confirms standard 1602A).

- [ ] **Step 5: Reboot multiple times to confirm reliability**

Power-cycle the board 5 times. Each time:

- Beep + LCD text appear identically.
- No relays click.
- Nothing else moves.

Reliability across 5 cycles is the Phase 0 done criterion.

- [ ] **Step 6: Commit**

```bash
git add EcoWorthyFirmware/src/main.c
git commit -m "feat: add LCD init and Hello text — Phase 0 complete"
```

---

## Task 8: Write `EcoWorthyFirmware/README.md`

**Files:**
- Create: `EcoWorthyFirmware/README.md`

Document the workflow so future-you (and the next phase) can rebuild from a fresh checkout.

- [ ] **Step 1: Write the README**

Create `EcoWorthyFirmware/README.md`:

```markdown
# EcoWorthyFirmware

Custom firmware for the Ecoworthy dual-axis solar tracker, targeting an STC15F2K60S2 (SOP28) MCU.

See `../CLAUDE.md` for hardware context and design rationale.
See `../docs/superpowers/specs/` for the design spec of each phase.

## Toolchain

| Tool | Version known to work | Install |
|---|---|---|
| SDCC | 4.4.0+ | https://sourceforge.net/projects/sdcc/ (Windows MSI, "Add to PATH" enabled) |
| Python | 3.11+ | https://python.org (Windows installer, "Add to PATH" enabled) |
| stcgal | 1.x | `pip install stcgal` |
| GNU Make | 4.x | `choco install make` (or via MSYS2 / scoop) |

## Hardware

- Target: STC15F2K60S2 SOP28 on the Ecoworthy tracker board
- Programmer: Prolific PL2303GT USB-TTL cable (sealed form factor)
- Programming pins: PL2303 TXD/Green → MCU pin 15; PL2303 RXD/White → MCU pin 16; GND ← → GND
- Reset: manual power-cycle (auto-reset hardware is a Phase 1 polish task)

## Build

```bash
make            # builds build/main.ihx
make clean      # removes build artifacts
```

## Flash

```bash
make flash COM_PORT=COM5    # builds (if needed) and flashes via PL2303
```

When stcgal prints "Waiting for MCU, please cycle power...", toggle the tracker's power switch off and back on within ~5 seconds.

## Phase 0 success check

After flashing the current code and power-cycling, the board should:

1. Beep once for ~200 ms within the first 100 ms after power-up
2. Display the following on its 1602A LCD ~500 ms later:
   ```
   EcoWorthy
   Phase 0 OK
   ```
3. Not click any relay or move any actuator at any point

## Troubleshooting

- **stcgal "Connection timeout"** — wrong COM port, or TX/RX swapped, or power cycled too slowly.
- **stcgal "Protocol error"** — try forcing slow baud: append `-b 9600` to the stcgal command in the Makefile.
- **No beep on flashed code** — check that `BUZZER_PIN` in `include/board.h` matches the actual pin 10 wiring.
- **Blank LCD** — first try the contrast trim pot. Then verify `LCD_*` pin assignments in `include/board.h`.
- **Garbled LCD characters** — D0–D7 are swapped somewhere; check `LCD_DATA_PORT` against the data-pin wiring.
- **Relay clicks at boot** — STOP. Disconnect actuators. The relay-safe-boot logic is wrong; one of `RELAY_*` pin assignments in `board.h` is on the wrong port.
```

- [ ] **Step 2: Commit**

```bash
git add EcoWorthyFirmware/README.md
git commit -m "docs: add EcoWorthyFirmware README with toolchain and workflow"
```

---

## Task 9: Update `CLAUDE.md`

**Files:**
- Modify: `Solarsystem/CLAUDE.md`

Reflect Phase 0 status so future sessions know where things stand.

- [ ] **Step 1: Update the "Repository status" section**

Find the existing `## Repository status` section in `CLAUDE.md`. Replace its body with:

```markdown
This directory holds:
- `SolarTracker/Ecoworthy Board Description.txt` — reverse-engineered hardware notes (authoritative for wiring)
- `EcoWorthyFirmware/` — custom firmware for the STC15F2K60S2. Phase 0 (toolchain bring-up: build → flash → buzzer + LCD hello) complete. See `EcoWorthyFirmware/README.md` for build/flash workflow.
- `docs/superpowers/` — phase specs and implementation plans
- Reference PDFs (`*.pdf` at root): Growatt SPH inverter and SG48100 battery manuals, unrelated to the tracker

Git: yes. Build/flash: `cd EcoWorthyFirmware && make flash COM_PORT=COMx`.
```

- [ ] **Step 2: Update the "When extending this project" section**

Replace the section's body with:

```markdown
- Read `EcoWorthyFirmware/README.md` for build/flash workflow and known-good toolchain versions.
- Hardware context lives above in this file; verify against `SolarTracker/Ecoworthy Board Description.txt` if anything looks off.
- Each subsequent phase has its own spec under `docs/superpowers/specs/`. Read the spec before writing code for that phase. Next phases: 1 (sensor/input drivers), 2 (relay/actuator + safety), 3 (tracking + state machine), 4 (ESP-01S bridge).
- The bridge electrical design (CLAUDE.md sections above) is locked. Do not redesign without new constraints.
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: update CLAUDE.md to reflect Phase 0 completion"
```

---

## Task 10: Final Verification

**Files:** None.

This is the formal Phase 0 done-check. Run it before declaring victory.

- [ ] **Step 1: Clean rebuild from scratch**

```bash
cd EcoWorthyFirmware
make clean
make
```

Expected: build completes cleanly, `build/main.ihx` exists.

- [ ] **Step 2: Fresh flash**

```bash
make flash COM_PORT=COM5
```

Expected: stcgal completes successfully, board returns to running state.

- [ ] **Step 3: Five-cycle reliability test**

Power-cycle the tracker board five times in a row. For each cycle, confirm:

| Observation | Expected |
|---|---|
| Beep within ~100 ms of power-up | Yes, audible, consistent pitch |
| ~500 ms silence after beep | Yes |
| LCD shows "EcoWorthy" line 0, "Phase 0 OK" line 1 | Yes, both lines, all characters legible |
| Relay click | **No, none** |
| Actuator motion | **No, none** |

If all five cycles meet all five criteria, Phase 0 is complete.

- [ ] **Step 4: Final commit if any tweaks happened during testing**

If you adjusted any calibration constants (e.g., `INNER_LOOPS_PER_US`) during testing, commit those final values:

```bash
git add -A
git status   # review what's about to be committed
git commit -m "tune: Phase 0 calibration finalized"
```

- [ ] **Step 5: Tag the milestone**

```bash
git tag -a phase-0-complete -m "Phase 0: dev loop, build, flash, buzzer, LCD all working"
```

---

## Done

Phase 0 is complete when Task 10 Step 3 passes for all five cycles. At that point:

- The full toolchain (SDCC + stcgal + Make + PL2303GT) works end-to-end.
- The relay-safe-boot pattern is proven on real hardware.
- The LCD is available as a debug output for Phase 1.
- The buzzer is available as an event-feedback channel.
- `board.h` has verified pin-to-port mappings ready for Phase 1's sensor drivers.

Phase 1 (sensor/input drivers) can begin immediately, inheriting all of the above.
