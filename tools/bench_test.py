#!/usr/bin/env python3
"""Bench-test harness for the solar-tracker mesh.

Connects to both ESPHome nodes via the native API (aioesphomeapi), subscribes
to log output and entity state, then runs automated tests that validate mesh
failover, storm interlock, config read-back, and HA command propagation.

Usage:
    python tools/bench_test.py --all
    python tools/bench_test.py --test test_force_park_release
    python tools/bench_test.py --list
    python tools/bench_test.py --all --include-manual

Configuration:
    Copy tools/bench_test_config.yaml.example to tools/bench_test_config.yaml
    and fill in real API keys.  The harness also accepts API keys via environment
    variables TRACKER1_API_KEY and TRACKER2_API_KEY (override config file).
"""

from __future__ import annotations

import argparse
import asyncio
import collections
import inspect
import os
import re
import socket
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Deque, Dict, List, Optional, Tuple

import yaml

# ---------------------------------------------------------------------------
# aioesphomeapi import (fail loudly if missing)
# ---------------------------------------------------------------------------
try:
    import aioesphomeapi
    from aioesphomeapi import (
        APIClient,
        ButtonInfo,
        NumberInfo,
        SwitchInfo,
        TextSensorState,
        NumberState,
        SwitchState,
    )
except ImportError:
    print(
        "ERROR: aioesphomeapi is not installed.\n"
        "Run:  pip install -r tools/requirements.txt",
        file=sys.stderr,
    )
    sys.exit(1)

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
CONFIG_PATH = Path(__file__).resolve().parent / "bench_test_config.yaml"
SECRETS_PATH = REPO_ROOT / "EcoWorthyFirmware" / "esphome" / "secrets.yaml"
STREAM_SERVER_PORT = 23

# Log ring buffer: keep 5 minutes of log lines per node.
LOG_RING_SIZE = 5 * 60 * 20  # ~20 lines/second worst-case


# ---------------------------------------------------------------------------
# Wire-framing (CRC-8/SMBUS) -- mirrored from scripts/send-cmd.py
# ---------------------------------------------------------------------------

