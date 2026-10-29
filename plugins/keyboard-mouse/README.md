# Keyboard & Mouse for Forza Horizon 2 Extra Options

Play Forza Horizon 2 with a keyboard and mouse, without a virtual controller.
This plugin for the mod loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions)
answers the game's own input actions from the keyboard, so a connected pad
keeps working and the game's menus, prompts and rewind behave as with a pad:

- **Keys** for driving and menus, after the PC Forza titles (WASD, Space
  handbrake, E/Q gears, Tab camera, R rewind, M map, P photo mode, Esc pause),
  up to three keys per action, all rebindable.
- **Mode=Auto** follows the device used last; `KBM` and `PAD` keep one.
  Gamepad vibration is muted while the keyboard is in use.
- **Mouse look** while driving (optionally up and down too), recentring after
  a moment.
- **A mouse pointer** in the pause menu, its tile hubs, tabs, lists and
  popups, the colour picker, the world map and its filters, photo mode and the
  car viewer: click chooses, right click goes back, the wheel scrolls.
- **Key prompts**: the game's button icons show the bound keys while the
  keyboard is in use.
- **A KEYBOARD & MOUSE page**: a tile next to Controls on the pause menu's
  Settings hub opens a page with the options (input mode, mouse look, mouse
  look up/down, invert mouse Y, mouse sensitivity, keyboard steering speed,
  menu pointer, cursor size, map cursor speed) and one row per action. Every
  change is written to `KeyboardMouse.ini` and applies at once. The texts
  follow the game's language (15 languages).
- **No endless "controller disconnected" pause** without a pad: the pause
  fires only when a pad that was connected is unplugged (also usable on its
  own, see `[Controller]`).

It needs no fork of Extra Options: it is one DLL in a mod folder, loaded by a
stock Extra Options 0.4.4. Installing it is the opt-in.

## Requirements

- Forza Horizon 2 on Azimuth **1.0.4** (`azimuth.exe` of the release, also
  when started by a launcher) or 1.0.2 (the release's, or a branded copy that
  only adds a section). Other Azimuth builds are refused: keyboard & mouse is
  not installed and `FH2_ExtraOptions.log` says which build it found.
- Forza Horizon 2 Extra Options 0.4.4 (its mod loader).

## Install

1. Close the game.
2. Copy the `mods\Keyboard & Mouse` folder (from a release ZIP, or `out\mods`
   after building) into the `mods` folder beside `azimuth.exe` and Extra
   Options' `winmm.dll`:

   ```text
   Azimuth\
     azimuth.exe
     winmm.dll
     FH2_ExtraOptions.ini
     mods\
       Keyboard & Mouse\
         mod.json
         plugins\
           KeyboardMouse.dll
           KeyboardMouse.ini
   ```

3. Start the game. Extra Options adds the folder to `mods\loadorder.txt` and
   loads the plugin; `FH2_ExtraOptions.log` shows lines starting with `KBM`
   (`KBM:`, `KBM MENU:`, `KBM REBIND:`, `KBM PITCH:`) and `CONTROLLER PAUSE:`.

To turn it off, set `Enabled=0` under `[KeyboardMouse]` in
`KeyboardMouse.ini`, comment the folder out in `mods\loadorder.txt`
(`# Keyboard & Mouse`), or delete the folder.

## Keys

| Driving | Default | Menus and HUD | Default |
|---|---|---|---|
| `Throttle` | W | `Map` | M |
| `Brake` | S | `Pause` | (Esc) |
| `SteerLeft` / `SteerRight` | A / D | `Confirm` | Enter |
| `Handbrake` | Space | `Back` | (Esc, right click) |
| `ShiftUp` / `ShiftDown` | E / Q | `Up` `Down` `Left` `Right` | arrow keys |
| `Clutch` | Left Shift | `TabLeft` / `TabRight` | Q / E |
| `Camera` | Tab | `MenuX` / `MenuY` | X / Y |
| `Rewind` | R | `MenuLeftTrigger` / `MenuRightTrigger` | Page Up / Page Down |
| `LookBack` | Down, B | `DPadUp` `DPadDown` `DPadLeft` `DPadRight` | Numpad 8 / 2 / 4 / 6 |
| `Activate` | Enter, F, X | | |
| `PhotoMode` | P | | |
| `RadioPrevious` / `RadioNext` | Left / Right | | |

