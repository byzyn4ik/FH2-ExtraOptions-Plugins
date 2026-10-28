# Azimuth Graphics Options for Forza Horizon 2 Extra Options

Azimuth's graphics settings as rows of Forza Horizon 2's own **Video** settings
page, so they can be changed in game. This is a plugin for the mod loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions):
one DLL in a mod folder, loaded by a stock Extra Options 0.3.2. It needs no
fork of Extra Options.

| Row | Choices | Saved in `data\azimuth.ini` | Applies |
|---|---|---|---|
| Frame rate | Azimuth's own list (see below) | `[title-1a96545b] frame_rate` | at once |
| V-sync | Off, On | `vsync` | after a restart |
| Base resolution | 960 x 540 ... 3840 x 2160 | `resolution` | after a restart |
| Display mode | Windowed, Fullscreen | `[launcher] display` | at once (as Alt+Enter) |
| Graphics | Low, Default, High, Ultra, Custom | `graphics` | shadow distance and update rate at once, the rest after a restart |
| Shadows | Low, Default, High, Ultra | `shadow_detail`, `shadow_distance`, `shadow_redraw` (sets Graphics to Custom) | distance and update rate at once, map size after a restart |
| Texture filtering | Default, 2x, 4x, 8x, 16x | `texture_filtering` (sets Graphics to Custom) | after a restart |
| Anti-aliasing | Off, 2x MSAA, 4x MSAA | `antialiasing` | after a restart |
| HDR | Off, On | `hdr` | after a restart |

Captions are in English, and in Polish when the game runs in Polish.
Azimuth 1.x has no frame pacing setting (it always paces in real time), so
there is no Frame pacing row.

**Frame rate choices** are the ones Azimuth 1.0.2's launcher offers (its
code at `0x10590`): 30, then 60, 90, 120, 144, 165 and 240 up to the refresh
rate of the display the game is on, then that refresh rate itself when it is
higher (for example 75 or 360). A saved rate between two choices shows as the
lower one, as in the launcher. The list is logged when the Video page is
first opened.

## Requirements

