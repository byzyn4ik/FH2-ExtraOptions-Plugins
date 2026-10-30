# Xtra Features for Forza Horizon 2 Extra Options

Four small additions to Forza Horizon 2 in one plugin for the mod loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions),
each with its own switch:

- **Map zoom.** The pause map always opens at its farthest zoom-out (2500 m on
  the mainland, 1100 m on Storm Island), and that is also as far as it zooms
  out. With Xtra Features it opens farther out and zooms out farther.
- **Garage thumbnails.** The pictures of your own cars in My Cars and the car
  strip are rendered at the console's 816x459 and 192x108, which looks soft at
  1440p and 4K. Xtra Features renders them 2, 3 or 4 times larger; **F8**
  re-renders the pictures you already have.
- **Photo scan.** Forza Horizon 6's quick car scan in Forza Horizon 2's photo
  mode: **RB** credits the cars in view to the photo collection, as after a
  real photo, without taking one.
- **Exit Game.** An Exit Game tile on the pause menu's Home tab that closes
  the game (the [Exit Game](../exit-game/README.md) plugin, carried here too).

The first three come from the Extra Options fork these plugins grew out of;
none of them needs a fork any more: it is one DLL in a mod folder, loaded by
a stock Extra Options 0.4.4.

## Requirements

- Forza Horizon 2 on Azimuth 1.0.4.
- Forza Horizon 2 Extra Options 0.4.4 (its mod loader).

## Install

1. Close the game.
2. Copy the `mods\Xtra Features` folder (from a release ZIP, or `out\mods`
   after building) into the `mods` folder beside `Azimuth.exe` and Extra
   Options' `winmm.dll`:

   ```text
   Azimuth\
     Azimuth.exe
     winmm.dll
     mods\
       Xtra Features\
         mod.json
         plugins\
           XtraFeatures.dll
           XtraFeatures.ini
   ```

3. Start the game. Extra Options adds the folder to `mods\loadorder.txt` and
   loads the plugin; `FH2_ExtraOptions.log` shows lines starting with
   `XTRA FEATURES:`.

To turn a feature off, set its switch to `0` in `XtraFeatures.ini`; to turn
the plugin off, comment the folder out in `mods\loadorder.txt`
(`# Xtra Features`) or delete the folder.

The standalone Exit Game plugin can stay installed beside it: whichever of the
two edits the pause page first adds the tile and binds it, the other finds it
there and leaves it. There is one tile either way; `ExitGame=0` here leaves it
to the standalone plugin.

## Settings

