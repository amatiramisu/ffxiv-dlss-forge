# FFXIV DLSS Forge

A ReShade add-on for FFXIV's DLSS: DLAA, render resolution, presets, exposure, DSR desktop resolutions and DLSS 5 neural rendering.

## Install

Requires ReShade 6.8 with add-on support and an NVIDIA GPU. Download the zip from [Releases](https://github.com/amatiramisu/ffxiv-dlss-forge/releases) and extract `ffxiv-dlss-forge.addon64` next to `ffxiv_dx11.exe`. Select NVIDIA DLSS in the game's Graphics Settings and open FFXIV DLSS Forge in the ReShade overlay.

DLSS 5 also needs `nvngx_dlssnr.dll` next to `ffxiv_dx11.exe`. The game does not ship it. Without it, DLSS 5 stays unavailable and everything else works.

This is a Linux-first implementation (Wayland - enabled in XIVLauncher as well), Windows is untested atm.

## DLSS

Choose DLAA, a fixed render scale, or the game's own resolution settings. Select the DLSS preset and whether exposure follows the game or DLSS auto-exposure.

Under Proton, dxvk-nvapi DRS preset overrides such as `DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION` take priority over the preset chosen here.

## Display supersampling

With [proton-wineland-ffxiv](https://github.com/amatiramisu/proton-wineland-ffxiv), choose Off, DSR or DL-DSR and the game output resolution. Apply changes while the game runs in Borderless Windowed mode. Each filter keeps its own smoothness. Off restores the physical display mode and keeps the selected supersampling resolution.

The controls use Wine display data on KDE, GNOME and other supported desktops. The panel shows the physical display size and game output once. DLSS shows a separate scene resolution when it differs from the output. Detailed buffer sizes and original game requests are under Diagnostics.

DSR uses a linear-light Gaussian filter. Native NVIDIA DL-DSR supports the 1.78× and 2.25× modes. Proton Wineland FFXIV releases include the DL-DSR assets and select them automatically. Source builds need user-provided assets as described in the Proton repository. Both filters currently support SDR. DLSS scene settings and DLSS 5 model scale remain separate from display supersampling.

On native Windows, NVIDIA driver settings control display supersampling.

## DLSS 5

DLSS 5 runs before upscaling on the DLSS input, after upscaling on the DLSS output, or at Present on the final game image with its UI, before ReShade effects. Each pass has its own model, network, intensity, structure and tone; later passes inherit the first until edited. Automask and developer masking set how characters, near and far scenery are affected.

Side-by-side and wipe comparisons show the change against the original frame. The on-screen indicator shows model resolution, pass count and GPU time.

Guest mode leaves neural rendering to another add-on and keeps the DLSS controls. It is not tested with every add-on. Under Proton, add-ons that use D3D12 need `WINEDLLOVERRIDES=d3d12,d3d12core=n` in XIVLauncher's environment, and those that run DLSS 5 on a native D3D12 device can crash with ReShade 6.8; ReShade pull request [#435](https://github.com/crosire/reshade/pull/435) fixes that.

## Build

```sh
./build.sh
```

Needs zig. The add-on is written to `build/`. `build_shaders.sh` regenerates `src/nr_shaders.h` with Microsoft's `d3dcompiler_47.dll` under Proton.

## License

MIT. The headers in `include/` keep their own licenses.
