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

## What the firmware does (Phases 0–4)

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
regardless of mode. A custom LCD glyph (CGRAM 0 — padlock) overlays
the bottom-right cell whenever an axis is duty-locked; cleared at
boot, forced after calibration (motor thermal protection).

### Phase 3: half-duplex single-wire HA bridge

Pin 17 (P3.2, INT0) hosts a bit-banged half-duplex 9600 8N1 link to
an ESP-01S running ESPHome. Timer 1 paces TX bits and samples
INT0-triggered RX mid-bit (no polling). Wire framing is
`\xAA\x55 <ASCII payload> <2-hex CRC8> \n` with CRC-8/SMBUS over the
payload bytes. The ESP polls `?` every 2 s; the STC replies with
`az=NN el=NN wind=NN mode=XXX`. The ESP-side custom component
(`tracker_bridge`, at `<repo>/esphome/components/`) parses the same
framing and publishes the four fields to Home Assistant. Reference
YAMLs at `esphome/solar-tracker-{1,2}.yaml`. Phase 4 extends the
bridge with bidirectional commands (HA → STC) and the ESP-NOW mesh
(see below).

### Phase 4: multi-tracker ESP-NOW mesh

Multiple `solar-tracker-N` nodes form a peer-to-peer mesh over ESP-NOW on
a shared channel (`mesh.channel:` in the YAML, must equal the WiFi AP
channel). All traffic is encrypted with AES-128-GCM; the key is derived as
SHA-256(`tracker_mesh_psk`) truncated to 16 bytes. Every node must share
the same PSK.

**Roles** — set via `local_role:` in the `mesh:` sub-block:

- `primary` — owns the physical wind sensor. Broadcasts WIND packets every
  5 s. Secondary nodes that have no local wind sensor use this reading.
- `secondary` (default) — receives WIND from the primary and forwards
  `!wind=NN` to its local STC over the UART bridge.

**Message types** broadcast every 5 s unless noted:

| Code | Name | Sender | Payload |
|------|------|--------|---------|
| 1 | WIND | primary only | `wind_mps(1)` `flags(1)` (bit 0 = storm) |
| 2 | TELEMETRY | every node | `az_pct(1)` `el_pct(1)` `wind_used(1)` `mode(1)` |
| 3 | GATEWAY_HB | every WiFi-up node | `rssi(int8)` |
| 4 | COMMAND | acting gateway | `target_mac(6)` `cmd(1)` `arg1(1)` `arg2(1)` |
| 5 | CONFIG | acting gateway / peer | `op(1)` `field_id(1)` `[val(1+)]` |

**Packet format** — 45-byte AAD-covered header followed by AES-128-GCM
ciphertext + 8-byte tag:

```
header:  type(1) src_mac(6) epoch(2) ctr(4) node_name(32)
nonce:   src_mac(6) || epoch(2) || ctr(4)   [12 bytes, per RFC 5116]
```

The `epoch` is a 16-bit counter persisted to flash and incremented on every
boot. The replay filter accepts a packet only when `(epoch, ctr)` is
strictly greater than the last accepted `(epoch, ctr)` for that
`node_name`.

**Acting gateway** — the lowest-MAC node among declared `peers:` that has
sent a GATEWAY_HB within the last 15 s. Only the acting gateway calls into
HA to publish per-peer telemetry, preventing duplicate entity updates.

**Per-peer HA entities** — each peer listed in `peers:` gets its own
`sensor:`, `text_sensor:`, and `number:` blocks keyed by
`peer_id: <esphome.name>`. The acting gateway populates them from received
TELEMETRY packets. RW config sliders with a `peer_id:` send a CONFIG
SET_REQ over ESP-NOW to the target node.

**Command surface** (exposed as ESPHome `button:` entities, broadcast to
all trackers):

