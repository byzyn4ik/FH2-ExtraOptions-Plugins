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

Use a clean Extra Options 0.4.4 release (`FH2_Extra_Options_0.4.4-azimuth.zip`)
on Azimuth 1.0.4. A plugin that finds the game, Azimuth or Extra Options other
than it was made for changes nothing and says why in `FH2_ExtraOptions.log`.
Forza Horizon 2 Builder can install them for you.

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
  build_common.py        Zig lookup, command runner, generated files, version resource
plugins/<id>/            one plugin: build.py, mod/ (mod.json, INI), src/, tests/, README.md
tests/                   all plugins together (run_combined.py, combined_test.cpp)
.github/workflows/       CI: build and test every push; release on a v* tag
```

The headers in `common/include` are C and C++, header only and without C
runtime calls, so the C++ plugin (Azimuth Graphics Options), the C plugin with
a C runtime (Exit Game) and the one without (Custom Radio) share them. The
plugins' own hook logic stays in each plugin.

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

`--plugin <id>` (repeatable) builds only those. `--test` runs every plugin's
offline tests and then `tests\run_combined.py`, which loads the test builds of
Azimuth Graphics Options, Exit Game and Custom Radio into one process with
the game executable and Azimuth 1.0.4 mapped, in all six install orders, with
the XAML resource call at `0x16ebb37` as the game and as Extra Options 0.4.4
leave it: Graphics and Exit Game both chain that call and must stack on each
other, Custom Radio installs beside them, and the Video, Audio and pause
pages are checked with each plugin's own page checks. No test runs the game
or writes to its files; they read them from `--game` (default
`%USERPROFILE%\FH2`), and the tests that need them skip themselves when they
are missing (as on CI).

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
all-in-one `FH2-ExtraOptions-Plugins-<version>.zip`.

## License

GPL-3.0-or-later (see `LICENSE`), as Extra Options. `common/include/fh2_plugin.h`
is Extra Options' plugin API header, unchanged.
