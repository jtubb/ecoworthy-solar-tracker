# Phase 5 — Deferred Cleanups + Mesh Hardening

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement
> the picked tasks one at a time, with two-stage review per task. Steps within each task use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Address items deferred from Phase 4 ship. Each task is independently shippable — pick any
subset to execute. No hard ordering except where noted.

**Architecture:** Same as Phase 4 — STC firmware + ESPHome external component (`tracker_bridge`) +
ESP-NOW mesh between ESP-01S nodes. No protocol breaks expected.

**Tech Stack:** SDCC + Make + stcgal (STC), ESPHome external component on ESP8266 Arduino +
rweather/Crypto + AES-128-GCM (ESP).

---

## Task selection matrix

| # | Task | Value | Effort | Risk | Notes |
|---|---|---|---|---|---|
| P5-1 | Unicast CONFIG SET targeting by tracker_name | High | Low | Low | Closes the "every peer applies every SET" hole |
| P5-2 | CONFIG GET_RESP read-back of slider state | High | Med | Low | Sliders show actual STC values, not optimistic |
| P5-3 | MULTI_CONF = False on tracker_bridge component | Low | Trivial | Low | Single-line correctness fix for singleton design |
| P5-4 | MSG_WIND self-guard for consistency | Low | Trivial | Low | Cosmetic alignment with TELEMETRY/HB |
| P5-5 | `peers_` LRU pruning / size cap | Med | Low | Low | Bounds memory if rogue peers join |
| P5-6 | Per-peer COMMAND buttons (goto, jog, calibrate) | Med | Med | Low | Exposes existing C++ methods to HA UI |
| P5-7 | STC role sync from EEPROM → ESP YAML | Med | Med | Med | Eliminates `local_role:` YAML duplication |
| P5-9 | Wind override slider gated by `local_role` | Low | Trivial | Low | Hides bench-helper from STC-equipped nodes |
| P5-10 | Schema validate `esphome.name <= 31 chars` at codegen | Low | Low | Low | Catch the truncation case before runtime warn |
| P5-11 | Stagger cfg_poll burst to avoid half-duplex collision | Med | Trivial | Low | One of four cfg reads is dropped each 30 s cycle |
| P5-12 | YAML migration when tracker-2 gets its own STC | Med | Low | Low | Drop tracker-1's peer mirrors for clean entity_ids |
| P5-13 | Bench harness per-test state isolation | Med | Low | Low | test_force_park_release / test_gateway_failover flaky in suite |
| P5-14 | expect_log default `since` cursor from last action | Med | Trivial | Low | Tight 100 ms back-window causes race against trigger actions |
| P5-15 | Add gateway-role LOGD; rework test_gateway_failover | Med | Low | Low | P5-12 removed the peer-publish log the test relied on |

(P5-8 — ESPHome dashboard bind-mount workflow doc — was dropped from scope.)

---

## Task P5-1: Unicast CONFIG SET targeting by tracker_name

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h`

**Why:** Currently MSG_CONFIG SET_REQ is broadcast and `target_matches_us_(src)` returns `true`
unconditionally. Every peer self-applies every SET. Functionally correct on a primary-bench setup
(only one STC) but wrong on a multi-tracker install — moving "T2 Wind Storm" on HA should only
affect T2.

**Approach:** Extend MSG_CONFIG payload from `op(1) field(1) val(1)` to
`op(1) target_name(32) field(1) val(1)`. Receiver compares target_name against its own `wire_id_`
and ignores SET if no match. GET_REQ stays broadcast-and-everyone-responds for v1 (P5-2 refines).

Wire payload becomes 35 bytes for SET_REQ; AAD still covers only the 45-byte header (payload is in
the ciphertext). Frame total: 45 + 35 + 8 = 88 bytes, well under 250-byte limit.

- [ ] **Step 1: Update MSG_CONFIG payload schema in tracker_bridge.h dispatcher**

```cpp
case MSG_CONFIG: {
  if (plen < 1) return;
  uint8_t op = p[0];
  if (op == CFG_OP_SET_REQ) {
    if (plen < 1 + 32 + 1 + 1) return;
    /* Reject if target doesn't match our wire_id_ */
    if (memcmp(p + 1, wire_id_, 32) != 0) break;
    uint8_t field_id = p[33];
    uint8_t value = p[34];
    char buf[32];
    int sn = snprintf(buf, sizeof(buf), "!cfg set id=%u val=%u", field_id, value);
    if (sn <= 0 || (size_t) sn >= sizeof(buf)) break;
    write_str_frame_(buf);
  } else if (op == CFG_OP_GET_REQ) {
    /* Keep v1 broadcast-and-respond; P5-2 will scope this. */
    if (plen < 2) return;
    uint8_t field_id = p[1];
    char buf[20];
    int sn = snprintf(buf, sizeof(buf), "!cfg get id=%u", field_id);
    if (sn <= 0 || (size_t) sn >= sizeof(buf)) break;
    write_str_frame_(buf);
  }
  /* GET_RESP / SET_ACK: P5-2 */
  break;
}
```

- [ ] **Step 2: Update outbound `config_set_for_peer` to include target_name**

```cpp
void config_set_for_peer(const std::string &peer_id, uint8_t field_id, uint8_t value) {
  if (peer_id.empty()) {
    /* Local path unchanged */
    ...
  } else {
    /* Build target name key from peer_id (32 bytes NUL-padded) */
    auto target = make_name_key_(peer_id);
    uint8_t p[1 + 32 + 1 + 1];
    p[0] = CFG_OP_SET_REQ;
    memcpy(p + 1, target.data(), 32);
    p[33] = field_id;
    p[34] = value;
    mesh_tx_(MSG_CONFIG, p, sizeof(p));
    ESP_LOGD(TAG, "config_set peer='%s' fid=%u val=%u", peer_id.c_str(), field_id, value);
  }
}
```

- [ ] **Step 3: Delete `target_matches_us_` stub**

It's now dead code — the comparison happens inline in the dispatcher.

- [ ] **Step 4: Bench-validate**

On the bench: move "T2 Wind Storm" on tracker-1's HA. Expected: tracker-2 receives the MSG_CONFIG,
target matches its `wire_id_`, applies SET. Other nodes (if any) ignore. Verify via the existing
runtime LOGD line.

- [ ] **Step 5: Commit**

```
git commit -m "feat(esp): P5-1 -- unicast CONFIG SET targeting by tracker_name"
```

---

## Task P5-2: CONFIG GET_RESP read-back of slider state

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h`
- Modify: `EcoWorthyFirmware/src/main.c` (STC `!cfg get` response → framed `cfg id=N val=V`)

