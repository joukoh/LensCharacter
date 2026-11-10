# LensCharacter on Windows

The Windows plugin renders with CUDA on NVIDIA GPUs and falls back to the CPU on other GPUs. The lens maths is the same `LensKernel.h` the Mac build uses, so both platforms give the same picture.

> **Status:** tested with DaVinci Resolve 21.2 on Windows 11 with an RTX 40-series GPU. The Windows version is new: please report anything that fails.

To use the plugin, install the download (below). You only need the build steps further down to change the code.

## Install the download

You need a 64-bit Windows 10 or 11 PC, DaVinci Resolve, and for GPU rendering an NVIDIA GPU (RTX 20 series or newer) with a current driver. Nothing else: the CUDA runtime is built into the plugin.

1. Download `LensCharacter-…-windows-x64.zip` from [Releases](../../../releases).
2. Close DaVinci Resolve.
3. Right-click the zip, choose **Extract All…**, and extract it.
4. Open `C:\Program Files\Common Files\` in File Explorer. If it has no `OFX\Plugins` folder, create `OFX` and then `Plugins` inside it. Windows asks for administrator permission.
5. Move the extracted `LensCharacter.ofx.bundle` folder into `C:\Program Files\Common Files\OFX\Plugins\`. If an older LensCharacter is there, replace it.
6. Open Resolve. The effect is under **Effects → OpenFX → LensCharacter** on the Edit and Color pages. Check **Preferences → System → Memory and GPU → GPU Processing Mode** is *CUDA* or *Auto*.

The plugin isn't code-signed. If Windows' **Smart App Control** is on, it may stop Resolve from loading it.

## Build from source

### What you need (one time)

1. **DaVinci Resolve.** It installs the OpenFX SDK the build uses: Resolve 21 puts it in
   `C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\OpenFX`, older versions in
   `C:\Program Files\Blackmagic Design\DaVinci Resolve\Developer\OpenFX`. The build looks in both.
2. **Visual Studio 2022 Community** with the **Desktop development with C++** workload: [visualstudio.microsoft.com](https://visualstudio.microsoft.com/downloads/).
3. **CMake 3.20 or newer:** `winget install Kitware.CMake`, or use the copy that comes with Visual Studio by building from the *Developer Command Prompt for VS 2022*.
4. **NVIDIA CUDA Toolkit 12.x:** [developer.nvidia.com/cuda-downloads](https://developer.nvidia.com/cuda-downloads).
   - Install it **after** Visual Studio, so it adds its Visual Studio integration.
   - Build from a terminal opened **after** the install. An older terminal lacks the `CUDA_PATH` variables, and the build then says the CUDA render path is OFF.
   - Use a 12.x toolkit. `winget install Nvidia.CUDA` installs the newest one (13.x), which may need a newer driver than you have. Pick a 12.x version with `winget install -e --id Nvidia.CUDA --version 12.9`.
   - Your NVIDIA driver must support the toolkit version. Run `nvidia-smi`: the "CUDA Version" it shows must be at least the toolkit's version. If it's lower, update the driver.
   - People who only *use* the plugin need just the NVIDIA driver. The CUDA runtime is built into the plugin.

### Build and test

Double-click `ofx\build-windows.bat`, or run it from a Command Prompt. It configures the build, compiles into `ofx\build-win\` and runs the self-test.

A good run includes lines like:

```
  CUDA device: NVIDIA GeForce RTX 4070 Ti (compute 8.9)
  ok    CUDA kernel compiled and ran (rc=0)
  ok    CPU vs CUDA: mean |diff| ..., 0 of 2073600 values differ by > 0.01
  CUDA time per UHD frame, Helios, Sharp Centre (best of 4): ...
All checks passed
```

The test images (`*.ppm`, `browser_preview_sw.ppm`) go to `ofx\build-win\`.

**Shared folder with the Mac:** the Windows build writes only to `ofx\build-win\` and the Mac build only to `ofx/build/`, so one folder can serve both machines. Run `git` from one machine only. If the folder is a network share, the script still works: `pushd` maps a temporary drive letter.

### Install your build

Close Resolve, then from an **administrator** Command Prompt:

```
ofx\build-windows.bat install
```

Or copy `ofx\build-win\LensCharacter.ofx.bundle` into `C:\Program Files\Common Files\OFX\Plugins\` yourself.

In Resolve, check **Preferences → System → Memory and GPU → GPU Processing Mode** is *CUDA* (or *Auto*). The effect is under **Effects → OpenFX → LensCharacter**.

To uninstall, delete `C:\Program Files\Common Files\OFX\Plugins\LensCharacter.ofx.bundle`.

## Where things go

| What | Where |
|---|---|
| Saved custom lenses | `%APPDATA%\LensCharacter\Profiles\` |
| Plugin log (render modes, CUDA errors) | `%LOCALAPPDATA%\LensCharacter\LensCharacter.log` |

## Troubleshooting

- **"CUDA Toolkit not found: building without the CUDA render path"**: the toolkit, or its Visual Studio integration, is missing, or the terminal was opened before the toolkit was installed. Open a new terminal; if that doesn't help, reinstall the CUDA Toolkit after Visual Studio. Then delete `ofx\build-win` and build again. Without CUDA, the plugin still works but renders on the CPU, which is much slower.
- **"OpenFX SDK not found"**: Resolve is installed somewhere else than the two places the build looks. Run `cmake -S . -B build-win -A x64 -DOFX_SDK="D:\path\to\DaVinci Resolve\Developer\OpenFX"` from `ofx\`, then run the script again.
- **The self-test says `rc=2`**: no usable CUDA device. Update the NVIDIA driver.
- **The effect doesn't appear in Resolve**: check the bundle is at `C:\Program Files\Common Files\OFX\Plugins\LensCharacter.ofx.bundle\Contents\Win64\LensCharacter.ofx`, then restart Resolve. If Resolve flagged the plugin as failed, it skips it until you reset its plugin cache.
- **A render error in Resolve**: look for `CUDA render failed (error N)` in the plugin log, and include it when reporting. To rule out the plugin's GPU memory pool, set the environment variable `LENSCHARACTER_CUDA_NOPOOL=1` (System Properties → Environment Variables), restart Resolve, and try again.

## GPU support

The build compiles machine code for RTX 20 (Turing), RTX 30 (Ampere) and RTX 40 (Ada) GPUs. It also includes PTX that newer GPUs compile when the plugin first loads. To target other GPUs, pass for example `-DCMAKE_CUDA_ARCHITECTURES="61-real;75-real;86-real;89-real;89-virtual"` when configuring. The oldest generations need an older CUDA Toolkit.
