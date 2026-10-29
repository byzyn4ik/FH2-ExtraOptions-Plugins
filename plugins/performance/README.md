# Performance for Forza Horizon 2 Extra Options

An FPS counter for Forza Horizon 2 on Azimuth, as a plugin for the mod loader
of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions):

- **FPS counter** in the top-right corner of the game: the frame rate, GPU
  load and video memory; **Detailed** adds the frame time, the 1% low and the
  GPU frame time.
- **FPS counter row** (Off / On / Detailed) on the game's Video settings page,
  in the game's language; it applies at once.
- **Performance log** (optional): one CSV line a second with the FPS, the
  average, 99th percentile and longest frame times, the 1% low and the GPU
  frame time.

It is the FPS meter of Extra Options pull request #106 (`fh2-monitor.dll`,
`[Monitor] ShowFps`), made into one plugin DLL for a stock Extra Options: no
fork, no second DLL, no Extra Options settings. Installing it is the opt-in.

## Requirements

- Forza Horizon 2 on Azimuth 1.0.4.
- Forza Horizon 2 Extra Options 0.4.4 (its mod loader).

## Install

1. Close the game.
2. Copy the `mods\Performance` folder (from a release ZIP, or `out\mods` after
   building) into the `mods` folder beside `Azimuth.exe` and Extra Options'
   `winmm.dll`:

   ```text
   Azimuth\
     Azimuth.exe
     winmm.dll
     mods\
       Performance\
         mod.json
         plugins\
           Performance.dll
           Performance.ini
   ```

3. Start the game. Extra Options adds the folder to `mods\loadorder.txt` and
   loads the plugin; `FH2_ExtraOptions.log` shows lines starting with
   `PERFORMANCE:`.

To turn it off, choose Off on the Video page, set `ShowFps=0` in
`Performance.ini`, comment the folder out in `mods\loadorder.txt`
(`# Performance`), or delete the folder.

## Settings

`mods\Performance\plugins\Performance.ini` (Extra Options' own
`FH2_ExtraOptions.ini` is not used or changed):

| Key in `[Performance]` | Default | Meaning |
|---|---|---|
| `ShowFps` | `1` | `0` off; `1` FPS, GPU load, video memory; `2` also the frame time, the 1% low and the GPU frame time |
| `PerfLog` | `0` | `1` appends one line a second to the performance log |
| `PerfLogFile` | (empty) | the log's path, full or relative to the plugin's folder; empty: `fh2-perf.csv` beside the INI |
| `MenuRow` | `1` | `1` adds the FPS counter row to the Video settings page |

The Video page row writes `ShowFps` here. While the counter or the log is on,
changes to `ShowFps` and `PerfLog` in the file apply within a second. With
`ShowFps=0` and `PerfLog=0` at the start nothing is hooked until the counter is
turned on in the Video page row.

