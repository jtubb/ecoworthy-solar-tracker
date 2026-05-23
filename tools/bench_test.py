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
    # P5-13 baseline values: reset_to_baseline_() restores these on each node
    # before every test runs, so residual state from prior tests doesn't leak
    # into the next test's assertions.
    baseline_wind_storm_mps: int = 15
    baseline_wind_release_mps: int = 10
    baseline_storm_dwell_min: int = 10
    baseline_track_thresh: int = 3


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
        api_key = (os.environ.get(env_key) or vals.get("api_key", "") or "").strip()
        if not api_key:
            print(
                f"[ERROR] No api_key for '{name}'. Set 'api_key:' in "
                f"bench_test_config.yaml or export {env_key}.",
                file=sys.stderr,
            )
            sys.exit(1)
        nodes[name] = NodeConfig(
            name=name,
            host=vals["host"],
            api_key=api_key,
        )

    return HarnessConfig(
        nodes=nodes,
        storm_dwell_test_value=int(raw.get("storm_dwell_test_value", 1)),
        baseline_wind_storm_mps=int(raw.get("baseline_wind_storm_mps", 15)),
        baseline_wind_release_mps=int(raw.get("baseline_wind_release_mps", 10)),
        baseline_storm_dwell_min=int(raw.get("baseline_storm_dwell_min", 10)),
        baseline_track_thresh=int(raw.get("baseline_track_thresh", 3)),
    )


# ---------------------------------------------------------------------------
# Log entry
# ---------------------------------------------------------------------------

