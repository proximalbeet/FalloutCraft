"""Steve on the Pip-Boy's STAT page: a converter that runs on the user's own files.

Fallout draws the STATUS figure from Interface/Components/ConditionClips: Condition_Body_0..16 (a headless walk
cycle, one per crippled-limb combination, with an empty "Head_mc" clip moved frame by frame) and Condition_Head
(the faces). This renders Minecraft's player model, textured with Steve's skin from the user's Minecraft jar, for
every one of those shapes: same 3/4 view, arms and legs swinging through the walk, the faces with the same
expressions. The art is white with brightness as alpha, like the originals, so the Pip-Boy tints it.

The shapes are swapped for the renders with JPEXS FFDec, which keeps each shape's bounds and scales the image into
them; the head clip's position (frame by frame) says where the neck is, so the body joins the head.

usage: pyenv/bin/python tools/pipboy_steve.py --skin steve.png --swfdir <extracted Interface/..> --ffdec ffdec.jar
       --java <java> --out <dir with Interface/Components/ConditionClips/*.swf to install> [--preview sheet.png]
"""
import argparse
import math
import re
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

CLIPS = "Interface/Components/ConditionClips"
PX = 4  # output pixels per original pixel (1 px = 20 twips)

# ---------------------------------------------------------------- Minecraft's player model (wide arms)
# box: size (w, h, d) in skin pixels, the skin's UV origin, pivot (model space, y up), offset of the box's min corner from the pivot
BOXES = {
	"head": dict(size=(8, 8, 8), uv=(0, 0), pivot=(0, 24, 0), off=(-4, 0, -4)),
	"body": dict(size=(8, 12, 4), uv=(16, 16), pivot=(0, 12, 0), off=(-4, 0, -2)),
	"rarm": dict(size=(4, 12, 4), uv=(40, 16), pivot=(-6, 22, 0), off=(-2, -10, -2)),
	"larm": dict(size=(4, 12, 4), uv=(32, 48), pivot=(6, 22, 0), off=(-2, -10, -2)),
	"rleg": dict(size=(4, 12, 4), uv=(0, 16), pivot=(-2, 12, 0), off=(-2, -12, -2)),
	"lleg": dict(size=(4, 12, 4), uv=(16, 48), pivot=(2, 12, 0), off=(-2, -12, -2)),
}


def face_uvs(uv, size):
	"""The skin rectangles of a box's faces (Minecraft's cube-net layout)."""
	u, v = uv
	w, h, d = size
	return {
		"top": (u + d, v, w, d), "bottom": (u + d + w, v, w, d),
		"right": (u, v + d, d, h), "front": (u + d, v + d, w, h),
		"left": (u + d + w, v + d, d, h), "back": (u + d + w + d, v + d, w, h),
	}


def rot_x(a):
	c, s = math.cos(a), math.sin(a)
	return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])


def rot_y(a):
	c, s = math.cos(a), math.sin(a)
	return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def box_faces(name, skin, limb_angle):
	"""Every texel of a box as a world-space quad with its colour: [(corners 4x3, normal, rgba)]."""
	b = BOXES[name]
	w, h, d = b["size"]
	ox, oy, oz = b["off"]
	piv = np.array(b["pivot"], float)
	r = rot_x(limb_angle)
	uvs = face_uvs(b["uv"], b["size"])
	quads = []

	def emit(face, origin, du, dv, normal, nu, nv):
		fu, fv, fw, fh = uvs[face]
		for j in range(nv):
			for i in range(nu):
				c = skin[fv + j, fu + i]
				if c[3] < 10:
					continue
				p0 = origin + du * i + dv * j
				corners = np.array([p0, p0 + du, p0 + du + dv, p0 + dv])
				quads.append(((corners @ r.T) + piv, r @ normal, c))

	x0, y0, z0 = ox, oy, oz
	x1, y1, z1 = ox + w, oy + h, oz + d
	# the model faces -z (towards the viewer at yaw 0); texture rows run top to bottom
	emit("front", np.array([x1, y1, z0], float), np.array([-1, 0, 0.]), np.array([0, -1, 0.]), np.array([0, 0, -1.]), w, h)
	emit("back", np.array([x0, y1, z1], float), np.array([1, 0, 0.]), np.array([0, -1, 0.]), np.array([0, 0, 1.]), w, h)
	emit("right", np.array([x1, y1, z1], float), np.array([0, 0, -1.]), np.array([0, -1, 0.]), np.array([1, 0, 0.]), d, h)
	emit("left", np.array([x0, y1, z0], float), np.array([0, 0, 1.]), np.array([0, -1, 0.]), np.array([-1, 0, 0.]), d, h)
	emit("top", np.array([x1, y1, z1], float), np.array([-1, 0, 0.]), np.array([0, 0, -1.]), np.array([0, 1, 0.]), w, d)
	emit("bottom", np.array([x1, y0, z0], float), np.array([-1, 0, 0.]), np.array([0, 0, 1.]), np.array([0, -1, 0.]), w, d)
	return quads


