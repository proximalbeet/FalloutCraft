"""A stand-in for the Fallout 4 plugin, to test the Minecraft half on Linux without the game.

It speaks the plugin's protocol (cam, ground, view, cmd), places a gold block and a primed TNT in front of the
camera, waits for the explosion event to come back, and saves the exported frame (world colour, depth, overlay)
from /dev/shm/mcpassthrough_frame as PNGs.

usage: toolchain/pyenv/bin/python host/fo4_fakehost.py [outdir]
"""
import asyncio
import json
import mmap
import struct
import sys
import time
from pathlib import Path

import numpy as np
import websockets
from PIL import Image

SHM = "/dev/shm/mcpassthrough_frame"
OUT = Path(sys.argv[1] if len(sys.argv) > 1 else "fakehost_out")


def read_frame():
	with open(SHM, "rb") as f:
		m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
	magic, version, header, slots, stride = struct.unpack_from("<iiiiq", m, 0)
	assert magic == 0x5450434D, hex(magic)
	published, latest = struct.unpack_from("<qi", m, 32)
	if latest < 0:
		return published, None
	d = 256 + 128 * latest
	seq, mcframe, hostframe, w, h, near, far, fov, flags = struct.unpack_from("<qqqiifffi", m, d)
	base = header + stride * latest
	n = w * h * 4
	world = np.frombuffer(m, np.uint8, n, base).reshape(h, w, 4)
	depth = np.frombuffer(m, np.float32, w * h, base + n).reshape(h, w)
	overlay = np.frombuffer(m, np.uint8, n, base + 2 * n).reshape(h, w, 4)
	info = dict(slot=latest, mcframe=mcframe, hostframe=hostframe, w=w, h=h, near=near, far=far, fov=fov, flags=flags)
	return published, (info, world.copy(), depth.copy(), overlay.copy())


async def main():
	OUT.mkdir(parents=True, exist_ok=True)
	async with websockets.connect("ws://127.0.0.1:25599", max_size=None) as ws:
		print("hello:", await ws.recv())
		await ws.send(json.dumps({"t": "view", "w": 1280, "h": 720}))
		await ws.send(json.dumps({"t": "hud", "hidden": False}))
		await ws.send(json.dumps({"t": "key", "k": "escape", "down": True}))
		await ws.send(json.dumps({"t": "clear"}))
		cols = []
		for x in range(-12, 13):
			for z in range(-12, 13):
				cols += [x, z, 63, 63]
		await ws.send(json.dumps({"t": "ground", "c": cols}))

		explosions = []

		async def reader():
			async for msg in ws:
				m = json.loads(msg)
				if m.get("t") == "explosion":
					explosions.append(m)
					print("explosion event:", m)

		task = asyncio.create_task(reader())
		# the camera: standing at z = 6 on the ground, looking north (-z), a little down
		t0 = time.monotonic()
		frame = 0
		placed = False
		while time.monotonic() - t0 < 12.0:
			frame += 1
			await ws.send(json.dumps({"t": "cam", "f": frame, "p": [0.5, 65.62, 6.5], "r": [180.0, 15.0, 0.0], "fov": 55.0,
				"fp": True, "pl": [0.5, 64.0, 6.5], "h": 180.0}))
			if not placed and time.monotonic() - t0 > 3.0:
				placed = True
				await ws.send(json.dumps({"t": "cmd", "c": "setblock -2 64 0 minecraft:gold_block"}))
				await ws.send(json.dumps({"t": "cmd", "c": "summon minecraft:tnt 2.5 64 0.5 {fuse:60}"}))
				print("placed a gold block and primed TNT")
			if placed and frame % 60 == 0 and not (OUT / "before_world.png").exists() and time.monotonic() - t0 > 4.0:
				pub, fr = read_frame()
				if fr:
					info, world, depth, overlay = fr
					print("frame:", info, "published", pub)
					Image.fromarray(world[::-1] if info["flags"] & 2 else world).save(OUT / "before_world.png")
					Image.fromarray(overlay[::-1] if info["flags"] & 2 else overlay).save(OUT / "before_overlay.png")
					d = depth[::-1] if info["flags"] & 2 else depth
					Image.fromarray((np.clip(d, 0, 1) * 255).astype(np.uint8)).save(OUT / "before_depth.png")
					cover = (world[..., 3] > 0).mean()
					print(f"world coverage {cover:.1%}, depth range {depth.min():.4f}..{depth.max():.4f}")
			await asyncio.sleep(1 / 60)
		task.cancel()
		pub, _ = read_frame()
		print("frames published:", pub, "explosions:", len(explosions))
		return 0 if explosions and (OUT / "before_world.png").exists() else 1


if __name__ == "__main__":
	sys.exit(asyncio.run(main()))
