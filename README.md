# Hellblade II on Apple Silicon: lab

Tools, measurements and notes from getting _Senua's Saga: Hellblade II_ to run well on a Mac Studio (M4 Max, 32-core GPU, 36 GB, macOS 27.2, 5120x1440 at 144 Hz). The game ran through Heroic, Game Porting Toolkit 3 (Wine 7.7 with D3DMetal 3.0) and Rosetta.

The finished launcher changes live in the Heroic fork, [Shaurya-Srivastav/HeroicGamesLauncher](https://github.com/Shaurya-Srivastav/HeroicGamesLauncher). Its `doc/macos.md` explains them, with root causes and results. This repository holds the experiments behind them.

The scripts expect this repository at `~/hb2lab`, the Heroic fork at `~/Documents/HeroicGamesLauncher`, and Hellblade II's Heroic prefix at `~/Games/Heroic/Prefixes/default/Senuas saga hellblade II`. They contain absolute paths from that setup.

## Layout

| Folder | What it is |
| --- | --- |
| `winrun.sh` | Runs any Windows program in Hellblade II's prefix, with the environment the game gets |
| `bin/` | Launch and measurement scripts |
| `probe/` | Windows programs that run inside the prefix and ask the questions the game asks, plus their output |
| `kmt/` | The prototype that unblocked DLSS Frame Generation |
| `inject/` | A native library injected into the game's process to hook MetalFX |
| `mfx/` | MetalFX benchmark |
| `gvas/`, `tsverify/` | Scripts that run the fork's TypeScript (save parser, graphics presets, gdi32 patcher) against real files |
| `recovered/` | Older scripts used for the Game Porting Toolkit 4 tests and menu screenshots |
| `out/` | Raw results: measurement logs, game logs, screenshots |

Not tracked: NVIDIA's Streamline SDK (`streamline-2.4.0`, from [NVIDIA-RTX/Streamline](https://github.com/NVIDIA-RTX/Streamline) tag v2.4.0), build outputs, save-game backups, and disassembly of Apple's D3DMetal and the game.

## Tools

### Launching and measuring (`bin/`, `winrun.sh`)

- **`run-gptk3.sh <runner> [KEY=VALUE ...] [-- game args]`** launches the game directly on a Heroic runner folder, without Heroic. It sets `D3DM_ENABLE_METALFX=1`, MSync, Rosetta AVX and the Metal HUD.
- **`perf.py <seconds> [label]`** summarizes the game's presented frames over the last N seconds from `metalperftrace`: fps, GPU time, MetalFX upscaled and interpolated frames, and render scale.
- **`measure.sh <label>`** waits until gameplay starts (GPU time above 12 ms), then records frame statistics, per-thread CPU use and a screenshot.
- **`ablate.py`** ran A/B/A experiments: change one console variable, measure, restore, measure the baseline again.
  - It wrote values straight into the running game's memory through `cvarpeek --write`.
  - Turning volumetric fog back on that way crashed the game, because the engine never rebuilt the fog's GPU data. **Don't run it during a play session.**
  - Its results are in `out/ablate.log` and `out/ablate.json`.

### Probes (`probe/`)

Built with mingw-w64 (`brew install mingw-w64`) and run with `winrun.sh <runner> ./probe.exe`.

- **`slprobe.cpp`** loads the game's own Streamline (`sl.interposer.dll`) and asks whether DLSS Frame Generation is supported. It also reads every value Streamline bases that decision on: OS build, adapters and LUIDs, hardware-accelerated GPU scheduling via `D3DKMTEnumAdapters2`/`D3DKMTQueryAdapterInfo`, NVAPI, and NGX. With `modes` it prints monitors, DPI, windows and display modes instead.
  - It found that Wine 7.7's `gdi32` lacks the two D3DKMT functions, so Streamline sees no hardware scheduling. That was the only frame generation blocker.
  - It also showed that Windows code sees the full 5120x1440, so the half-resolution bug was inside the game.
  - Outputs: `slprobe-gptk3.txt` (stock runner), `slprobe-fg*.txt` (after the fix), `slprobe-ts.txt` (with the fork's TypeScript patcher).

  ```sh
  x86_64-w64-mingw32-g++ -std=c++17 -O1 -municode -I ../streamline-2.4.0/include slprobe.cpp -o slprobe.exe \
    -ldxgi -lshcore -luser32 -lgdi32 -static -static-libgcc -static-libstdc++
  ```

- **`cvarpeek.cpp`** finds Unreal console variables in the running game through the console manager's map and prints them. With `--set`, it changes them.
  - It showed `r.SecondaryScreenPercentage.GameViewport` locked at 50 with console priority, which caused the half-resolution output.
  - Setting it to 100 live switched the output to full size.

  ```sh
  x86_64-w64-mingw32-g++ -std=c++17 -O2 -municode cvarpeek.cpp -o cvarpeek.exe -static -static-libgcc -static-libstdc++
  ```

### Frame generation prototype (`kmt/`)

- **`d3dkmt_hws.c`/`.def`** is a small DLL implementing `D3DKMTEnumAdapters2` and `D3DKMTQueryAdapterInfo`, reporting D3DMetal's adapter with hardware scheduling enabled.
  - It must not call back into DXGI. D3DMetal's `dxgi.dll` calls `D3DKMTEnumAdapters2` from its own callback thread, and a version that called `CreateDXGIFactory1` there crashed with an illegal instruction.
- **`add_forwarders.py`** adds forwarded exports to Wine's `gdi32.dll` that point at that DLL. It needs `pefile`, installed in `venv/`.

Heroic now does this without a helper DLL: `src/backend/utils/macos/d3dkmt.ts` writes the two functions as machine code into a new section of `gdi32.dll`.

```sh
x86_64-w64-mingw32-gcc -O2 -Wall -shared -nostdlib -o d3dkmt_hws.dll d3dkmt_hws.c d3dkmt_hws.def -Wl,--entry,DllMain -lkernel32 -s
venv/bin/python kmt/add_forwarders.py gdi32.dll gdi32-patched.dll \
  D3DKMTEnumAdapters2=d3dkmt_hws.D3DKMTEnumAdapters2 D3DKMTQueryAdapterInfo=d3dkmt_hws.D3DKMTQueryAdapterInfo
```

### MetalFX hook (`inject/`)

- **`hb2hook.m`** replaces `-[MTLFXFrameInterpolatorDescriptor newFrameInterpolatorWithDevice:]` inside the game's x86_64 process and logs every frame interpolator D3DMetal creates to `/tmp/hb2hook.log`.
- macOS strips `DYLD_INSERT_LIBRARIES` when it passes through a protected binary like `/bin/zsh`. Export it inside the launch script, right before `wine64` starts.

```sh
xcrun clang -arch x86_64 -arch arm64 -dynamiclib -fobjc-arc -O2 -mmacosx-version-min=26.0 -framework Foundation -o hb2hook.dylib hb2hook.m
```

### MetalFX benchmark (`mfx/`)

- **`mfxbench.m`** times MetalFX temporal scaling and frame interpolation on the GPU for given input and output sizes.
- At 5120x1440 output, frame interpolation costs about 5.4 ms of GPU time per generated frame.

```sh
for a in arm64 x86_64; do
  xcrun clang -O2 -fobjc-arc -mmacosx-version-min=26.0 -arch $a -framework Foundation -framework Metal -framework MetalFX mfxbench.m -o mfxbench-$a
done
./mfxbench-arm64 1705 480 5120 1440 60
```

### Checks against the fork's TypeScript (`gvas/`, `tsverify/`)

Bundle with the fork's esbuild, then run with Node:

```sh
npx --prefix ~/Documents/HeroicGamesLauncher esbuild tsverify/compare.ts --bundle --platform=node --outfile=tsverify/compare.js
```

- **`dump.ts`** prints properties of an Unreal save (`GraphicsSettings.Windows.sav`, `SystemSettings.Windows.sav`) with the fork's GVAS parser.
- **`compare.ts`** applies the fork's Hellblade II preset to an earlier save and checks that the DLSS entry is identical to the one the game wrote itself. It is.
- **`run.ts`** patches a `gdi32.dll` with the fork's D3DKMT code and checks exports and checksum.
- **`sync.ts`** runs the fork's full graphics preset step against a prefix.

## Results

All results are from gameplay at 5120x1440 output with frame generation on, every setting at High. They were measured with `metalperftrace` (`out/ablate.log`).

| Experiment | Render resolution | Shown fps | Real fps | GPU ms per real frame |
| --- | --- | --- | --- | --- |
| DLSS Quality (baseline) | 3416x961 | 26.2 | 13.2 | 77.1 |
| DLSS Balanced | 2970x836 | 31.2 | 15.6 | 64.7 |
| DLSS Performance | 2560x720 | 29.8 | 15.0 | 67.9 |
| DLSS Ultra Performance | 1705x480 | 37.4 | 18.4 | 56.8 |
| Lumen global illumination off | 3416x961 | 34.3 | 16.9 | 59.1 |
| Lumen reflections off | 3416x961 | 26.8 | 13.4 | 75.4 |
| Virtual shadow map LOD bias 0.5 | 3416x961 | 26.7 | 13.4 | 75.8 |
| Volumetric fog off | 3416x961 | 28.8 | 14.4 | 70.6 |

- The baseline was measured again between experiments, and stayed between 12.6 and 13.6 real fps.
- A screen probe downsample factor of 32 stalled rendering (4.5 fps), so it's not a usable setting.
- In normal play at Ultra Performance the game shows about 45 fps.

What the numbers say:

- **Resolution only explains part of the cost.** Cutting rendered pixels by 75% saved 21 of 77 ms. Roughly 35–40 ms per frame goes to work that doesn't scale with resolution: lighting, shadows, geometry and fog. That's an estimate from two runs in different scenes.
- **Memory isn't the limit.** The game uses about 9.6 GB of GPU memory.
- **D3DMetal has a lot to translate.** Each frame carries about 1,200 draws, 970 `ExecuteIndirect` calls, 500 compute dispatches and 94 command buffers.
- **Frame interpolation adds latency.** It delays the picture by one real-frame interval, 77 ms at Quality per the HUD.
- **The Neural Engine refuses the game's Rosetta process.** The game logs (`out/game-fg-*.log`) show repeated "Bad argument" errors compiling Neural Engine models, so MetalFX's model runs on the GPU instead.

`out/fg-menu.jpg` shows the half-resolution bug before the fix. On the game's startup screen, the Metal HUD reports MetalFX upscaling 1708x481 to 2560x720, which is then stretched to the 5120x1440 screen.
