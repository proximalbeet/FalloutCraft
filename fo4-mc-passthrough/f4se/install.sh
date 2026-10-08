#!/usr/bin/env bash
# Installs the Fallout 4 half into the game folder: F4SE (from your own download), MCPassthrough.dll, and ReShade
# (as dxgi.dll) with the compositing effect. Every file it adds is listed in .mcpassthrough-installed, and
#   install.sh --remove
# deletes exactly those. It stops rather than replace a file that isn't the same as ours (FORCE=1 replaces).
#
#   F4SE_DIR   the unpacked F4SE 0.7.9 folder (the one with f4se_loader.exe); default: ../../downloads/f4se-0.7.9/f4se_0_07_09
#   FO4_DIR    the game folder; default: Steam's Fallout 4
#   PIPBOY_DIR the Pip-Boy STAT clips made by tools/pipboy_steve.py (Steve and zombie faces), if present; default:
#              ../../work/interface/steve_out. Loose interface files need [Archive] in Fallout4Custom.ini: it's backed
#              up (.bak-mcpassthrough) and restored by --remove.
#
# Launch it through F4SE with ReShade loaded: in Steam, Fallout 4 > Properties > Launch options:
#   WINEDLLOVERRIDES="dxgi=n,b" bash -c 'exec "${@/Fallout4Launcher.exe/f4se_loader.exe}"' -- %command%
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FO4_DIR="${FO4_DIR:-$HOME/.local/share/Steam/steamapps/common/Fallout 4}"
F4SE_DIR="${F4SE_DIR:-$HERE/../../downloads/f4se-0.7.9/f4se_0_07_09}"
PIPBOY_DIR="${PIPBOY_DIR:-$HERE/../../work/interface/steve_out}"
HEIGHTS="${HEIGHTS:-$HERE/../../work/heights/MCPassthrough_heights.bin}" # tools/heightmap.py: the ground follows the terrain
CUSTOM_INI="${CUSTOM_INI:-$HOME/.local/share/Steam/steamapps/compatdata/377160/pfx/drive_c/users/steamuser/Documents/My Games/Fallout4/Fallout4Custom.ini}"
MANIFEST="$FO4_DIR/.mcpassthrough-installed"
[ -f "$FO4_DIR/Fallout4.exe" ] || { echo "Fallout4.exe not found in: $FO4_DIR"; exit 1; }

if [ "${1:-}" = "--remove" ]; then
	[ -f "$MANIFEST" ] || { echo "nothing to remove (no $MANIFEST)"; exit 0; }
	while IFS= read -r f; do rm -fv "$FO4_DIR/$f"; done < "$MANIFEST"
	rm -f "$MANIFEST"
	[ -f "$CUSTOM_INI.bak-mcpassthrough" ] && mv -v "$CUSTOM_INI.bak-mcpassthrough" "$CUSTOM_INI"
	rmdir "$FO4_DIR/Data/Interface/Components/ConditionClips" "$FO4_DIR/Data/Interface/Components" "$FO4_DIR/Data/Interface" 2>/dev/null || true
	rmdir "$FO4_DIR/reshade-shaders/Shaders" "$FO4_DIR/reshade-shaders" "$FO4_DIR/Data/F4SE/Plugins" "$FO4_DIR/Data/F4SE" 2>/dev/null || true
	exit 0
fi

[ -f "$HERE/build/MCPassthrough.dll" ] || { echo "build it first: ./build.sh"; exit 1; }
[ -f "$HERE/third_party/runtime/ReShade64.dll" ] || { echo "fetch ReShade first: ./fetch_deps.sh"; exit 1; }
[ -f "$F4SE_DIR/f4se_loader.exe" ] || { echo "F4SE not found in: $F4SE_DIR (set F4SE_DIR)"; exit 1; }

# source -> destination (relative to the game folder)
pairs=(
	"$F4SE_DIR/f4se_loader.exe|f4se_loader.exe"
	"$F4SE_DIR/f4se_1_11_240.dll|f4se_1_11_240.dll"
	"$HERE/build/MCPassthrough.dll|Data/F4SE/Plugins/MCPassthrough.dll"
	"$HERE/third_party/runtime/ReShade64.dll|dxgi.dll"
	"$HERE/shaders/MCPassthrough.fx|reshade-shaders/Shaders/MCPassthrough.fx"
	"$HERE/third_party/ReShade.fxh|reshade-shaders/Shaders/ReShade.fxh"
	"$HERE/third_party/ReShadeUI.fxh|reshade-shaders/Shaders/ReShadeUI.fxh"
)
while IFS= read -r -d '' s; do
	pairs+=("$s|Data/Scripts/${s#"$F4SE_DIR/Data/Scripts/"}")