- Forza Horizon 2 on Azimuth 1.0.2 (`azimuth.exe` of the release, or the
  Forza Horizon 2 Builder's branded `Forza Horizon 2.exe` of the same build).
  Other builds are refused: nothing is installed and the log says so.
- Forza Horizon 2 Extra Options 0.3.2 or later (its mod loader).

## Install

1. Close the game.
2. Copy the `mods\Azimuth Graphics Options` folder (from `out\mods` after
   building) into the `mods` folder beside the Azimuth executable and Extra
   Options' `winmm.dll`:

   ```text
   mods\
     Azimuth Graphics Options\
       mod.json
       plugins\
         AzimuthGraphicsOptions.dll
         AzimuthGraphicsOptions.ini
   ```

3. Start the game. `FH2_ExtraOptions.log` (and `AzimuthGraphicsOptions.log`
   beside the DLL) show lines starting with `AZIMUTH GRAPHICS OPTIONS:`.

To turn it off, set `Enabled=0` in `AzimuthGraphicsOptions.ini`, or comment the
folder out in `mods\loadorder.txt`.

## Settings

`AzimuthGraphicsOptions.ini` beside the DLL, read when the game starts (Extra
Options' own INI is not used or changed):

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[Graphics]` | `Enabled` | `1` | add the rows to the Video page |
| | `LiveApply` | `1` | apply frame rate, display mode and shadows at once; `0`: every change after a restart |
| | `AzimuthIni` | `<launcher>\data\azimuth.ini` | Azimuth's settings file (full, or relative to the launcher's folder) |
| `[Logging]` | `Enabled` | `1` | `AzimuthGraphicsOptions.log` beside the plugin |

## How it works

- **Video page.** The game reads every XAML resource through one call on its
  UIRenderer path, `call 0xaabec0` at `0x16ebb37`. Extra Options 0.3.2 already
  points that call at its own thunk (`0x13fe354`) to extend the Video, HUD and
  Audio pages. The plugin keeps whatever the call goes to (Extra Options'
  hook, the game's reader, or another module's hook), points the call at a
  jump placed within 2 GB (only the call's 4-byte displacement is swapped,
  atomically) and calls the previous target first. For the Video page only,
  it then adds its rows to the XAML that came back: after the page's own
  Brightness and Subtitles rows and before Extra Options' rows (Realtime
  shadows, photo samples, livery), with the list made scrollable (440 px).
  Pages Extra Options builds from the Video page (KEYBOARD & MOUSE, World)
  are left alone. The rows bind to properties (`GfxFrameRate`, ...) the plugin
  registers in the Video page type's registry (`0x35470d0`) with the game's
  own int and collection property descriptors, as Extra Options does for its
  rows.
- **Frame rate at once.** At startup Azimuth 1.0.2 calls `0x4cc50` from
  `0x92f3` with the title's `frame_rate`; that stores it in `0x8e7540`, the
  target rate its pacing reads for every frame (frame interval = QPC
  frequency / rate). The row writes `frame_rate` and the new rate there.
- **Display mode at once.** The row posts Azimuth's Alt+Enter to its window
  when the mode (`0x909d61`) differs.
- **Shadows at once.** The cascade distances (`0x2207288`) and the shadow
  renderer's maps-per-frame (`[renderer+0x21bc]`) are set for the level in
  effect, as Azimuth sets them at startup.

### Guards and coexistence

Every site is checked byte for byte before use; on a mismatch nothing (or only
that live part) is installed and the log says why.

| What | Guard |
|---|---|
| host | `TimeDateStamp 0x6ae05dd1`, `SizeOfImage >= 0x9af000`, the launcher's frame rate table at `0x105f8` |
| frame rate | `0x4cc50` is `mov [0x8e7540],ecx; mov [0x8e7544],edx; ret`, called from `0x92f3`; the pacing reads `0x8e7540` at `0x4fae7`; the value is a rate (30..1000) |
| display mode | `cmp byte [0x909d61],0` at `0x6f5f8`; `cmp rdi,0Dh; jne; bt rsi,1Dh` at `0x6fb27` |
| game | image size `0x3a18000`; the first 12 bytes of each of the 17 native functions used; `E8` at `0x16ebb37` followed by the game's own code; the call's target is executable |
| `data\azimuth.ini` | has `[launcher]` and `[title-1a96545b]` (Azimuth 1.x) |
| shadow distance | the cascades hold one of Azimuth's three levels |
| shadow update rate | `mov [0x36609a0],rcx` at `0x112d760`, the original read at `0x1131c02`; at apply time the renderer's vtable and a value of 1 or 4 |

- Extra Options 0.3.2 does not write `azimuth.ini` and has none of these rows,
  so no key is written twice. Its *Realtime shadows* row is a different
  setting: when it is On, Extra Options replaces the read at `0x1131c02` and
  the plugin leaves the shadow update rate to it.
- Motion blur (`0x2e834e4`) is not touched: Extra Options' photo sampling uses it.
- The game's FrameRate render scenario class (vtable `0x2eb98d0`, raised to 60
  in menus by the *smooth menus* Extra Options change) is not touched: that is
  the game's own update rate, while this plugin moves Azimuth's presentation
  rate.

## Build and test

```text
py build.py --zig <zig 0.15.2>\zig.exe --test
```

writes `out\mods\Azimuth Graphics Options\...`, a test build of the DLL with
extra entry points (`out\tests\AzimuthGraphicsOptionsTest.dll`) and
`out\tests\plugin_test.exe`, then runs `tests\run_tests.py`. The tests never run
the game and only read its files (`--game`, default `%USERPROFILE%\FH2`):
the game executable and `UI.zip`, Azimuth's release ZIP (`--azimuth`), a copy
of `data\azimuth.ini`, and Extra Options' own Video page code from a git clone
(`--upstream`, refs `origin/main` and `combined-10`). Missing inputs skip the
tests that need them.

## License

GPL-3.0-or-later (see `LICENSE`). `src\fh2_plugin.h` is Extra Options' plugin
API header, unchanged.
