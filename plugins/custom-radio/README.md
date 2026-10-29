# Custom Radio for Forza Horizon 2 Extra Options

Your own MP3s as an extra station on Forza Horizon 2's car radio. This is a
plugin for the mod loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions):
the MP3 files of a folder play, in folder order, as one more station of the
game's own radio, so the in-game radio volume, ducking, pausing and station
banner all apply. Windows decodes the MP3s.

It needs no fork of Extra Options: it is one DLL in a mod folder, loaded by a
stock Extra Options 0.4.4. It is the custom radio of the Extra Options pull
request "custom radio", made into a standalone plugin.

## Requirements

- Forza Horizon 2 on Azimuth (tested offline against the game executable that
  every Azimuth version runs, `forza_durango_release_final.exe`).
- Forza Horizon 2 Extra Options 0.4.4 or later (its mod loader).

## Install

1. Close the game.
2. Copy the `mods\Custom Radio` folder (from a release ZIP, or `out\mods`
   after building) into the `mods` folder beside `Azimuth.exe` and Extra
   Options' `winmm.dll`, and put your MP3s in a `Music` folder beside
   `Azimuth.exe`:

   ```text
   Azimuth\
     Azimuth.exe
     winmm.dll
     Music\
       01 First song.mp3
       02 Second song.mp3
     mods\
       Custom Radio\
         mod.json
         plugins\
           CustomRadio.dll
           CustomRadio.ini
   ```

3. Start the game. Extra Options adds the folder to `mods\loadorder.txt` and
   loads the plugin; `FH2_ExtraOptions.log` shows lines starting with
   `CUSTOM RADIO`. The station (`MY MUSIC` by default) is added when the game
   loads its radio stations, if the folder has playable MP3 files.

To turn it off, set `Enabled=0` in `CustomRadio.ini`, or comment the folder
out in `mods\loadorder.txt` (`# Custom Radio`).

## Keys

| Key | Default | Meaning |
|---|---|---|
| `RadioStation` | F9 | tunes the radio to the custom station |
| `RadioNext` | F7 | next track |
| `RadioRestart` | F6 | restarts the track; within 3 seconds of its start, the previous track |

