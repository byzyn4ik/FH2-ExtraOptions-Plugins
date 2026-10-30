# Language Support for Forza Horizon 2 Extra Options

Forza Horizon 2 ships 15 text languages. This plugin for the mod loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions)
lets a **language pack** put another language into one of the game's language
slots (the first pack is Ukrainian, in the `ru-RU` slot), and shows
**subtitles for spoken lines** the game shows none for (Ben, Ashley, ANNA),
from the pack's `VoiceSubtitles.str`.

It ports the language support of our Extra Options fork (0.3.2-openbox:
custom languages and voice subtitles) to a stock, unmodified Extra Options
0.4.4. The pack itself needs no plugin code at all: it is an ordinary Extra
Options mod folder, whose files Extra Options mounts.

## Requirements

- Forza Horizon 2 on Azimuth 1.0.4 (1.0.2 also works; Mezcal: see below).
- Forza Horizon 2 Extra Options 0.4.4 (its mod loader). Other versions: the
  plugin installs nothing and says why in the log.
- A language pack (a mod folder with `language.ini` and `media\`).

## Install

1. Close the game.
2. Copy the `mods\Language Support` folder (from a release ZIP, or `out\mods`
   after building) and the language pack's folder into the `mods` folder
   beside `Azimuth.exe` and Extra Options' `winmm.dll`:

   ```text
   Azimuth\
     Azimuth.exe
     winmm.dll
     mods\
       Language Support\
         mod.json
         plugins\
           LangSupport.dll
           LangSupport.ini
       Ukrainian\                      the language pack
         mod.json
         language.ini
         media\
           stripped\stringtables\RU\*.str
           stripped\stringtables\RU\VoiceSubtitles.str
           ui\Fonts\*.vfont
           Cinematics\Subtitles\*_RU.xml
           ...
   ```

3. In the Azimuth launcher choose the pack's slot as the game's language (for
   the Ukrainian pack: Russian, `ru-RU`).
4. Start the game. Extra Options adds both folders to `mods\loadorder.txt`,
   mounts the pack's `media\` and loads the plugin; `FH2_ExtraOptions.log`
   shows lines starting with `LANG SUPPORT:`.

**Coming from the fork:** remove `[Language] Custom=...` from
`FH2_ExtraOptions.ini`. A stock Extra Options refuses an INI with a key it does
not know (the whole INI, not just the line); the plugin warns about it in the
log. Convert the fork's pack (`<game>\Mount\languages\<id>\`) into a mod folder
with `tools\make_language_pack.py convert` (below) and delete the old folder.

To turn the pack off, comment it out in `mods\loadorder.txt` (`# Ukrainian`) or
delete its folder; to turn only the voice subtitles off, set
`VoiceSubtitles=0`.

## Settings

`mods\Language Support\plugins\LangSupport.ini`, read when the game starts
(Extra Options' own `FH2_ExtraOptions.ini` is not used or changed):

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[LangSupport]` | `Pack` | empty | the pack's folder name in `mods\`; empty: the one folder in `mods\` that holds a `language.ini` (with several, name one) |
| `[LangSupport]` | `VoiceSubtitles` | `1` | subtitles for spoken lines while the game runs in the pack's slot; `0`: off (the pack's files are still used) |

The pack's `language.ini`:

```ini
[Language]
Name=Українська
Slot=ru-RU
```

`Slot` is an Azimuth language code the game has a string table archive for:
`en-US` (EN), `en-GB` (GB), `de-DE`, `fr-FR`, `it-IT`, `es-ES`, `es-MX`,
`pt-BR` (br), `pl-PL`, `ru-RU`, `ja-JP` (JP), `ko-KR` (KO), `zh-TW`/`zh-HK`
(CHT), `nl-NL`, `cs-CZ` (cz), `hu-HU`.

## What can be a plugin, and what cannot

Extra Options 0.4.4 loads plugin DLLs on a **worker thread** that the game's
first file lookup starts (`plugins_start()` in its file resolve hook,
`CreateThread`); the lookup does not wait for it. So a plugin may start after
the game has already read its string tables and mounted its archives, and
nothing a plugin does can be relied on to change what the game reads at
startup. Each part of the fork's language support was checked against that
(the release's `winmm.dll` and its source, commit `d66c85d`):

| Part (fork) | As a plugin | How it works now |
|---|---|---|
| The pack's files: string tables, fonts, subtitles, textures (fork: mounted from `Mount\languages\<id>\` at archive slot `0x7ffffffd`, `mediapc` moved to `0x7ffffffc`) | **not needed, and not possible as a plugin**: it must be in place before the first string table read, and a plugin cannot change Extra Options' mount priorities | Extra Options 0.4.4 already has it: a mod folder's `media\` overrides the game's files in the same layout (`media\X.zip` member `a\b` becomes `media\X\a\b`). It mounts every mod's `media\` in its file resolve hook on the game's **first** file lookup, synchronously, before the plugin loader starts; a mod without a database is active from discovery. The pack is such a mod folder |
| Pack priority (fork: below `mediaoverride`, above `mediapc`) | not possible | Extra Options' order: `mediaoverride` > `mediapc` > mods (later in `loadorder.txt` first) > the game's files. A pack's file loses to the same file in `mediaoverride`, `mediapc` or a later mod. The game itself ships no `mediapc` |
| Pack selection: `FH2_ExtraOptions.ini [Language] Custom` | moved | `LangSupport.ini [LangSupport] Pack`, or the one pack in `mods\`. Extra Options 0.4.4 refuses the old key |
| Azimuth language code -> string table archive (`custom_language.c`) | yes | `src\pack.c` |
| The launcher's language (fork: `data\azimuth.ini [launcher] language`) | yes, fixed | Azimuth 1.0.0 and later keep it in `[title-<game>] language` (`[launcher] game` names the title); `system` or none: the Windows locale. The fork's key was empty on 1.0.x |
| Voice subtitles (`voice_subtitles.c`, `voice_subtitles_code.h`) | **yes**: the hooks act only when a line is spoken, long after the plugin loads | `src\voice_subtitles.c`; installed only when the game runs in the pack's slot |
| Extra Options' own menu text in the pack's language (`custom_language_ui`) | **no** | that text is Extra Options' own; it follows the game's language code (Russian in the `ru-RU` slot) |
| `tools\make_language_pack.py` | yes | writes a mod folder (`mod.json`, `language.ini`, `media\`); `convert` turns a fork pack into one |

Extra Options 0.4.4 also has `strings.json` (per-key string additions in a
mod) and a plugin media directory (`FH2PluginMediaDirectory`): neither is used.
`strings.json` replaces single keys of tables, not whole languages, and the
plugin media directory is queried only after the plugin has loaded (late) and
ranks below the mod's own `media\`.

## Voice subtitles

- The game has no subtitles for many spoken lines. A pack can add them in
  `media\stripped\stringtables\<archive>\VoiceSubtitles.str` (a game LSB2
  string table), loose or **stored** (not compressed) in `<archive>.zip`.
  A row's id is the voice event's last path component; an `<id>~t` row gives
  the line's length in seconds.
- They are shown in the game's subtitle box, for the `~t` time + 0.6 s (2 to
  60 s; 3.6 s without a time). The time follows the system clock
  (`KUSER_SHARED_DATA.InterruptTime`), not the frame rate. At most 2048 lines.
- Installed only when the launcher runs the game in the pack's slot (Mezcal:
  when the Windows locale is the slot).

### How it works

The hook code (`src\voice_subtitles_code.h`, generated by
`tools\voice_subtitles\gen_voice_code.py` from `voice_hooks.py` with keystone,
unchanged from the fork) is placed with its table in free memory within 2 GB of
the game image and relocated there: its pointers move with it, its jumps back
and its call of `SetText` with the game image (the fork needed the image at its
preferred base). Five function entries become `jmp` to it (sixth byte of the
parser: `nop`), written all at once with every other thread of the process
suspended and none of them inside the bytes being replaced:

| Site | What | Replaced |
|---|---|---|
| `0xf50ee0` | PlaySpeech parameter constructor | `mov [rsp+20h], r9` (5) |
| `0xf51100` | PreparePlayVO parameter constructor | `mov [rsp+20h], r9` (5) |
| `0xf512a0` | PreparePlay3DVO parameter constructor | `mov [rsp+20h], r9` (5) |
| `0xc3df10` | `SubtitleDataSource::Update` (every frame) | `push rbp; push rbx; push rsi; push rdi` (5) |
| `0x85afa0` | DialogueScript parser (a counter) | `push rbp; push rsi; push rdi; push r12` (6) |

The constructors hash the event name's last path component (FNV-1a 64) and look
it up; `Update` calls `ICinematicSubtitlesDisplay::SetText` (`0xc3a4b0`) with
the line, and with an empty text when its time is up.

No other plugin of this repository and no patch or hook of Extra Options 0.4.4
(its `patches.json`: 156 patches, runtime hooks and data) touches these sites.
They are not call sites another module can chain, so a site that is not as
inspected (another module's jump on it) is reported, not chained.

### Guards

Every site is checked byte for byte before use; on any mismatch nothing is
installed, the game is left as it is and the log says why.

| What | Guard |
|---|---|
| host | Extra Options 0.4.4 (its version string); on Azimuth, azimuth.exe 1.0.4 (TimeDateStamp `0x6ae393eb`) or 1.0.2 (`0x6ae05dd1`) |
| game | image size `0x3a18000` (forza_durango_release_final.exe, SHA-256 `3ec04c61...`, the one Extra Options 0.4.4 patches) |
| hook sites | the first 16 bytes of each of the five functions and of `SetText` |
| pack | `language.ini` with a known `Slot`; not commented out in `loadorder.txt`; `VoiceSubtitles.str` a valid LSB2 table with lines |

## Make a pack

```text
py -I plugins\lang-support\tools\make_language_pack.py build <game>\Mount\media <translation>\media mods\Ukrainian --slot ru-RU --name "Українська" --id uk
py -I plugins\lang-support\tools\make_language_pack.py convert <game>\Mount\languages\uk "mods\Ukrainian"
```

`build` compares a translation mod's replacement files with the game's and
keeps only what changed, in Extra Options' mod layout (`--whole-zips` copies
changed ZIPs whole instead); `zipmanifest.xml` is never copied. `convert` turns
a pack of the fork into a mod folder. Both write `mod.json` (a media-only mod:
`"database": null`), `language.ini` and `media\`.

A Cyrillic language should use the `ru-RU` slot: only there do the game's fonts
fall back to a Cyrillic font (`Fonts.zip\fontsettings.xml` loads `verdana` for
`RU`), and the slot has Russian string tables, subtitles and speech to replace.
The pack ships the fonts its extra letters need (`media\ui\Fonts\*.vfont`); they
replace the game's fonts in every language, so keep the game's glyphs in them.

## Build and test

From the repository's root:

```text
py build.py --zig <zig 0.15.2>\zig.exe --plugin lang-support --test
```

writes `out\mods\Language Support\...`, `out\release\lang-support-<version>.zip`
and a test build of the DLL with extra entry points
(`out\build\lang-support\tests\LangSupportTest.dll`), then runs
`tests\run_tests.py`. The tests never run the game and only read its files
(`--game`, default `%USERPROFILE%\FH2`): the game executable (mapped, relocated
and patched in the test's own memory), the fork's Ukrainian pack's
`VoiceSubtitles.str` if present (982 lines), Azimuth's release ZIP
(`--azimuth`) and `data\azimuth.ini`, the Extra Options 0.4.4 release
(`--release`) and its source (`--upstream`, a git checkout). The hook code runs
in the test with the game's functions behind it stubbed: a spoken line is shown
through `SetText` and cleared when its time is up. Missing inputs skip the
tests that need them. Nothing has been tried in the game yet.

## License

GPL-3.0-or-later, as Extra Options. See `LICENSE` at the repository's root.