done < <(find "$F4SE_DIR/Data/Scripts" -type f -print0)

# the HUD from tools/hud_layout.py (compass top right, no health bar), made into the same folder
if [ -f "$PIPBOY_DIR/Interface/HUDMenu.swf" ]; then
	pairs+=("$PIPBOY_DIR/Interface/HUDMenu.swf|Data/Interface/HUDMenu.swf")
fi
if [ -d "$PIPBOY_DIR/Interface/Components/ConditionClips" ]; then
	for s in "$PIPBOY_DIR"/Interface/Components/ConditionClips/*.swf; do
		pairs+=("$s|Data/Interface/Components/ConditionClips/$(basename "$s")")
	done
fi

if [ -f "$HEIGHTS" ]; then
	pairs+=("$HEIGHTS|Data/F4SE/Plugins/MCPassthrough_heights.bin")
fi

clash=""
for p in "${pairs[@]}"; do
	src="${p%%|*}" dst="${p#*|}"
	if [ -f "$FO4_DIR/$dst" ] && ! cmp -s "$src" "$FO4_DIR/$dst" && ! grep -qxF "$dst" "$MANIFEST" 2>/dev/null; then
		clash+=" $dst"
	fi
done
[ -z "$clash" ] || [ -n "${FORCE:-}" ] || { echo "not replacing files that aren't ours:$clash (FORCE=1 to replace)"; exit 1; }

touch "$MANIFEST"
add() { grep -qxF "$1" "$MANIFEST" || echo "$1" >> "$MANIFEST"; }
for p in "${pairs[@]}"; do
	src="${p%%|*}" dst="${p#*|}"
	[ -f "$FO4_DIR/$dst" ] && ! grep -qxF "$dst" "$MANIFEST" && cmp -s "$src" "$FO4_DIR/$dst" && continue # already there, not ours
	mkdir -p "$(dirname "$FO4_DIR/$dst")"
	cp "$src" "$FO4_DIR/$dst"
	add "$dst"
done

# ReShade's settings: Fallout 4's depth is not reversed; the preset turns the effect on (the add-on gates it).
if [ ! -f "$FO4_DIR/ReShade.ini" ] || grep -qxF "ReShade.ini" "$MANIFEST"; then
	printf '[GENERAL]\r\nEffectSearchPaths=.\\reshade-shaders\\Shaders\\\r\nTextureSearchPaths=.\\reshade-shaders\\Textures\\\r\nPresetPath=.\\ReShadePreset.ini\r\nPreprocessorDefinitions=RESHADE_DEPTH_INPUT_IS_REVERSED=0,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0,RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0,RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000\r\n\r\n[OVERLAY]\r\nTutorialProgress=4\r\nShowClock=0\r\nShowFPS=0\r\n\r\n[SCREENSHOT]\r\nSavePath=.\\\r\n' > "$FO4_DIR/ReShade.ini"
	add ReShade.ini
fi
if [ ! -f "$FO4_DIR/ReShadePreset.ini" ] || grep -qxF "ReShadePreset.ini" "$MANIFEST"; then
	printf 'Techniques=MCPassthrough@MCPassthrough.fx\r\nTechniqueSorting=MCPassthrough@MCPassthrough.fx\r\n' > "$FO4_DIR/ReShadePreset.ini"
	add ReShadePreset.ini
fi
# loose files (the Pip-Boy clips) only win over the archives with these; the old ini is kept to restore
if [ -d "$PIPBOY_DIR/Interface/Components/ConditionClips" ] && [ -f "$CUSTOM_INI" ] && ! grep -q "bInvalidateOlderFiles=1" "$CUSTOM_INI"; then
	[ -f "$CUSTOM_INI.bak-mcpassthrough" ] || cp "$CUSTOM_INI" "$CUSTOM_INI.bak-mcpassthrough"
	printf '\r\n[Archive]\r\nbInvalidateOlderFiles=1\r\nsResourceDataDirsFinal=\r\n' >> "$CUSTOM_INI"
	echo "enabled loose files in Fallout4Custom.ini (backup: Fallout4Custom.ini.bak-mcpassthrough)"
fi
# files ReShade writes next to itself, removed with the rest
add ReShade.log
echo "installed into $FO4_DIR ($(wc -l < "$MANIFEST") files listed in .mcpassthrough-installed)"
echo "Steam launch options for Fallout 4:"
echo "  WINEDLLOVERRIDES=\"dxgi=n,b\" bash -c 'exec \"\${@/Fallout4Launcher.exe/f4se_loader.exe}\"' -- %command%"
