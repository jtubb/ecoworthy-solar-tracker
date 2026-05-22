# Solar Tracker Bench-Test Harness

Automated failure-mode tests for the two-node solar-tracker ESP-NOW mesh.
Tests run against live hardware over the ESPHome native API without physical
access (no relay jiggling, no serial cables).

## What it tests

| Test | What it validates |
|------|-------------------|
| `test_force_park_release` | Force Park drives both nodes into storm mode; Force Release + storm-dwell expiry returns tracker-1 to track/idle |
| `test_wsrc_failsafe_recovery` | Setting WSrc=1 (remote-wind required) arms the failsafe when no WIND broadcasts arrive; restoring WSrc=0 recovers |
| `test_no_primary_baseline` | When the only has_wind_sensor node (tracker-1) is silenced, tracker-2 never self-elects as wind primary |
| `test_dual_primary_election_failover` | MANUAL — validates primary failover when two nodes both have has_wind_sensor=true |
| `test_gateway_failover` | Silencing the acting gateway causes the other node to take over within HB stale time |
| `test_ha_command_propagation` | Force Park propagates over ESP-NOW to tracker-2 and the AA 55 UART frame reaches the STC |
| `test_cfg_get_resp_readback` | Wind Storm config slider SET reaches the STC and the GET read-back matches |

Known failure modes not covered by automated tests (require manual setup):
- **F3 PSK mismatch**: change tracker_mesh_psk on one node — should see "AES auth fail" in logs
- **F4 flash-wipe replay**: wipe flash preferences and confirm epoch increments
- **F9 dual-primary with real sensors**: requires two physical wind sensors

## Prerequisites

- Python 3.9+
- Both ESP nodes on the network and flashed with firmware that includes the
  `test_offline_switch` template switch (see firmware hook notes below)

## Installation

```
pip install -r tools/requirements.txt
```

## Configuration

```
cp tools/bench_test_config.yaml.example tools/bench_test_config.yaml
# Edit bench_test_config.yaml: add real API keys from secrets.yaml
```

`bench_test_config.yaml` is gitignored and never committed.

The harness also reads API keys from environment variables
`TRACKER1_API_KEY` and `TRACKER2_API_KEY` (override the config file).

### storm_dwell_test_value

The force-park tests temporarily lower `storm_dwell_min` so tests finish
in < 2 minutes.  The default is `1` (one minute, the firmware minimum).
The original value is always restored in a `finally` block.

## Usage

```
# Run all automated tests
python tools/bench_test.py --all

# Run a single test
python tools/bench_test.py --test test_gateway_failover

# List tests
python tools/bench_test.py --list

# Include the manual dual-primary test (requires reflashing tracker-2 first)
python tools/bench_test.py --all --include-manual
```

### Expected output (all pass)

```
[INFO] Connecting to solar-tracker-1 (192.168.1.193)...
[INFO] Connecting to solar-tracker-2 (192.168.1.194)...
[INFO] Subscribed to logs and entity state for all nodes

[RUN]  test_force_park_release  PASS (74.2s)
[RUN]  test_wsrc_failsafe_recovery  PASS (52.8s)
[RUN]  test_no_primary_baseline  PASS (75.1s)
[SKIP] test_dual_primary_election_failover (0.0s)
       Requires solar-tracker-2.yaml has_wind_sensor: true + reflash. ...
[RUN]  test_gateway_failover  PASS (43.5s)
[RUN]  test_ha_command_propagation  PASS (10.4s)
[RUN]  test_cfg_get_resp_readback  PASS (8.9s)

6 passed, 1 skipped in 264.9s
```

On failure: the failing assertion and the last 30 log lines from all nodes
are printed for diagnosis.  Exit code is non-zero.

## Firmware test hook (`test_offline_switch`)

Both tracker YAMLs include a `switch: platform: template` entity named
**"Solar Tracker N Test Offline"** (`entity_category: diagnostic`).

When ON:
- Stops the 2 s STC UART poll (no more `?` polls sent to the STC)
- Stops the 5 s mesh broadcasts (no TELEMETRY, WIND, or GATEWAY_HB)
- Stops the 30 s cfg_poll

When OFF (default):
- All timers run normally

The node **still receives** mesh frames and remains reachable over WiFi/HA API.
This simulates a tracker that is "alive enough to be seen on the network but
has stopped participating" — the most useful failure mode for gateway and
primary failover tests without physical disconnection.

The switch defaults to OFF.  It has no effect on production behaviour unless
someone deliberately turns it on.

## Manual dual-primary test setup

1. Edit `EcoWorthyFirmware/esphome/solar-tracker-2.yaml`:
   ```
   has_wind_sensor: false   →   has_wind_sensor: true
   ```
2. Flash tracker-2: `esphome run EcoWorthyFirmware/esphome/solar-tracker-2.yaml`
3. Run: `python tools/bench_test.py --test test_dual_primary_election_failover --include-manual`
4. Revert the YAML and reflash tracker-2 to restore production config.