**Why:** Currently config sliders are write-only. The slider position reflects the operator's
last input, not the actual STC value. After a power cycle on a remote tracker, its slider on HA
shows whatever HA cached, not the truth.

**Approach:** STC firmware already supports `!cfg get id=N` (P4-6). Wire up the ESP to:
- (a) On boot and every N seconds, the acting gateway broadcasts CONFIG GET_REQ for each field
  for each declared peer.
- (b) STC responds to `!cfg get` via UART with `cfg id=N val=V` framed reply.
- (c) ESP forwarder on a non-target peer ignores GET_REQ that doesn't target it.
- (d) The targeted peer's ESP parses STC reply, wraps as MSG_CONFIG GET_RESP, broadcasts to mesh.
- (e) Receivers parse GET_RESP, look up the right `ConfigNumber*` in `peer_decls_[target].wind_storm`
  etc., call `publish_state(value)`.

This requires the GET_REQ to also include the target_name field (so non-target peers can ignore).

- [ ] **Step 1: Extend MSG_CONFIG GET_REQ payload with target_name**

```cpp
struct CfgGetReq { uint8_t op; uint8_t target[32]; uint8_t field_id; };  // 34 bytes
```

Same `op = CFG_OP_GET_REQ = 1` byte, then 32-byte target name, then field id.
Dispatcher rejects on mismatched target.

- [ ] **Step 2: GET_RESP payload**

```cpp
struct CfgGetResp { uint8_t op; uint8_t src_name[32]; uint8_t field_id; uint8_t value; };  // 35 bytes
```

Note `src_name` not `target` — the responder identifies itself. Receivers route by `src_name` to
the right `peer_decls_[src_name].wind_storm` etc.

- [ ] **Step 3: STC UART parser handles `cfg id=N val=V` response**

The STC firmware's `uart_cfg_send_value()` (in main.c) already emits `cfg id=N val=V` framed
replies. ESP's `handle_payload_` needs to add a `cfg ` prefix branch alongside the existing
`az=` branch. Parse `id=` and `val=`, then build outbound MSG_CONFIG GET_RESP frame, broadcast.

- [ ] **Step 4: GET_RESP dispatch on receiver**

```cpp
case MSG_CONFIG: {
  ...
  } else if (op == CFG_OP_GET_RESP) {
    if (plen < 1 + 32 + 1 + 1) return;
    auto src_key = make_name_key_(std::string((const char *) (p + 1), 32));
    auto it = peer_decls_.find(src_key);
    if (it == peer_decls_.end()) return;
    uint8_t field_id = p[33];
    uint8_t value = p[34];
    number::Number *target = nullptr;
    switch (field_id) {
      case CFG_F_WIND_STORM:   target = it->second.wind_storm; break;
      case CFG_F_WIND_RELEASE: target = it->second.wind_release; break;
      case CFG_F_STORM_DWELL:  target = it->second.storm_dwell; break;
      case CFG_F_TRACK_THRESH: target = it->second.track_thresh; break;
    }
    if (target) target->publish_state(float(value));
  }
  ...
}
```

This requires `peer_decls_[name].wind_storm` etc. to actually be wired from `number.py`'s
`to_code`. They are NOT currently — Task P4-11 left the `number::Number*` pointers as
unconnected. P5-2 must finish that wiring.

- [ ] **Step 5: Wire `peer_decls_[name].wind_storm` from `number.py` `to_code`**

When a `peer_id` is set on a `ConfigNumber`, add a `set_*_number_for(peer_id, n)` setter on
`TrackerBridge` that finds the peer in `peer_decls_` and stores the pointer.

- [ ] **Step 6: Acting-gateway periodic GET_REQ broadcast**

`set_interval("cfg_poll", 30000, ...)` in `mesh_setup_` — every 30s the acting gateway iterates
declared peers and broadcasts a GET_REQ for each (field × peer = N*4 broadcasts). 30s is slow
enough that bandwidth cost is negligible; fast enough that HA shows current values within a minute
of any external change.

- [ ] **Step 7: Bench-validate**

Move "T2 Wind Storm" slider to 17. Wait 30s. Reset HA's cached value. Slider should re-populate
to 17 via the GET cycle. Move it to 22, wait, restart HA. Should still show 22.

- [ ] **Step 8: Commit**

```
git commit -m "feat(esp,stc): P5-2 -- CONFIG GET_RESP read-back of remote slider state"
```