def render(skin, parts, angles, yaw, pitch, scale, size, anchor_model, anchor_px):
	"""Orthographic render of some boxes: model point anchor_model lands on output pixel anchor_px."""
	view = rot_x(pitch) @ rot_y(yaw)
	light = np.array([0.35, 0.6, -0.7])
	light /= np.linalg.norm(light)
	quads = []
	for name in parts:
		quads += box_faces(name, skin, angles.get(name, 0.0))
	img = Image.new("RGBA", size, (0, 0, 0, 0))
	draw = ImageDraw.Draw(img)
	a = view @ np.array(anchor_model, float)
	drawn = []
	for corners, normal, c in quads:
		n = view @ normal
		if n[2] >= -1e-6:  # facing away (the viewer looks along +z)
			continue
		p = corners @ view.T
		shade = 0.62 + 0.38 * max(0.0, float(np.dot(n, light)))
		drawn.append((p[:, 2].mean(), p, c, shade))
	drawn.sort(key=lambda t: -t[0])  # far first
	for _, p, c, shade in drawn:
		pts = [((x - a[0]) * scale + anchor_px[0], -(y - a[1]) * scale + anchor_px[1]) for x, y, _ in p]
		draw.polygon(pts, fill=(int(c[0] * shade), int(c[1] * shade), int(c[2] * shade), 255))
	return img


def pipboy_style(img):
	"""White, with brightness as alpha (the Pip-Boy tints it), and a clear gap around the silhouette like the originals' ink lines."""
	a = np.asarray(img).astype(float)
	lum = (0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]) / 255.0
	inside = a[..., 3] > 0
	if inside.any():
		# stretch what's there to the full range (Steve's colours are all mid-tones), a little gamma for punch
		lo, hi = np.percentile(lum[inside], [2, 98])
		lum = np.clip((lum - lo) / max(hi - lo, 1e-3), 0.0, 1.0) ** 0.8
	alpha = np.where(inside, 0.18 + 0.82 * lum, 0.0)
	out = np.zeros_like(a)
	out[..., :3] = 255
	out[..., 3] = alpha * 255
	return Image.fromarray(out.astype(np.uint8))


# ---------------------------------------------------------------- faces
def expression(skin, kind, bandage, zombie=None):
	"""Steve's face (front of the head, skin (8..15, 8..15)) with the head clip's expression painted in. The Vault
	Boy's ghoul face becomes a Minecraft zombie's head."""
	s = skin.copy()
	if kind == "dead" and zombie is not None:
		s[0:16, 0:32] = zombie[0:16, 0:32]
	f = s[8:16, 8:16]
	dark = np.array([40, 26, 16, 255], np.uint8)
	pale = np.array([235, 235, 235, 255], np.uint8)
	skin_c = f[6, 1].copy()
	if kind == "hurt":  # a frown
		f[6, 2:6] = skin_c
		f[7, 2:6] = skin_c
		f[6, 3:5] = dark
		f[7, 2] = dark
		f[7, 5] = dark
	elif kind == "tongue":
		f[7, 3:5] = np.array([210, 120, 130, 255], np.uint8)
	elif kind == "dead" and zombie is None:  # X eyes
		for ex in (1, 5):
			f[3:6, ex:ex + 2] = skin_c
			f[3, ex] = f[3, ex + 1] = dark
			f[4, ex] = dark
			f[5, ex] = f[5, ex + 1] = dark
	if bandage:
		f[1:3, :] = pale
		s[8:16, 0:8][1:3, :] = pale   # right side
		s[8:16, 16:24][1:3, :] = pale  # left side
	return s


# head shape order in Condition_Head (see the clip): healthy, hurt, tongue, dead, then hurt/tongue/dead bandaged
HEADS = [("normal", False), ("hurt", False), ("tongue", False), ("dead", False), ("hurt", True), ("tongue", True), ("dead", True)]


# ---------------------------------------------------------------- the clips
def swf_xml(java, ffdec, swf, out):
	subprocess.run([java, "-jar", ffdec, "-swf2xml", str(swf), str(out)], check=True, capture_output=True)
	return ET.parse(out).getroot()


def body_frames(root):
	"""[(shape id, bounds (xmin, ymin, xmax, ymax) twips, Head_mc (tx, ty) twips)] in timeline order."""
	frames, head = [], None
	pending = None
	tags = root.find("tags")
	for item in tags:
		t = item.get("type")
		if t in ("DefineShapeTag", "DefineShape2Tag", "DefineShape3Tag", "DefineShape4Tag"):
			r = item.find("shapeBounds")
			pending = (int(item.get("shapeId")), tuple(int(r.get(k)) for k in ("Xmin", "Ymin", "Xmax", "Ymax")))
		elif t == "PlaceObject2Tag" and item.get("depth") == "2":
			m = item.find("matrix")
			if m is not None:
				head = (int(m.get("translateX")), int(m.get("translateY")))
			if pending:
				frames.append((pending[0], pending[1], head))
				pending = None
		elif t == "PlaceObject2Tag" and item.get("depth") == "1" and pending and head is not None and item.get("characterId") is None:
			pass
	return frames


