# Exit Game for Forza Horizon 2 Extra Options

Forza Horizon 2 has no way to quit from its menus. This plugin for the mod
loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions)
adds an **Exit Game** tile to the pause menu's Home tab: the game's Quit icon
and a caption in the game's language, in the square beside Online Road Trip
(which becomes a square tile). Choosing it closes the game the way the game
window's close button does.

Azimuth can already close the game by holding View, so Extra Options does not
include this; the plugin is for those who want it in the menu. Installing it is
the opt-in.

It needs no fork of Extra Options: it is one DLL in a mod folder, loaded by a
stock Extra Options 0.4.4.

## Requirements

- Forza Horizon 2 on Azimuth 1.0.4.
- Forza Horizon 2 Extra Options 0.4.4 (its mod loader).

## Install

1. Close the game.
2. Copy the `mods\Exit Game` folder (from a release ZIP, or `out\mods` after
   building) into the `mods` folder beside `Azimuth.exe` and Extra Options'
   `winmm.dll`:

   ```text
   Azimuth\
     Azimuth.exe
     winmm.dll
     mods\
       Exit Game\
         mod.json
         plugins\
           ExitGame.dll
           ExitGame.ini
   ```

3. Start the game. Extra Options adds the folder to `mods\loadorder.txt` and
   loads the plugin; `FH2_ExtraOptions.log` shows lines starting with
   `EXIT GAME:`.

To turn it off, set `Enabled=0` in `ExitGame.ini`, comment the folder out in
`mods\loadorder.txt` (`# Exit Game`), or delete the folder.

## Settings

`mods\Exit Game\plugins\ExitGame.ini`, read when the game starts (Extra
Options' own `FH2_ExtraOptions.ini` is not used or changed):

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[ExitGame]` | `Enabled` | `1` | add the Exit Game tile; `0`: the plugin changes nothing |

## The tile

- On the Home tab, Online Road Trip (or Horizon Solo, which the game shows in
  its place) becomes a 1x1 tile with its picture cropped to the middle, and
  Exit Game takes the square beside it (grid column 1, row 1). Everything else
  on the page stays as it was, including Extra Options' Cars tab next to Home.
- The caption follows the game's language: EXIT GAME, SAIR DO JOGO, SPIEL
  BEENDEN, QUITTER LE JEU, SALIR DEL JUEGO, ESCI DAL GIOCO, ゲームを終了,
  게임 종료, 結束遊戲, UKONČIT HRU, KILÉPÉS A JÁTÉKBÓL, WYJDŹ Z GRY,
  ВЫЙТИ ИЗ ИГРЫ, SPEL AFSLUITEN (`src\translations.json`).
- Choosing it sends `WM_CLOSE` to Azimuth's game window (class
  `Azimuth.Game.Win32`), whose window procedure answers with `DestroyWindow`,
  as for the title bar's close button. There is no confirmation: progress the
  game has not saved yet is lost, as with that button.

## How it works

Two hooks, both chained onto whatever is there when the plugin starts (Extra
Options 0.4.4 has its own hook on each site; other plugins, such as Weather
Control or Azimuth Graphics Options, may chain them too). Each handler calls
the previous target first.

- **The pause page XAML.** The game reads every XAML resource through one
  call on its UIRenderer path, `call 0xaabec0` at `0x16ebb37`. Extra Options
  points that call at its thunk (`0x13fe354`), whose reader edits the pause
  page (`CarsNextToHome`) and others. The plugin keeps whatever the call goes
  to, points the call at a jump placed within 2 GB (only the call's 4-byte
  displacement is swapped, atomically) and, for `A_PauseMenu` only, adds the
  tile to the XAML that came back, in a new resource buffer, as Extra Options
  does for its own edits (`src\pause_xaml.c`).
- **The pause page init.** The pause page's vtable slot `0x2e14310` holds its
  init (the game's `0xdc6e30`; Extra Options' Return to Garage and the Weather
  Control plugin chain it). The init binds every tile with the game's binder
  `0xd9f9f0(page, "PART_Button_<tile>", &{handler, 0})`, as for Car Packs at
  `0xdc8155`. The plugin swaps the slot (compare-exchange) for its own init,
  which runs the previous one and then binds `PART_Button_EOExitGame` the same
  way, when the page got the tile.

### Guards

Every site is checked byte for byte before use; on any mismatch nothing is
installed, the game is left as it is and the log says why.

| What | Guard |
|---|---|
| host | Extra Options' Azimuth adapter; image size `0x3a18000` |
| pause page | the first 16 bytes of its init `0xdc6e30` and update `0xdce660`; the vtable slots `0x2e14310` / `0x2e14350` hold those functions or executable code outside the game (another module's hook) |
| tile binder | the first 16 bytes of `0xd9f9f0`; the whole Car Packs binding at `0xdc8155` (53 bytes, ending in the call to `0xd9f9f0`) and its name `PART_Button_CarPacks` |
| XAML resource call | `E8` at `0x16ebb37` followed by the game's own code; it calls the game's reader `0xaabec0` (first 12 bytes), Extra Options' thunk (`mov rax, imm64; jmp rax` to code outside the game) or another module's code |
| native functions | the first 12 bytes of allocate, free, object init/cleanup and delete |
| the page | `x:Class="ForzaUI.A_PauseMenu"`; one Home tab canvas; Online Road Trip and Horizon Solo there as self-closing wide image tiles; nothing in its column 1; the page resources before it. Otherwise the page is left as it came |

## Build and test

From the repository's root:

```text
py build.py --zig <zig 0.15.2>\zig.exe --plugin exit-game --test
```

writes `out\mods\Exit Game\...`, `out\release\exit-game-<version>.zip` and a
test build of the DLL with extra entry points
(`out\build\exit-game\tests\ExitGameTest.dll`), then runs
`tests\run_tests.py`. The
tests never run the game and only read its files (`--game`, default
`%USERPROFILE%\FH2`): the game executable (mapped and patched in the test's
own memory) and `UI.zip`, Azimuth's release ZIP (`--azimuth`), the Extra
Options 0.4.4 release (`--release`) and its source (`--upstream`, a git
checkout; its pause page edit is built for the test). Missing inputs skip the
tests that need them.

## License

GPL-3.0-or-later, as Extra Options. See `LICENSE` at the repository's root.
