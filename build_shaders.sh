#!/usr/bin/env bash
# Compiles the HLSL in src/ to DXBC with Microsoft's d3dcompiler_47 (under Proton) into generated headers.
# Run after editing the HLSL; build.sh uses the generated headers. CPU only: no graphics device.
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-$(command -v zig || realpath ../../tools/ziglang/ziglang/zig)}"
COMPILER_DLL="${D3DCOMPILER:-$HOME/Games/FFXIV/game/d3dcompiler_47.dll}"
PROTON="${PROTONPATH:-$HOME/.local/share/Steam/compatibilitytools.d/proton-wineland-ffxiv-11.0-20261103-x86_64_v3}"
work="$(mktemp -d /tmp/forge-shaderc.XXXXXX)"
trap 'rm -rf "$work"' EXIT

"$ZIG" c++ -target x86_64-windows-gnu -std=c++20 -O2 -Wl,--subsystem,windows -o "$work/shaderc.exe" tools/shaderc.cpp
rm -f "$work"/*.pdb
cp "$COMPILER_DLL" "$work/d3dcompiler_47.dll"

# compile <name> <array:entry:DEFINE[:profile]>...
compile() {
	local name="$1"
	shift
	WINEPREFIX="${FORGE_TEST_PREFIX:-$PWD/build/proton/pfx}" PROTONPATH="$PROTON" GAMEID=umu-0 WINEDLLOVERRIDES="d3dcompiler_47=n" \
		umu-run "$work/shaderc.exe" "Z:$PWD/src/$name.hlsl" "Z:$work/$name.h" "$@" \
		> "$work/umu.out" 2>&1 < /dev/null || true
	cat "$work/$name.h.log" 2>/dev/null || true; [[ -s "$work/$name.h" ]] || tail -20 "$work/umu.out" >&2
	if [[ ! -s "$work/$name.h" ]]; then
		echo "$name: shader compilation failed" >&2
		exit 1
	fi
	mv "$work/$name.h" "src/$name.h"
	echo "src/$name.h updated"
}

compile nr_shaders nr_meter:meter:METER nr_transfer:transfer:TRANSFER nr_pack_color:pack_color:PACK_COLOR nr_pack_guides:pack_guides:PACK_GUIDES nr_pack_work:pack_work:PACK_WORK nr_pack_mask:pack_mask:PACK_MASK nr_resolve:resolve:RESOLVE
