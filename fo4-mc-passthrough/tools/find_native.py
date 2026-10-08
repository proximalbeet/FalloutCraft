"""Find where Fallout4.exe registers a Papyrus native function, and the C++ function it registers.

The game registers natives with code like:  lea rdx, "SetPosition" ; lea r8, "ObjectReference" ; ... lea rax, <fn>.
This finds every RIP-relative LEA of the function's name, then lists the other LEAs nearby that point into .text
(the candidates for the native's implementation) and the class-name strings next to it.

usage: pyenv/bin/python tools/find_native.py <Fallout4.exe> <FunctionName> [ClassName]
Prints offsets relative to the exe's base (what the plugin adds to GetModuleHandle(nullptr)).
"""
import sys

import capstone
import pefile


def main(path, name, cls=None):
	pe = pefile.PE(path, fast_load=True)
	base = pe.OPTIONAL_HEADER.ImageBase
	data = open(path, "rb").read()
	sections = {s.Name.rstrip(b"\0").decode(): s for s in pe.sections}
	text = sections[".text"]
	tstart, tsize = text.VirtualAddress, text.Misc_VirtualSize

	def rva_of_offset(off):
		for s in pe.sections:
			if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
				return off - s.PointerToRawData + s.VirtualAddress
		return None

	def string_at(rva):
		off = pe.get_offset_from_rva(rva)
		end = data.find(b"\0", off, off + 80)
		return data[off:end].decode("latin-1", "replace") if end > off else ""

	targets = set()
	needle = name.encode() + b"\0"
	i = data.find(needle)
	while i != -1:
		if data[i - 1] == 0:  # the whole string, not a suffix
			targets.add(rva_of_offset(i))
		i = data.find(needle, i + 1)
	print(f"'{name}' strings at", [hex(t) for t in targets if t])

	md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
	md.detail = False
	code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
	# find LEA reg, [rip+disp32] whose target is one of the strings: 48/4C 8D modrm(05/0D/15/../3D) disp32
	hits = []
	for off in range(len(code) - 7):
		if code[off] in (0x48, 0x4C) and code[off + 1] == 0x8D and (code[off + 2] & 0xC7) == 0x05:
			disp = int.from_bytes(code[off + 3:off + 7], "little", signed=True)
			rva = tstart + off + 7 + disp
			if rva in targets:
				hits.append(tstart + off)
	for h in hits:
		window = code[h - tstart - 64:h - tstart + 160]
		leas = []
		for ins in md.disasm(window, h - 64):
			if ins.mnemonic == "lea" and "rip" in ins.op_str:
				disp = int(ins.op_str.split("rip")[1].strip(" +]").replace("- ", "-"), 16) if "rip +" in ins.op_str or "rip -" in ins.op_str else 0
				if "rip -" in ins.op_str:
					disp = -int(ins.op_str.split("rip -")[1].strip(" ]"), 16)
				rva = ins.address + ins.size + disp
				kind = "text" if tstart <= rva < tstart + tsize else "data"
				label = string_at(rva) if kind == "data" else ""
				leas.append((ins.address, rva, kind, label))
		names = [l for _, _, k, l in leas if k == "data" and l]
		if cls and cls not in names:
			continue
		print(f"\nLEA of '{name}' at +{h:#x}; strings nearby: {names}")
		for a, rva, kind, label in leas:
			if kind == "text":
				print(f"   code pointer: +{rva:#x}  (lea at +{a:#x})")


main(*sys.argv[1:])
