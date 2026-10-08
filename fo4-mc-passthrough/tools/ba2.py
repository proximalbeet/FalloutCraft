"""List or extract files from a Fallout 4 general (GNRL) .ba2 archive.

usage: python3 -I tools/ba2.py <archive.ba2> list [filter]
       python3 -I tools/ba2.py <archive.ba2> extract <outdir> <filter>
Reads the user's own archive; extracted files stay on the user's machine.
"""
import struct
import sys
import zlib
from pathlib import Path


def entries(path):
	f = open(path, "rb")
	magic, version, kind, count, names_at = struct.unpack("<4sI4sIQ", f.read(24))
	assert magic == b"BTDX" and kind == b"GNRL", (magic, kind)
	records = [struct.unpack("<IIIIQIII", f.read(36)) for _ in range(count)]
	f.seek(names_at)
	names = []
	for _ in range(count):
		(n,) = struct.unpack("<H", f.read(2))
		names.append(f.read(n).decode("latin-1").replace("\\", "/"))
	return f, list(zip(names, records))


def read(f, record):
	_, _, _, _, offset, packed, size, _ = record
	f.seek(offset)
	data = f.read(packed or size)
	return zlib.decompress(data) if packed else data


def main():
	path, cmd = sys.argv[1], sys.argv[2]
	f, items = entries(path)
	if cmd == "list":
		needle = sys.argv[3].lower() if len(sys.argv) > 3 else ""
		for name, r in items:
			if needle in name.lower():
				print(f"{r[6]:>10}  {name}")
	elif cmd == "extract":
		out, needle = Path(sys.argv[3]), sys.argv[4].lower()
		for name, r in items:
			if needle in name.lower():
				dst = out / name
				dst.parent.mkdir(parents=True, exist_ok=True)
				dst.write_bytes(read(f, r))
				print("extracted", name)


main()