---

## Task P5-3: `MULTI_CONF = False` on tracker_bridge component

**Files:**
- Modify: `esphome/components/tracker_bridge/__init__.py`

**Why:** Code review for P4-7 flagged that `MULTI_CONF = True` allows multiple `tracker_bridge:`
blocks in one YAML, but the C++ uses a `static TrackerBridge *instance_` singleton for ESP-NOW
callback dispatch. Two instances would silently lose mesh frames for whichever didn't grab the
singleton last. There's no scenario where two `tracker_bridge` instances make sense on one ESP
(one STC, one HA endpoint per device), so demote.

- [ ] **Step 1: Change `MULTI_CONF = True` to `MULTI_CONF = False` in `__init__.py`**
- [ ] **Step 2: Verify compile** — no other code changes needed.
- [ ] **Step 3: Commit**

```
git commit -m "fix(esp): P5-3 -- MULTI_CONF=False to match singleton design"
```

---

## Task P5-4: MSG_WIND self-guard for consistency

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h`

**Why:** P4-12's code review noted that MSG_TELEMETRY and MSG_GATEWAY_HB cases self-guard via
`memcmp(name_key, wire_id_, 32)` but MSG_WIND doesn't. On the primary node, this means the primary
receives its own WIND broadcast and dispatches `!wind=N` to its OWN STC, which then has remote_wind
overridden by its own broadcast. Currently harmless on a node with `wind_source=0` and harmless on
a primary with `wind_source=1` (the value is the primary's own reading), but inconsistent.

- [ ] **Step 1: Add `if (memcmp(name_key.data(), wire_id_, 32) == 0) break;` at top of MSG_WIND case**
- [ ] **Step 2: Commit**

```
git commit -m "fix(esp): P5-4 -- MSG_WIND self-guard for dispatcher consistency"
```

---

## Task P5-5: `peers_` LRU pruning / size cap

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h`

**Why:** P4-9 / P4-11 reviews flagged that `peers_` (and by extension `peer_decls_`,
`peer_last_session_`) can accumulate entries from any node that broadcasts a valid frame. In a
fleet of 3-10 trackers, fine. If a stale or test broadcast joins, entries leak. On a long-running
device with constrained heap (ESP-01S has ~50KB free), this is a slow-leak risk.

**Approach:** Periodic prune pass alongside the existing 5s mesh_broadcasts timer. Drop
`peers_` entries whose `last_telemetry_ms` AND `last_gateway_hb_ms` are both older than
`PEER_STALE_MS = 60000` (12 missed broadcasts). Don't touch `peer_decls_` (operator-declared,
static) or `peer_last_session_` (replay protection needs full history).

Actually `peer_last_session_` should ALSO be pruned, but with a much longer timeout (say 1h)
since the cost of accepting a replay is small after that long anyway.

- [ ] **Step 1: Add `PEER_STALE_MS = 60000` constant**
- [ ] **Step 2: Add `peers_prune_()` method that iterates `peers_` and erases stale entries**
- [ ] **Step 3: Call `peers_prune_()` once per cycle inside the `mesh_broadcasts` set_interval lambda**
- [ ] **Step 4: Log peer count at prune time at LOGD level**
- [ ] **Step 5: Commit**

```
git commit -m "fix(esp): P5-5 -- prune stale peers from peers_ map"
```

---

## Task P5-6: Per-peer COMMAND buttons

**Files:**
- Modify: `esphome/components/tracker_bridge/button.py`
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h`

**Why:** P4-10 added the C++ methods `send_goto`, `send_jog`, `send_calibrate` but `button.py`
only exposes broadcast buttons (`force_park`, `force_release`, `stop`). Targeted operations
(park a specific tracker, calibrate a specific tracker) aren't reachable from HA.

**Approach:** Extend button.py with optional `peer_id` field on a per-tracker `calibrate` button.
For `goto`, the action needs az/el values — a button doesn't fit; better as a `script` or `action`
in HA YAML. Defer goto and jog to operator-defined YAML actions (which can call any C++ method
via `lambda`); ship calibrate per-peer as a button.

- [ ] **Step 1: Add `calibrate` button class with `set_peer_id` setter and `press_action` calling
      `send_calibrate(find_peer_mac_(peer_id_))` or `mesh_tx_command_` broadcast if empty**
- [ ] **Step 2: button.py schema accepts `calibrate:` with optional `peer_id`**
- [ ] **Step 3: Document goto/jog as lambda actions in the README (no button entity for v1)**
- [ ] **Step 4: Commit**

```
git commit -m "feat(esp): P5-6 -- per-peer calibrate button + lambda action docs for goto/jog"
```

---

## Task P5-7: STC role sync from EEPROM → ESP YAML

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h`
- Modify: `esphome/components/tracker_bridge/__init__.py`

**Why:** The STC stores `role` (primary/secondary) in EEPROM byte 12, settable via Settings menu.
The ESP YAML has its own `mesh.local_role:` field. These can drift — operator changes the STC's
role but forgets to update YAML. Single source of truth would be cleaner.

**Approach:** On boot, ESP queries `!cfg get id=32` (CFG_F_ROLE) via UART. Cache the reply.
Use the cached value as `local_role_`. Remove `mesh.local_role:` from YAML schema (or keep
as a fallback for no-STC nodes like tracker-2).

This is a hostable improvement but it does require careful handling for nodes without STCs:
the bench-helper tracker-2 has no STC, so the `!cfg get` will time out. Fall back to YAML if
no STC reply within 5s.