def head_shapes(root):
	return [(int(i.get("shapeId")), tuple(int(i.find("shapeBounds").get(k)) for k in ("Xmin", "Ymin", "Xmax", "Ymax")))
		for i in root.find("tags") if i.get("type", "").startswith("DefineShape")]


YAW = math.radians(-28)   # 3/4 view, facing right like the Vault Boy
PITCH = math.radians(8)


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("--skin", required=True)
	ap.add_argument("--zombie", help="zombie.png from the same jar: the ghoul faces become a zombie's head")
	ap.add_argument("--swfdir", required=True)
	ap.add_argument("--ffdec", required=True)
	ap.add_argument("--java", required=True)
	ap.add_argument("--out", required=True)
	ap.add_argument("--work", default="pipboy_work")
	ap.add_argument("--preview")
	a = ap.parse_args()
	skin = np.asarray(Image.open(a.skin).convert("RGBA")).copy()
	zombie = np.asarray(Image.open(a.zombie).convert("RGBA")).copy() if a.zombie else None
	src, out, work = Path(a.swfdir), Path(a.out), Path(a.work)
	(out / CLIPS).mkdir(parents=True, exist_ok=True)
	work.mkdir(parents=True, exist_ok=True)

	bodies = {}
	for swf in sorted((src / CLIPS).glob("Condition_Body_*.swf"), key=lambda p: int(re.findall(r"\d+", p.stem)[0])):
		bodies[swf] = body_frames(swf_xml(a.java, a.ffdec, swf, work / (swf.stem + ".xml")))
	head_swf = src / CLIPS / "Condition_Head.swf"
	heads = head_shapes(swf_xml(a.java, a.ffdec, head_swf, work / "head.xml"))

	# one size for Steve everywhere: the head is 8 Steve pixels across the head clip's width, and the body must fit
	# under every frame's neck
	head_w = min(b[2] - b[0] for _, b in heads)
	unit = head_w / 8.0
	for frames in bodies.values():
		for _, (x0, y0, x1, y1), (hx, hy) in frames:
			unit = min(unit, (y1 - hy) / 24.5)
	print(f"Steve: {unit:.1f} twips per skin pixel")

	previews = []
	for swf, frames in bodies.items():
		args = []
		k = len(frames)
		for i, (sid, (x0, y0, x1, y1), (hx, hy)) in enumerate(frames):
			phase = 2 * math.pi * i / k
			swing = math.radians(32) * math.sin(phase)
			angles = {"rarm": swing, "larm": -swing, "rleg": -swing, "lleg": swing}
			w, h = (x1 - x0) * PX // 20, (y1 - y0) * PX // 20
			neck = (hx + head_w / 2.0, hy)  # twips, body clip space
			anchor_px = ((neck[0] - x0) * PX / 20.0, (neck[1] - y0) * PX / 20.0)
			img = render(skin, ["rleg", "lleg", "body", "rarm", "larm"], angles, YAW, PITCH, unit * PX / 20.0, (w, h), (0, 24, 0), anchor_px)
			png = work / f"{swf.stem}_{sid}.png"
			pipboy_style(img).save(png)
			args += [str(sid), str(png), "lossless2"]
			if swf.stem == "Condition_Body_0":
				previews.append(png)
		subprocess.run([a.java, "-jar", a.ffdec, "-replace", str(swf), str(out / CLIPS / swf.name)] + args, check=True, capture_output=True)
		print("steve:", swf.name, f"({k} frames)")

	args = []
	for (sid, (x0, y0, x1, y1)), (kind, bandage) in zip(heads, HEADS):
		face = expression(skin, kind, bandage, zombie)
		w, h = (x1 - x0) * PX // 20, (y1 - y0) * PX // 20
		# the neck is the middle of the clip's bottom edge (its origin is the bottom-left corner)
		anchor_px = ((head_w / 2.0 - x0) * PX / 20.0, (0 - y0) * PX / 20.0)
		img = render(face, ["head"], {}, YAW, PITCH, unit * PX / 20.0, (w, h), (0, 24, 0), anchor_px)
		png = work / f"head_{sid}.png"
		pipboy_style(img).save(png)
		args += [str(sid), str(png), "lossless2"]
		previews.append(png)
	subprocess.run([a.java, "-jar", a.ffdec, "-replace", str(head_swf), str(out / CLIPS / head_swf.name)] + args, check=True, capture_output=True)
	print("steve:", head_swf.name, f"({len(heads)} faces)")

	if a.preview:
		ims = [Image.open(p) for p in previews]
		sheet = Image.new("RGBA", (sum(i.width + 12 for i in ims), max(i.height for i in ims)), (24, 52, 28, 255))
		x = 0
		for im in ims:
			sheet.alpha_composite(im, (x, 0))
			x += im.width + 12
		sheet.save(a.preview)


main()