Esc pauses in the world and goes back everywhere else (`Escape=1`); in a
yes/no popup without a back action it chooses No. Keys are physical: they
stay put with any keyboard layout. One key cannot drive two actions of the
same group (driving, or menus); a driving and a menu action may share one
(Q/E are gears and tabs).

On the KEYBOARD & MOUSE page, choose an action's row (Enter, or Left/Right),
then press the new key: Esc cancels, Backspace or Delete clears it, and a key
another action of the group already uses is refused ("In use (Throttle)").
Reset Key Bindings restores the defaults; the page's own Reset to Default
restores the options. Esc, F2, F8, Alt and the Windows keys cannot be bound.

### With Extra Options' own keys

Extra Options 0.4.4 reads some keys itself, whatever this plugin does (its
`[Bindings]` in `FH2_ExtraOptions.ini`): F1 hides the UI, F2 is Autodrive,
F3 headlights, F4 the roof, and Tab jumps to a manufacturer in the car lists.
None of the defaults above does something there at the same time (Tab is
Camera only while driving). At start, the log warns about any key in
`KeyboardMouse.ini` that one of Extra Options' keyboard bindings also uses
in the same situation, and the page refuses such a key ("In use (Extra
Options)"). Its gamepad bindings (D-pad Down for Autodrive, holding D-pad
Left/Right for signals, ...) read the pad itself, so use its keyboard
bindings for those; driving keys do not cancel Autodrive (press F2 again or
pause).

Weather Control's pause menu shortcut reads the pad's Y; set its own
`ChangeTimeKeyboard` (a virtual-key number, e.g. `117` for F6) to open it
from the keyboard. Exit Game's tile is a normal tile: click it or choose it
with Enter.

## Settings