- [ ] **Step 1: Add `role_pending_query_` flag + retry timer in `setup()`**
- [ ] **Step 2: STC reply parser already handles `cfg id=N val=V` (after P5-2) — extend to update
      `local_role_` when `id == CFG_F_ROLE`**
- [ ] **Step 3: Schema: change `local_role:` from required-ish (with default secondary) to optional
      override only when no STC present**
- [ ] **Step 4: Bench-validate role pulled correctly from STC, falls back to YAML on tracker-2**
- [ ] **Step 5: Commit**

```
git commit -m "feat(esp): P5-7 -- STC role authoritatively pulled via !cfg get on boot"
```

Note: this task depends on P5-2 (the `cfg id=N val=V` STC-reply parser).

---

## Task P5-9: Wind override slider gated by `local_role`

**Files:**
- Modify: `esphome/components/tracker_bridge/number.py` (or tracker_bridge.h)

**Why:** The `wind_override` bench helper is only useful on no-STC nodes. On a real STC-equipped
node, moving the slider does nothing because the next STC poll (2s later) overwrites the
cached value. Currently the schema doesn't enforce this — operator could add the slider
anywhere and be confused why it doesn't work.

**Approach:** Either:
- (a) Codegen-time check: if `wind_override:` is configured AND the YAML doesn't declare this
  as a no-STC node, raise a `cv.Invalid` warning.
- (b) Runtime: `WindOverrideNumber::control()` warns when called on an STC-equipped node.
- (c) Just better docs.

(c) is cheapest. (b) is most operator-friendly. (a) needs a way to identify "this is a no-STC
node" which is implicit, not explicit.

- [ ] **Step 1: Pick (b): in `WindOverrideNumber::control`, log a WARN if the local STC has
      responded to a status poll within the last 30s**
- [ ] **Step 2: Commit**

```
git commit -m "chore(esp): P5-9 -- warn when wind_override is moved on STC-equipped node"
```

---

## Task P5-10: Schema validate `esphome.name <= 31 chars` at codegen

**Files:**
- Modify: `esphome/components/tracker_bridge/__init__.py`

**Why:** Current schema validates `peers:` entries (`_validate_peer_name` rejects > 31 chars) but
NOT the local `esphome.name` field. If an operator names a device `north-roof-tracker-1-array-A`
(28 chars, fits) and renames it `north-roof-tracker-1-array-A-storm` (33 chars), it truncates at
runtime with a WARN. Should reject at codegen so the issue surfaces before flash.

