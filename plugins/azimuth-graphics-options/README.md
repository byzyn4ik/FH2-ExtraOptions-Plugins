# Azimuth Graphics Options for Forza Horizon 2 Extra Options

Azimuth's graphics settings as rows of Forza Horizon 2's own **Video** settings
page, and its sound settings as rows of the **Audio** page, so they can be
changed in game. This is a plugin for the mod loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions):
one DLL in a mod folder, loaded by a stock Extra Options 0.4.4 (or 0.3.2). It
needs no fork of Extra Options.

| Page | Row | Choices | Saved in `data\azimuth.ini` | Applies |
|---|---|---|---|---|
| Video | Frame rate | Azimuth's own list (see below) | `[title-1a96545b] frame_rate` | at once |
| | V-sync | Off, On | `vsync` | after a restart |
| | Base resolution | 960 x 540 ... 3840 x 2160 | `resolution` | after a restart |
| | Display mode | Windowed, Fullscreen | `[launcher] display` | at once (as Alt+Enter) |
| | Graphics | Low, Default, High, Ultra, Custom | `graphics` | shadow distance and update rate at once, the rest after a restart |
| | Shadows | Low, Default, High, Ultra | `shadow_detail`, `shadow_distance`, `shadow_redraw` (sets Graphics to Custom) | distance and update rate at once, map size after a restart |
| | Texture filtering | Default, 2x, 4x, 8x, 16x | `texture_filtering` (sets Graphics to Custom) | after a restart |
| | Anti-aliasing | Off, 2x MSAA, 4x MSAA | `antialiasing` | after a restart |
| | HDR | Off, On | `hdr` | after a restart |
| Audio | Audio output (Azimuth) | Stereo, System (Windows), Multichannel | `audio` = `stereo`, `system`, `multichannel` | after a restart |
| | Volume (Azimuth) | slider 0 ... 100 % in 5 % steps | `volume` = 0 ... 100 | at once |

The Video rows come after the page's own Brightness and Subtitles and before
Extra Options' rows; the Audio rows come last, after the page's own rows and
Extra Options' (Master Volume, dialogue and checkpoint rows). *Volume
(Azimuth)* is Azimuth's output level, on top of the game's own volumes and
Extra Options' *Master Volume*.

Captions are in English; in Polish when the game runs in Polish; in Ukrainian
when the game runs a custom Ukrainian language pack in the ru-RU slot (as the
Extra Options fork's custom languages lay it out: `FH2_ExtraOptions.ini`
`[Language] Custom=<id>` and `Mount\languages\<id>\language.ini` with
`Slot=ru-RU`, `Ui=uk`). `[Captions] Language` in the plugin's INI can force one.

Azimuth 1.x has no frame pacing setting (it always paces in real time), so
there is no Frame pacing row. Keys no row writes (`aspect`, `reflections`,
`car_detail`, `particles`, `motion_blur`, `scenery_detail`, `language`,
`speech`) are kept as they are.

**Frame rate choices** are the ones Azimuth's launcher offers (the same code
in 1.0.4, at `0x104c0`, and 1.0.2, at `0x10590`): 30, then 60, 90, 120, 144,
165 and 240 up to the refresh rate of the display the game is on, then that
refresh rate itself when it is higher (for example 75 or 360). A saved rate
between two choices shows as the lower one, as in the launcher; without
`frame_rate` Azimuth runs FH2 at its own 30 FPS, and the row shows 30. The
list is logged when the Video page is first opened.

## Requirements

- Forza Horizon 2 on Azimuth **1.0.4** (`azimuth.exe` of the release) or
  1.0.2 (the release's, or the Forza Horizon 2 Builder's branded
  `Forza Horizon 2.exe` of that build). Other builds are refused: nothing is
  installed and the log says so.
- Forza Horizon 2 Extra Options 0.4.4 (0.3.2 works too): its mod loader.

## Install

1. Close the game.
2. Copy the `mods\Azimuth Graphics Options` folder (from `out\mods` after
   building, or from a release ZIP) into the `mods` folder beside the Azimuth
   executable and Extra Options' `winmm.dll`:

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

To turn it off, set `Enabled=0` under `[Graphics]` and `[Audio]` in
`AzimuthGraphicsOptions.ini`, or comment the folder out in
`mods\loadorder.txt`.

## Settings