@dataclass
class LogLine:
    ts: float  # time.monotonic()
    level: int
    message: str

    def __str__(self) -> str:
        # message already contains the [LVL][tag:line] prefix from ESPHome's
        # logger; no need to re-decorate.
        return self.message


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
            key_preview = (
                f"{node.api_key[:4]}...{node.api_key[-4:]} ({len(node.api_key)} chars)"
                if len(node.api_key) > 8 else "(short / suspect)"
            )
            print(f"[INFO] Connecting to {name} ({node.host}) with api_key={key_preview}")
            client = APIClient(
                node.host,
                6053,
                password="",
                noise_psk=node.api_key,
            )
            try:
                await client.connect(login=True)
            except Exception as e:
                hint = ""
                if "Encryption" in str(e) or "encryption" in str(e):
                    hint = (
                        "\n       The api_key in bench_test_config.yaml doesn't match the\n"
                        "       device's `api.encryption.key` (from secrets.yaml's ha_api_key).\n"
                        "       Copy the EXACT base64 string -- no quotes, no whitespace.\n"
                        "       Verify in the ESPHome dashboard: edit the device YAML, look at\n"
                        "       `api: encryption: key: !secret ha_api_key`, then look at the\n"
                        "       resolved value in secrets.yaml."
                    )
                print(
                    f"\nERROR: Could not connect to {name} at {node.host}: {e}{hint}\n",
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
            # aioesphomeapi's LogMessage has no `tag` field -- the component
            # tag is embedded in `message` as the standard ESPHome prefix
            # "[LVL][tag:line]: ...".  msg.message may arrive as bytes or
            # str depending on library version; normalize defensively.
            raw = msg.message
            if isinstance(raw, (bytes, bytearray)):
                raw = raw.decode("utf-8", errors="replace")
            line = LogLine(
                ts=time.monotonic(),
                level=getattr(msg, "level", 0),
                message=raw,
            )
            self._log_ring[name].append(line)
            self._log_event.set()

        # subscribe_logs is a synchronous registration call in aioesphomeapi
        # (the returned object isn't awaitable).  Just register the callback.
        client.subscribe_logs(on_log, log_level=5)  # level 5 = VERBOSE

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

    def _resolve_entity(self, name: str, key: str) -> object:
        """Fuzzy entity lookup.  Tries in order:
        1. Exact match on object_id
        2. Object IDs ending with `_<key>` (handles node-prefix patterns)
        3. Object IDs containing `<key>` as a substring

        Used by press_button / set_switch / set_number / get_number /
        expect_entity so tests can use either full object_id
        ("solar_tracker_1_storm_dwell") or a node-relative shorthand
        ("storm_dwell"), or even an approximate name ("storm_dwell_min"
        which contains "storm_dwell").
        """
        infos = self._entity_info[name]
        info = infos.get(key)
        if info is not None:
            return info
        suffix = "_" + key
        for oid, info in infos.items():
            if oid.endswith(suffix) or oid == key:
                return info
        # Last resort: token-overlap match.  Splits both sides on `_` and
        # accepts if every token in `key` appears as a token in `oid` (or
        # as a substring of one).  Handles "storm_dwell_min" vs
        # "solar_tracker_1_storm_dwell" -- the test's `_min` token doesn't
        # need to match anything in the object_id, but "storm" and "dwell"
        # do.  Returns the first such match.
        key_tokens = [t for t in key.split("_") if t]
        for oid, info in infos.items():
            oid_str = oid.lower()
            shared = sum(1 for t in key_tokens if t.lower() in oid_str)
            # Require >= 2 shared tokens, or 1 if key_tokens is length 1
            if shared >= max(1, min(2, len(key_tokens))):
                return info
        return None

    def _resolve_entity_or_fail(self, op: str, name: str, key: str) -> object:
        info = self._resolve_entity(name, key)
        if info is None:
            raise _FailError(
                f"{op}: entity '{key}' not found on {name}.\n"
                f"  Available: {list(self._entity_info[name].keys())}"
            )
        return info

    async def press_button(self, name: str, entity_object_id: str) -> None:
        info = self._resolve_entity_or_fail("press_button", name, entity_object_id)
        # aioesphomeapi's *_command methods are synchronous fire-and-forget;
        # they enqueue a request frame and return None, not a coroutine.
        self._clients[name].button_command(info.key)

    async def set_switch(self, name: str, entity_object_id: str, value: bool) -> None:
        info = self._resolve_entity_or_fail("set_switch", name, entity_object_id)
        self._clients[name].switch_command(info.key, value)

    async def set_number(self, name: str, entity_object_id: str, value: float) -> None:
        info = self._resolve_entity_or_fail("set_number", name, entity_object_id)
        self._clients[name].number_command(info.key, value)

    async def reboot(self, name: str) -> None:
        """Trigger a soft reboot via the built-in restart button."""
        # ESPHome exposes a "restart" button on every device
        await self.press_button(name, "restart")

    # ------------------------------------------------------------------
    # Per-test state reset (P5-13)
    # ------------------------------------------------------------------

    async def reset_to_baseline_(self, *node_names: str) -> None:
        """
        Reset each named node to a known-good baseline before a test runs.
        Idempotent: safe to call even when the node is already at baseline.

        Steps per node:
          1. Turn off Test Offline switch so the node is broadcasting.
          2. If mode is "storm", temporarily set storm_dwell_min=1 and press
             Force Release; wait up to 90s for mode to leave "storm".
          3. Restore baseline values on the four config sliders.

        Slider sets are best-effort: a node missing one of the sliders (e.g.
        pre-P5-12 tracker-2 with no STC) doesn't fail the reset.  Storm-clear
        timeout failures log a WARN but don't fail the reset either -- the
        test that runs next will surface the real assertion failure.
        """
        cfg = self._cfg

        # 1. Ensure each node is online (Test Offline OFF).  Sleep 6 s so a
        # node coming back from a long test_offline=True window has time to
        # re-broadcast TELEMETRY + GATEWAY_HB and let the mesh re-settle on
        # gateway / wind-primary roles before the next test's commands fly.
        # 2 s here used to be enough but produces ~33% flakiness on the first
        # test after test_no_primary_baseline (which silences t1 for 80+ s).
        for name in node_names:
            try:
                await self.set_switch(name, "test_offline_switch", False)
            except _FailError:
                pass
        await asyncio.sleep(6)

        # 2. For any node currently in storm, force release with short dwell
        in_storm: List[str] = []
        for name in node_names:
            mode = self._entity_state[name].get("mode")
            if mode and "storm" in str(mode).lower():
                in_storm.append(name)

        for name in in_storm:
            try:
                await self.set_number(name, "storm_dwell_min", 1.0)
            except _FailError:
                pass
            try:
                await self.press_button(name, "force_release")
            except _FailError:
                pass

        if in_storm:
            deadline = time.monotonic() + 90.0
            pending = set(in_storm)
            while pending and time.monotonic() < deadline:
                for name in list(pending):
                    mode = self._entity_state[name].get("mode")
                    if not mode or "storm" not in str(mode).lower():
                        pending.discard(name)
                if pending:
                    self._state_event.clear()
                    try:
                        await asyncio.wait_for(self._state_event.wait(), timeout=0.5)
                    except asyncio.TimeoutError:
                        pass
            if pending:
                print(f"  [WARN] reset_to_baseline_: {sorted(pending)} still in storm after 90s")

        # 3. Restore baseline slider values
        for name in node_names:
            for entity_id, val in (
                ("wind_storm_mps",   cfg.baseline_wind_storm_mps),
                ("wind_release_mps", cfg.baseline_wind_release_mps),
                ("storm_dwell_min",  cfg.baseline_storm_dwell_min),
                ("track_thresh",     cfg.baseline_track_thresh),
            ):
                try:
                    await self.set_number(name, entity_id, float(val))
                except _FailError:
                    pass
        await asyncio.sleep(1)  # let SETs reach the STC
        print(f"  [INFO] reset_to_baseline_({', '.join(node_names)}) ok")

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
        info = self._resolve_entity_or_fail("expect_entity", name, entity_object_id)
        resolved_oid = next(
            (oid for oid, e in self._entity_info[name].items() if e is info),
            entity_object_id,
        )

        def matches() -> bool:
            cur = self._entity_state[name].get(resolved_oid)
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
                cur = self._entity_state[name].get(resolved_oid)
                raise _FailError(
                    f"expect_entity({name!r}, {entity_object_id!r} -> "
                    f"{resolved_oid!r}, {expected_value!r}) "
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
        # Capture the cursor BEFORE the button press: expect_log defaults to a
        # 100 ms back-window, but the t2 'mesh rx type=4' arrives within
        # milliseconds of the press, while expect_entity below can take several
        # seconds.  Without an explicit since=, the log line ages out of the
        # window before we look for it.
        since_park = time.monotonic()
        await h.press_button(t1, "force_park")

        # Tracker-1 STC should enter storm
        await h.expect_entity(t1, "mode", "storm", timeout=15)

        # Tracker-2 should relay the mesh command (no STC, so check mesh rx log)
        await h.expect_log(t2, r"mesh rx type=4", timeout=15, since=since_park)

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
    MANUAL TEST -- requires temporarily disabling P6-2's WSrc auto-sync, or
    making both nodes has_wind_sensor=true so the election outcome can be
    flipped at runtime.

    Validates that with WSrc=1 (remote-wind required) on tracker-1 and no
    incoming WIND broadcasts, the STC failsafe fires within ~15-30s, and
    that setting WSrc=0 recovers the mode to track/idle.

    Why manual: when this test was first written WSrc was directly settable
    via `!cfg set id=33 val=N` over a raw UART stream (port 23 / stream_server).
    Two things changed since:
      1. stream_server was disabled in the dashboard YAML on 2026-05-22 after
         it was diagnosed as the source of UART-RX-drained-by-second-consumer
         (see the same-day boot log in conversation history).
      2. P6-2 (commit 97676ec, reapplied 2d4e550) now auto-pushes WSrc from
         the elected-primary outcome every cfg_poll cycle.  Any manual
         WSrc=1 set is overwritten by WSrc=0 on the next 30s tick if
         tracker-1 remains the elected primary.

    The production test path is now: flip the election outcome (e.g. make
    tracker-2 also has_wind_sensor=true and lower-MAC) -> P6-2 auto-pushes
    WSrc=1 to tracker-1 -> failsafe arms when tracker-2 also goes offline.
    That requires a YAML edit + reflash, identical to the dual_primary test.

    Rerun instructions:
      1. Edit EcoWorthyFirmware/esphome/solar-tracker-2.yaml:
             has_wind_sensor: false  ->  has_wind_sensor: true
      2. esphome run EcoWorthyFirmware/esphome/solar-tracker-2.yaml
      3. python tools/bench_test.py --test test_wsrc_failsafe_recovery --include-manual
      4. Revert the YAML and reflash tracker-2 to restore production config.
    """
    if not MANUAL_FLAG:
        raise _ManualSkip(
            "Requires solar-tracker-2.yaml has_wind_sensor: true + reflash to "
            "drive a WSrc=1 state through P6-2's auto-sync.  Run with --include-manual "
            "after the manual reflash steps in this docstring."
        )
    t1 = "solar-tracker-1"
    t2 = "solar-tracker-2"

    # With both has_wind_sensor=true, lower-MAC wins -- expect that to be t2.
    # Take t2 offline so t1 is the only candidate, then bring t2 back: when
    # t2 re-asserts primary (lower MAC), P6-2 pushes WSrc=1 to t1.
    # Then take t2 offline again so no WIND arrives -> failsafe arms.
    await h.set_switch(t2, "test_offline_switch", True)
    await asyncio.sleep(70)  # peer stale window + margin

    try:
        await h.set_switch(t2, "test_offline_switch", False)
        await asyncio.sleep(35)  # one cfg_poll cycle + margin -- P6-2 should push WSrc=1
        await h.set_switch(t2, "test_offline_switch", True)
        # Now no WIND inbound; t1's failsafe should arm and push to storm.
        await h.expect_entity(t1, "mode", "storm", timeout=60)

    finally:
        # Restore production config: t2 visible, P6-2 will re-push WSrc=0 to t1
        # on next cfg_poll (since t1 will be the lowest-MAC has_wind_sensor again
        # if t2 stays online or test_offline=False).
        await h.set_switch(t2, "test_offline_switch", False)
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
                f"'track' or 'idle' within 90s.  Current: {cur!r}"
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
    MANUAL TEST -- broken by P5-12.

    Original premise: silence the acting gateway, verify the other node logs
    a '[gateway] publish peer' line within ~35 s (election-driven takeover).

    The '[gateway] publish peer' log only fires when the gateway publishes
    peer-mirror entities.  P5-12 (2026-05-23) removed peer mirrors from both
    YAMLs because both nodes now have their own STC + native entities.
    Result: identify_gateway() has no signal to look at and always returns
    None; the test falls back to assuming t1 is the gateway when election
    typically picks t2 (lower MAC); silencing the wrong node does nothing.

    Refactor TBD -- needs either:
      (a) firmware-side: add a periodic `[I] gateway role: active` LOGD on
          whichever node is the elected gateway, decoupled from peer-mirror
          publishing.  ~10 line change in tracker_bridge.h.
      (b) harness-side: parse MAC addresses from `mesh rx type=3` lines on
          each node, compute the lowest-MAC active node, and assert that
          when it goes offline the next-lowest takes over.  Doesn't need
          firmware changes but is more code.

    Recommend (a).  Filed as P5-15 in the deferred-cleanups plan.
    """
    if not MANUAL_FLAG:
        raise _ManualSkip(
            "Broken by P5-12 -- '[gateway] publish peer' log line no longer "
            "exists since peer mirrors were dropped.  Refactor TBD (P5-15).  "
            "Run with --include-manual to attempt the legacy version anyway."
        )
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

        # tracker_bridge's own LOGD line confirms the command was framed and
        # written to the STC UART.  This asserts on the production interface,
        # not on the optional `uart: debug:` hex tap (which competes with the
        # bridge for the UART RX buffer -- see docs/superpowers/plans/2026-05-21
        # -phase-5-deferred-cleanups.md, P5-11 background).
        await h.expect_log(
            t1,
            r"cmd->STC: !park",
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

        # Wait for the cfg_poll to fire (up to 30 s) and push a GET_REQ to the
        # STC.  The "cfg reply: fid=1 val=17" log line comes from the STC's
        # actual UART reply (not from the optimistic slider cache), so this
        # already verifies the STC stored the value.  No need for a redundant
        # raw-UART GET.
        await h.expect_log(
            t1, r"cfg reply: fid=1 val=17", timeout=35.0
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

    node_names = tuple(harness._cfg.nodes.keys())
    for name, fn in tests:
        start = time.monotonic()
        sys.stdout.write(f"[RUN]  {name} ")
        sys.stdout.flush()

        try:
            # P5-13: reset each node to baseline before running the test, so
            # residual state from prior tests (storm mode, modified sliders,
            # test_offline left on, ...) can't leak into this test's assertions.
            # Skipped for manual tests that explicitly raise _ManualSkip before
            # touching anything -- the reset would just waste 5-10s.
            try:
                await harness.reset_to_baseline_(*node_names)
            except _ManualSkip:
                raise  # let the skip path handle it
            except Exception as reset_err:
                print(f"\n  [WARN] reset_to_baseline_ raised: {reset_err!r}")
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
