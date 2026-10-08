"""List the explosion (EXPL) records in a Fallout 4 master: form ID, editor ID, damage and radii.

Reads the user's own Fallout4.esm; prints nothing from it but IDs and numbers.
usage: python3 -I tools/esm_expl.py "<Fallout 4>/Data/Fallout4.esm" [filter]
"""
import struct
import sys
import zlib


def subrecords(data):
	i = 0
	while i + 6 <= len(data):
		tag, size = struct.unpack_from("<4sH", data, i)
		yield tag, data[i + 6:i + 6 + size]
		i += 6 + size


def main(path, needle=""):
	with open(path, "rb") as f:
		buf = f.read()
	# skip the TES4 header record
	_, size = struct.unpack_from("<4sI", buf, 0)
	i = 24 + size
	while i < len(buf):
		tag, gsize, label = struct.unpack_from("<4sI4s", buf, i)
		if tag == b"GRUP" and label == b"EXPL":
			end, j = i + gsize, i + 24
			while j < end:
				rtype, rsize, flags, formid = struct.unpack_from("<4sIII", buf, j)
				data = buf[j + 24:j + 24 + rsize]
				if flags & 0x00040000:
					data = zlib.decompress(data[4:])
				edid, dmg, inner, outer, force = "", None, None, None, None
				for st, sd in subrecords(data):
					if st == b"EDID":
						edid = sd.rstrip(b"\0").decode("latin-1")
					elif st == b"DATA" and len(sd) >= 0x2C:
						# light, sound1, sound2, impact set, placed obj, spawn proj (6 form IDs) then force, damage, inner, outer
						force, dmg, inner, outer = struct.unpack_from("<4f", sd, 24)
				if needle.lower() in edid.lower():
					print(f"{formid:08X}  {edid:48s} dmg={dmg!s:>8.8} inner={inner!s:>8.8} outer={outer!s:>8.8} force={force!s:>8.8}")
				j += 24 + rsize
			return
		i += gsize if tag == b"GRUP" else 24 + gsize


if __name__ == "__main__":
	main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "")
