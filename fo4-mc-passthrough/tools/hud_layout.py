"""Fallout's HUD for the passthrough: the compass moves to the top right, and the health bar goes (Minecraft's hearts
show Fallout's health now). Runs on the user's own HUDMenu.swf (tools/ba2.py extracts it) with JPEXS FFDec.

Only two placements change, in HUDMenu's timeline (twips, 1/20 px, stage 1280x720):
  BottomCenterGroup_mc (at 640,684) > CompassWidget_mc: to 1110,100 px (the bar's right end 20 px from the edge)
  LeftMeters_mc (at 64,684) > HPMeter_mc: far below the screen (the HUD's scripts still update it; nothing moves it)

usage: pyenv/bin/python tools/hud_layout.py <HUDMenu.swf> <out HUDMenu.swf> <ffdec.jar> <java>
"""
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

MOVES = {
	"CompassWidget_mc": (22200 - 12800, 2000 - 13680),
	"HPMeter_mc": (640, 40000),
}


def main(src, dst, ffdec, java):
	with tempfile.TemporaryDirectory() as tmp:
		xml = Path(tmp) / "hud.xml"
		subprocess.run([java, "-jar", ffdec, "-swf2xml", src, str(xml)], check=True, capture_output=True)
		tree = ET.parse(xml)
		moved = 0
		for item in tree.getroot().iter("item"):
			if item.get("type", "").startswith("PlaceObject") and item.get("name") in MOVES:
				x, y = MOVES[item.get("name")]
				m = item.find("matrix")
				m.set("translateX", str(x))
				m.set("translateY", str(y))
				m.set("nTranslateBits", "18")
				moved += 1
		if moved != len(MOVES):
			sys.exit(f"expected {len(MOVES)} clips, moved {moved}: a different HUDMenu.swf (a HUD mod?)")
		tree.write(xml, encoding="utf-8", xml_declaration=True)
		Path(dst).parent.mkdir(parents=True, exist_ok=True)
		subprocess.run([java, "-jar", ffdec, "-xml2swf", str(xml), dst], check=True, capture_output=True)
	print("HUD:", dst)


main(*sys.argv[1:])