**Approach:** Schema validators can access top-level config via `CORE.config[CONF_ESPHOME]`.
Check `esphome.name` length in our `to_code` (not in CONFIG_SCHEMA validation since that's per-key).
Raise `cv.Invalid` with a clear message if > 31 chars.

- [ ] **Step 1: Add length check at start of `to_code` when `CONF_MESH in config`**
- [ ] **Step 2: Commit**

```
git commit -m "fix(esp): P5-10 -- reject esphome.name > 31 chars when mesh: is configured"
```

---

## Self-review

- All tasks are independent — pick any subset.
- P5-7 depends on P5-2 (STC `cfg id=N val=V` parser).
- P5-1, P5-2, P5-4, P5-5 share the dispatcher code surface — economical to bundle reviews.
- P5-3, P5-9, P5-10 are independent cleanups.
- P5-6 (per-peer buttons) is a UX feature; not blocking anything else.

**Execution order (locked):** numerical order P5-1, P5-2, P5-3, P5-4, P5-5, P5-6, P5-7, P5-9, P5-10.
P5-8 dropped from scope.

---

## Task P6-1: Auto-elect wind primary (drop role declaration)

**Goal:** Replace YAML-declared `local_role: primary|secondary` with a `has_wind_sensor:` capability bit advertised in TELEMETRY broadcasts. Lowest-MAC peer with the bit set is the active wind broadcaster. Failover is automatic.

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h`
- Modify: `esphome/components/tracker_bridge/__init__.py`
- Modify: `EcoWorthyFirmware/esphome/solar-tracker-1.yaml` (uses sensor)
- Modify: `EcoWorthyFirmware/esphome/solar-tracker-2.yaml` (no sensor)

**Wire-format change:**
TELEMETRY payload grows from 4 to 5 bytes: `az_pct(1) el_pct(1) wind_used(1) mode(1) flags(1)`.
Bit 0 of `flags` = `has_wind_sensor`. Bits 1-7 reserved for future.
Receivers update `PeerEntry.has_wind_sensor` from the bit on every TELEMETRY rx.

**Election logic** (mirrors gateway election):
```cpp
bool am_i_wind_primary_() {
  if (!has_wind_sensor_) return false;
  uint8_t my_mac[6];
  WiFi.macAddress(my_mac);
  for (const auto &kv : peers_) {
    if (!kv.second.has_wind_sensor) continue;
    if ((millis() - kv.second.last_telemetry_ms) > PEER_STALE_MS) continue;
    if (kv.second.mac < my_mac) return false;  // lexicographic, lowest wins
  }
  return true;
}
```

**Behavior changes:**
- `mesh_tx_wind_()` is gated by `am_i_wind_primary_()` instead of `local_role_ == ROLE_PRIMARY`.
- MSG_WIND dispatcher self-guard branches on `am_i_wind_primary_()` instead of role.
- `local_role_` member, `ROLE_PRIMARY`/`ROLE_SECONDARY` constants, and `set_local_role()` setter all deleted.
- `__init__.py` schema: `local_role:` removed, `has_wind_sensor:` added (cv.Optional default False).
- YAML `solar-tracker-1.yaml` (has STC + sensor): `has_wind_sensor: true`.
- YAML `solar-tracker-2.yaml` (no STC, no sensor): omit or `has_wind_sensor: false`.

**Out of scope for P6-1:**
- STC's `wind_source` EEPROM byte stays operator-controlled via LCD. Document that secondaries should have wind_source=1 to receive the auto-elected broadcasts. A future P6-2 could auto-sync STC `wind_source` based on `am_i_wind_primary_()` via `!cfg set id=33`.
- Backward compat: clean break. TELEMETRY plen growing from 4 to 5 means old-firmware frames fail the new `plen < 5` guard and are dropped. Pre-release; acceptable.

- [ ] **Step 1: Update wire schema** — add the 5th `flags` byte to `mesh_tx_telemetry_()`. Construct `flags = has_wind_sensor_ ? 0x01 : 0x00`.

- [ ] **Step 2: Receive-side parsing** — in MSG_TELEMETRY dispatcher, `plen < 5` guard. Read `peers_[name_key].has_wind_sensor = (p[4] & 0x01) != 0;`.

- [ ] **Step 3: Add `am_i_wind_primary_()` method** alongside `is_acting_gateway_()`.

- [ ] **Step 4: Gate `mesh_tx_wind_()` in the 5s broadcast lambda** — `if (am_i_wind_primary_()) mesh_tx_wind_();`.

- [ ] **Step 5: Update MSG_WIND self-guard** — replace `local_role_ == ROLE_PRIMARY` check with `am_i_wind_primary_()` check. Still drop self-echo via name_key compare.

- [ ] **Step 6: Delete `local_role_`, `ROLE_PRIMARY`, `ROLE_SECONDARY`, `set_local_role()`** — and remove the `local_role:` schema entry in `__init__.py`.

- [ ] **Step 7: Add `has_wind_sensor_` member + `set_has_wind_sensor()` setter** wired from `__init__.py`'s new schema field.

- [ ] **Step 8: Update YAMLs**:
  - `solar-tracker-1.yaml`: `local_role: secondary` → `has_wind_sensor: true` (it has the STC + real sensor).
  - `solar-tracker-2.yaml`: `local_role: primary` → omit `has_wind_sensor:` (or set false; it has no sensor — was a bench helper that *pretended* to be primary).

- [ ] **Step 9: Bench-validate** — flash both, confirm tracker-1 elects itself as primary (broadcasts WIND), tracker-2 doesn't broadcast WIND but its `wind_override` slider still works for the bench-helper case. Power off tracker-1: tracker-2 stays silent (no auto-promotion since it has no sensor). Power tracker-1 back on: WIND resumes within 5s.

- [ ] **Step 10: Commit**: `feat(esp): P6-1 -- auto-elect wind primary via has_wind_sensor capability bit`

---

## Task P5-11: Stagger cfg_poll burst to avoid half-duplex collision

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h` (the `cfg_poll` set_interval lambda, ~line 706)

**Why:** The 30 s `cfg_poll` lambda fires four `!cfg get id=N` frames back-to-back into the UART TX
buffer with zero spacing (the `for (uint8_t fid : {1,2,3,4})` loop at ~line 727). On a half-duplex
single-wire bus the STC starts replying after parsing the first complete frame in its RX, while the
ESP is still TXing frames 3/4. The collision corrupts one of the four replies (most often id=3 in
bench logs from 2026-05-22), so one slider's HA state never updates from STC truth on that cycle. A
later cycle usually recovers it, but the slider can be stale for up to 30 s after a value change
made via the LCD.

**Approach:** Replace the tight for-loop with four `set_timeout` calls offset by 150 ms each. 150 ms
is roughly 7× the per-frame airtime at 9600 baud (~22 ms per 21-byte frame including AA 55 prefix +
CRC + newline), leaving ample headroom for the STC's reply to land cleanly before the next request
goes out. The `cfg_poll` cadence stays at 30 s; only the *internal* burst becomes spread across
~600 ms.

**Diagnostic anchor (keep until task ships):** boot log from 2026-05-22 showing the symptom is in
the conversation history — `cmd->STC: !cfg get id=1..4` all sent at 18:15:28.290–.398, but only
`cfg id=1 val=17`, `cfg id=2 val=10`, `cfg id=4 val=3` come back. id=3 (storm_dwell) reply is
absent, and the id=4 reply arrives 111 ms after id=2 — characteristic of the wire clearing after a
collision.

- [ ] **Step 1: Write a failing test** that asserts all four cfg sliders update within 5 s of boot.
  Use `tools/bench_test.py` harness. The test should subscribe to the four `number` entities on
  `solar-tracker-1` (`*_wind_storm`, `*_wind_release`, `*_storm_dwell`, `*_track_thresh`), restart
  the device via the `Restart` switch, then assert all four publish a non-NaN state within 5 s. Run
  it and confirm it fails consistently (id=3 / storm_dwell is the usual victim, but it may rotate).

- [ ] **Step 2: Replace the burst with staggered set_timeout calls.** In
  `esphome/components/tracker_bridge/tracker_bridge.h` ~line 727, change:

  ```cpp
  for (uint8_t fid : {(uint8_t)1, (uint8_t)2, (uint8_t)3, (uint8_t)4}) {
    char buf[20];
    int sn = snprintf(buf, sizeof(buf), "!cfg get id=%u", (unsigned)fid);
    if (sn > 0 && (size_t)sn < sizeof(buf)) this->write_str_frame_(buf);
  }
  ```

  to:

  ```cpp
  for (uint8_t i = 0; i < 4; i++) {
    uint8_t fid = uint8_t(i + 1);
    this->set_timeout(uint32_t(i) * 150u, [this, fid]() {
      char buf[20];
      int sn = snprintf(buf, sizeof(buf), "!cfg get id=%u", (unsigned)fid);
      if (sn > 0 && (size_t)sn < sizeof(buf)) this->write_str_frame_(buf);
    });
  }
  ```

- [ ] **Step 3: Re-run the test from Step 1** and confirm it passes. All four sliders should
  publish within ~600 ms of the cfg_poll tick firing.

- [ ] **Step 4: Bench-test 30 s cycle stability.** Watch the boot log for two full cfg_poll cycles
  (~60 s of runtime). Expect to see four `handle_payload_ p='cfg id=N val=V'` lines per cycle, with
  no missing IDs across multiple consecutive cycles.

- [ ] **Step 5: Commit**: `fix(esp): P5-11 -- stagger cfg_poll burst by 150 ms to avoid half-duplex collision`

---

## Task P5-12: YAML migration when tracker-2 gets its own STC

**Files:**
- Modify: `EcoWorthyFirmware/esphome/solar-tracker-1.yaml`
- (Optional) Modify: `EcoWorthyFirmware/esphome/solar-tracker-2.yaml`

**Status before this task:** Tracker-2 has no STC, so its local STC entity blocks
(already declared in `solar-tracker-2.yaml` lines 114-121 and 138-141) stay empty.
Tracker-1 publishes mirror entities for tracker-2 (via `peer_id: solar-tracker-2`
blocks) when it is the acting gateway -- this is the only way HA sees tracker-2's
state today.  The mirror entity_id is awkward: `sensor.solar_tracker_1_solar_tracker_2_azimuth_pct`
(both device-slug and entity-name-slug prefix the same fact).

**Trigger:** Run this task only after physically connecting an STC15F2K60S2 board
to solar-tracker-2 and flashing it with the standard tracker firmware (same
`EcoWorthyFirmware/` build that tracker-1 runs).  Verify the new STC responds to
`!cfg get id=1` via the local UART on tracker-2 before touching the YAML.

**Why:** With both nodes locally connected to STCs, tracker-2's native entity
blocks become populated and the peer mirrors on tracker-1 become redundant.  HA
gets one clean entity per fact, owned by the device that owns the hardware.

**Trade-off documented for the future-you:** the peer mirrors were originally
intended as cross-node failover view -- if tracker-1's API briefly drops while
mesh is still up, tracker-2's mirror (when it was acting gateway) gave HA a
backup window into tracker-1's state.  Dropping the mirrors removes that
secondary view.  In practice WiFi/API outages are bursty enough that the mesh
itself goes silent when the API does, so the failover view rarely covers a
real outage; the entity_id ugliness is the bigger ongoing cost.  If you want
to keep the failover view, leave the mirrors in and just rename their
entity_ids via the HA UI (the friendly names are already correct).

- [ ] **Step 1: Verify tracker-2's STC is alive.** Run the bench harness on
  `solar-tracker-2`:
  ```
  python tools/bench_test.py --test test_cfg_get_resp_readback
  ```
  Adjust the test temporarily to target tracker-2 (or just check the boot log
  for `handle_payload_ p='cfg id=1 val=N'` lines on the tracker-2 ESPHome log
  stream).  All four sliders should populate within ~30 s.  If they don't,
  STOP and debug the new STC's UART wiring before touching any YAML.

- [ ] **Step 2: Drop the peer_id blocks from solar-tracker-1.yaml.** Remove
  the following three blocks (line numbers approximate):

  Sensor block (~lines 112-120):
  ```yaml
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    peer_id: "solar-tracker-2"
    az:   { name: "Solar Tracker 2 Azimuth %" }
    el:   { name: "Solar Tracker 2 Elevation %" }
    wind: { name: "Solar Tracker 2 Wind" }
  ```

  Text_sensor block (~lines 134-138):
  ```yaml
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    peer_id: "solar-tracker-2"
    mode: { name: "Solar Tracker 2 Mode" }
  ```

  Number slider block (~lines 179-201):
  ```yaml
  - platform: tracker_bridge
    tracker_bridge_id: bridge
    peer_id: "solar-tracker-2"
    wind_storm_mps:   { name: "Solar Tracker 2 Wind Storm", ... }
    wind_release_mps: { name: "Solar Tracker 2 Wind Release", ... }
    storm_dwell_min:  { name: "Solar Tracker 2 Storm Dwell", ... }
    track_thresh:     { name: "Solar Tracker 2 Track Threshold", ... }
  ```

- [ ] **Step 3: Decide on the peer mirrors in solar-tracker-2.yaml.**
  Tracker-2 has equivalent mirrors of tracker-1 (lines 125-130 and 142-145).
  By symmetry, drop them too unless you want the failover-view trade-off
  described above.  Default recommendation: drop.

- [ ] **Step 4: Add the missing per-node config sliders to solar-tracker-2.yaml.**
  Tracker-2 currently has NO local `wind_storm_mps`/`wind_release_mps`/`storm_dwell_min`/
  `track_thresh` number blocks (no local STC meant no sliders to wire).  Add them
  now, mirroring solar-tracker-1.yaml's local-STC number block (~lines 154-177):
  ```yaml
  number:
    - platform: tracker_bridge
      tracker_bridge_id: bridge
      wind_storm_mps:
        name: "$friendly_name Wind Storm"
        min_value: 5
        max_value: 30
        step: 1
      wind_release_mps:
        name: "$friendly_name Wind Release"
        min_value: 0
        max_value: 20
        step: 1
      storm_dwell_min:
        name: "$friendly_name Storm Dwell"
        min_value: 1
        max_value: 60
        step: 1
      track_thresh:
        name: "$friendly_name Track Threshold"
        min_value: 1
        max_value: 99
        step: 1
  ```
  Keep the `wind_override:` bench-helper number block too if it's still useful;
  otherwise drop it (a real STC now provides real wind via the sensor, so the
  helper is mostly redundant on this node).

- [ ] **Step 5: Add the missing operator buttons to solar-tracker-2.yaml.**
  Mirror solar-tracker-1.yaml's button block (~lines 146-151):
  ```yaml
  button:
    - platform: tracker_bridge
      tracker_bridge_id: bridge
      force_park:    { name: "$friendly_name Force Park" }
      force_release: { name: "$friendly_name Force Release" }
      stop:          { name: "$friendly_name Emergency Stop" }
  ```

- [ ] **Step 6: Flip tracker-2's has_wind_sensor flag if appropriate.**
  Only if the new STC has a real wind sensor physically attached: edit
  `solar-tracker-2.yaml` line 101 `has_wind_sensor: false` → `true`.  The
  election will then race lowest-MAC between t1 and t2.  Leave as `false`
  if t2's STC has no sensor and just receives WIND broadcasts from t1.

- [ ] **Step 7: Flash both nodes.** Build via the dashboard (or
  `esphome run EcoWorthyFirmware/esphome/solar-tracker-{1,2}.yaml`).  After
  reflash, both devices should appear as independent ESPHome devices in HA
  with clean entity_ids:
  - `sensor.solar_tracker_1_azimuth_pct`
  - `sensor.solar_tracker_2_azimuth_pct`
  - matching for el / wind / mode / wind_storm / wind_release / storm_dwell / track_thresh

- [ ] **Step 8: Clean up stale HA entities.** Settings -> Devices & Services
  -> ESPHome -> solar-tracker-1 -> remove the now-orphaned
  `sensor.solar_tracker_1_solar_tracker_2_*` entities if HA didn't auto-cull
  them.

- [ ] **Step 9: Re-run the bench harness.** All 5 automated tests should
  still pass.  test_no_primary_baseline and test_dual_primary_election_failover
  may now be exercisable as automated tests instead of manual-gated ones,
  depending on Step 6's decision -- update their gates if applicable.

- [ ] **Step 10: Commit**: `feat(yaml): P5-12 -- migrate to per-device entities now that tracker-2 has STC`

---

## Task P5-13: Bench harness per-test state isolation

**Files:**
- Modify: `tools/bench_test.py`

**Why:** Across three back-to-back full-suite runs on 2026-05-22, results varied
3p/3f/2s, 3p/2f/2s, 5p/0f/2s.  The variance came entirely from
`test_force_park_release` and `test_gateway_failover`.  Both run after siblings
that may leave tracker state non-default (e.g. `storm_dwell_min` slider
moved to test value 1, tracker stuck in storm mode if `force_release` racing
the dwell timer, `test_offline` switch left on if a finally block didn't
finish cleanly).  When the next test starts, its "wait for mode = storm" or
"wait for mode = track" assertion either matches immediately (false-positive)
or races a state machine that's already in the wrong phase (timeout).

Both tests pass deterministically when run individually -- isolated context is
not the same as suite context.

**Approach:** Add a `reset_to_baseline_(harness, *node_names)` helper that
runs at the START of each test's body (before any try block).  It should:

1. For each node: toggle `Test Offline` OFF (via `set_switch`) and wait
   for confirmation that mesh broadcasts have resumed (`tx TELEMETRY` log
   line appears within 6 s).
2. For each node: press `Force Release` to clear any sticky storm state,
   then wait for mode != "storm" (timeout: ~max_dwell + margin).
3. For each node: restore the four config sliders to canonical baseline
   values read from `bench_test_config.yaml` (e.g. `baseline_wind_storm=15`,
   `baseline_storm_dwell=10`).  This is essential because tests like
   `test_force_park_release` and `test_ha_command_propagation` temporarily
   set `storm_dwell_min = 1` and rely on a `finally` to restore -- a
   finally that didn't run leaves it at 1 for the next test.
4. Log a single `[INFO] reset_to_baseline_ ok` line so suite output stays
   tidy.

The helper takes the longest of any per-test setup but only runs once per
test, so the suite walltime grows by ~5-10 s per test (still well under the
3-minute total).

- [ ] **Step 1: Write a failing test** that demonstrates the leakage.
  Run the suite three times back-to-back via `python tools/bench_test.py --all`.
  Confirm at least one run produces a different pass/fail mix than the others
  (record the seed/timing in a comment, since this is inherently flaky).

- [ ] **Step 2: Add the `reset_to_baseline_` helper** to `tools/bench_test.py`
  in the `TestHarness` class.  Take `*node_names` as varargs so tests can
  pass `t1, t2` or just `t1`.

- [ ] **Step 3: Add baseline values to bench_test_config.yaml.example.**
  Suggested keys (with defaults):
  ```yaml
  baseline_wind_storm_mps: 15
  baseline_wind_release_mps: 10
  baseline_storm_dwell_min: 10
  baseline_track_thresh: 3
  ```

- [ ] **Step 4: Call `reset_to_baseline_` at the start of every test body.**
  This includes the currently-passing tests too -- defensive resetting
  shouldn't hurt their pass/fail outcomes, and it makes the suite robust
  against any future test that forgets to clean up.

- [ ] **Step 5: Re-run the suite three times** back-to-back.  Expect
  identical 5-pass-2-skip results on all three runs.

- [ ] **Step 6: Commit**: `test: P5-13 -- per-test state isolation via reset_to_baseline_`

---

## Task P5-14: expect_log default `since` cursor from last action

**Files:**
- Modify: `tools/bench_test.py`

**Why:** Currently `TestHarness.expect_log(name, pattern, timeout=10)` defaults
to `since = time.monotonic() - 0.1` -- it only looks back 100 ms.  This breaks
the very common pattern:

```python
await h.press_button(t1, "force_park")              # logs hit t2 ~5 ms later
await h.expect_entity(t1, "mode", "storm", t=15)    # waits up to 15 s
await h.expect_log(t2, r"mesh rx type=4", t=15)     # back-window = 100 ms -- TOO LATE
```

`test_force_park_release` and `test_ha_command_propagation` already work
around this by manually capturing `since_action = time.monotonic()` before
the trigger and passing `since=since_action` explicitly.  But every new test
has to remember this footgun, and forgetting it produces non-obvious
race-condition failures (the line IS in the buffer, just timestamped earlier
than the cursor).

**Approach:** Add a `_last_action_ts` field on `TestHarness`, updated by every
action method (`press_button`, `set_switch`, `set_number`, `reboot`,
`send_uart_cmd` if it stays).  When `expect_log` is called with `since=None`,
default to `self._last_action_ts` (and fall back to `now - 0.1` if no action
has been recorded yet).

- [ ] **Step 1: Add `_last_action_ts: float = 0.0` field** to `TestHarness.__init__`.

- [ ] **Step 2: Update every action method** to set
  `self._last_action_ts = time.monotonic()` as its first statement.

- [ ] **Step 3: Modify `expect_log`** so the `if since is None` branch
  uses `self._last_action_ts or (time.monotonic() - 0.1)`.

- [ ] **Step 4: Strip the manual `since_action = time.monotonic()` /
  `since=since_action` boilerplate** from tests that no longer need it.
  This is `test_force_park_release` lines ~580-590 and
  `test_ha_command_propagation` lines ~858, ~863, ~876.

- [ ] **Step 5: Re-run the suite.**  Should still pass 5-and-skip-2 with
  no behavior change (the new default just makes the back-window wider when
  callers don't specify).

- [ ] **Step 6: Commit**: `test: P5-14 -- expect_log defaults since= to last action timestamp`

---

## Task P5-15: Add gateway-role LOGD; rework test_gateway_failover

**Files:**
- Modify: `esphome/components/tracker_bridge/tracker_bridge.h` (~the mesh_broadcasts set_interval lambda at ~line 648)
- Modify: `tools/bench_test.py` (`identify_gateway()` and `test_gateway_failover`)

**Why:** P5-12 removed peer-mirror entities from both tracker YAMLs.  The
`[gateway] publish peer 'X' az=... el=...` LOGD that the harness used to
identify the acting gateway only fires when the gateway publishes peer
entities -- with no peer mirrors, that log line never appears.

Concrete symptom (observed 2026-05-23 during P5-13 validation):
`identify_gateway()` always returns None, the test falls back to
`gw = t1`, but the actual elected gateway is t2 (it has the lower MAC:
`84:F3:EB:...` < `EC:FA:BC:...`).  Silencing t1 doesn't change anything;
the 35 s `expect_log(t2, r"\[gateway\]")` times out.

P5-13 worked around this by `_ManualSkip`-gating the test.  Restore it
to automated status here.

**Approach:** Decouple gateway identification from peer-publish.  In the
existing 5 s `mesh_broadcasts` set_interval lambda, log a one-line
`[I] gateway role: active` (or `: standby`) whenever the
`is_acting_gateway_()` computation returns true (or false).  Throttle so
the log only appears on STATE CHANGE (active -> standby or vice versa),
not every 5 s -- otherwise we spam the log.  The harness watches for
`gateway role: active` to identify the gateway.

- [ ] **Step 1: Add a `was_acting_gateway_` boolean** to TrackerBridge,
  initialized to `false` in the constructor.

- [ ] **Step 2: In the mesh_broadcasts lambda**, compute `bool now_gw =
  is_acting_gateway_();` and compare to `was_acting_gateway_`.  If they
  differ, log:
  ```cpp
  ESP_LOGI(TAG, "gateway role: %s", now_gw ? "active" : "standby");
  was_acting_gateway_ = now_gw;
  ```

- [ ] **Step 3: Update `identify_gateway()`** in bench_test.py to watch
  for `gateway role: active` log lines instead of `[gateway] publish peer`.
  Keep the 20 s timeout window.

- [ ] **Step 4: Remove the _ManualSkip gate** from test_gateway_failover
  (added in P5-13).  Restore the original assertion to watch for
  `gateway role: active` on the OTHER node after silencing the current
  gateway:
  ```python
  await h.expect_log(other, r"gateway role: active", timeout=35.0, since=since_offline)
  ```

- [ ] **Step 5: Run the harness three times back-to-back.**  Expect
  deterministic 5p/0f/2s (or 6p/0f/1s if test_gateway_failover is now
  automated again).

- [ ] **Step 6: Commit**: `feat(esp,test): P5-15 -- gateway-role LOGD + restored test_gateway_failover`

