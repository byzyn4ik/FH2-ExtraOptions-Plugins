# Saves in Documents (mirror) for Forza Horizon 2 Extra Options

Azimuth keeps Forza Horizon 2's saves in its own folder,
`data\titles\1A96545B\saves` beside `Azimuth.exe`. This plugin for the mod
loader of
[Forza Horizon 2 Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions)
keeps a **verified copy** of every save in
`Documents\My Games\Forza Horizon 2\Saves\1A96545B`, where backup tools and
cloud folders usually look.

**It is a mirror, not a move.** The game keeps reading and writing its saves in
Azimuth's folder; the copy in Documents follows it a few seconds after each
save. The plugin only reads Azimuth's folder and never writes, links or
deletes anything there. Installing it is the opt-in. It needs no fork of Extra
Options: one DLL in a mod folder, loaded by a stock Extra Options 0.4.4.

## Why a mirror

The earlier Extra Options fork had `[Saves] InDocuments=1`, which moved the
saves themselves to Documents by redirecting Azimuth's file calls from the
moment Extra Options was loaded. A plugin cannot do that safely:

- Extra Options 0.4.4 starts plugins only when the game looks up its first
  file (or hands over its database), on a worker thread, while the game keeps
  running (`features/plugins.c`, `plugins_start`).
- By then Azimuth 1.0.4 has already been at its saves folder: its `wWinMain`
  prepares the "title folder" (`data\titles\1A96545B\saves`: moves saves of
  older versions there, refuses links) before it shows its launcher and before
  the game starts, and the launcher's Saves and Gallery pages read, back up and
  restore the saves in that folder.
- A redirect switched on that late would split one profile in two: Azimuth's
  launcher (backups, restores, the photo gallery) would see the game folder,
  the game would see Documents, and turning the plugin off would silently bring
  back older saves. Azimuth also refuses a saves folder that is a link, so a
  junction is not an option either.

So the saves stay where Azimuth wants them, and Documents gets a copy that is
checked file by file.

## Requirements

- Forza Horizon 2 on Azimuth 1.0.4 (1.0.1 to 1.0.3 keep their saves the same
  way and work too).
- Forza Horizon 2 Extra Options 0.4.4 (its mod loader).
- Disk space in Documents: a profile save is about 6.5 MB, and every save is
  kept (see `KeepRevisions`).

## Install

1. Close the game.
2. Copy the `mods\Saves in Documents (mirror)` folder (from a release ZIP, or
   `out\mods` after building) into the `mods` folder beside `Azimuth.exe` and
   Extra Options' `winmm.dll`:

   ```text
   Azimuth\
     Azimuth.exe
     winmm.dll
     mods\
       Saves in Documents (mirror)\
         mod.json
         plugins\
           DocumentsSaves.dll
           DocumentsSaves.ini
   ```

3. Start the game. `FH2_ExtraOptions.log` shows lines starting with
   `SAVES IN DOCUMENTS:`; about 10 seconds after the game started the first
   pass copies the saves already there, then each new save is copied within
   5 seconds of the game writing it.

To turn it off, set `Enabled=0` in `DocumentsSaves.ini`, comment the folder out
in `mods\loadorder.txt`, or delete the folder. The copy in Documents stays.

## Settings

`mods\Saves in Documents (mirror)\plugins\DocumentsSaves.ini`, read when the game starts:

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[DocumentsSaves]` | `Enabled` | `1` | copy the saves; `0`: the plugin does nothing |
| `[DocumentsSaves]` | `Folder` | (empty) | the copy; empty: `Documents\My Games\Forza Horizon 2\Saves\1A96545B` (the Documents folder wherever Windows has it). A full path (`X:\...` or `\\server\share\...`); environment variables work. Not a link, and not inside the game's saves folder (or around it) |
| `[DocumentsSaves]` | `KeepRevisions` | `0` | `0`: every save ever copied is kept. `2` or more: of each save (the profile, each photo, livery ...) only that many of the newest copies are kept in `Folder`; older copies there are removed (`1` counts as `2`) |

## How it works

Every save of the game is a new revision folder in Azimuth's layout:

```text
data\titles\1A96545B\saves\player-1\<save>\revisions\<16 hex digits>\
  data\<file> ...
  commit
