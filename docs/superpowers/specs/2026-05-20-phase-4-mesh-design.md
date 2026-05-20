# Phase 4 — Multi-tracker ESP-NOW Mesh (Design)

## Goal

Extend the single-tracker Phase 3 bridge into a **field-deployable
mesh** of up to 10 trackers where:

- Some trackers are out of base-WiFi range but in radio range of
  each other.
- All trackers report telemetry to Home Assistant (out-of-WiFi nodes
  via in-range "gateway" peers).
- HA can send commands (including movement) to any tracker.
- One designated "wind primary" tracker drives storm decisions for
  the whole array, so secondaries don't need wind sensors.
- Config values (thresholds, calibration data, role) are visible in
  HA; user-tunable fields are writable from HA.

After Phase 4, the operator can add or remove trackers, tune storm
thresholds from a HA dashboard, and trust that a storm event
propagates across the array even when most trackers can't reach
the base-station AP.

## Locked design decisions

| # | Decision | Choice |
|---|---|---|
| 1 | Scale | 3 trackers now, designed for up to 10 |
| 2 | WiFi reality | Some trackers out of WiFi range; bidirectional mesh required |
| 3 | Gateway topology | All trackers gateway-capable; runtime depends on WiFi reachability |
| 4 | Multi-gateway dedup | Lowest-MAC election with periodic heartbeat, automatic failover |
| 5 | Wind-loss failsafe | Fail-safe to storm-park after 3–4 missed broadcasts (~15–20 s) |
| 6 | HA command surface | Full bidirectional incl. movement (`force_park`, `force_release`, `goto`, `jog`, `stop`) plus calibration trigger |
| 7 | Security | AES-128-CCM + monotonic counter, per-array PSK |
| 8 | Config | Get-anywhere, Set-only-on-tunable-fields (cal data is read-only over mesh) |

## High-level architecture

```
                ┌─────────────────── Home Assistant ───────────────────┐
                │                                                       │
                ▲ WiFi (ESPHome API)        ▲ WiFi (ESPHome API)        │
                │                            │                          │
        ┌───────┴────────┐          ┌────────┴───────┐                  │
        │  Tracker A     │          │  Tracker B     │                  │
        │  (acting       │   ESP-NOW│  (gateway-     │                  │
        │   gateway)     │◀────────▶│   capable)     │                  │
        └────────┬───────┘  broadcast└────────┬──────┘                  │
                 ▲                            ▲                         │
                 │                            │                         │
                 │ ESP-NOW broadcast          │                         │
                 ▼                            ▼                         │
        ┌─────────────────┐          ┌─────────────────┐                │
        │  Tracker C      │          │  Tracker D      │                │
        │  (remote, no    │          │  (wind primary, │                │
        │   WiFi today)   │          │   has sensor)   │                │
        └─────────────────┘          └─────────────────┘                │
                                                                        │
        All trackers single-hop reachable on same WiFi channel          │
        ───────────────────────────────────────────────────────────────┘
```

### Three orthogonal roles

| Role | Designation | Behavior |
|---|---|---|
| **Wind primary** | Static, set in STC Settings (`role: primary`). Exactly one per array. | Reads local wind sensor, broadcasts `WIND` every 5 s. Drives its own storm logic from local sensor. |
| **Acting gateway** | Dynamic: lowest-MAC among currently-WiFi-up gateway-capable trackers. | Publishes the entire array's telemetry to HA. Forwards HA commands into the mesh. |
| **Remote** | Default state of every tracker. | Listens for `WIND`, runs local `storm_check` against broadcast value. Emits own `TELEMETRY`. Consumes `COMMAND` broadcasts. |

A tracker can simultaneously be wind primary AND acting gateway.
Lose WiFi → keep being primary, stop being gateway.
Primary tracker dies → next-MAC gateway, but no wind primary at all
until operator reconfigures (failsafe park triggers everywhere).

## Wire protocol

### Frame

All ESP-NOW broadcasts use a fixed header + variable payload + auth tag:

```
┌─────────┬──────────┬─────────┬──────────────┬─────────┐
│ type 1B │ src 6B   │ ctr 4B  │ payload var  │ tag 8B  │
├─────────┼──────────┼─────────┼──────────────┼─────────┤
│ plaintext header (1+6+4=11B) │ encrypted    │ AES-CCM │
└──────────────────────────────┴──────────────┴─────────┘
```

- **`type`**: see message table below
- **`src`**: sender's 6-byte MAC
- **`ctr`**: 32-bit monotonic counter per sender (anti-replay)
- **`payload`**: encrypted with **AES-128-CCM**, key = array PSK,
  nonce = `src || ctr || 00 00` (15 bytes total)
- **`tag`**: 8-byte CCM authentication tag

