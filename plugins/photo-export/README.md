# Photo Export for Forza Horizon 2 Extra Options

Photos taken in Forza Horizon 2 on Azimuth stay inside the game's saves. This
plugin for the mod loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions)
copies each new photo, once, as a JPG file into a folder you can open:
`Documents\My Games\Forza Horizon 2\Photos` by default. The game already stores
a photo as a JPEG, so the file is the game's own image, byte for byte.

It changes nothing in the game and only reads the saves. Installing it is the
opt-in. It needs no fork of Extra Options: one DLL in a mod folder, loaded by a
stock Extra Options 0.4.4.

## Requirements

- Forza Horizon 2 on Azimuth 1.0.4 (1.0.1 to 1.0.3 keep their saves the same
  way and work too).
- Forza Horizon 2 Extra Options 0.4.4 (its mod loader).

## Install

1. Close the game.
2. Copy the `mods\Photo Export` folder (from a release ZIP, or `out\mods` after
   building) into the `mods` folder beside `Azimuth.exe` and Extra Options'
   `winmm.dll`:

   ```text
   Azimuth\
     Azimuth.exe
     winmm.dll
     mods\
       Photo Export\
         mod.json
         plugins\
           PhotoExport.dll
           PhotoExport.ini
   ```

3. Start the game. `FH2_ExtraOptions.log` shows lines starting with
   `PHOTO EXPORT:`; the first scan, about 5 seconds after the game started,
   exports the photos already taken.

To turn it off, set `Enabled=0` in `PhotoExport.ini`, comment the folder out in
`mods\loadorder.txt` (`# Photo Export`), or delete the folder. Exported JPG
files are never removed.

## Settings

`mods\Photo Export\plugins\PhotoExport.ini`, read when the game starts:

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[PhotoExport]` | `Enabled` | `1` | export photos; `0`: the plugin does nothing |
| `[PhotoExport]` | `Folder` | (empty) | where the JPG files go; empty: `Documents\My Games\Forza Horizon 2\Photos` (the Documents folder wherever Windows has it). A full path (`X:\...` or `\\server\share\...`); environment variables work, e.g. `%USERPROFILE%\Pictures\Forza Horizon 2` |

## What it does

Azimuth keeps the saves in `data\titles\1A96545B\saves` beside `Azimuth.exe`.
Each photo is a save of its own, `player-1\Photo_<number>_<time taken, UTC>`,
and each save of it a revision folder `revisions\<16 hex digits>` with
`data\image` (the JPEG), `data\thumb`, `data\header`, `data\metadata` and a
`commit` file Azimuth writes last, listing every file's size and SHA-256.

A low-priority thread looks at the photo saves every 3 seconds. For each photo:

- only the newest revision that has its `commit` is read (one without it is
  still being written and is looked at again on the next pass);
- the image is exported only when its size and SHA-256 are the ones the
  commit lists and it starts with a JPEG marker; otherwise it is left alone and
  the log says why (once, after 5 tries);
- the file is named by the time the photo was taken, in your time zone:
  `FH2 2026-10-15 22-32-17.jpg`; when that name is taken by a different file,
  the photo's number is added (`FH2 2026-10-15 22-32-17 (0008).jpg`, then
  `(0008-2)` ...). An existing file is **never overwritten**; a file with the
  same bytes counts as already exported;
- the JPG is written beside its final name (`*.jpg.photo-export.tmp`), read back
  and compared, and then renamed into place; a temporary file left by a game
  that closed during an export is deleted at the next start;
- the file keeps the image's time.

What was exported is listed in `PhotoExport.list`, a hidden file in the JPG
folder, so a JPG you delete is not exported again, and a photo you delete in
the game keeps its JPG. With another `Folder`, the photos are exported there
anew. Delete `PhotoExport.list` to export everything again (photos already in
the folder with the same bytes are not duplicated).

The saves are opened read-only and shared, so Azimuth can write, rename or
delete them at any time; the plugin never writes there.

### Guards

Nothing is exported, and the log says why, when Extra Options' host is not
Azimuth, the Azimuth executable is not 1.0.1 to 1.0.4 (its TimeDateStamp and
SizeOfImage, as Extra Options 0.4.4 tells them apart), `Folder` is not a full
path, or the JPG folder and the saves folder are inside each other.

## Build and test

From the repository's root:

```text
py build.py --zig <zig 0.15.2>\zig.exe --plugin photo-export --test
```

writes `out\mods\Photo Export\...`, `out\release\photo-export-<version>.zip`
and a test build of the DLL with extra entry points
(`out\build\photo-export\tests\PhotoExportTest.dll`), then runs
`tests\run_tests.py`. The tests work in a temporary folder on save trees they
make in Azimuth 1.0.4's layout: exports, names, collisions, the list, damaged
and unfinished photos, the thread started from `FH2PluginInitialize`, and that
the saves are only read. With Azimuth's release ZIP (`--azimuth`, default
`%USERPROFILE%\FH2\azimuth-windows-1.0.4.zip`) they check in `azimuth.exe`
1.0.4 the layout's names, the commit format and that a revision is published by
renaming `pending` to `commit`. With a game folder that has saves (`--game`),
they copy its saves into the temporary folder and export every photo of the
copy; the game folder itself is never opened for writing.

## License

GPL-3.0-or-later, as Extra Options. See `LICENSE` at the repository's root.