```

Azimuth 1.0.4 creates the revision folder, writes and hashes its files, writes
a `pending` list and renames it to `commit` last; it never writes that revision
again, and it removes revisions older than the newest two. The copy uses the
same layout below `Folder`. A low-priority thread looks every 5 seconds:

- **Only complete saves are read.** A revision is copied once it has its
  `commit`; one without it is still being written and is left alone. Files are
  opened read-only and shared, so Azimuth can write, rename or delete them
  while a copy runs (a revision Azimuth removes meanwhile is simply not copied).
- **Every file is verified.** Each file the commit lists is copied into a
  staging folder (`<Folder>.staging`) while its SHA-256 is computed and
  compared with the commit's, then read back from the disk and compared again.
  The commit is written last and the folder is renamed into place. A file that
  does not match is never kept; the log says why (once, after 5 tries).
- **Nothing is overwritten.** A revision already in the copy with the same
  commit is left alone. One with other contents (for example saves an earlier
  build kept in Documents), or a damaged copy, is kept exactly as it is, and the
  game folder's revision goes beside it, to
  `<Folder>.conflicts\...\revisions\<revision>-<12 hex digits>`.
- **Nothing is deleted** by default: the copy keeps every save, also the ones
  Azimuth has since removed from its own folder. With `KeepRevisions`, only
  older copies in `Folder` are removed, each only when it holds exactly the
  files its commit lists.
- **A copy cut short** (the game closed during one) stays in the staging
  folder. At the next start it is renamed into place when every file verifies,
  and removed otherwise; only the plugin's own staging copies are ever removed.
- Only the save spaces (`player-*`, `machine`) are copied; folders that are
  links are never followed, and a link inside `Folder` is refused.

When the game starts, the log also says when the copy holds newer saves of a
container than the game folder (for example ones an earlier build kept in
Documents) and when the game folder has no saves while the copy has some. The
plugin then changes nothing: the game uses its own folder.

## Restore

The plugin never copies anything back into the game's saves folder while the
game runs. To play from the copy (a new PC, a reinstalled game, or saves an
earlier build kept in Documents):

1. Close the game. In Azimuth's launcher, **Saves → Back up all** first if the
   game folder has saves you may want later.
2. Copy the folders inside `Documents\My Games\Forza Horizon 2\Saves\1A96545B`
   (`player-1`, ...) into `data\titles\1A96545B\saves` beside `Azimuth.exe`,
   replacing what is there. Azimuth uses the newest revision of each save whose
   files verify and removes the older ones on the next save.
3. Start the game.

Forza Horizon 2 Builder does this for you when it builds with this plugin
chosen and the game folder has no saves yet (it copies the copy's newest saves
into the game folder, and never moves them out of Documents).

## Guards

Nothing is copied, and the log says why, when Extra Options' host is not
Azimuth, the Azimuth executable is not 1.0.1 to 1.0.4 (its TimeDateStamp and
SizeOfImage, as Extra Options 0.4.4 tells them apart), `Folder` is not a full
path, is a link or a file, or it and the saves folder are inside each other,
or the disk of `Folder` has less free space than a revision plus 64 MB.

## Build and test

From the repository's root:

```text
py build.py --zig <zig 0.15.2>\zig.exe --plugin documents-saves --test
```

writes `out\mods\Saves in Documents (mirror)\...`,
`out\release\documents-saves-<version>.zip` and a test build of the DLL with
extra entry points (`out\build\documents-saves\tests\DocumentsSavesTest.dll`),
then runs `tests\run_tests.py`. The tests work in a temporary folder on save
trees they make in Azimuth 1.0.4's layout: first copies and restarts (nothing
rewritten), revisions being written, conflicts, damaged sources, the game
closing during a copy (in a file, after the commit, before the rename), Azimuth
writing, renaming and removing files while a copy runs, `KeepRevisions`, links,
low disk space, the warnings, the thread started from `FH2PluginInitialize`,
and that the game folder is only read. With Azimuth's release ZIP
(`--azimuth`) they check in `azimuth.exe` 1.0.4 the layout's names, the commit
format and that a revision is published by renaming `pending` to `commit`.
With a game folder that has saves (`--game`), they copy its saves into the
temporary folder, mirror the copy and restore from the mirror; the game folder
itself is never opened for writing.

## License

GPL-3.0-or-later, as Extra Options. See `LICENSE` at the repository's root.