Max packet = 11 + payload + 8 = well under ESP-NOW's 250-byte limit
for all defined message types.

### Message types

| Code | Name | Sender | Cadence | Payload |
|---|---|---|---|---|
| 1 | `WIND` | wind primary | every 5 s | `wind_mps:u8`, `flags:u8` (bit 0 = primary's local storm-active) |
| 2 | `TELEMETRY` | every tracker | every 5 s | `az_pct:u8`, `el_pct:u8`, `wind_used:u8`, `mode:u8` |
| 3 | `GATEWAY_HB` | every WiFi-up tracker | every 5 s | `wifi_rssi:i8` |
| 4 | `COMMAND` | acting gateway | on demand | `target_mac:6B`, `cmd:u8`, `arg1:u8`, `arg2:u8` |
| 5 | `CONFIG` | any | on demand | `op:u8`, `field_id:u8`, `value:0..4B` |

### Command codes

| Code | Command | Args |
|---|---|---|
| 1 | `force_park` | (none) |
| 2 | `force_release` | (none) |
| 3 | `goto` | `arg1` = az %, `arg2` = el % |
| 4 | `jog` | `arg1`: bit 7 = axis (0=NS, 1=EW), bit 0 = direction (0=−, 1=+); `arg2` = duration in 100 ms units (1–25) |
| 5 | `stop` | (none) — emergency stop both axes |
| 6 | `calibrate_trigger` | (none) — STC accepts only if not in storm; full cal sequence runs |

Target = `FF·6` means broadcast (all trackers act).

### Config protocol

`CONFIG` op codes:
| Op | Meaning |
|---|---|
| 1 | GET request (response is op=2) |
| 2 | GET response |
| 3 | SET request |
| 4 | SET ack (status byte: 0=ok, 1=read-only, 2=out-of-range, 3=unknown-field) |

Field IDs:

| ID | Field | Type | Access |
|---|---|---|---|
| `0x01` | `wind_storm_mps` | u8 | RW |
| `0x02` | `wind_release_mps` | u8 | RW (clamped < storm on save) |
| `0x03` | `storm_dwell_min` | u8 | RW |
| `0x04` | `track_thresh` | u8 | RW |
| `0x10` | `ns_stroke_ms` | u16 | R only |
| `0x11` | `ew_stroke_ms` | u16 | R only |
| `0x12` | `horiz_ns_pct` | u8 | R only |
| `0x13` | `horiz_ew_pct` | u8 | R only |
| `0x20` | `role` (1=primary, 2=secondary) | u8 | R only |
| `0x21` | `wind_source` (0=local, 1=remote) | u8 | R only |

Read-only enforcement is STC-side (rejects with NACK ack) AND
ESP-side (won't issue SET commands for RO fields). Defense in depth.

### Counter / nonce management

- Each sender holds its own 32-bit monotonic counter.
- Persisted in ESPHome Preferences flash. To minimize wear, the
  in-RAM counter is flushed every 100 ticks; on boot, restore the
  last-persisted value + 100 (safety jump) to guarantee the
  next-used value exceeds any previously broadcast value.
- Each receiver tracks `(src_mac → last_accepted_ctr)`. Reject if
  `ctr ≤ last_accepted`.
- 32-bit counter at 1 send/sec ≈ 136 years before wrap — no
  wraparound logic needed.

### Channel handling

ESP-NOW requires all peers on the same WiFi channel. Operator
responsibilities:

- Configure the home AP to a fixed channel (not auto). All
  gateway-capable trackers will follow.
- Remote-only trackers (no WiFi association) set
  `wifi.channel: N` matching the AP's channel in their ESPHome YAML.

Mismatch symptoms: trackers see each other when both are
WiFi-connected to the same AP, but lose contact when one is
remote-only. Always check the channel first when commissioning a
remote-only node.

## STC firmware additions

### New Settings menu entries

| Entry | Type | Default | Persistence |
|---|---|---|---|
| `Role` | enum {primary, secondary} | secondary | EEPROM |
| `Wind src` | enum {local, remote} | local on primary, remote on secondary | EEPROM |

The primary should always have `Wind src = local`; secondaries should
have `Wind src = remote`. The defaults are coupled to `Role` for
convenience but separately settable.

### New EEPROM bytes

Extend the config struct (current bytes 0..11 used; add 12..13):

```
12 : role          (u8: 1=primary, 2=secondary)
13 : wind_source   (u8: 0=local, 1=remote)
```

`config_load` adds range-check + sensible defaults (role=secondary,
wind_source=local). `config_save` writes them.

### Storm logic extension

`storm_check()` uses one of two wind sources based on
`wind_source`:

- **local**: existing path — read `ADC_CH_WIND`, cache, compare.
- **remote**: use a new `remote_wind_mps_cached` variable, written
  by `uart_service` when it parses a CONFIG/`WIND` event from the
  ESP. If `now - remote_wind_last_update_ms > REMOTE_WIND_TIMEOUT_MS`,
  force storm-park (the Q5 failsafe).

`REMOTE_WIND_TIMEOUT_MS = 20000` (4 missed 5-s broadcasts).

A primary that loses ESP communication is detected by the local UART
RX timing out (no responses from the ESP) — same failsafe trigger
from the STC side via a separate watchdog.

### Command protocol over local UART

The existing `\xAA\x55` + payload + 2-hex CRC + `\n` framing already
supports inbound non-`?` payloads. New payloads the STC accepts:

| Payload prefix | Meaning |
|---|---|
| `?` | (existing) status request |
| `!park` | force_park |
| `!release` | force_release |
| `!goto az=NN el=NN` | go to position |
| `!jog ax=N dir=+/- ms=NNN` | pulse axis |
| `!stop` | emergency stop |
| `!cal` | trigger calibration |
| `!cfg get id=NN` | config get; STC replies with framed `cfg id=NN val=...` |
| `!cfg set id=NN val=...` | config set; STC replies with `cfg ack id=NN status=N` |
| `!wind=NN` | remote wind update (broadcast from ESP after receiving WIND from primary) |

ASCII command parsing on the STC: extend the existing
`uart_frame_feed` → `uart_service` dispatcher with a switch over the
first character (`?` → status, `!` → command). Sub-parse the `!`
commands by simple token matching (limited surface; not a full
parser). Reject unknown / malformed commands silently (don't reply).

### Safety bounds (STC-side, defense in depth)

- `goto` clamps az/el % to [0,100].
- `goto` respects duty interlock (won't start motion if locked).
- `goto` aborts immediately if storm state becomes active mid-motion.
- `jog` clamps duration to 1–25 (100ms–2.5s).
- `jog`/`goto` are no-ops while in storm.
- `force_release` doesn't release storm if broadcast wind is still
  ≥ release threshold (HA can't override a real wind condition).
- `calibrate_trigger` only accepted when state == ST_IDLE and not in
  storm. While calibrating, the STC is unresponsive to mesh commands
  by design (blocking).

## ESP-side additions (tracker_bridge component)

Expand the existing `tracker_bridge` external_component:

### New responsibilities

1. **ESP-NOW init** at startup: register the broadcast peer, set the
   AES-128 key from `secrets.yaml`, install RX callback.
2. **Outbound message scheduling**:
   - WIND (if local STC reports `role=primary`): every 5 s, after
     polling the STC's wind via existing UART protocol.
   - TELEMETRY: every 5 s, derived from the existing `?` poll
     response.
   - GATEWAY_HB: every 5 s, only when WiFi is associated.
3. **Inbound message dispatch**:
   - WIND → forward to STC via `!wind=NN` UART command, update local
     freshness timestamp.
   - TELEMETRY → if this ESP is the acting gateway, publish to HA
     under `tracker_<srcmac_short>_az/el/wind/mode` entities.
   - GATEWAY_HB → update peer table; recompute acting-gateway-MAC.
   - COMMAND → if target is broadcast or our MAC, translate to UART
     `!cmd` and forward to STC. (Acting gateways do not forward
     non-broadcast commands not targeted to them — they were merely
     the broadcasting source.)
   - CONFIG → translate to `!cfg get/set` UART command; on STC
     response, broadcast `CONFIG` response over ESP-NOW.
4. **Gateway election**:
   - Track `(mac → last_heartbeat_ms)` for all gateway-capable
     peers (anyone who's sent a GATEWAY_HB within 15 s).
   - Acting gateway = lowest MAC in that set, including ourselves
     if WiFi is up.
   - Only the acting gateway publishes others' TELEMETRY to HA;
     others stay silent on the HA side for that data.
   - On gateway change, briefly announce role change via debug log;
     no special protocol.

### Configuration in YAML

New keys on the bridge:

```yaml
tracker_bridge:
  id: bridge
  uart_id: uart_bus
  mesh:
    enabled: true
    channel: 6        # must match the AP and all peers
    psk: !secret tracker_mesh_psk    # 16-byte AES key
    tracker_id: "T1" # short label used in HA entity names
```

`secrets.yaml` (per-array, identical on every tracker):

```yaml
tracker_mesh_psk: "your-array-16-byte-key-here"
```

### New HA entities

Per remote tracker the gateway is bridging:

| Entity | Type | Source |
|---|---|---|
| `tracker_<id>_az` | sensor (%) | TELEMETRY |
| `tracker_<id>_el` | sensor (%) | TELEMETRY |
| `tracker_<id>_wind` | sensor (m/s) | TELEMETRY |
| `tracker_<id>_mode` | text_sensor | TELEMETRY |
| `tracker_<id>_wind_storm_mps` | number (RW) | CONFIG |
| `tracker_<id>_wind_release_mps` | number (RW) | CONFIG |
| `tracker_<id>_storm_dwell_min` | number (RW) | CONFIG |
| `tracker_<id>_track_thresh` | number (RW) | CONFIG |
| `tracker_<id>_ns_stroke` | sensor (s, RO) | CONFIG |
| `tracker_<id>_ew_stroke` | sensor (s, RO) | CONFIG |
| `tracker_<id>_horiz_ns_pct` | sensor (%, RO) | CONFIG |
| `tracker_<id>_horiz_ew_pct` | sensor (%, RO) | CONFIG |
| `tracker_<id>_role` | text_sensor (RO) | CONFIG |

Plus array-wide service buttons on the acting gateway:

| Entity | Action |
|---|---|
| `array_force_park` | button: broadcast `force_park` to all |
| `array_force_release` | button: broadcast `force_release` to all |

Per-tracker control buttons:
| Entity | Action |
|---|---|
| `tracker_<id>_goto_az` | number with set_action sending `goto` |
| `tracker_<id>_goto_el` | number with set_action sending `goto` |
| `tracker_<id>_jog_n/s/e/w` | button: jog 500 ms in that direction |
| `tracker_<id>_calibrate` | button (confirmation in HA): `calibrate_trigger` |

## Failure model

| Failure | Detection | Behavior |
|---|---|---|
| Wind primary tracker offline | Each tracker's local "last WIND seen" timestamp ages past `REMOTE_WIND_TIMEOUT_MS` (20 s) | All trackers force storm-park. Cleared when WIND broadcasts resume. |
| Acting gateway loses WiFi | Other trackers stop seeing its GATEWAY_HB after 15 s | Next-lowest MAC takes over publishing. HA sees brief gap. |
| All gateways offline | No GATEWAY_HB at all | No HA visibility but storm logic still runs (relies only on WIND broadcasts, not gateways). |
| Single tracker's ESP-01S crashes | No TELEMETRY from that MAC | HA entity for that tracker stops updating (ESPHome's natural unavailable behavior). STC continues operating autonomously (still does storm_check on remote_wind cached value, hits failsafe after timeout). |
| Counter desync (e.g., one tracker reflashed, counter reset) | Counter ≤ last_accepted → packets rejected | **v1 recovery: power-cycle all trackers** (simplest; counters and `last_accepted` tables all reset). Phase 5 polish item: add a per-tracker "reset counters" admin button on the acting gateway that broadcasts a signed `RESET_CTR` admin command. |
| Channel mismatch on remote-only tracker | Tracker never appears on the mesh | Operator must check AP channel matches tracker YAML channel. Document prominently. |
| AES key mismatch between trackers | Packets decrypt-fail and are silently dropped | Diagnostic log shows "AES auth fail src=XX:..."; operator checks `secrets.yaml` consistency. |

## Out of scope (later phases)

- **HA → STC bulk config updates via YAML** (push a whole settings
  profile). Would be useful for multi-tracker arrays; deferred.
- **Per-tracker timezone-aware schedules** (e.g., auto-park at
  sunset). Better implemented as HA automations sending COMMAND.
- **Cross-channel ESP-NOW** (different WiFi channels). Painlessmesh
  territory, very heavy on ESP-01S. Stay single-channel.
- **OTA upgrade via ESP-NOW for remote-only nodes**. Remote trackers
  must be reflashed via serial or by temporarily bringing them
  in WiFi range. Documented limitation.
- **Multi-array federation** (multiple arrays on different PSKs).
  Possible but adds operator complexity; not needed for v1.

## Open implementation questions (for the plan phase)

1. **AES-CCM library**: use the Arduino `Crypto` library by rweather
   (well-supported, header-only-friendly, has `AES_CCM` directly).
   Already compatible with ESPHome's Arduino framework on ESP8266.
   Flash budget verification deferred to first build; if tight, fall
   back to a hand-rolled `aes-128` + minimal CCM (only the bits we
   use) at ~2 KB.
2. **Counter persistence policy**: 100-tick flush interval is a
   starting estimate. May need tuning based on observed flash wear.
3. **STC `!cfg` command parser implementation**: the existing
   single-character `?` dispatch is trivial; extending to a small
   `!cmd k=v` parser needs careful bounds-checking. Reuse the
   existing token-append helpers.
4. **HA entity discovery and naming**: how the gateway dynamically
   creates entities for remote trackers as they appear (vs.
   pre-declared in YAML). ESPHome doesn't natively support
   discovery — likely solution is a configured-in-YAML list of
   expected tracker IDs and the gateway maps incoming MACs to that
   list.
