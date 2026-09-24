#!/usr/bin/env python3
"""Give the bridge's ZHA entities stable ids and names, by endpoint.

ZHA names the analog-input entities of the bridge generically and binds them by
unique id `<ieee>-<endpoint>-12-analog_input`. When a firmware adds a sensor, the
endpoints shift and ZHA keeps the *old* entity ids, so ids and values no longer
match. This script maps every endpoint to `sensor.<prefix>_<type>` and an English
name, in the order of the sensor list in esp32c6.yaml. The renaming can be
cyclic (every id moves one on), so it runs in two passes over temporary ids.

Credentials come from the environment (HASS_URL, HASS_TOKEN = a long-lived
access token) or from --env-file. The bridge is found by its Zigbee model string
(`zigbee: model:` in esp32c6.yaml) unless --ieee is given.

Usage:
    python3 tools/ha_fix_bridge_entities.py [--dry-run] [--prefix inverter_zb]
        [--model "HMS-2000-4WB Bridge"] [--ieee xx:xx:...] [--env-file FILE]
        [--hide-extras | --unhide]

--hide-extras hides every entity except the AC power (hidden_by: user), which
keeps auto-generated dashboards short; --unhide shows them all again.

Needs the websockets package (a venv, see the README).
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from pathlib import Path

import websockets

# Endpoint -> (entity suffix, name). The order is the order of the sensors in
# esp32c6.yaml; the status stays last so the measurements keep their endpoints.
LAYOUT: dict[int, tuple[str, str]] = {
    1: ("ac_power", "AC power"),
    2: ("ac_voltage", "AC voltage"),
    3: ("ac_current", "AC current"),
    4: ("ac_frequency", "AC frequency"),
    5: ("temperature", "Temperature"),
    6: ("energy_total", "Energy total"),
    7: ("energy_daily", "Energy today"),
    8: ("port1_power", "Port 1 power"),
    9: ("port1_voltage", "Port 1 voltage"),
    10: ("port1_current", "Port 1 current"),
    11: ("port2_power", "Port 2 power"),
    12: ("port2_voltage", "Port 2 voltage"),
    13: ("port2_current", "Port 2 current"),
    14: ("port3_power", "Port 3 power"),
    15: ("port3_voltage", "Port 3 voltage"),
    16: ("port3_current", "Port 3 current"),
    17: ("port4_power", "Port 4 power"),
    18: ("port4_voltage", "Port 4 voltage"),
    19: ("port4_current", "Port 4 current"),
    20: ("status", "Session status"),
}
VISIBLE = "ac_power"  # the one entity --hide-extras leaves visible


def load_env(env_file: str | None) -> tuple[str, str]:
    env = dict(os.environ)
    if env_file:
        for line in Path(env_file).expanduser().read_text().splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                env.setdefault(key.strip(), value.strip())
    url, token = env.get("HASS_URL", "").rstrip("/"), env.get("HASS_TOKEN", "")
    if not url or not token:
        sys.exit("HASS_URL/HASS_TOKEN missing (environment or --env-file)")
    return url, token


def endpoint_of(unique_id: str) -> int | None:
    parts = unique_id.split("-")
    if len(parts) > 1 and parts[1].isdigit():
        return int(parts[1])
    return None


async def run(args: argparse.Namespace) -> int:
    url, token = load_env(args.env_file)
    ws_url = url.replace("https://", "wss://").replace("http://", "ws://") + "/api/websocket"
    async with websockets.connect(ws_url, max_size=8 * 1024 * 1024) as ws:
        await ws.recv()
        await ws.send(json.dumps({"type": "auth", "access_token": token}))
        if json.loads(await ws.recv()).get("type") != "auth_ok":
            sys.exit("authentication failed")

        msg_id = 0

        async def cmd(payload: dict) -> dict:
            nonlocal msg_id
            msg_id += 1
            await ws.send(json.dumps({"id": msg_id, **payload}))
            while True:
                msg = json.loads(await ws.recv())
                if msg.get("id") == msg_id:
                    return msg

        async def update(entity_id: str, **fields) -> bool:
            res = await cmd({"type": "config/entity_registry/update", "entity_id": entity_id, **fields})
            if not res.get("success"):
                print(f"  ERROR {entity_id}: {res.get('error')}")
                return False
            return True

        ieee = args.ieee
        if ieee is None:
            devices = (await cmd({"type": "config/device_registry/list"})).get("result", [])
            found = [d for d in devices if d.get("model") == args.model]
            if len(found) != 1:
                sys.exit(f"{len(found)} ZHA devices with model {args.model!r}; pass --ieee")
            ieee = next((i[1] for i in found[0].get("identifiers", []) if i[0] == "zha"), None)
            if ieee is None:
                sys.exit(f"the device {args.model!r} is not a ZHA device; pass --ieee")
        print(f"bridge: {ieee}")

        registry = (await cmd({"type": "config/entity_registry/list"})).get("result", [])
        todo = []
        for entity in registry:
            unique_id = entity.get("unique_id") or ""
            # only the measurements: ZHA's LQI/RSSI diagnostics share the last endpoint
            if not (unique_id.startswith(ieee) and unique_id.endswith("-12-analog_input")):
                continue
            ep = endpoint_of(unique_id)
            if ep in LAYOUT and entity["entity_id"].startswith("sensor."):
                todo.append((entity, ep))
        todo.sort(key=lambda item: item[1])
        print(f"{len(todo)} sensors found; endpoints: {[ep for _, ep in todo]}")

        if args.hide_extras or args.unhide:
            for entity, ep in todo:
                hidden = None if args.unhide or LAYOUT[ep][0] == VISIBLE else "user"
                print(f"  {entity['entity_id']}: hidden={hidden}")
                if not args.dry_run:
                    await update(entity["entity_id"], hidden_by=hidden)
            return 0

        tmp_of = {ep: f"sensor.{args.prefix}_tmp{ep:02d}" for _, ep in todo}
        print("pass 1: temporary ids")
        for entity, ep in todo:
            if entity["entity_id"] == tmp_of[ep]:
                continue
            if args.dry_run:
                print(f"  {entity['entity_id']} -> {tmp_of[ep]}")
            else:
                await update(entity["entity_id"], new_entity_id=tmp_of[ep])

        print("pass 2: final ids and names")
        for _, ep in todo:
            suffix, name = LAYOUT[ep]
            target = f"sensor.{args.prefix}_{suffix}"
            if args.dry_run:
                print(f"  {tmp_of[ep]} -> {target} ({name})")
            elif await update(tmp_of[ep], new_entity_id=target, name=name):
                print(f"  {target}: {name}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--prefix", default="inverter_zb")
    parser.add_argument("--model", default="HMS-2000-4WB Bridge")
    parser.add_argument("--ieee")
    parser.add_argument("--env-file")
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--hide-extras", action="store_true")
    group.add_argument("--unhide", action="store_true")
    return asyncio.run(run(parser.parse_args()))


if __name__ == "__main__":
    sys.exit(main())
