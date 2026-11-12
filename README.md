# FFXIV DLSS Forge

A ReShade add-on for FFXIV's DLSS(5) specifically for Linux: DLAA, render resolution, presets, exposure, DSR desktop resolutions and DLSS 5 neural rendering.

## Install

Download the zip from [Releases](https://github.com/amatiramisu/ffxiv-dlss-forge/releases) and extract `ffxiv-dlss-forge.addon64` next to `ffxiv_dx11.exe`. 
Select NVIDIA DLSS in the game's Graphics Settings. Use the ReShade Overlay for the Forge.

DLSS 5 needs `nvngx_dlssnr.dll` next to `ffxiv_dx11.exe`.
(DL)-DSR on Linux would require [proton-wineland-ffxiv](https://github.com/amatiramisu/proton-wineland-ffxiv) - You might want to use the wineland release zip, rather than building from source.
Although both ways are viable, you'd need to provide the weights...

## Misc

Guest mode leaves neural rendering to another add-on (DFC, renodx etc.) and keeps the DLSS/DLAA controls, helpful if you want to use another DLSS5 addon on top, which is fair.
Might not work for some of the available ones though. Add-ons that use D3D12 need `WINEDLLOVERRIDES=d3d12,d3d12core=n` in the XIVLauncher environment. Add-ons that run DLSS 5 on a native D3D12 device can crash with ReShade 6.8. ReShade pull request [#435](https://github.com/crosire/reshade/pull/435) fixes that crash.

Only sharing what I am personally working on and using github as my backup for this.
Could be helpful if this ever get found. Could not. Don't care.

## Build

```sh
./build.sh
```

The build needs zig. `build.sh` writes the add-on to `build/`. `build_shaders.sh` regenerates `src/nr_shaders.h` with Microsoft's `d3dcompiler_47.dll` under Proton.

## License

MIT. The headers in `include/` keep their own licenses.