| Button | Effect on STC |
|--------|---------------|
| `force_park` | Sets `storm_forced`; parks to horizontal and holds |
| `force_release` | Clears `storm_forced`; returns to tracking after dwell |
| `stop` | Halts motion immediately |
| `calibrate` | Triggers a full stall-to-stall calibration run |

`calibrate` accepts an optional `peer_id` to unicast to one tracker; omit `peer_id` to
broadcast to all trackers.  `force_park`, `force_release`, and `stop` are broadcast-only.

#### HA goto / jog actions via lambda

`send_goto(target, az, el)` and `send_jog(target, ax_dir, dur_100ms)` take binary
arguments that don't map cleanly to a button entity (they need numeric parameters).
Use a `script:` block and a `lambda:` action to call them from HA automations:

```yaml
script:
  # Move a specific tracker to the horizontal (parked) position by percentage.
  - id: t2_goto_horizontal
    then:
      - lambda: |-
          auto *b = id(bridge);
          // Replace with the actual MAC from tracker-2's ESPHome log on first boot.
          uint8_t target[6] = {0xEC, 0xFA, 0xBC, 0x00, 0x00, 0x02};
          b->send_goto(target, 50, 50);   // 50% az, 50% el = horizontal

  # Broadcast goto to every tracker (all bytes 0xFF).
  - id: all_goto_horizontal
    then:
      - lambda: |-
          auto *b = id(bridge);
          uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
          b->send_goto(bcast, 50, 50);

  # Jog tracker-2's N/S axis toward North for 2 s (20 × 100 ms).
  # ax_dir encoding: bit 7 = axis (0=N/S, 1=E/W), bit 0 = dir (1=+, 0=−).
  - id: t2_jog_ns_north_2s
    then:
      - lambda: |-
          auto *b = id(bridge);
          uint8_t target[6] = {0xEC, 0xFA, 0xBC, 0x00, 0x00, 0x02};
          b->send_jog(target, 0x01, 20);  // axis=0 (N/S), dir=+ (North), 2 s
```

`az` and `el` are 0–100 (percentage of calibrated stroke).  `ax_dir` packs axis and
direction into one byte: bits 7..1 = axis (0 = N/S, 1 = E/W), bit 0 = direction
(1 = extend/+, 0 = retract/−).  `dur_100ms` is duration in units of 100 ms (1–255).

**YAML schema** — `mesh:` sub-block under `tracker_bridge:`:

```yaml
tracker_bridge:
  mesh:
    channel: 6          # integer 1-13; must match the WiFi AP channel
    psk: !secret tracker_mesh_psk   # string >= 16 chars
    local_role: primary             # "primary" or "secondary" (default)
    test_broadcast: false           # set true to emit type=99 every 5s for bench validation
    peers:
      - solar-tracker-2             # list of esphome.name of known mesh peers
```

**Storm semantics on the STC** — two independent flags:

- `storm_forced` — operator-asserted via `!park` command; sticky until
  `!release`. Set/cleared by the acting gateway's `force_park` /
  `force_release` button presses propagated over ESP-NOW COMMAND packets.
- `wind_failsafe` — auto-managed: set when `wind_source=1` (remote) and no
  WIND broadcast has arrived for 20 s; cleared when fresh broadcasts resume.

**ESPHome dashboard vs. repo YAMLs** — the YAMLs in `EcoWorthyFirmware/esphome/`
are the reference copy maintained in the repo. The operational YAMLs live
inside the ESPHome dashboard's docker container. Sync repo → dashboard
manually after edits, or bind-mount the repo directory as the dashboard's
config directory to keep them in sync automatically.

## Debugging notes

- The SDCC busy-wait `delay_us(N)` is currently ~2-3× slower than naive math
  suggests (~3 µs per "us" at 22.1184 MHz). Timings in the LCD init are
  generous enough to absorb this. When real-time accuracy is needed
  (software UART), switch to Timer 0 / PCA hardware timing.
- See `../CLAUDE.md` "Firmware gotchas" for the STC15 port-mode SFR
  ordering trap that bit Phase 0.
