#!/usr/bin/env bash
# Cross-builds the add-on (and the Proton test harness) for x86_64 Windows with zig's clang + mingw-w64.
set -euo pipefail
cd "$(dirname "$0")"
ZIG="${ZIG:-$(command -v zig || realpath ../../tools/ziglang/ziglang/zig)}"
mkdir -p build

COMMON=(-target x86_64-windows-gnu -std=c++20 -O2 -fno-sanitize=all -fno-exceptions -fno-rtti
	-Wall -Wno-unused-function -Wno-missing-braces -Wno-unknown-attributes -Wno-date-time
	-Iinclude/shim -Iinclude/reshade -Iinclude/imgui -Iinclude/vulkan -Isrc
	-DNOMINMAX -DUNICODE -D_UNICODE)
CORE=(src/ngx_hook.cpp src/motion_vectors.cpp src/nr_bridge.cpp src/nr_dll.cpp src/desktop.cpp src/settings.cpp src/log.cpp)

# ReShade (MSVC) returns structures from member functions through a hidden pointer, this GNU-ABI build in RAX:
# ReShade methods returning api:: structures must stay out of the add-on (they wrote to address 0 at runtime)
if grep -nE '(get_back_buffer|get_current_back_buffer|get_resource_desc|get_resource_view_desc|get_resource_from_view|find_technique|find_uniform_variable|find_texture_variable)[[:space:]]*\(' src/addon.cpp; then
	echo "src/addon.cpp calls a ReShade method that returns a structure; use the native object instead" >&2
	exit 1
fi

"$ZIG" c++ "${COMMON[@]}" -shared -o build/ffxiv-dlss-forge.addon64 src/addon.cpp "${CORE[@]}" -ladvapi32 -luser32 -lversion
rm -f build/*.lib build/*.pdb

if [[ "${1:-}" != "--addon-only" ]]; then
	# Windowless test programs: no console window opens under Wine; results go to their .log files
	"$ZIG" c++ "${COMMON[@]}" -Wl,--subsystem,windows -o build/forge-harness.exe test/harness.cpp "${CORE[@]}" -ld3d11 -ldxgi -ladvapi32 -luser32 -lversion
	# CPU-only: no D3D11/DXGI import, no graphics device
	# CPU-only: the snippet's weight table read from the file
	"$ZIG" c++ "${COMMON[@]}" -Wl,--subsystem,windows -o build/forge-dllinfo.exe test/dllinfo.cpp src/nr_dll.cpp src/log.cpp -lversion
	"$ZIG" c++ "${COMMON[@]}" -Wl,--subsystem,windows -o build/forge-interop.exe test/interop.cpp -ld3d11 -ldxgi
	# DLSS NR through NGX's Vulkan API on DXVK's own device
	"$ZIG" c++ "${COMMON[@]}" -Wl,--subsystem,windows -o build/forge-nrvk.exe test/nrvk.cpp "${CORE[@]}" -ld3d11 -ldxgi -ladvapi32 -luser32 -lversion
	rm -f build/forge-harness.pdb build/forge-interop.pdb build/forge-dllinfo.pdb build/forge-nrvk.pdb
fi
ls -la build
