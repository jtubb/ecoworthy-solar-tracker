# Phase 3 — Home Assistant Bridge (read-only status, v1)

## Goal

Expose the tracker's live state to Home Assistant over the single-wire
ESP-01S link on STC pin 17 (P3.2 / INT0). **v1 is read-only**: HA
observes az/el (axis position %), wind, and mode. No command path yet —
`!goto`/`!cmd` handling is deferred to a later phase. This removes the
entire untrusted-input attack surface from v1 and shrinks P3-4 to "emit
a status frame on poll."

The transport design is already finalized in `CLAUDE.md` ("Home
Assistant bridge" section) — this plan does not re-litigate it. It
plans the *implementation*.

## Hard architectural decisions (settled)

- **Timer 1 owns UART bit timing.** Timer 0 stays the 1 kHz `millis()`
  tick (position/duty/storm/cal/debounce all depend on it). Timer 1 is
  otherwise unused.
- **RX is INT0-driven.** P3.2 = INT0. Falling edge of a start bit fires
  the INT0 ISR, which arms Timer 1 to sample subsequent bits mid-bit.
  No polling.
- **P3.2 open-drain, idle HIGH** (breakout supplies the pull-up). TX a
  `0` = drive LOW for one bit time; TX a `1` = release for one bit time.
- **9600 baud** (104.166 µs/bit). Push to 38400 only if proven needed.
- **Half-duplex, ESP is master.** STC only ever transmits in response
  to a poll. The shared wire means the STC hears its own echo — the
  framing prefix handles that.
- **Protocol**: `\xAA\x55` magic prefix, then ASCII `k=v` fields, then
  1-byte CRC8, then `\n`. STC ignores everything received until a valid
  prefix (drops ESP boot spam + self-echo). v1 STC→ESP only; the STC
  still needs minimal RX to recognize the `?` poll.

## Sub-phases

### P3-0 — Hardware gating checks (USER, before code lands on hardware)

These can invalidate the wiring plan; resolve first. None block UART
*development* (P3-1/P3-2 only need a logic analyzer or USB-TTL adapter).

1. **IR receiver on pin 17**: is the vendor IR receiver module still
   populated? Its output actively drives the line and will fight the
   bus. Measure pin 17 idle state with the IR module in place; decide
   desolder vs. leave. (CLAUDE.md open-question 2.)
2. **ESP-01S flash size**: `esptool.py flash_id`. Need ≥ 1 MB for
   ESPHome OTA. < 1 MB works serially but forces enclosure reopening
   for every update. (open-question 3.)
3. **Breakout pull-up resistance**: measure TX/RX pull-ups on the
   actual BSS138 breakout (assumed 10 kΩ each → 5 kΩ effective tied).
   Confirms the achievable baud ceiling. (open-question 4.)

### P3-1 — Software UART TX (Timer 1, 9600, open-drain P3.2)

- Configure P3.2 open-drain, release HIGH (CLAUDE.md "STC firmware
  setup" snippet).
- Timer 1 reload for 104.166 µs at 22.1184 MHz, 1T mode.
- TX state machine: idle → start bit (LOW) → 8 data bits LSB-first →
  stop bit (release) → idle. Driven by Timer 1 ISR.
- A `uart_tx_byte()` / `uart_tx_str()` that enqueues and blocks-or-
  returns (decide: small ring buffer vs. busy a frame at a time).
- **Validate**: scope or USB-TTL RX confirms clean 9600 bytes.

### P3-2 — Software UART RX (INT0 start bit + Timer 1 sampling)

- IT0=1 (falling edge), EX0=1. INT0 ISR: on start-bit edge, disable
  INT0, arm Timer 1 to fire at 1.5 bit times (mid first data bit),
  then every 1 bit time, sampling 8 bits, then re-enable INT0.
- RX ring buffer. Half-duplex interlock: ignore RX while TX active
  (we'll hear our own echo otherwise).
- **Validate**: external UART → STC, byte echoed back via P3-1 TX.

### P3-3 — Framing + CRC8

- CRC8 (poly 0x07 or Dallas/Maxim 0x31 — pick one, document it; ESP
  side must match).
- TX: build `\xAA\x55 <fields> <crc8> \n`.
- RX: state machine that scans for `\xAA\x55`, accumulates to `\n`,
  verifies CRC8, exposes the payload. Anything before a valid prefix
  is discarded (kills boot spam + self-echo).

### P3-4 — STC status responder (read-only)

- Minimal RX: recognize a bare `?` poll line (with prefix+CRC).
- On poll: emit `\xAA\x55 az=<ew%> el=<ns%> wind=<mps> mode=<m> <crc>\n`.
  - `el` = NS position % (tilt), `az` = EW position % (rotate) — or
    pick the mapping that matches how the panel is physically mounted;
    document it.
  - `mode` = one of idle/track/storm/jog/cal.
- Wire into the 50 ms loop **without disturbing it**: the UART is
  fully ISR-driven; the main loop only enqueues the response string
  when the RX layer reports a complete valid poll. No blocking.
- **Validate**: USB-TTL sends `?` frames, STC replies with correct
  live values as you jog/track.

### P3-5 — ESPHome YAML (ESP-01S)

- `EcoWorthyFirmware/esphome/` (per CLAUDE.md "When extending").
- Custom UART text-sensor / component: poll `?` every ~2 s, parse the
  framed reply, expose `az`/`el`/`wind`/`mode` as HA entities.
- `logger: baud_rate: 0` (hardware UART dedicated to the STC link).
- WiFi + OTA; discard non-`\xAA\x55` RX (self-echo).

### P3-6 — Hardware integration + tag

- 470 µF + 100 nF across breakout 5V VIN (WiFi TX current spikes).
- Antenna placement away from relay coils / metal.
- End-to-end: HA dashboard shows live tracker state.
- Tag `phase-3-complete`.

## Risks / watch-items

- **Soft-UART layered on an existing timer ISR is the riskiest code in
  the project.** Prototype P3-1/P3-2 in isolation before integrating.
- **`delay_us()` is ~2-3× slow** (README debug note) — irrelevant for
  Timer-1-paced UART, but do not use busy-wait for bit timing.
- **Half-duplex echo** is the subtle one: every byte the STC sends, it
  also receives. The framing-prefix discard is mandatory, not optional.
- **Stack**: `--stack-auto` is in effect. Two ISRs (Timer 0, Timer 1)
  + INT0 + the main call graph all share the ~223-byte internal stack.
  Keep ISRs shallow; watch for corruption on the deepest path (cal)
  with UART active.

## Out of scope for v1

- HA → STC commands (`!goto`, remote calibrate). Separate later phase;
  adds the untrusted-input surface we're deliberately deferring.
- 38400 baud (only if 9600 proves insufficient).

## Deferred to Phase 4 — multi-tracker mesh

Field deployment will have multiple trackers, some out of base-WiFi
range but in range of each other. A designated "primary" with a wind
sensor should be able to broadcast storm/wind state to secondaries
(which can then omit their own wind sensors). Sketched out here so
P3-5's choices don't accidentally close off the path:

- **Transport**: ESP-NOW broadcast (point-to-multipoint, no WiFi
  association needed on receivers). Lighter than painless-mesh and a
  natural fit for one-way primary→secondaries storm signaling on
  ESP-01S's constrained flash/RAM.
- **Protocol additions**: extend the `\xAA\x55` framing with a
  command path (the framing already supports it; v1 STC only parses
  bare `?`). New STC-accepted commands: at minimum a "set remote
  wind = N" / "force storm-enter" / "force storm-release" trio,
  authenticated/scoped so only the configured primary's broadcasts
  are honored.
- **STC firmware additions**: an "external wind override" input that
  storm_check uses in place of (or fused with) the local sensor when
  configured. Settings entry: tracker role (primary/secondary), and
  on secondaries, the primary's identifier.
- **Failover**: secondary behavior if the primary stops broadcasting
  for N minutes (fall back to local wind if equipped, or fail-safe to
  storm-park if not).
- **Security**: a misconfigured or malicious neighbor must not be
  able to false-trigger storm across the array. At minimum the
  command path should require a per-array pre-shared key on top of
  CRC.

None of this affects v1 implementation, but the bridge component is
designed so the same UART/framing layer accepts inbound commands when
Phase 4 builds them.
