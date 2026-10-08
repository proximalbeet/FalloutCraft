"""The Commonwealth's terrain heights, from the user's own Fallout4.esm, for the plugin's Minecraft ground.

Every exterior cell (4096 units square) has a LAND record whose VHGT holds 33x33 heights 128 units apart: a float
offset, then signed byte deltas, accumulated along each row (the first value of a row accumulates down the column);
height = 8 * value. The cell's grid position is its CELL record's XCLC.

Output (little-endian): "MCHM", int32 version 2, uint32 worldspace form ID, int32 minX, minY, maxX, maxY (cells),
then for every cell row by row (y from minY, x from minX) 33*33 int16 heights in units of 2 game units (the terrain
runs -8320..44872, in steps of 8); -32768 = no land. The plugin memory-maps it.

usage: python3 -I tools/heightmap.py <Fallout4.esm> <out.bin> [worldspace form ID, default 0x3C Commonwealth]
"""
import struct
import sys
import zlib


def records(buf, start, end):
	"""(type, flags, formid, data, offset, size) for records and groups between start and end."""
	i = start
	while i < end:
		tag = buf[i:i + 4]
		size = struct.unpack_from("<I", buf, i + 4)[0]
		if tag == b"GRUP":
			yield b"GRUP", buf[i + 8:i + 12], struct.unpack_from("<i", buf, i + 12)[0], None, i, size
			i += size
		else:
			flags, formid = struct.unpack_from("<II", buf, i + 8)
			data = buf[i + 24:i + 24 + size]
			if flags & 0x00040000:
				data = zlib.decompress(data[4:])
			yield tag, flags, formid, data, i, size
			i += 24 + size


def subrecords(data):
	i = 0
	while i + 6 <= len(data):
		tag, size = struct.unpack_from("<4sH", data, i)
		yield tag, data[i + 6:i + 6 + size]
		i += 6 + size


def heights_of(vhgt):
	offset = struct.unpack_from("<f", vhgt, 0)[0]
	deltas = struct.unpack_from("<1089b", vhgt, 4)
	out = [0.0] * 1089
	row_start = offset
	for r in range(33):
		row_start += deltas[r * 33]
		v = row_start
		out[r * 33] = v * 8.0
		for c in range(1, 33):
			v += deltas[r * 33 + c]
			out[r * 33 + c] = v * 8.0
	return out


def main(esm, out, world=0x3C):
	buf = open(esm, "rb").read()
	_, size = struct.unpack_from("<4sI", buf, 0)
	cells = {}
	# top level: find the WRLD group, then the world's children group (type 1, label = world form ID)
	for tag, label, gtype, _, off, gsize in records(buf, 24 + size, len(buf)):
		if tag != b"GRUP" or label != b"WRLD":
			continue

		def walk(start, end, xy):
			for t, a, b, data, o, s in records(buf, start, end):
				if t == b"GRUP":
					walk(o + 24, o + s, xy)
				elif t == b"CELL":
					for st, sd in subrecords(data):
						if st == b"XCLC":
							xy[0] = struct.unpack_from("<ii", sd, 0)
				elif t == b"LAND" and xy[0] is not None:
					for st, sd in subrecords(data):
						if st == b"VHGT":
							cells[xy[0]] = heights_of(sd)

		for t, a, gt, _, o, s in records(buf, off + 24, off + gsize):
			if t == b"GRUP" and gt == 1 and struct.unpack("<I", a)[0] == world:
				walk(o + 24, o + s, [None])
		break
	if not cells:
		sys.exit("no land found")
	xs = [x for x, _ in cells]
	ys = [y for _, y in cells]
	minx, maxx, miny, maxy = min(xs), max(xs), min(ys), max(ys)
	none = [-32768] * 1089
	with open(out, "wb") as f:
		f.write(b"MCHM" + struct.pack("<iIiiii", 2, world, minx, miny, maxx, maxy))
		for y in range(miny, maxy + 1):
			for x in range(minx, maxx + 1):
				h = cells.get((x, y))
				f.write(struct.pack("<1089h", *(none if h is None else [max(-32767, min(32767, round(v / 2))) for v in h])))
	print(f"{len(cells)} cells, x {minx}..{maxx}, y {miny}..{maxy} -> {out}")


main(sys.argv[1], sys.argv[2], int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x3C)
