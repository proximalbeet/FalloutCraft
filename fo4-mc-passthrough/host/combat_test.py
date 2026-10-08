"""Shared health and Fallout shots, tested against the running Minecraft mod without Fallout.

1. health 0.5 from the "host": Steve's hearts drop to half (overlay saved as combat_hearts.png)
2. three shots at a zombie (NoAI) in front of the camera: it dies ("shot hit" in Minecraft's log)
3. a hostile zombie next to Steve: its hits come back as {"t":"pdmg"} events

usage: toolchain/pyenv/bin/python host/combat_test.py [outdir]
"""
import asyncio
import json
import sys
import time
from pathlib import Path

import websockets

sys.path.insert(0, str(Path(__file__).parent))
from fo4_fakehost import read_frame  # noqa: E402  (reads /dev/shm/mcpassthrough_frame)

OUT = Path(sys.argv[1] if len(sys.argv) > 1 else "combat_out")
CAM = {"t": "cam", "p": [0.5, 65.62, 6.5], "r": [180.0, 0.0, 0.0], "fov": 55.0, "fp": True, "pl": [0.5, 64.0, 6.5], "h": 180.0}


async def main():
	OUT.mkdir(parents=True, exist_ok=True)
	async with websockets.connect("ws://127.0.0.1:25599", max_size=None) as ws:
		await ws.recv()
		for m in ({"t": "view", "w": 1280, "h": 720}, {"t": "hud", "hidden": False}, {"t": "key", "k": "escape", "down": True},
				{"t": "hand", "show": False}, {"t": "clear"}):
			await ws.send(json.dumps(m))
		await ws.send(json.dumps({"t": "ground", "c": [v for x in range(-12, 13) for z in range(-12, 13) for v in (x, z, 63, 63)]}))
		events = []

		async def reader():
			async for msg in ws:
				m = json.loads(msg)
				if m.get("t") in ("pdmg", "explosion"):
					events.append(m)
					print("event:", m)

		task = asyncio.create_task(reader())
		frame = 0

		async def run(seconds, every=None):
			nonlocal frame
			t0 = time.monotonic()
			while time.monotonic() - t0 < seconds:
				frame += 1
				await ws.send(json.dumps(dict(CAM, f=frame)))
				if every:
					await every(frame)
				await asyncio.sleep(1 / 60)

		await run(2.0)  # the setup commands (survival, rules) run 10 ticks after join; give them time
		await ws.send(json.dumps({"t": "cmd", "c": "kill @e[type=minecraft:zombie]"}))
		await ws.send(json.dumps({"t": "health", "f": 0.5}))
		await run(1.5)
		from PIL import Image
		_, fr = read_frame()
		if fr:
			info, world, depth, overlay = fr
			Image.fromarray(overlay[::-1]).save(OUT / "combat_hearts.png")

		print("-- shots")
		await ws.send(json.dumps({"t": "cmd", "c": "summon minecraft:zombie 0.5 64 0.5 {NoAI:1b,Tags:[\"target\"]}"}))
		await run(1.0)
		for _ in range(3):
			await ws.send(json.dumps({"t": "shot", "o": CAM["p"], "d": [0.0, -0.12, -1.0], "dmg": 7.0}))
			await run(0.3)

		print("-- a hostile zombie next to Steve")
		await ws.send(json.dumps({"t": "health", "f": 1.0}))
		await ws.send(json.dumps({"t": "cmd", "c": "summon minecraft:zombie 0.5 64 5.0"}))
		await run(6.0)
		await ws.send(json.dumps({"t": "cmd", "c": "kill @e[type=minecraft:zombie]"}))
		task.cancel()
		pdmg = [e for e in events if e["t"] == "pdmg"]
		print(f"pdmg events: {len(pdmg)}")
		return 0 if pdmg else 1


sys.exit(asyncio.run(main()))
