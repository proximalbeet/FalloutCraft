#!/usr/bin/env bash
# Fetches what the Fallout 4 half needs but doesn't redistribute:
#   third_party/reshade/  ReShade's add-on API headers (crosire/reshade)
#   third_party/*.fxh     ReShade.fxh and ReShadeUI.fxh (crosire/reshade-shaders, slim branch)
#   third_party/runtime/  ReShade64.dll with add-on support (from reshade.me), installed as dxgi.dll
# F4SE itself comes from Nexus (it needs a login): download it yourself, install.sh picks it up from ~/Downloads.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
RESHADE=6.8.0
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$HERE/third_party/reshade" "$HERE/third_party/runtime"

for f in reshade.hpp reshade_api.hpp reshade_api_device.hpp reshade_api_pipeline.hpp reshade_api_resource.hpp reshade_api_format.hpp reshade_events.hpp reshade_overlay.hpp; do
	curl -fsSL "https://raw.githubusercontent.com/crosire/reshade/v$RESHADE/include/$f" -o "$HERE/third_party/reshade/$f"
done
for f in ReShade.fxh ReShadeUI.fxh; do
	curl -fsSL "https://raw.githubusercontent.com/crosire/reshade-shaders/slim/Shaders/$f" -o "$HERE/third_party/$f"
done

# The add-on build's installer is a self-extracting zip; the DLL is inside.
curl -fsSL -A "$UA" -H "Referer: https://reshade.me/" "https://reshade.me/downloads/ReShade_Setup_${RESHADE}_Addon.exe" -o "$TMP/reshade.exe"
unzip -qo "$TMP/reshade.exe" ReShade64.dll -d "$TMP" 2>/dev/null || true
cp "$TMP/ReShade64.dll" "$HERE/third_party/runtime/"
echo "fetched ReShade $RESHADE headers, shader includes and ReShade64.dll"
