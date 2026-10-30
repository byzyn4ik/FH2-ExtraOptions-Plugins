# FH2 Extra Options plugins

Plugins for the mod loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions).
Each one is a mod folder with one DLL and its INI, loaded by a **stock**
Extra Options; none of them needs a fork of Extra Options, and they work
together in any combination and load order.

| Plugin | What it does | Settings (`mods\<Mod>\plugins\`) | Keys | Requirements |
|---|---|---|---|---|
| [Azimuth Graphics Options](plugins/azimuth-graphics-options/README.md) | Azimuth's graphics settings (frame rate, V-sync, resolution, display mode, graphics preset, shadows, texture filtering, anti-aliasing, HDR) as rows of the game's **Video** page and its sound settings (audio output, volume) on the **Audio** page; frame rate, display mode, shadow distance and volume apply at once | `AzimuthGraphicsOptions.ini` | none (game menus) | Azimuth 1.0.4 (or 1.0.2), Extra Options 0.4.4 (or 0.3.2) |
| [Exit Game](plugins/exit-game/README.md) | An **Exit Game** tile on the pause menu's Home tab (beside Online Road Trip, made square) that closes the game | `ExitGame.ini` | none (pause menu) | Azimuth 1.0.4, Extra Options 0.4.4 |
| [Custom Radio](plugins/custom-radio/README.md) | Your own MP3s (a `Music` folder beside `Azimuth.exe`) as one more station of the game's car radio | `CustomRadio.ini` | F9 tunes in, F7 next track, F6 restart / previous (keyboard or Xbox buttons, set in the INI) | Azimuth 1.0.4, Extra Options 0.4.4 |
| [Performance](plugins/performance/README.md) | An FPS counter in the top-right corner (FPS, GPU load, video memory; **Detailed** adds frame times, the 1% low and the GPU frame time), an **FPS counter** row (Off / On / Detailed) on the game's **Video** page and an optional performance log (CSV) | `Performance.ini` | none (Video page row, INI) | Azimuth 1.0.4 (D3D12), Extra Options 0.4.4 |
| [Keyboard & Mouse](plugins/keyboard-mouse/README.md) | Keyboard and mouse without a virtual controller: rebindable keys for driving and menus, mouse look, a menu pointer, key prompts, and a **KEYBOARD & MOUSE** page from a tile on the pause menu's Settings hub | `KeyboardMouse.ini` | WASD drive, Space handbrake, E/Q gears, Tab camera, R rewind, M map, P photo mode, Esc pause / back, Enter confirm, arrows in menus (all rebindable; see [Keyboard keys](#keyboard-keys)) | Azimuth 1.0.4 (or 1.0.2), Extra Options 0.4.4 |
| [Photo Export](plugins/photo-export/README.md) | Every photo taken in the game, copied once as a JPG file into `Documents\My Games\Forza Horizon 2\Photos` (or a folder you set); the saves are only read | `PhotoExport.ini` | none | Azimuth 1.0.4 (1.0.1 to 1.0.3 too), Extra Options 0.4.4 |
| [Saves in Documents (mirror)](plugins/documents-saves/README.md) | A verified copy (SHA-256) of every save in `Documents\My Games\Forza Horizon 2\Saves\1A96545B`, a few seconds after the game writes it. A mirror, not a move: the game keeps its saves in Azimuth's folder, which the plugin only reads; nothing is deleted | `DocumentsSaves.ini` | none | Azimuth 1.0.4 (1.0.1 to 1.0.3 too), Extra Options 0.4.4 |
| [Xtra Features](plugins/xtra-features/README.md) | Map zoom (the pause map opens and zooms out farther), sharper garage thumbnails (F8 re-renders them), photo scan (RB in photo mode credits the cars in view without a photo) and the Exit Game tile, each switchable | `XtraFeatures.ini` | F8 (thumbnails), RB in photo mode | Azimuth 1.0.4, Extra Options 0.4.4 |
| [Language Support](plugins/lang-support/README.md) | A language pack (an ordinary mod folder) in one of the game's language slots, and subtitles for spoken lines the game shows none for | `LangSupport.ini` | none | Azimuth 1.0.4 (or 1.0.2), Extra Options 0.4.4 |

Use a clean Extra Options 0.4.4 release (`FH2_Extra_Options_0.4.4-azimuth.zip`)
on Azimuth 1.0.4. A plugin that finds the game, Azimuth or Extra Options other
than it was made for changes nothing and says why in `FH2_ExtraOptions.log`.
Forza Horizon 2 Builder can install them for you.

### Keyboard keys

Default keys of Extra Options 0.4.4 (set in `[Bindings]` of
`FH2_ExtraOptions.ini`) and of the plugins:

| Key | Used by | Action |
|---|---|---|
| F1 | Extra Options | Hide the UI |
| F2 | Extra Options | Autodrive on / off |
| F3 | Extra Options | Headlights |
| F4 | Extra Options | Roof (hold while stationary) |
| Tab | Extra Options | Jump to a manufacturer in the car lists |
| Tab | Keyboard & Mouse | Camera (only while driving) |
| F9 / F7 / F6 | Custom Radio | Tune in / next track / restart or previous track |
| WASD, Space, E/Q, R, M, P, Esc, Enter, arrows | Keyboard & Mouse | Driving and menus (see its page) |

In Keyboard & Mouse:

- Its defaults don't clash with any of the keys above.
- Esc (pause / back), F2, F8, Alt and the Windows keys can't be bound.
- If you bind a key that Extra Options uses in the same situation, the log
  warns at start and the in-game page refuses it ("In use (Extra Options)").
- It does **not** check Custom Radio's keys: don't bind F9, F7 or F6 there,
  or change Custom Radio's keys in `CustomRadio.ini`.

Extra Options' gamepad bindings (D-pad Down for Autodrive, ...) read the pad
directly and are not affected by any plugin.

## Install

1. Close the game.
2. From the [latest release](https://github.com/byzyn4ik/FH2-ExtraOptions-Plugins/releases/latest)
   take `<plugin>-<version>.zip` for one plugin, or
   `FH2-ExtraOptions-Plugins-<version>.zip` for all of them.
3. Unpack it into the folder of `Azimuth.exe` (where Extra Options'
   `winmm.dll` is), so that each plugin ends up in its own `mods` folder:

   ```text
   Azimuth\
     Azimuth.exe
     winmm.dll                      (Extra Options)
     mods\
       Azimuth Graphics Options\
         mod.json
         plugins\AzimuthGraphicsOptions.dll, AzimuthGraphicsOptions.ini
       Exit Game\
         mod.json
         plugins\ExitGame.dll, ExitGame.ini
       Custom Radio\
         mod.json
         plugins\CustomRadio.dll, CustomRadio.ini
       Performance\
         mod.json
         plugins\Performance.dll, Performance.ini
       Keyboard & Mouse\
         mod.json
         plugins\KeyboardMouse.dll, KeyboardMouse.ini
       Photo Export\
         mod.json
         plugins\PhotoExport.dll, PhotoExport.ini
       Saves in Documents (mirror)\
         mod.json
         plugins\DocumentsSaves.dll, DocumentsSaves.ini
       Xtra Features\
         mod.json
         plugins\XtraFeatures.dll, XtraFeatures.ini
       Language Support\
         mod.json
         plugins\LangSupport.dll, LangSupport.ini
   ```

4. Start the game. Each plugin logs what it did in `FH2_ExtraOptions.log`.

To remove a plugin, delete its folder in `mods`. Each plugin's page has its
settings, how it works and how it checks the game before changing anything.

## Repository

```text
build.py                 builds every plugin (or --plugin <id>), --test runs the tests
VERSION                  the version of the all-in-one release ZIP
common/
  include/fh2_plugin.h   Extra Options' plugin API header (unchanged)
  include/fh2_guard.h    guarded reads, executable-code check, call targets,
                         near jump allocation, call and vtable-slot chaining
  include/fh2_game.h     the game's text language, the plugin's INI path
  include/fh2_saves.h    Azimuth's save layout (commit markers, revisions), SHA-256,
                         the Documents folder, the Azimuth build (Photo Export,
                         Saves in Documents)
  build_common.py        Zig lookup, command runner, generated files, version resource
  saves_testing.py       fake save trees in Azimuth 1.0.4's layout and the
                         azimuth.exe 1.0.4 facts the save plugins rely on (tests)
plugins/<id>/            one plugin: build.py, mod/ (mod.json, INI), src/, tests/, README.md
tests/                   all plugins together (run_combined.py, combined_test.cpp)
.github/workflows/       CI: build and test every push; release on a v* tag
```

The headers in `common/include` are C and C++, header only and without C
runtime calls, so the C++ plugins (Azimuth Graphics Options, Performance), the
C plugins with a C runtime (Exit Game, Keyboard & Mouse) and the one without
(Custom Radio) share them. The plugins' own hook logic stays in each plugin
(Keyboard & Mouse keeps its own fault-tolerant reads and its patch code, which
writes code with the other threads suspended).

### Build and test

Python 3.10+ and [Zig 0.15.2](https://ziglang.org/download/) (no Visual
Studio needed):

```text
py build.py --zig <zig-x86_64-windows-0.15.2>\zig.exe --all --test
```

writes, below `out\`:

- `mods\<Mod Name>\...`: each plugin's mod folder, as installed;
- `release\<id>-<version>.zip` (`mods\<Mod Name>\...` of one plugin, the
  plugin's version from its `mod.json`) and
  `release\FH2-ExtraOptions-Plugins-<version>.zip` (all of them; `--version`,
  else `VERSION`);
- `build\<id>\...`: each plugin's test builds, objects and test runs.

Options:

- `--plugin <id>` (repeatable) builds only those plugins. Performance's and
  Keyboard & Mouse's tests also use Azimuth Graphics Options, so build it
  with them.
- `--test` runs every plugin's offline tests, then the combined test.
- `--game <FH2 folder>` is where the tests read the game files from (default
  `%USERPROFILE%\FH2`). Tests never run the game or write to its files; the
  ones that need game files skip themselves when they're missing, as on CI.

The **combined test** (`tests\run_combined.py`) loads all five plugins' test
builds into one process with the game executable and Azimuth 1.0.4 mapped.
It runs every install order (120; `--orders cover` runs 24), both with the
game's own page loading and with Extra Options 0.4.4's patch on it. Four
plugins hook the same page-loading call (`0x16ebb37`), so each run checks
that they stack into one chain and that:

- the Video page gets Graphics' rows and the FPS counter row, once each;
- the Audio page gets Graphics' sound rows;
- the pause menu gets Exit Game's tile;
- the Settings hub gets the KEYBOARD & MOUSE tile, and that page stays
  standalone (no other plugin's rows);
- Reset on the KEYBOARD & MOUSE page resets only its options, while Reset on
  the Video page still reaches Extra Options' or the game's reset;
- every page is read once, and pages nobody edits are left untouched;
- Custom Radio installs beside the others.

Performance's DXGI/D3D12 hooks are tested by its own `d3d12_test.exe` (on
the GPU when there is one), not in the combined test. Photo Export and Saves
in Documents hook nothing in the game or in Azimuth (each runs one thread that
reads the saves folder), so they are tested on their own, on save trees in a
temporary folder.

### Adding a plugin

Put it in `plugins/<id>/` with `mod/mod.json` (its `id` is the folder's name)
and a `build.py` that takes `--zig`, `--output <folder>`, `--test` and
`--game <FH2 folder>`, writes `<output>\mods\<Mod Name>\` and, with `--test`,
runs its tests (printing `  ok`, `  FAIL` and `  skip` lines) and fails on a
failure. `build.py` and the workflow find it there; nothing lists the plugins.

### Releases

CI (`.github/workflows/build.yml`) builds and tests every push and pull
request with Zig 0.15.2 (checked by SHA-256). A pushed tag `v<version>`
publishes a release with each plugin's `<id>-<version>.zip` and the
all-in-one `FH2-ExtraOptions-Plugins-<version>.zip`. Release assets are named
by the plugin's id (`keyboard-mouse-0.0.1.zip`), never by its mod name, and
Forza Horizon 2 Builder finds them by `^<id>-[0-9]...\.zip$`; inside, the mod
folder keeps its name (`mods/Keyboard & Mouse/...`).

## License

GPL-3.0-or-later (see `LICENSE`), as Extra Options. `common/include/fh2_plugin.h`
is Extra Options' plugin API header, unchanged.