The keys work only while driving with the game window focused (Extra
Options' "gameplay input" state: not in menus, loading or the pause menu).
Each press acts once; a key held from a menu must be released first. They do
not clash with Extra Options 0.4.4's own default keys (F1-F4, Tab). Xbox
buttons can be added in the INI (none by default). The D-pad radio cycle of
the game also reaches the custom station.

## Settings

`mods\Custom Radio\plugins\CustomRadio.ini`, read when the game starts (Extra
Options' own `FH2_ExtraOptions.ini` is not used or changed). An empty or
missing value means the default; a value that is not valid is logged and its
default used.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[CustomRadio]` | `Enabled` | `1` | add the station |
| | `Folder` | `Music` | the MP3 folder: full, or relative to the folder of `Azimuth.exe` (`..` allowed) |
| | `Name` | `MY MUSIC` | the station name shown in game (`MOJA MUZYKA` for Polish) |
| | `Volume` | `100` | station volume in percent, 0-400 |
| | `UnlockAllStations` | `0` | every station is available, whatever the progression (also with `Enabled=0`) |
| | `PcmDecode` | `1` | `1`: Windows decodes the MP3s; `0`: the game's FMOD decoder plays them (it loses the centre of joint-stereo files) |
| `[Keys]` | `RadioStationKeyboard` | `F9` | |
| | `RadioNextKeyboard` | `F7` | |
| | `RadioRestartKeyboard` | `F6` | |
| | `RadioStationXbox`, `RadioNextXbox`, `RadioRestartXbox` | `None` | |

Keyboard keys: `F1`-`F24`, `A`-`Z`, `0`-`9`, `Escape`, `Enter`, `Space`,
`Tab`, `Backspace`, `Insert`, `Delete`, `Home`, `End`, `PageUp`, `PageDown`,
`Up`, `Down`, `Left`, `Right`, `Shift`, `Ctrl`, `Alt`, a virtual-key code
(`0x78` or `120`), or `None`. Xbox buttons: `A`, `B`, `X`, `Y`, `LB`, `RB`,
`DPadUp`, `DPadDown`, `DPadLeft`, `DPadRight`, `View`, `Menu`, `LeftStick`,
`RightStick`, combined with `+` (`LB+Y`), or `None`.

A `Name` with characters outside your Windows code page needs the INI saved
as Unicode (UTF-16 LE, "Unicode" in Notepad); Windows reads other INI files
in the code page.

### Music

Files ending in `.mp3` directly in the folder (no subfolders), in the order
Windows lists them (by name), up to 2048 files of up to 256 MB each. Titles
and artists come from the ID3v2 (`TIT2`, `TPE1`) or ID3v1 tag, else the file
name. A file whose length cannot be read, or that is shorter than 3 seconds,
is skipped (and logged). The playlist wraps around at both ends.

## How it works

The game reads `media\audio\RadioInfo_<language>.xml` and builds one station
per `<RadioStation>`; each track names an FMOD Studio event and a sound key.
The plugin hooks:

| Site (RVA in the game) | Kind | What the plugin does |
|---|---|---|
| CXML4Resource read, slot `0x2ec4fd0` | virtual-table slot | adds one `<RadioStation>` with `FH2MUSIC_<n>` sound keys to RadioInfo |
| getSoundInfo call `0x10189a0` | rel32 call | answers `FH2MUSIC_<n>` with the decoded file in memory |
| getSubSound call `0x10221f2` | rel32 call | returns the stream itself for its own sounds |
| radio player update, slot `0x2e76248` | virtual-table slot | next track / restart / previous on the radio thread |
| playlist next pick, slot `0x2e76740` | virtual-table slot | folder order with wrap-around for its own playlists |
| recently-played search call `0x1009140` | rel32 call | its own station ignores the recently-played list |
| IsStationLocked calls `0x660fd1`, `0x78a092`, `0x8fc8bf` | rel32 calls | the custom station (or, with `UnlockAllStations=1`, every station) is never locked |
| ChangeStation calls `0x8fc659`, `0xc9fad5` | rel32 calls | the D-pad cycle includes the custom station |

and calls the game's `RadioModule::SetStation` (F9), its event timeline and
volume calls, and its allocator. All other sounds, stations and files go to
the game unchanged.

Every site is checked byte for byte first (`src\sites.h`: the hooked
function's first bytes, the slot's value, and the 8 bytes around each call).
A slot or call that another module has hooked already (code outside the
expected function) is chained: the plugin calls that code instead of the game
function. A group of sites is written only when all of them pass, through an
interlocked compare-and-swap of the checked value; on any mismatch the plugin
logs the site and leaves that group, or the whole station, alone. Calls are
redirected through jumps in a memory page of the plugin's own next to the
game; no bytes of the game other than the slots and the call displacements
change.

Extra Options 0.4.4 does not hook any of these sites itself (its XML hook is
the next slot of the same table, `0x2ec4fe0`), and its mod-loader plugin API
gives the plugin the local car's update with the gameplay-input state, which
is where the keys are read.

The plugin starts when Extra Options loads mod plugins (on its first game
file lookup, on a worker thread). If the game had already read its radio
stations by then, the log says so and the station appears from the next
start.

## Building

Python 3.10+ and [Zig 0.15.2](https://ziglang.org/download/) (C, every file
built with `-Wall -Wextra -Werror`; no Visual Studio needed). Like Extra
Options' own plugins, the DLL has no C runtime of its own: it links import
libraries made from Windows' `kernel32`, `user32` and `msvcrt`.

```text
py build.py --zig <zig-x86_64-windows-0.15.2>\zig.exe --test
```

Output in `out\`: `mods\Custom Radio\` (the mod to install) and
`obj\CustomRadio-test.dll` (the same code with test exports). `--test` runs
the offline tests. CI (GitHub Actions) builds and tests every push and pull
request and keeps the mod folder as an artifact; a `v*` tag publishes it as a
release ZIP.

## Testing

`py tests\run_tests.py` (after a build; `build.py --test` runs it) checks,
without the game:

1. **Package**: `mod.json` as Extra Options validates it, the folder layout,
   the DLL (x64, exactly the loader's two exports, only Windows imports) and
   the INI.
2. **Sites**: `src\sites.h` is well formed; with `--game-exe <path>` (or
   `FH2_GAME_EXE`) every guard byte is compared with
   `forza_durango_release_final.exe` and none of them is relocated.
3. **Coexistence**: the default keys are not Extra Options 0.4.4's; with
   `--extra-options <Azimuth folder>` (or `EO_RELEASE_DIR`) its INI bindings,
   `winmm.dll` and bundled plugins are checked too (no key and no hook site
   in common).
4. **Config**: INI parsing (defaults, valid and bad values, unknown keys,
   relative, absolute and UNC folders, key and button names).
5. **Keys**: one action per press, only while driving, release before rearm.
6. **MP3**: generated silent MP3s (MPEG-1 Layer III): ID3v2 and ID3v1 tags,
   Xing and constant-bit-rate lengths, and the Windows decode to 16-bit PCM.
7. **Plugin**: the test DLL loaded with a fake game image built from
   `sites.h`, one child process per scenario: vanilla sites; sites already
   hooked by another module (chained: RadioInfo injection, sound and subsound
   serving, the station lock, the D-pad and playlist hooks, and the keys
   through `FH2PluginVehicleUpdate` to the radio thread); a mismatch in the
   station's core (nothing written) and in an optional group (only that group
   left alone); `Enabled=0`; `UnlockAllStations=1` alone; a slot without code;
   another image size.

## License

GPL-3.0 (see `LICENSE`). The station code comes from Forza Horizon 2 Extra
Options (the custom radio pull request) and `src\fh2_plugin.h` from Extra
Options itself, both GPL-3.0.
