# FFXIV DLSS Forge

A ReShade add-on for FFXIV's DLSS: DLAA, render resolution, presets, exposure, DSR desktop resolutions and DLSS 5 neural rendering.

## Install

Forge needs ReShade 6.8 with add-on support and an NVIDIA GPU. Download the zip from [Releases](https://github.com/amatiramisu/ffxiv-dlss-forge/releases) and extract `ffxiv-dlss-forge.addon64` next to `ffxiv_dx11.exe`. Select NVIDIA DLSS in the game's Graphics Settings and open FFXIV DLSS Forge in the ReShade overlay.

DLSS 5 also needs `nvngx_dlssnr.dll` next to `ffxiv_dx11.exe`. The game ships without it. Without the DLL, DLSS 5 stays unavailable and all other features work.

Forge is Linux-first and tested with the Wayland driver, enabled in XIVLauncher. Windows is untested.

## DLSS

Choose DLAA, a fixed render scale, or the game's own resolution settings. Select the DLSS preset and whether exposure follows the game or DLSS auto-exposure.

Under Proton, dxvk-nvapi DRS preset overrides such as `DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION` take priority over the preset chosen here.

## Display supersampling

With [proton-wineland-ffxiv](https://github.com/amatiramisu/proton-wineland-ffxiv), choose Off, DSR or DL-DSR and the game output resolution. Apply changes while the game runs in Borderless Windowed mode. Each filter keeps its own resolution and smoothness. Off restores the physical display mode.

DSR uses a linear-light Gaussian filter. Native NVIDIA DL-DSR supports the 1.78× and 2.25× modes. Both filters are SDR only.

Proton Wineland FFXIV releases include the DL-DSR assets and select them automatically. Source builds need user-provided assets as described in the Proton repository. DLSS scene settings and DLSS 5 model scale remain separate from display supersampling.

On native Windows, NVIDIA driver settings control display supersampling.

## DLSS 5

DLSS 5 runs at one of three stages. Before upscaling, it works on the DLSS input, and after upscaling on the DLSS output. At Present, it works on the final game image with its UI, before ReShade effects.

Each pass has its own model, network, intensity, structure and tone. Later passes copy the first pass until you edit them. Automask and developer masking set the effect on characters, near scenery and far scenery.

Side-by-side and wipe comparisons show the change against the original frame. The on-screen indicator shows model resolution, pass count and GPU time.

Guest mode leaves neural rendering to another add-on and keeps the DLSS controls. Testing covers some add-ons only.

Under Proton, add-ons that use D3D12 need `WINEDLLOVERRIDES=d3d12,d3d12core=n` in the XIVLauncher environment. Add-ons that run DLSS 5 on a native D3D12 device can crash with ReShade 6.8. ReShade pull request [#435](https://github.com/crosire/reshade/pull/435) fixes that crash.

## Build

```sh
./build.sh
```

The build needs zig. `build.sh` writes the add-on to `build/`. `build_shaders.sh` regenerates `src/nr_shaders.h` with Microsoft's `d3dcompiler_47.dll` under Proton.

## License

MIT. The headers in `include/` keep their own licenses.