The log's columns: `time, session, elapsed_s, interval_s, frames, avg_fps,
avg_ms, p99_ms, max_ms, low1_fps, gpu_ms, gpu_samples, overlay`.

## How it works

### Attaching late

Extra Options starts plugins on its loader worker after its own hooks, usually
after Azimuth has made its D3D12 device and swap chain, so the plugin cannot
wait for them to be made (the PR's monitor was loaded by Extra Options at the
game's first `D3D12CreateDevice`). From `FH2PluginInitialize` it starts a
thread of its own that makes small objects of its own and takes the vtables
from them (`src\attach.cpp`):

- a DXGI factory: `CreateSwapChain` and `CreateSwapChainForHwnd` are hooked,
  for swap chains made later;
- a D3D12 device on the default adapter (WARP when there is no D3D12 GPU).
  D3D12 devices are one per adapter: when the game's device exists, this is the
  game's device and nothing new is made;
- a direct command queue: `ExecuteCommandLists` is hooked;
- a 64x64 flip-model swap chain on a hidden window of its own: `Present`,
  `Present1` and `SetColorSpace1` are hooked.

All of them are released again; the vtables live in `dxgi.dll` and
`D3D12Core.dll`. The game's swap chain is the same DXGI class, so its next
`Present` comes through the hook whether it was made before or after the
attach.

A swap chain made after the attach is known from the factory hook, with the
queue it was made with. For one made before it, DXGI does not tell the queue
(`GetDevice` on a D3D12 swap chain gives the device), so the plugin takes the
direct queue of that device that submitted command lists on the presenting
thread since that thread's previous `Present`: Azimuth 1.0.4 submits its copy
into the back buffer on the swap chain's queue right before presenting, on the
same thread.

### Hooks and chaining

Every hook is a vtable slot swap that keeps what the slot held (DXGI's or
D3D12's function, or another overlay's hook) and calls it, one original per
vtable. Another overlay that hooked the slot first stays in the chain behind
the plugin; one that hooks it later chains onto the plugin. Overlays that patch
the functions' code instead of the vtable are called through as well. The log
names each slot and the module it was chained onto. Extra Options 0.4.4 itself
does not hook DXGI or D3D12.

### Measuring and drawing

- Frame times come from the `Present` calls of the measured swap chain. When
  several swap chains present, Azimuth's game window (class
  `Azimuth.Game.Win32`) is measured; otherwise the newest; another one takes
  over once the measured one has been idle for a second.
- The counter is one alpha-blended quad drawn into the back buffer just before
  the real `Present`, on the presenting queue, with shaders compiled at build
  time (`src\overlay.hlsl`, no shader compiler in the game). On HDR (PQ or
  scRGB) the yellow is about 200 nits; a swap chain made before the attach
  gets its colour space from its format and display until `SetColorSpace1`.
- The GPU frame time is the span between two timestamp queries: one before
  the first command list after a `Present`, one at the next `Present`.
- GPU load and video memory come from PDH (Task Manager's 3D engine load and
  dedicated memory of the adapter) on a worker thread, once a second.
- Any GPU failure turns off the counter and the GPU time and keeps the CPU
  frame times; the log says why.

### The Video page row

The game reads every XAML resource through one call, `call 0xaabec0` at
`0x16ebb37`. Extra Options 0.4.4 points that call at its thunk (`0x13fe354`);
other plugins (Azimuth Graphics Options, Exit Game, Weather Control) chain it
too. The plugin chains it the same way (only the call's 4-byte displacement is
swapped, to a jump within 2 GB; the handler calls whatever the site called
first) and, for the Video page only, adds one `TileMenuDataItem` and one
`SpinnerControl` as the list's last row (`src\video_row.cpp`). A list with the
fixed style gets the scrollable style, and its 110 px height (the page alone)
becomes 275 (as after Extra Options); other heights are kept, so Azimuth
Graphics Options still finds its 110/275 list and both plugins make a valid
page in either order. The row binds to `PerfFpsCounter` and
`PerfFpsCounterChoices`, registered in the Video page type's registry with the
game's own property descriptors, as Extra Options registers its rows.

### Guards

| What | Guard |
|---|---|
| host | Extra Options' Azimuth adapter; otherwise nothing is installed. No Azimuth address is used; the log names the Azimuth build (1.0.4 by TimeDateStamp `0x6ae393eb`) |
| the counter | each vtable slot is written only when it holds code; a failed attach is logged and leaves the game as it is |
| the Video page row (game image) | image size `0x3a18000`; the first 12 bytes of every native function it calls; the Subtitles and OnOffOptions property templates; the Video page type's registration; `E8` at `0x16ebb37` followed by the game's own code, calling the game's reader, Extra Options' thunk or another module's code. On any mismatch the row is not added (the INI still works) |
| the page | `x:Class="ForzaUI.A_VideoOptions"`, its title and brightness preview, not a plugin's standalone page, the resources before the options list, no row of ours yet |

## Azimuth 1.0.4 presentation

From `azimuth.exe` 1.0.4 (checked byte for byte by the tests):

- DXGI through `CreateDXGIFactory2`; D3D12 through `D3D12CreateDevice`
  (ordinal 101); no D3D11.
- Its swap chain is made with `IDXGIFactory2::CreateSwapChainForHwnd` on its
  D3D12 queue: flip discard, 3 buffers, `FRAME_LATENCY_WAITABLE_OBJECT` (plus
  `ALLOW_TEARING` when the factory supports it), `SetMaximumFrameLatency(1)`,
  `MakeWindowAssociation(NO_WINDOW_CHANGES | NO_ALT_ENTER)`; the format is the
  game's, or `R10G10B10A2_UNORM` with one of Azimuth's settings on (HDR, by
  its log texts).
- Each frame it submits its copy into the back buffer on the queue
  (`ExecuteCommandLists`, `Signal`), then calls
  `IDXGISwapChain::Present` (slot 8) on the same thread. It never calls
  `Present1`.

## Build and test

From the repository's root:

```text
py build.py --zig <zig 0.15.2>\zig.exe --plugin performance --test
```

writes `out\mods\Performance\...`, `outelease\performance-<version>.zip`, a
test build of the DLL with extra entry points
(`outuild\performance	ests\PerformanceTest.dll`) and the D3D12 test
(`outuild\performance	ests\d3d12_test.exe`), then runs
`testsun_tests.py`. The shaders are compiled with Windows'
`d3dcompiler_47.dll` at build time. The tests never run the game and only read
its files (`--game`, default `%USERPROFILE%\FH2`): the game executable (mapped
and patched in the test's own memory) and `UI.zip`, Azimuth's release ZIP
(`--azimuth`), the Extra Options 0.4.4 release (`--release`) and source
(`--upstream`; its Video page edit is built for the test) and the Azimuth
Graphics Options source (`pluginszimuth-graphics-options`, or `--graphics`;
its page edit is built for the test). The D3D12 test makes its own device and
swap chains on hidden windows and attaches the plugin after them (and, in a
second process, before any device); `--warp` runs it on WARP. Missing inputs
skip the tests that need them; the D3D12 debug layer is used when Windows'
optional "Graphics Tools" feature is installed.

## License

GPL-3.0-or-later, as Extra Options. See `LICENSE` at the repository's root.