`mods\Xtra Features\plugins\XtraFeatures.ini`, read when the game starts
(Extra Options' own `FH2_ExtraOptions.ini` is not used or changed). A value
out of range turns its feature off (the log names the line); a missing key
keeps its default.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[XtraFeatures]` | `MapZoom` | `1` | the pause map zoom below |
| | `GarageThumbnails` | `1` | larger garage thumbnails and F8 |
| | `PhotoScan` | `1` | RB in photo mode scans the cars in view |
| | `ExitGame` | `1` | the Exit Game tile |
| `[MapZoom]` | `MapStartZoom` | `150` | where the mainland map opens, in % of the game's distance (25-400) |
| | `MapMaxZoomOut` | `225` | how far it zooms out, in % of the game's 2500 m (100-400) |
| | `MapIslandStartZoom` | `100` | the same for Storm Island (25-400) |
| | `MapIslandMaxZoomOut` | `175` | the same for Storm Island's 1100 m (100-400) |
| `[GarageThumbnails]` | `CarPreviewScale` | `3` | thumbnails at 2, 3 or 4 times the console's size; `1`: the console's size (F8 still re-renders) |

The keys and ranges are the fork's (`[Frontend] MapStartZoom` ...,
`CarPreviewScale`, `[PhotoMode] QuickCarScan`), now each in its own section.

## The features

### Map zoom

- With the defaults the mainland map opens at 3750 m and zooms out to
  5625 m; Storm Island opens as before and zooms out to 1925 m (its map
  texture ends close to the island, so it has its own values).
- Panning and zooming keep the game's speed at a given height: the pan speed
  is scaled with the distance and the zoom speed with the range.
- Where a race or story script narrows the zoom range, the game's zoom is
  kept. The distance stays under 90% of the map camera's far plane. Beyond the
  game's own zoom-out the map shows its low-resolution world tile, as the
  game's maximum already does.
- With all four values at 100 nothing is installed.

### Garage thumbnails

- New pictures (a car bought, painted or upgraded) are rendered at
  `CarPreviewScale` times 816x459 and 192x108; 2 is pixel for pixel in the My
  Cars panel at 2160p. Larger scales make the save larger (2 has four times
  the pixels).
- **F8** (with the game's window in front) re-renders the picture of every car
  with a saved one, one car at a time, through the game's own background save
  (the same save tasks the game makes for a saved car). Stock cars keep the
  game's packaged pictures. The log says how many cars were rendered. F8 is
  one of the keys Keyboard & Mouse keeps free.

### Photo scan

- In photo mode's camera view (and with the interface hidden) **RB** opens the
  game's own photo analysis for the current camera: the same car detection
  (frustum, distance, detection sphere and occlusion rays; no picture is
  used), payout (1000 CR per new car plus the rank bonus) and "Cars
  photographed" result as after a real photo, but nothing is captured or
  saved and photo mode stays in its camera view. In other photo flows the cars
  are credited without the result screen.
- RB keeps its usual job in the effects panel (vignette) and before saving
  (watermark position). The help bar shows "Scan cars" (Polish: "Skanuj
  auta"; other languages English, `src\photo_scan_captions.json`).
- Each detected car is logged with its name and whether it was new. A fault
  during a scan turns the scan off until the game restarts.

### Exit Game

As in the [Exit Game](../exit-game/README.md) plugin (the same code,
`src\exit_game.c` and `src\pause_xaml.c`): a square Exit Game tile beside
Online Road Trip with the game's Quit icon and a caption in the game's
language (`src\exit_game_captions.json`); choosing it sends `WM_CLOSE` to
Azimuth's game window, as the title bar's close button does.

## How it works

Everything is in the game's image; no Azimuth or Extra Options code is called
or changed. Each feature checks its sites byte for byte and chains onto
whatever is there (the game's code, Extra Options' hook or another plugin's),
so plugins that hook the same sites stack in any load order. A feature whose
code is not as inspected changes nothing and says why; the others still
install.

| Feature | Site | How |
|---|---|---|
| map zoom | the pause map page setup's only `ResetView` call (`0xd80442` -> `0xb803f0`) | the call goes to a jump within 2 GB; the hook raises `MaxMapDistance` (and pan and zoom speed with it) and sets the opening zoom on the map view (`MapViewControllerInteractive`, vtable `0x2d9cd48`), then calls the previous target |
| thumbnails | the renderer's size block `0xaa6a81` (38 bytes) and the stored size table `0x1dfde70` (32 bytes, read only by `0x274121`, each time) | rewritten in place with every other thread suspended and none inside them |
| F8 | UserInterface's per-frame vtable slot 8, `0x2ecfbc0` (game `0x140ba10`) | the slot points at a stub that keeps the call's registers, runs the F8 check and the re-render step, then jumps to what the slot held |
| photo scan | the photo page's RB functor `_Do_call` slot `0x2e21b90` (game `0xde8070`) | the scan in the camera and hidden-interface states, the previous handler in all others |
| RB help text | the button bar's empty-string call for RB, `0xde3735` (-> `0x140be20`) | the call goes to a jump within 2 GB that builds "Scan cars"; left alone when another module already changed the call |
| Exit Game | the XAML resource call `0x16ebb37` and the pause page init slot `0x2e14310` | as in the Exit Game plugin (`fh2_chain_call`, `fh2_chain_slot`) |

The two calls at `0xd80442` and `0xde3735` are not 4-byte aligned, so the
shared `fh2_chain_call` cannot take them: `src\patch.c` swaps their
displacement inside the aligned 8-byte (`0xd80440`) or 16-byte (`0xde3730`)
block that holds it, with one compare-exchange of the whole block. The fork
used the int3 padding after the functions instead; that padding is left alone
(Extra Options uses such padding for its own thunks).

None of these sites is touched by Extra Options 0.4.4 (its patch list and
runtime hooks, and its bundled Weather Control and Garage Revving plugins) or
by the other plugins of this repository, apart from the two Exit Game sites,
which are chained like everyone else's.

### Guards

| What | Guard |
|---|---|
| host | Extra Options' Azimuth adapter; image size `0x3a18000` |
| map zoom | `ResetView` (84 bytes), the page setup's call (`E8`, its target `ResetView` or another module's code), the camera distance (36 bytes) and zoom input (111 bytes), the seven XAML property setters, the initial zoom in the constructor, the far plane read |
| thumbnails | the size block and table as the game has them, the table's reader, the first bytes of the nine game functions the re-render calls, the background save task's and UserInterface's vtables, slot 8 holding `0x140ba10` or another module's code |
| photo scan | the fork's 29 code blocks (the help text call's split around its displacement: 30) and 10 vtable entries (the RB functor and handler, detection, credit, entity queries, camera, database and loc strings, the photo analysis opener); the RB slot and help text call holding the game's target or another module's code |
| Exit Game | as in the Exit Game plugin |

## Build and test

From the repository's root:

```text
py build.py --zig <zig 0.15.2>\zig.exe --plugin xtra-features --test
```

writes `out\mods\Xtra Features\...`, `out\release\xtra-features-<version>.zip`
and a test build of the DLL with extra entry points
(`out\build\xtra-features\tests\XtraFeaturesTest.dll`), then runs
`tests\run_tests.py`: the package, the INI, the Extra Options 0.4.4 release
(none of these sites in its patch data or bundled plugins), every guard
against the game executable (each changed byte refused for its feature only;
sites already chained by another module accepted), the unaligned call
chaining and the suspended code write, the zoom arithmetic, the thumbnail
sizes (disassembled), the Exit Game page and captions, and
`FH2PluginInitialize` on the mapped game (alone and behind other modules'
hooks, which keep being called with their arguments; nothing changed when off,
on another adapter or image). The tests never run the game and only read its
files (`--game`, default `%USERPROFILE%\FH2`), Azimuth's release ZIP
(`--azimuth`) and the Extra Options release (`--release`); missing inputs
skip the tests that need them. `tests\run_combined.py` at the root then loads
it with every other plugin in one process.

What the offline tests cannot show: the features in the running game. Map
zoom, the thumbnails, F8 and the photo scan were tested in game in the fork on
Azimuth 1.0.2 (the game executable is the same); as a plugin on Azimuth 1.0.4
they are not yet tested in game.

## License

GPL-3.0-or-later, as Extra Options. See `LICENSE` at the repository's root.