`mods\Keyboard & Mouse\plugins\KeyboardMouse.ini`, read when the game starts
(the page writes the `[KeyboardMouse]` keys back). Extra Options' own
`FH2_ExtraOptions.ini` is not changed (its `[Bindings]` are only read, for
the clash check). An unknown key, a bad value or one key on two actions of a
group refuses `[KeyboardMouse]` (keyboard & mouse stays off, the log names
the line); a bad `[Controller]` line only turns the pause fix off.

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[KeyboardMouse]` | `Enabled` | `1` | `0`: keyboard & mouse changes nothing |
| | `Mode` | `Auto` | `Auto` follows the device used last; `KBM`; `PAD` (no input hooks at all; restart to change) |
| | `MouseLook` | `1` | the mouse turns the driving look camera |
| | `MouseLookPitch` | `0` | it also tilts it up and down (bumper, hood, cockpit and driver views) |
| | `InvertMouseY` | `0` | swaps up and down for the mouse (look, photo camera, car viewer) |
| | `MouseCapture` | `1` | keeps the pointer in the window while the mouse steers the view |
| | `MouseSensitivity` | `80` | look speed, 1-2000 (thousandths of a degree per count) |
| | `MouseReturnDelay` | `1200` | ms without movement before the view recentres, 0-60000 |
| | `MouseReturnSpeed` | `240` | recentring speed in degrees per second, 10-3600 |
| | `SteeringSpeed` | `100` | keyboard steering speed in percent, 25-400 |
| | `InvertSteering` | `0` | swaps the steering keys |
| | `Escape` | `1` | Esc pauses in the world and goes back elsewhere |
| | `MenuCursor` | `1` | the menu pointer |
| | `CursorSize` | `250` | pointer size in percent (of 32 px at 2160 lines), 50-800 |
| | `MapCursorSpeed` | `400` | world map cursor speed in percent, 50-800 |
| | `MenuArrows` | `Stick` | arrow keys as the left stick (`Stick`) or the D-pad (`DPad`) in menus |
| | `Trace` | `0` | logs every key and mouse event (diagnostics) |
| | `Throttle` ... `DPadRight` | see Keys | up to three key names, comma separated, or `None` |
| `[Controller]` | `PauseOnlyOnUnplug` | `1` | the "controller disconnected" pause fires only when a pad that was connected is unplugged; `0`: the game's rule (keyboard & mouse still skips it while in use with no pad) |

## How it works

The plugin starts on Extra Options' loader worker after the game started. It
reads its INI, identifies the Azimuth build, checks every site byte for byte
and chains onto whatever is there (the game's code, Extra Options' hooks,
other plugins'); on a mismatch that part is left out and the log says why.

- **Keys.** The game polls its pads into `CControllerManager` and asks a
  controller object for an action of a context (`0x1072500` pressed,
  `0x106f040` axis, `0x106e3e0` trigger, `0x106e690` state, `0x106d640`
  consume). While the keyboard is the active source these answer from the
  keys, with the game's own context gate, trigger types, repeat timing and
  consume rules; pad readings, controller state and Azimuth's gamepad objects
  are never written. The input manager update (`0x1075490`) runs the input
  tick; the driving look (`0x103f9d0`) takes the mouse yaw.
- **Pointer and prompts.** `UserInterface::Update` (vtable slot `0x2ecfbf8`)
  feeds AVUI's own mouse input provider (`0x149a2f0`) and the hubs, tabs and
  lists under the pointer; the UI texture loads (`0x1104e70`, `0x1105510`)
  swap the button icons for keycaps drawn with GDI.
- **Menus without a pad.** The title sign-in (`0xea0f66`), the disconnect
  page raises (`0x228bd4`, `0x228c65`), the UI's controller (`0x2ecfc68`), the
  pairing page (`0x2de3898`), button waits (`0x2d2c728`, `0x2d2c668`), popups
  (`0x2ecfd58`, `0x2e246c0`), PauseGame enter (`0x2d16698`, after Extra
  Options' Autodrive hook), the tile hub's navigation vector (`0x142bfaa`)
  and the controller-missing pause (`0x9648fa`).
- **Windows.** Keys and the mouse come through Raw Input on a private
  message-only window on the plugin's own thread, only while the game window
  has the focus and Azimuth shows no prompt of its own. Azimuth's globals
  (1.0.4: window `0x7fcb80`, prompt `0x84e80a`, cursor `0x7fcbb0`; 1.0.2:
  `0x909d20`, `0x95b97a`, `0x909d50`) are each proven by the instruction that
  uses them; its `IGamepad::SetVibration` vtable slot (1.0.4 `0x6eda00`, 1.0.2
  `0x801930`) is chained to mute vibration while the keyboard is in use.
- **The page.** The Settings hub's init (slot `0x2e15940`, chained) binds
  the tile; the XAML resource call `0x16ebb37` (chained after Extra Options'
  thunk and other plugins) adds the tile to `A_PauseOptions` and, right after
  the tile was chosen, rebuilds the Video page as the KEYBOARD & MOUSE page,
  marked `<!-- EO:StandaloneOptionsPage -->` so Extra Options and other
  plugins leave it alone. Its rows bind to properties registered on the
  VideoOptions type; the Video page's reset (slot `0x2e60900`, chained) resets
  only this page's options when it is this page.

### Patching a running game

Slots are swapped with one aligned compare-exchange. Calls are pointed at a
14-byte jump allocated within 2 GB of the game (never the game's int3
padding, which Extra Options uses). Function entries become an absolute jump
with a trampoline for the moved bytes. Code is only written with every other
thread of the process suspended and none of them stopped inside a byte being
replaced, all entry hooks at once.

## Build and test

From the repository's root:

```text
py build.py --zig <zig 0.15.2>\zig.exe --plugin azimuth-graphics-options --plugin keyboard-mouse --test
```

writes `out\mods\Keyboard & Mouse\...`, `outelease\keyboard-mouse-<version>.zip`
and a test build of the DLL with extra entry points
(`outuild\keyboard-mouse	ests\KeyboardMouseTest.dll`), then runs
`testsun_tests.py`. The tests never run the game and only read its files
(`--game`, default `%USERPROFILE%\FH2`): the game executable and Azimuth's
executables (mapped and patched in the test's own memory), `UI.zip`, Azimuth's
release ZIPs (`--azimuth`, `--azimuth102`), the Extra Options 0.4.4 release
(`--release`) and source (`--upstream`; its settings page code is built for
the test), and Azimuth Graphics Options' test DLL (built alongside, or
`--graphics`). Missing inputs skip the tests that need them. They cover the
package, the INI, every Azimuth host site (found again from the code, with
capstone when installed), Extra Options' patch sites and keys, the game guards
and every hook and chain, the hub and page XAML on top of Extra Options' and
Graphics Options' pages, all languages, key rebinding and the plugin entry
behind simulated Extra Options hooks.

The sources (`src`) keep the keyboard & mouse code of Extra Options pull
request #101; `common\includeh2_plugin.h` is Extra Options' plugin API
header, unchanged.

## License

GPL-3.0-or-later, as Extra Options. See `LICENSE` at the repository's root.