`AzimuthGraphicsOptions.ini` beside the DLL, read when the game starts (Extra
Options' own INI is not changed; it is only read for its custom language):

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[Graphics]` | `Enabled` | `1` | add the rows to the Video page |
| | `LiveApply` | `1` | apply frame rate, display mode, shadows and volume at once; `0`: every change after a restart |
| | `AzimuthIni` | `<launcher>\data\azimuth.ini` | Azimuth's settings file (full, or relative to the launcher's folder) |
| `[Audio]` | `Enabled` | `1` | add the rows to the Audio page |
| `[Captions]` | `Language` | `auto` | `auto`, or always `en`, `pl` or `uk` |
| `[Logging]` | `Enabled` | `1` | `AzimuthGraphicsOptions.log` beside the plugin |

## How it works

- **Settings pages.** The game reads every XAML resource through one call on
  its UIRenderer path, `call 0xaabec0` at `0x16ebb37`. Extra Options (0.4.4
  as 0.3.2) points that call at its own thunk (`0x13fe354`) to extend the
  Video, HUD, Audio and other pages. The plugin keeps whatever the call goes
  to (Extra Options' hook, the game's reader, or another module's hook),
  points the call at a jump placed within 2 GB (only the call's 4-byte
  displacement is swapped, atomically) and calls the previous target first.
  It then adds its rows to the XAML that came back, for the Video and Audio
  pages only, with the list made scrollable (Video 440 px, Audio 550 px).
  Pages marked `<!-- EO:StandaloneOptionsPage -->` (Extra Options 0.4.4
  plugins' own pages, such as Weather Control's) and the fork's pages built
  from the Video page (KEYBOARD & MOUSE, World) are left alone. The rows bind
  to properties (`GfxFrameRate`, `AzVolume`, ...) the plugin registers in the
  page type's registry (Video `0x35470d0`, Audio `0x35107b0`, after the
  game's own flag-guarded set-up `0xeca710` / `0xc803c0`) with the game's own
  int, float and collection property descriptors, as Extra Options does for
  its rows.
- **Frame rate at once.** At startup Azimuth calls its rate setter with the
  title's `frame_rate` (1.0.4: `0x4ba30` from `0x945f`; 1.0.2: `0x4cc50` from
  `0x92f3`); that stores it as the target rate its pacing reads for every
  frame (1.0.4 `0x7d8554`, 1.0.2 `0x8e7540`; frame interval = QPC frequency /
  rate). The row writes `frame_rate` and the new rate there.
- **Display mode at once.** The row posts Azimuth's Alt+Enter to its window
  when the mode (1.0.4 `0x7fcbc1`, 1.0.2 `0x909d61`) differs.
- **Volume at once.** The row calls Azimuth's own volume setter (1.0.4
  `0xc37f0`, 1.0.2 `0xbaea0`), the one its startup and its in-game menu call:
  it clamps to 0..100 under Azimuth's audio lock and sets every output's level.
- **Audio output after a restart.** Azimuth latches the channel layout when it
  opens the audio output.
- **Shadows at once.** The cascade distances (`0x2207288`) and the shadow
  renderer's maps-per-frame (`[renderer+0x21bc]`) are set for the level in
  effect, as Azimuth sets them at startup.

### Guards and coexistence

Every site is checked byte for byte before use; on a mismatch nothing (or only
that live part) is installed and the log says why.

| What | Guard |
|---|---|
| host | `TimeDateStamp` 1.0.4 `0x6ae393eb` (`SizeOfImage >= 0x8a2000`) or 1.0.2 `0x6ae05dd1` (`>= 0x9af000`); the launcher's frame rate table (1.0.4 `0x10528`, 1.0.2 `0x105f8`) |
| frame rate | the setter is `mov [target],ecx; mov [title],edx; ret`, called from startup; the pacing reads the target (1.0.4 `0x4e8e7`, 1.0.2 `0x4fae7`); the value is a rate (30..1000) |
| display mode | `cmp byte [fullscreen],0` at the toggle (1.0.4 `0x6e868`, 1.0.2 `0x6f5f8`); `cmp rdi,0Dh; jne; bt rsi,1Dh` (1.0.4 `0x6eda7`, 1.0.2 `0x6fb27`) |
| volume | the setter's first 0x69 bytes (lock, clamp to 100, store, level), its stores pointing at the volume, called from startup; the volume holds 0..100 |
| game | image size `0x3a18000`; the first 12 bytes of each of the 20 native functions used; the int, float and list property templates (`Subtitles`, `Gamma`, `OnOffOptions`); the Video and Audio types' registries; `E8` at `0x16ebb37` followed by the game's own code; the call's target is executable |
| `data\azimuth.ini` | has `[launcher]` and `[title-1a96545b]` (Azimuth 1.x; 1.0.4 reads the same keys and values as 1.0.2) |
| shadow distance | the cascades hold one of Azimuth's three levels |
| shadow update rate | `mov [0x36609a0],rcx` at `0x112d760`, the original read at `0x1131c02`; at apply time the renderer's vtable and a value of 1 or 4 |

- Extra Options 0.4.4 patches none of the game bytes the plugin checks except
  the resource call (chained) and, with *Realtime shadows* On, the read at
  `0x1131c02`: the plugin then leaves the shadow update rate to it. It does not
  write `azimuth.ini`, and its rows (Video: Realtime shadows, photo samples,
  livery; Audio: Master Volume, dialogue, checkpoint sound) are other
  settings, so no key is written twice. Its Weather Control plugin registers
  its own properties on the Video type and shows its own page; its Garage
  Revving plugin does not touch the settings pages.
- Motion blur (`0x2e834e4`) is not touched: Extra Options' photo sampling uses it.
- The game's FrameRate render scenario class (vtable `0x2eb98d0`) is not
  touched: that is the game's own update rate, while this plugin moves
  Azimuth's presentation rate.

## Build and test

```text
py build.py --zig <zig 0.15.2>\zig.exe --test
```

writes `out\mods\Azimuth Graphics Options\...`, a test build of the DLL with
extra entry points (`out\tests\AzimuthGraphicsOptionsTest.dll`) and
`out\tests\plugin_test.exe`, then runs `tests\run_tests.py`. The tests never run
the game and only read its files (`--game`, default `%USERPROFILE%\FH2`):
the game executable and `UI.zip`, Azimuth's release ZIPs (`--azimuth` for
1.0.4, `--azimuth102`), a copy of `data\azimuth.ini`, Extra Options 0.4.4's
release (`--eo-release`, its `winmm.dll`) and source (`--eo-source`), and an
Extra Options git clone (`--upstream`, refs `origin/main` and `combined-10`).
Missing inputs skip the tests that need them.

Every push builds and tests the plugin on GitHub Actions
(`.github/workflows/build.yml`, Zig 0.15.2 checked by SHA-256); a pushed tag
`v*` also publishes a release with the mod folder zipped.

## License

GPL-3.0-or-later (see `LICENSE`). `src\fh2_plugin.h` is Extra Options' plugin
API header, unchanged.
