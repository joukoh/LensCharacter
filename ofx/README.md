# LensCharacter OpenFX plugin

GPU lens emulation for DaVinci Resolve: macOS (Metal) and Windows (CUDA on NVIDIA GPUs, CPU otherwise). Architecture: [`../docs/architecture.html`](../docs/architecture.html).

## Build and install

On Windows, see [`WINDOWS.md`](WINDOWS.md). On macOS:

```bash
make            # builds build/LensCharacter.ofx.bundle
make test       # library, overdrive-limit, CPU-vs-Metal and bokeh-evenness checks; writes build/*.ppm
sudo make install
```

Restart Resolve after installing. The effect is under **OpenFX → LensCharacter** on the Edit and Color pages.

## Layout

| Path | What it is |
|---|---|
| `src/LensKernel.h` | The per-pixel lens simulation. Compiled as C++ for the CPU path, as Metal at runtime on macOS, and as CUDA by nvcc on Windows (same source). |
| `src/Mixer.cpp` | Trait slots, A/B parameter blending, overdrive with validity budgets. |
| `src/Profiles.cpp` | Profile library: load, search, sample traits by focal length and aperture, save user lenses. |
| `src/LensCharacterPlugin.cpp` | OpenFX parameters, render dispatch, custom lens builder. |
| `src/MetalKernel.mm` | Metal pipeline (built from `LensKernel.h` at runtime, cached per command queue). |
| `src/CudaKernel.cu` | CUDA path: the same three passes as Metal, on Resolve's CUDA stream, with stream-ordered scratch memory. |
| `src/Platform.cpp` | The OS-specific bits: bundle and user folders, file listing, UTF-8 file paths. |
| `src/LensBrowserModel.cpp` | What the lens browser shows (categories, search, facts, preview geometry), shared by both platforms. |
| `src/LensBrowser.mm`, `src/LensBrowserWin.cpp` | The lens browser window: Cocoa on macOS, Win32 on Windows. |
| `Makefile` | macOS build. |
| `CMakeLists.txt`, `build-windows.bat` | Windows build. |
| `resources/profiles/lensfun.json` | 1,540 measured lenses converted from Lensfun (CC BY-SA 3.0). |
| `resources/profiles/builtin.json` | Hand-built character lenses (bokeh, swirl, glow), linked to measured geometry where Lensfun has the lens. |
| `tools/import_lensfun.py` | Regenerates `lensfun.json` from a Lensfun checkout: `python3 -I tools/import_lensfun.py <lensfun>/data/db resources/profiles/lensfun.json` |
| `tools/make_builtin_profiles.py` | Regenerates `builtin.json`. |

Saved custom lenses go to `~/Library/Application Support/LensCharacter/Profiles/` (Windows: `%APPDATA%\LensCharacter\Profiles\`) and are searchable by name in any lens field.
