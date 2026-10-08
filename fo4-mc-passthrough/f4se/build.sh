#!/usr/bin/env bash
# Builds build/MCPassthrough.dll (F4SE plugin + ReShade add-on) on Linux with clang-cl and lld-link, against the MSVC
# CRT and Windows SDK that xwin downloads (XWIN=<dir from `xwin splat`>, default ../../toolchain/msvc).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
XWIN="${XWIN:-$HERE/../../toolchain/msvc}"
[ -f "$HERE/third_party/reshade/reshade.hpp" ] || { echo "third_party is missing: run fetch_deps.sh first"; exit 1; }
[ -d "$XWIN/crt" ] || { echo "no MSVC CRT/SDK at $XWIN (xwin --accept-license splat --output <dir>)"; exit 1; }
mkdir -p "$HERE/build"

INC=(/imsvc "$XWIN/crt/include" /imsvc "$XWIN/sdk/include/ucrt" /imsvc "$XWIN/sdk/include/um" /imsvc "$XWIN/sdk/include/shared")
LIB=(/libpath:"$XWIN/crt/lib/x86_64" /libpath:"$XWIN/sdk/lib/um/x86_64" /libpath:"$XWIN/sdk/lib/ucrt/x86_64")

objs=()
for src in plugin compositor ws; do
	clang-cl --target=x86_64-pc-windows-msvc /nologo /c /O2 /EHsc /std:c++20 /MT /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX \
		/D_CRT_SECURE_NO_WARNINGS -Wno-microsoft-cast "${INC[@]}" /I "$HERE/third_party/reshade" \
		"$HERE/src/$src.cpp" /Fo"$HERE/build/$src.obj"
	objs+=("$HERE/build/$src.obj")
done
lld-link /nologo /dll /machine:x64 /out:"$HERE/build/MCPassthrough.dll" "${LIB[@]}" "${objs[@]}" ws2_32.lib user32.lib kernel32.lib
echo "built $HERE/build/MCPassthrough.dll"