def crc8(data: bytes) -> int:
    """CRC-8/SMBUS: poly=0x07, init=0x00, MSB-first, no reflection."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def build_frame(payload: str) -> bytes:
    """Wrap payload in AA 55 <ascii> <2-hex-CRC> LF."""
    pb = payload.encode("ascii")
    c = crc8(pb)
    return bytes([0xAA, 0x55]) + pb + f"{c:02X}".encode("ascii") + b"\n"


# ---------------------------------------------------------------------------
# Configuration loading
# ---------------------------------------------------------------------------

@dataclass
class NodeConfig:
    name: str
    host: str
    api_key: str


@dataclass
class HarnessConfig:
    nodes: Dict[str, NodeConfig]
    storm_dwell_test_value: int = 1


def load_config() -> HarnessConfig:
    """Load node config from bench_test_config.yaml (or fail with a clear error)."""
    if not CONFIG_PATH.exists():
        print(
            f"\nERROR: Configuration file not found: {CONFIG_PATH}\n"
            f"\nTo create it:\n"
            f"  cp tools/bench_test_config.yaml.example tools/bench_test_config.yaml\n"
            f"  # then edit it to add real API keys\n",
            file=sys.stderr,
        )
        sys.exit(1)

    with CONFIG_PATH.open() as f:
        raw = yaml.safe_load(f)

    nodes_raw = raw.get("nodes", {})
    if not nodes_raw:
        print(
            f"ERROR: bench_test_config.yaml has no 'nodes:' section.",
            file=sys.stderr,
        )
        sys.exit(1)

    nodes: Dict[str, NodeConfig] = {}
    for name, vals in nodes_raw.items():
        # Allow env-var overrides: TRACKER1_API_KEY, TRACKER2_API_KEY (by index)
        env_key = f"TRACKER{list(nodes_raw.keys()).index(name) + 1}_API_KEY"
        api_key = os.environ.get(env_key) or vals.get("api_key", "")
        nodes[name] = NodeConfig(
            name=name,
            host=vals["host"],
            api_key=api_key,
        )

    return HarnessConfig(
        nodes=nodes,
        storm_dwell_test_value=int(raw.get("storm_dwell_test_value", 1)),
    )


# ---------------------------------------------------------------------------
# Log entry
# ---------------------------------------------------------------------------

@dataclass
class LogLine:
    ts: float  # time.monotonic()
    level: int
    tag: str
    message: str

    def __str__(self) -> str:
        lvl = ["", "ERR", "WRN", "INF", "DBG", "VRB"][min(self.level, 5)]
        return f"[{lvl}][{self.tag}] {self.message}"


# ---------------------------------------------------------------------------
# TestHarness
# ---------------------------------------------------------------------------

class _FailError(Exception):
    """Raised by assertion helpers to abort the current test."""


class TestHarness:
    """Manages connections to both nodes and provides test primitives."""

    def __init__(self, cfg: HarnessConfig) -> None:
        self._cfg = cfg
        self._clients: Dict[str, APIClient] = {}
        # entity_state[node_name][object_id] = state value (str for text_sensor/switch, float for number)
        self._entity_state: Dict[str, Dict[str, object]] = {n: {} for n in cfg.nodes}
        # entity_info[node_name][object_id] = entity info object
        self._entity_info: Dict[str, Dict[str, object]] = {n: {} for n in cfg.nodes}
        # ring buffer of LogLine per node
        self._log_ring: Dict[str, Deque[LogLine]] = {
            n: collections.deque(maxlen=LOG_RING_SIZE) for n in cfg.nodes
        }
        # asyncio event that fires whenever a new log line arrives (any node)
        self._log_event = asyncio.Event()
        # asyncio event that fires whenever any entity state changes
        self._state_event = asyncio.Event()

    # ------------------------------------------------------------------
    # Connection management
    # ------------------------------------------------------------------

    async def connect_all(self) -> None:
        for name, node in self._cfg.nodes.items():
            print(f"[INFO] Connecting to {name} ({node.host})...")
            client = APIClient(
                node.host,
                6053,
                password="",
                noise_psk=node.api_key,
            )
            try:
                await client.connect(login=True)
            except Exception as e:
                print(
                    f"\nERROR: Could not connect to {name} at {node.host}: {e}\n"
                    f"       Check that the node is powered, on the network, and that\n"
                    f"       bench_test_config.yaml has the correct api_key.\n",
                    file=sys.stderr,
                )
                # Disconnect any already-connected clients before exiting
                for other_client in self._clients.values():
                    try:
                        await other_client.disconnect()
                    except Exception:
                        pass
                sys.exit(1)

            self._clients[name] = client
            await self._subscribe_logs(name, client)
            await self._subscribe_states(name, client)

        print(f"[INFO] Subscribed to logs and entity state for all nodes\n")

    async def disconnect_all(self) -> None:
        for client in self._clients.values():
            try:
                await client.disconnect()
            except Exception:
                pass

    # ------------------------------------------------------------------
    # Subscription helpers
    # ------------------------------------------------------------------

    async def _subscribe_logs(self, name: str, client: APIClient) -> None:
        def on_log(msg: aioesphomeapi.LogMessage) -> None:
            line = LogLine(
                ts=time.monotonic(),
                level=msg.level,
                tag=msg.tag,
                message=msg.message,
            )
            self._log_ring[name].append(line)
            self._log_event.set()

        await client.subscribe_logs(on_log, log_level=5)  # level 5 = VERBOSE

    async def _subscribe_states(self, name: str, client: APIClient) -> None:
        entities, services = await client.list_entities_services()
        for e in entities:
            oid = e.object_id
            self._entity_info[name][oid] = e

        def on_state(state: object) -> None:
            # Match state to entity by key
            for oid, info in self._entity_info[name].items():
                if info.key == state.key:
                    if isinstance(state, TextSensorState):
                        self._entity_state[name][oid] = state.state
                    elif isinstance(state, NumberState):
                        self._entity_state[name][oid] = state.state
                    elif isinstance(state, SwitchState):
                        self._entity_state[name][oid] = state.state
                    else:
                        # Generic: store the state object itself
                        self._entity_state[name][oid] = state
                    self._state_event.set()
                    break

        self._clients[name].subscribe_states(on_state)

    # ------------------------------------------------------------------
    # Outbound actions
    # ------------------------------------------------------------------

    async def send_uart_cmd(self, name: str, payload: str) -> None:
        """Send a framed STC command over stream_server port 23."""
        node = self._cfg.nodes[name]
        frame = build_frame(payload)
        loop = asyncio.get_event_loop()
        await loop.run_in_executor(None, self._raw_send, node.host, frame)

    @staticmethod
    def _raw_send(host: str, frame: bytes) -> None:
        s = socket.create_connection((host, STREAM_SERVER_PORT), timeout=5)
        try:
            s.sendall(frame)
        finally:
            s.close()

    async def press_button(self, name: str, entity_object_id: str) -> None:
        client = self._clients[name]
        info = self._entity_info[name].get(entity_object_id)
        if info is None:
            raise _FailError(
                f"press_button: entity '{entity_object_id}' not found on {name}.\n"
                f"  Available: {list(self._entity_info[name].keys())}"
            )
        await client.button_command(info.key)

    async def set_switch(self, name: str, entity_object_id: str, value: bool) -> None:
        client = self._clients[name]
        info = self._entity_info[name].get(entity_object_id)
        if info is None:
            raise _FailError(
                f"set_switch: entity '{entity_object_id}' not found on {name}.\n"
                f"  Available: {list(self._entity_info[name].keys())}"
            )
        await client.switch_command(info.key, value)

    async def set_number(self, name: str, entity_object_id: str, value: float) -> None:
        client = self._clients[name]
        info = self._entity_info[name].get(entity_object_id)
        if info is None:
            raise _FailError(
                f"set_number: entity '{entity_object_id}' not found on {name}.\n"
                f"  Available: {list(self._entity_info[name].keys())}"
            )
        await client.number_command(info.key, value)

    async def reboot(self, name: str) -> None:
        """Trigger a soft reboot via the built-in restart button."""
        # ESPHome exposes a "restart" button on every device
        await self.press_button(name, "restart")

    # ------------------------------------------------------------------
    # Assertion helpers
    # ------------------------------------------------------------------

    async def expect_log(
        self,
        name: str,
        pattern: str,
        timeout: float = 10.0,
        since: Optional[float] = None,
    ) -> str:
        """
        Wait for a log line from `name` matching `pattern` (regex).
        `since` is a monotonic timestamp; only lines after it are considered.
        Returns the first matching line's message string.
        Raises _FailError on timeout.
        """
        re_pat = re.compile(pattern)
        deadline = time.monotonic() + timeout
        if since is None:
            since = time.monotonic() - 0.1  # accept lines arriving right now

        while True:
            # Scan the current ring buffer
            for line in self._log_ring[name]:
                if line.ts >= since and re_pat.search(str(line)):
                    return str(line)

            remaining = deadline - time.monotonic()
            if remaining <= 0:
                # Collect recent context for the error message
                recent = list(self._log_ring[name])[-30:]
                ctx = "\n    ".join(str(l) for l in recent)
                raise _FailError(
                    f"expect_log({name!r}, {pattern!r}) timed out after {timeout}s\n"
                    f"  Last {len(recent)} log lines from {name}:\n    {ctx}"
                )

            # Wait for a new log line to arrive
            self._log_event.clear()
            try:
                await asyncio.wait_for(
                    asyncio.shield(self._wait_log_event()), timeout=min(remaining, 0.5)
                )
            except asyncio.TimeoutError:
                pass

    async def _wait_log_event(self) -> None:
        while not self._log_event.is_set():
            await asyncio.sleep(0.05)
        self._log_event.clear()

    async def expect_entity(
        self,
        name: str,
        entity_object_id: str,
        expected_value: object,
        timeout: float = 10.0,
    ) -> None:
        """
        Poll entity_state until it matches expected_value or timeout.
        For text_sensor: exact string match.
        For number: float equality (rounded to 1 dp).
        For switch: bool match.
        Raises _FailError on timeout.
        """
        deadline = time.monotonic() + timeout

        def matches() -> bool:
            cur = self._entity_state[name].get(entity_object_id)
            if cur is None:
                return False
            if isinstance(expected_value, bool):
                return bool(cur) == expected_value
            if isinstance(expected_value, (int, float)):
                try:
                    return round(float(cur), 1) == round(float(expected_value), 1)
                except (TypeError, ValueError):
                    return False
            return str(cur) == str(expected_value)

        while not matches():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                cur = self._entity_state[name].get(entity_object_id)
                raise _FailError(
                    f"expect_entity({name!r}, {entity_object_id!r}, {expected_value!r}) "
                    f"timed out after {timeout}s -- current value: {cur!r}"
                )
            self._state_event.clear()
            try:
                await asyncio.wait_for(self._state_event.wait(), timeout=min(remaining, 0.5))
            except asyncio.TimeoutError:
                pass

    async def expect_no_log(
        self,
        name: str,
        pattern: str,
        window: float = 15.0,
    ) -> None:
        """
        Assert that no log line matching `pattern` arrives within `window` seconds.
        Raises _FailError if a match is found.
        """
        re_pat = re.compile(pattern)
        since = time.monotonic()
        deadline = since + window

        while time.monotonic() < deadline:
            for line in self._log_ring[name]:
                if line.ts >= since and re_pat.search(str(line)):
                    raise _FailError(
                        f"expect_no_log({name!r}, {pattern!r}): unexpected match found:\n"
                        f"  {line}"
                    )
            self._log_event.clear()
            remaining = deadline - time.monotonic()
            try:
                await asyncio.wait_for(self._state_event.wait(), timeout=min(remaining, 0.5))
            except asyncio.TimeoutError:
                pass

    # ------------------------------------------------------------------
    # Convenience helpers
    # ------------------------------------------------------------------

    async def identify_gateway(self) -> Optional[str]:
        """
        Return the name of the currently acting gateway by watching for
        '[gateway] publish peer' log lines in the next 20 seconds.
        Returns None if neither node logs that string.
        """
        since = time.monotonic()
        deadline = since + 20.0
        gw_pat = re.compile(r"\[gateway\]")

        while time.monotonic() < deadline:
            for name in self._cfg.nodes:
                for line in self._log_ring[name]:
                    if line.ts >= since and gw_pat.search(str(line)):
                        return name
            self._log_event.clear()
            remaining = deadline - time.monotonic()
            try:
                await asyncio.wait_for(self._state_event.wait(), timeout=min(remaining, 0.5))
            except asyncio.TimeoutError:
                pass

        return None

    async def wait_steady_state(self, secs: float = 10.0) -> None:
        """Sleep to let log buffers drain / entity states stabilise."""
        await asyncio.sleep(secs)

    def log_context(self, name: str, last_n: int = 30) -> str:
        """Return the last N log lines from a node as a single string."""
        lines = list(self._log_ring[name])[-last_n:]
        return "\n  ".join(str(l) for l in lines) or "(no log lines yet)"


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

MANUAL_FLAG = False  # Set to True by --include-manual


async def test_force_park_release(h: TestHarness) -> None:
    """
    Press Force Park on tracker-1, verify both nodes enter storm mode,
    then press Force Release and verify recovery.

    storm_dwell_min is temporarily lowered to 1 minute so the release
    fires in ≤ 90 s instead of the default dwell period.
    """
    cfg = h._cfg
    t1 = "solar-tracker-1"
    t2 = "solar-tracker-2"

    # --- Save original storm_dwell and set to minimum ---
    orig_dwell = h._entity_state[t1].get("storm_dwell_min")
    test_dwell = cfg.storm_dwell_test_value

    await h.set_number(t1, "storm_dwell_min", float(test_dwell))
    await asyncio.sleep(2)  # let the SET reach the STC

    try:
        # --- Press Force Park ---
        await h.press_button(t1, "force_park")

        # Tracker-1 STC should enter storm
        await h.expect_entity(t1, "mode", "storm", timeout=15)

        # Tracker-2 should relay the mesh command (no STC, so check mesh rx log)
        await h.expect_log(t2, r"mesh rx type=4", timeout=15)

        # --- Press Force Release ---
        since_release = time.monotonic()
        await h.press_button(t1, "force_release")

        # After dwell expires, tracker-1 should exit storm
        dwell_timeout = (test_dwell * 60) + 30  # dwell minutes + 30 s margin
        await h.expect_entity(
            t1, "mode", "track", timeout=dwell_timeout
        )

    finally:
        # Restore original storm_dwell
        if orig_dwell is not None:
            try:
                await h.set_number(t1, "storm_dwell_min", float(orig_dwell))
            except Exception:
                pass


async def test_wsrc_failsafe_recovery(h: TestHarness) -> None:
    """
    Set WSrc=1 (remote-wind required) on tracker-1 via UART, then verify
    that the failsafe fires when no WIND mesh frames arrive (tracker-2 has
    has_wind_sensor=false and never broadcasts WIND).

    Recovery path: restore WSrc=0 (no remote wind required) and verify
    tracker-1 exits storm.

    NOTE: This test validates failsafe arming + WSrc=0 recovery only.
    Auto-recovery from incoming remote WIND is covered by test_no_primary_baseline
    (which tests the broadcast-absent case) but full end-to-end WIND-driven
    recovery requires a second has_wind_sensor node -- see test_dual_primary
    for that manual variant.
    """
    t1 = "solar-tracker-1"

    # WSrc=1 means "require remote WIND broadcast" (config field id=33 in STC).
    # There is no HA slider for field 33 so we send it directly via UART.
    await h.send_uart_cmd(t1, "!cfg set id=33 val=1")
    await asyncio.sleep(3)  # let STC absorb the config

    try:
        # With WSrc=1 and no WIND broadcasts arriving (tracker-2 has no sensor),
        # the STC failsafe should arm and push to storm within ~15-30 s.
        await h.expect_entity(t1, "mode", "storm", timeout=45)

    finally:
        # Restore WSrc=0 (local wind, no remote required)
        await h.send_uart_cmd(t1, "!cfg set id=33 val=0")
        await asyncio.sleep(3)

        # Verify recovery: mode should return to track or idle
        # We check for track first; if dwell hasn't cleared yet, check idle.
        deadline = time.monotonic() + 90
        recovered = False
        for target_mode in ("track", "idle"):
            try:
                remaining = max(1.0, deadline - time.monotonic())
                await h.expect_entity(t1, "mode", target_mode, timeout=remaining)
                recovered = True
                break
            except _FailError:
                pass
        if not recovered:
            cur = h._entity_state[t1].get("mode")
            raise _FailError(
                f"test_wsrc_failsafe_recovery: tracker-1 mode did not recover to "
                f"'track' or 'idle' within 90s after WSrc=0 restore.  Current: {cur!r}"
            )


async def test_no_primary_baseline(h: TestHarness) -> None:
    """
    Verify that when the only has_wind_sensor node (tracker-1) is silenced
    via Test Offline, NO WIND mesh frames appear on tracker-2.

    Tracker-2 has has_wind_sensor=false so it should never self-elect as
    wind primary regardless.

    Recovery: turn Test Offline OFF on tracker-1 and verify WIND broadcasts
    resume within one full 5-second mesh_broadcasts cycle + margin.
    """
    t1 = "solar-tracker-1"
    t2 = "solar-tracker-2"

    # Confirm initial condition
    t1_has_wind = h._entity_state[t1].get("test_offline_switch", False)
    # It should be False (not offline)
    if t1_has_wind:
        await h.set_switch(t1, "test_offline_switch", False)
        await asyncio.sleep(3)

    # Turn tracker-1 offline (stops broadcasting, including WIND)
    await h.set_switch(t1, "test_offline_switch", True)
    await asyncio.sleep(2)  # let the switch take effect

    try:
        # Wait for tracker-1's peer entry to stale out on tracker-2.
        # PEER_STALE_MS = 60 s, so wait 65 s.
        await asyncio.sleep(65)

        # For the next 15 seconds, assert no "tx WIND" appears on tracker-2
        # (since it does not have has_wind_sensor).
        since = time.monotonic()
        await h.expect_no_log(t2, r"tx WIND", window=15.0)

    finally:
        # Restore: turn tracker-1 back online
        await h.set_switch(t1, "test_offline_switch", False)

    # After tracker-1 resumes, it should start broadcasting WIND again within ~10s
    await h.expect_log(t1, r"tx WIND", timeout=15.0)


async def test_dual_primary_election_failover(h: TestHarness) -> None:
    """
    MANUAL TEST -- requires temporarily editing solar-tracker-2.yaml to set
    has_wind_sensor: true, reflashing tracker-2, and then running with
    --include-manual.

    This test validates that when two nodes have has_wind_sensor=true, only
    the lowest-MAC node broadcasts WIND (election), and when the elected
    primary is silenced, the other takes over.

    Rerun instructions:
      1. Edit EcoWorthyFirmware/esphome/solar-tracker-2.yaml:
             has_wind_sensor: false  ->  has_wind_sensor: true
      2. esphome run EcoWorthyFirmware/esphome/solar-tracker-2.yaml
      3. python tools/bench_test.py --test test_dual_primary_election_failover --include-manual
      4. Revert the YAML and reflash tracker-2 to restore production config.
    """
    if not MANUAL_FLAG:
        raise _ManualSkip(
            "Requires solar-tracker-2.yaml has_wind_sensor: true + reflash. "
            "Rerun with --include-manual after completing manual setup steps."
        )

    t1 = "solar-tracker-1"
    t2 = "solar-tracker-2"

    # Wait for both nodes to be in steady state
    await h.wait_steady_state(10)

    # Identify the current elected primary by watching WIND logs
    # (lowest MAC wins; usually tracker-1 but depends on hardware)
    primary = None
    secondary = None
    since = time.monotonic()
    deadline = since + 15.0
    while time.monotonic() < deadline:
        for name in (t1, t2):
            for line in h._log_ring[name]:
                if line.ts >= since and re.search(r"tx WIND", str(line)):
                    primary = name
                    secondary = t2 if name == t1 else t1
                    break
        if primary:
            break
        await asyncio.sleep(1)

    if primary is None:
        raise _FailError("Could not identify wind primary within 15s.")

    # Silence the primary
    await h.set_switch(primary, "test_offline_switch", True)

    try:
        # Wait for PEER_STALE_MS (60 s) for the primary to stale out
        await asyncio.sleep(65)

        # Secondary should take over: expect "tx WIND" on the secondary
        await h.expect_log(secondary, r"tx WIND", timeout=20.0)

    finally:
        await h.set_switch(primary, "test_offline_switch", False)

    # Confirm primary resumes after coming back online
    await h.expect_log(primary, r"tx WIND", timeout=15.0)


async def test_gateway_failover(h: TestHarness) -> None:
    """
    Identify the acting gateway, silence it via Test Offline, then verify the
    other node takes over the gateway role within GATEWAY_HB stale time (15 s).
    """
    t1 = "solar-tracker-1"
    t2 = "solar-tracker-2"

    # Let steady-state GATEWAY_HB broadcasts run for a bit
    await h.wait_steady_state(6)

    # Identify acting gateway by watching for '[gateway] publish peer' log lines
    gw = await h.identify_gateway()
    if gw is None:
        # Fallback: assume tracker-1 is gateway (lowest MAC on most installations)
        gw = t1
    other = t2 if gw == t1 else t1

    # Silence the gateway
    since_offline = time.monotonic()
    await h.set_switch(gw, "test_offline_switch", True)

    try:
        # GATEWAY_HB stale = 15 s (3 missed @ 5s).  Allow 35 s for the other
        # node to observe the stale + win the next election.
        await h.expect_log(other, r"\[gateway\]", timeout=35.0, since=since_offline)

    finally:
        # Restore original gateway
        await h.set_switch(gw, "test_offline_switch", False)

    # After original gateway comes back online, wait ~30 s for it to resume
    # (needs to broadcast GATEWAY_HB and win the election back).
    await asyncio.sleep(30)
    # Just verify it is still reachable; a log line of any type is sufficient
    since_restore = time.monotonic() - 35
    recent_from_gw = any(
        line.ts >= since_restore for line in h._log_ring[gw]
    )
    if not recent_from_gw:
        raise _FailError(
            f"test_gateway_failover: original gateway {gw!r} has no log lines "
            f"after restore.  Node may be unreachable."
        )


async def test_ha_command_propagation(h: TestHarness) -> None:
    """
    Press Force Park from HA, verify the mesh command reaches tracker-2 and
    the UART frame reaches tracker-1's STC wire.  Then press Force Release
    and verify tracker-1 recovers.
    """
    cfg = h._cfg
    t1 = "solar-tracker-1"
    t2 = "solar-tracker-2"

    orig_dwell = h._entity_state[t1].get("storm_dwell_min")
    test_dwell = cfg.storm_dwell_test_value
    await h.set_number(t1, "storm_dwell_min", float(test_dwell))
    await asyncio.sleep(2)

    since_park = time.monotonic()

    try:
        # Press Force Park
        await h.press_button(t1, "force_park")

        # Mesh command should arrive at tracker-2
        await h.expect_log(
            t2, r"mesh rx type=4", timeout=15.0, since=since_park
        )

        # UART debug should show the AA 55 21 70 61 72 6B ("!park") outbound frame
        # on tracker-1 (uart debug logs hex bytes)
        await h.expect_log(
            t1,
            r"AA 55 21 70 61 72 6B",
            timeout=10.0,
            since=since_park,
        )

        # Press Force Release
        since_release = time.monotonic()
        await h.press_button(t1, "force_release")

        # Tracker-1 should recover to track or idle after dwell
        dwell_timeout = (test_dwell * 60) + 30
        recovered = False
        for target_mode in ("track", "idle"):
            try:
                remaining = max(1.0, since_release + dwell_timeout - time.monotonic())
                await h.expect_entity(t1, "mode", target_mode, timeout=remaining)
                recovered = True
                break
            except _FailError:
                pass
        if not recovered:
            cur = h._entity_state[t1].get("mode")
            raise _FailError(
                f"test_ha_command_propagation: mode did not recover.  Current: {cur!r}"
            )

    finally:
        if orig_dwell is not None:
            try:
                await h.set_number(t1, "storm_dwell_min", float(orig_dwell))
            except Exception:
                pass


async def test_cfg_get_resp_readback(h: TestHarness) -> None:
    """
    Change the Wind Storm threshold on tracker-1 to a known test value,
    verify the HA slider echoes it, then send a direct UART cfg get to
    confirm the STC actually stored the value.  Restore original value at end.
    """
    t1 = "solar-tracker-1"

    TEST_VALUE = 17.0  # deliberately odd value unlikely to be the default

    # Read current value
    orig_value = h._entity_state[t1].get("wind_storm_mps")

    # Set to test value via HA API
    await h.set_number(t1, "wind_storm_mps", TEST_VALUE)

    try:
        # The slider is optimistic: it should immediately reflect the new value
        await h.expect_entity(t1, "wind_storm_mps", TEST_VALUE, timeout=5.0)

        # Wait for the cfg_poll to fire (up to 30 s) and push a GET_REQ to the STC.
        # Then watch for the STC's "cfg id=1 val=17" reply in the log.
        await h.expect_log(
            t1, r"cfg reply: fid=1 val=17", timeout=35.0
        )

        # Also verify via direct UART GET (bypasses optimistic slider)
        since_uart = time.monotonic()
        await h.send_uart_cmd(t1, "!cfg get id=1")
        await h.expect_log(
            t1, r"cfg reply: fid=1 val=17", timeout=10.0, since=since_uart
        )

    finally:
        if orig_value is not None:
            try:
                await h.set_number(t1, "wind_storm_mps", float(orig_value))
                await asyncio.sleep(2)
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Test registry
# ---------------------------------------------------------------------------

class _ManualSkip(Exception):
    """Raised by manual tests when --include-manual is not set."""


ALL_TESTS: List[Tuple[str, Callable]] = [
    ("test_force_park_release",          test_force_park_release),
    ("test_wsrc_failsafe_recovery",      test_wsrc_failsafe_recovery),
    ("test_no_primary_baseline",         test_no_primary_baseline),
    ("test_dual_primary_election_failover", test_dual_primary_election_failover),
    ("test_gateway_failover",            test_gateway_failover),
    ("test_ha_command_propagation",      test_ha_command_propagation),
    ("test_cfg_get_resp_readback",       test_cfg_get_resp_readback),
]


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

async def run_tests(
    harness: TestHarness,
    tests: List[Tuple[str, Callable]],
    include_manual: bool,
) -> int:
    """Run the given tests, print results, return exit code (0 = all pass)."""
    global MANUAL_FLAG
    MANUAL_FLAG = include_manual

    passed = 0
    failed = 0
    skipped = 0
    total_start = time.monotonic()

    for name, fn in tests:
        start = time.monotonic()
        sys.stdout.write(f"[RUN]  {name} ")
        sys.stdout.flush()

        try:
            await fn(harness)
            elapsed = time.monotonic() - start
            print(f" PASS ({elapsed:.1f}s)")
            passed += 1

        except _ManualSkip as e:
            elapsed = time.monotonic() - start
            print(f"\n[SKIP] {name} ({elapsed:.1f}s)")
            print(f"       {e}")
            skipped += 1

        except _FailError as e:
            elapsed = time.monotonic() - start
            print(f" FAIL ({elapsed:.1f}s)")
            print(f"\n  Assertion failed:\n  {e}\n")
            print(f"  --- Last 30 log lines (solar-tracker-1) ---")
            print(f"  {harness.log_context('solar-tracker-1', 30)}")
            for extra_name in harness._cfg.nodes:
                if extra_name != "solar-tracker-1":
                    print(f"\n  --- Last 30 log lines ({extra_name}) ---")
                    print(f"  {harness.log_context(extra_name, 30)}")
            print()
            failed += 1

        except Exception as e:
            elapsed = time.monotonic() - start
            print(f" ERROR ({elapsed:.1f}s)")
            print(f"\n  Unexpected exception: {type(e).__name__}: {e}\n")
            import traceback
            traceback.print_exc()
            print()
            failed += 1

    total = time.monotonic() - total_start
    parts = []
    if passed:
        parts.append(f"{passed} passed")
    if failed:
        parts.append(f"{failed} failed")
    if skipped:
        parts.append(f"{skipped} skipped")
    print(f"\n{', '.join(parts)} in {total:.1f}s")

    return 0 if failed == 0 else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Bench-test harness for the solar-tracker mesh.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument(
        "--all", action="store_true", help="Run all automated tests."
    )
    ap.add_argument(
        "--test", metavar="NAME", help="Run a single named test."
    )
    ap.add_argument(
        "--list", action="store_true", help="List available tests and exit."
    )
    ap.add_argument(
        "--include-manual",
        action="store_true",
        help="Include manual tests (they print setup instructions if not prepared).",
    )
    args = ap.parse_args()

    test_map = dict(ALL_TESTS)

    if args.list:
        print("Available tests:")
        for name, fn in ALL_TESTS:
            doc = (fn.__doc__ or "").strip().splitlines()[0] if fn.__doc__ else ""
            print(f"  {name}")
            if doc:
                print(f"    {doc}")
        return 0

    if not args.all and not args.test:
        ap.print_help()
        return 0

    cfg = load_config()

    if args.test:
        if args.test not in test_map:
            print(
                f"ERROR: unknown test {args.test!r}.\n"
                f"Run with --list to see available tests.",
                file=sys.stderr,
            )
            return 1
        tests_to_run = [(args.test, test_map[args.test])]
    else:
        tests_to_run = list(ALL_TESTS)

    async def _main() -> int:
        harness = TestHarness(cfg)
        await harness.connect_all()
        try:
            return await run_tests(harness, tests_to_run, args.include_manual)
        finally:
            await harness.disconnect_all()

    return asyncio.run(_main())


if __name__ == "__main__":
    sys.exit(main())
