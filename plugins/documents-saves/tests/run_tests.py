"""Offline tests of the Saves in Documents (mirror) plugin (no game run, nothing installed).

Usage: py tests/run_tests.py [--output out] [--game <FH2 folder>] [--azimuth <azimuth-windows-1.0.4.zip>]

Everything is written below a temporary folder: fake save trees in Azimuth
1.0.4's layout (the "game folder"), the mirror, the plugin's INI. Real files
are only read: azimuth.exe from Azimuth's ZIP (--azimuth, default
<game>/azimuth-windows-1.0.4.zip) and, for the last group, the game folder's
saves (<game>/data/titles/1A96545B/saves, default game ~/FH2), which are copied
into the temporary folder first and never opened in place. A missing input
skips the tests that need it.

1. package     mod.json as Extra Options 0.4.4 validates it, the mod layout,
               the DLL (PE32+ x64, the one export, only Windows imports,
               version resource), the INI.
2. azimuth     azimuth.exe 1.0.4: the save layout's names, commit v2, a
               revision published by renaming "pending" to "commit".
3. mirror      every committed revision copied once, exactly what its commit
               lists, verified; revisions being written, other folders and
               links left alone; the game folder only read; Azimuth dropping
               old revisions never removes their copies; a restart rewrites nothing.
4. conflicts   a revision already in the mirror with other contents (or
               damaged) is kept as it is; the game folder's goes beside it, once.
5. damaged     a source blob that does not match its commit, a commit Azimuth
               would refuse: nothing copied, logged once after 5 tries.
6. interrupted the game closing during a copy (in a blob, after the commit,
               before the rename): the next start finishes a complete copy or
               removes the unfinished one; nothing but .staging is cleaned up.
7. concurrent  Azimuth writing while a copy runs: a blob changing, the revision
               deleted (retention), a new revision appearing, files renamed
               and deleted while the plugin has them open.
8. keep        KeepRevisions=2 keeps the newest two verified copies of each
               container; a copy with files its commit does not list stays.
9. safety      little free space, a link inside the mirror, warnings when
               Documents holds newer saves or the game folder none (nothing
               copied back).
10. plugin     FH2PluginInitialize: the thread copies into Documents\\My Games\\
               Forza Horizon 2\\Saves\\1A96545B or Folder=; Enabled=0, another
               host, an unknown Azimuth, a bad, overlapping or linked Folder
               change nothing.
11. real       a copy of the game folder's saves: every committed revision
               mirrored, every blob the SHA-256 its commit lists; a restore
               from the mirror gives the game the same newest saves.
"""

import argparse
import configparser
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
REPO = ROOT.parents[1]
sys.path.insert(0, str(REPO / "common"))
import saves_testing as st  # noqa: E402

MOD_NAME = "Saves in Documents (mirror)"
DLL_NAME = "DocumentsSaves.dll"
INI_NAME = "DocumentsSaves.ini"
TEST_DLL = "DocumentsSavesTest.dll"
SYSTEM_DLLS = re.compile(r"^(kernel32|user32|bcrypt|shell32|ole32|msvcrt|api-ms-win-[a-z0-9-]+)\.dll$", re.I)
PREFIX = "SAVES IN DOCUMENTS: "

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


def skip(what):
    print("  skip " + what, flush=True)


def valid_mod_id(value):
    if not isinstance(value, str) or not 0 < len(value) <= 128:
        return False
    for i, ch in enumerate(value):
        if not (("a" <= ch <= "z") or ch.isdigit() and ch.isascii() or ch == "-"):
            return False
        if ch == "-" and (i == 0 or i == len(value) - 1 or value[i - 1] == "-"):
            return False
    return True


def pe_info(path):
    d = Path(path).read_bytes()
    e = struct.unpack_from("<I", d, 0x3C)[0]
    machine = struct.unpack_from("<H", d, e + 4)[0]
    characteristics = struct.unpack_from("<H", d, e + 22)[0]
    pe = st.PE(d)
    optional = e + 24
    magic = struct.unpack_from("<H", d, optional)[0]
    exports_rva = struct.unpack_from("<I", d, optional + 112)[0]
    exports = []
    if exports_rva:
        o = pe.offset(exports_rva)
        count, names = struct.unpack_from("<I", d, o + 24)[0], struct.unpack_from("<I", d, o + 32)[0]
        exports = [pe.cstr(struct.unpack_from("<I", d, pe.offset(names) + 4 * i)[0]) for i in range(count)]
    imports, o = [], pe.offset(pe.imports_rva)
    while True:
        name = struct.unpack_from("<I", d, o + 12)[0]
        if not name:
            break
        imports.append(pe.cstr(name))
        o += 20
    resources = struct.unpack_from("<I", d, optional + 112 + 16)[0]
    return machine, magic, characteristics, exports, imports, resources, d


# ---------------------------------------------------------------- 1. package

def test_package(output):
    print("package")
    mod = output / "mods" / MOD_NAME
    manifest = json.loads((mod / "mod.json").read_text(encoding="utf-8"))
    check(manifest.get("format_version") == 1 and valid_mod_id(manifest.get("id")) and manifest.get("id") == "documents-saves",
          f"mod.json: format_version 1 and the id documents-saves ({manifest.get('id')})")
    check(manifest.get("name") == MOD_NAME and manifest.get("version") == "0.0.1",
          "mod.json: name Saves in Documents (mirror), version 0.0.1")
    check(isinstance(manifest.get("dependencies"), list) and "media" not in manifest and "database" not in manifest,
          "mod.json: dependencies list, no media or database (nothing for the media merge)")
    files = sorted(str(p.relative_to(mod)).replace("\\", "/") for p in mod.rglob("*") if p.is_file())
    check(files == ["mod.json", "plugins/" + DLL_NAME, "plugins/" + INI_NAME],
          f"out/mods/{MOD_NAME}: mod.json, plugins/{DLL_NAME}, plugins/{INI_NAME} ({files})")
    machine, magic, characteristics, exports, imports, resources, blob = pe_info(mod / "plugins" / DLL_NAME)
    check(machine == 0x8664 and magic == 0x20B and characteristics & 0x2000, "the DLL is a PE32+ x64 DLL")
    check(exports == ["FH2PluginInitialize"], f"the one export is FH2PluginInitialize ({exports})")
    check(all(SYSTEM_DLLS.match(name) for name in imports), f"only Windows DLLs are imported ({', '.join(imports)})")
    check(bool(resources) and ("FileVersion\0" + manifest["version"]).encode("utf-16le") in blob.replace(b"\0\0\0", b"\0"),
          f"version resource with FileVersion {manifest['version']}")
    text = blob.decode("latin1")
    imported = set(re.findall(r"[A-Za-z0-9_]{4,}", text))
    patching = {"WriteProcessMemory", "SetFileInformationByHandle", "CreateSymbolicLinkW", "DeviceIoControl",
                "CreateHardLinkW", "ReplaceFileW"} & imported
    check(not patching, f"the DLL patches no process memory and makes no links ({sorted(patching) or 'none of those imports'})")
    exports = pe_info(output / "tests" / TEST_DLL)[3]
    check("FH2PluginInitialize" in exports and "DocumentsSavesTestPass" in exports,
          "the test DLL has the plugin entry and the test entry points")
    ini = configparser.ConfigParser()
    ini.read(mod / "plugins" / INI_NAME, encoding="utf-8")
    check(ini.get("DocumentsSaves", "Enabled", fallback=None) == "1" and ini.get("DocumentsSaves", "Folder", fallback=None) == ""
          and ini.get("DocumentsSaves", "KeepRevisions", fallback=None) == "0",
          "DocumentsSaves.ini: Enabled=1, Folder= (Documents\\My Games\\Forza Horizon 2\\Saves\\1A96545B), KeepRevisions=0")


# ---------------------------------------------------------------- the test DLL

LOG = ctypes.CFUNCTYPE(None, ctypes.c_char_p)
FAULT = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int, ctypes.c_wchar_p)


class Plugin:
    def __init__(self, output):
        self.dll = d = ctypes.CDLL(str(output / "tests" / TEST_DLL))
        d.DocumentsSavesTestConfigure.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_uint, LOG]
        d.DocumentsSavesTestHooks.argtypes = [FAULT, ctypes.c_uint, ctypes.c_ulonglong]
        d.DocumentsSavesTestOverride.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
        d.FH2PluginInitialize.argtypes = [ctypes.c_void_p]
        self.lines = []
        self._keep = []
        self.log = LOG(lambda text: self.lines.append(text.decode("utf-8", "replace")))
        self.fault = None
        self.no_fault = FAULT(lambda step, path: 0)

    def text(self):
        return "".join(self.lines)

    def start(self, source, mirror, keep=0, fault=None, chunk=4096, space=0):
        """A new session (as at a game start): configure, hooks, prepare."""
        self.lines.clear()
        ok = self.dll.DocumentsSavesTestConfigure(str(source), str(mirror), keep, self.log)
        self.hooks(fault, chunk, space)
        self.dll.DocumentsSavesTestPrepare()
        return ok

    def hooks(self, fault=None, chunk=4096, space=0):
        self.fault = FAULT(fault) if fault else self.no_fault
        self.dll.DocumentsSavesTestHooks(self.fault, chunk, space)

    def run(self):
        return self.dll.DocumentsSavesTestPass()

    def passes(self, count):
        return sum(self.run() for _ in range(count))


class Context(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("api_version", ctypes.c_uint32), ("adapter", ctypes.c_uint32),
                ("game_image_size", ctypes.c_uint32), ("game_image", ctypes.c_void_p),
                ("plugin_module", ctypes.c_void_p), ("plugins_directory", ctypes.c_wchar_p),
                ("plugin_path", ctypes.c_wchar_p), ("extra_options_version", ctypes.c_char_p),
                ("log_utf8", LOG)]


# ---------------------------------------------------------------- helpers

def committed(root):
    """{space/container/revision: {name: bytes}} of the committed revisions below a saves root,
    with exactly the blobs the commit lists (and the commit)."""
    result = {}
    root = Path(root)
    if not root.exists():
        return result
    for commit in root.glob("*/*/revisions/*/commit"):
        revision = commit.parent
        space, container = revision.parent.parent.parent, revision.parent.parent
        if not re.fullmatch(r"player-.+|machine", space.name, re.I) or any(
                os.path.isjunction(p) or p.is_symlink() for p in (space, container, revision)):
            continue  # what the plugin copies: save spaces, never through links
        lines = commit.read_text(encoding="ascii").splitlines()
        blobs = {}
        for line in lines[2:]:
            digest, size, name = line.split(" ", 2)
            blobs[name] = (revision / "data" / name).read_bytes()
        blobs["commit"] = commit.read_bytes()
        result[revision.relative_to(root).as_posix()] = blobs
    return result


def files_below(root):
    root = Path(root)
    return sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()) if root.exists() else []


def verified(root):
    """Every committed revision below root: each blob's size and SHA-256 as its commit lists, and nothing else in data."""
    for key, blobs in committed(root).items():
        lines = blobs["commit"].decode().splitlines()[2:]
        listed = {line.split(" ", 2)[2]: line.split(" ", 2)[0] for line in lines}
        data = Path(root) / key / "data"
        if sorted(p.name for p in data.iterdir()) != sorted(listed):
            return False
        for name, digest in listed.items():
            if hashlib.sha256(blobs[name]).hexdigest() != digest:
                return False
    return True


# ---------------------------------------------------------------- 3. mirror

def test_mirror(plugin, work):
    print("mirror")
    source, mirror = work / "game" / "saves", work / "Documents" / "Saves" / st.TITLE
    st.make_revision(source, "player-1", "User_90000839FC65C", 0xAE2, st.profile_blobs(100, seed=1))
    st.make_revision(source, "player-1", "User_90000839FC65C", 0xAE3, st.profile_blobs(100, seed=2))
    being_written = st.make_revision(source, "player-1", "User_90000839FC65C", 0xAE4, st.profile_blobs(100, seed=3),
                                     commit=False, pending=True)
    st.make_revision(source, "player-1", "Photo_0000_20261015203217", 1, st.photo_blobs(st.jpeg(30000)))
    st.make_revision(source, "player-1", "BaseLivery_0265_20261020212913", 1, {"livery": os.urandom(5000)})
    st.make_revision(source, "machine", "Settings", 7, {"settings": os.urandom(300)})
    extra = st.make_revision(source, "player-1", "Livery_2256_20261018221715", 2, {"livery": os.urandom(4000)})
    (extra / "data" / "unlisted").write_bytes(b"not in the commit")  # Azimuth passes over such files
    st.make_revision(source, "damaged-saves-20261001", "User_9000000000000", 1, {"x": b"x"})  # not a space
    st.make_revision(source, "player-1", "NoCommit_1", 1, {"x": b"x"}, commit=False)
    (source / "player-1" / "Odd" / "revisions" / "not-hex").mkdir(parents=True)
    outside = work / "outside"
    st.make_revision(outside, "x", "Linked", 1, {"x": b"linked"})
    linked = st.junction(source / "player-1" / "Linked_1", outside / "x" / "Linked")
    before = st.snapshot(source)

    check(plugin.start(source, mirror) == 1, "configured")
    copied = plugin.run()
    check(copied == 6, f"the first pass copies the 6 committed revisions ({copied})")
    want = committed(source)
    got = committed(mirror)
    check(got == want, f"the mirror holds exactly the committed revisions, byte for byte ({sorted(got)})")
    check(verified(mirror), "every copied blob has the size and SHA-256 its commit lists; no other file in data")
    check(not (mirror / "player-1" / "User_90000839FC65C" / "revisions" / "0000000000000ae4").exists(),
          "a revision being written (pending, no commit) is not copied")
    check(not (mirror / "damaged-saves-20261001").exists() and not (mirror / "player-1" / "NoCommit_1").exists(),
          "folders that are not save spaces, and containers without a commit, are not copied")
    if linked:
        check(not (mirror / "player-1" / "Linked_1").exists(), "a linked container is not followed")
    else:
        skip("junctions cannot be made here")
    check(not (mirror.parent / (st.TITLE + ".staging")).exists(), "no staging folder is left")
    check(st.snapshot(source) == before, "the game folder's saves were only read (every file and time as it was)")
    text = plugin.text()
    check(text.count(PREFIX + "copied ") == 6 and "SHA-256 verified" in text, "one log line per copied revision")
    check(plugin.passes(3) == 0, "the next passes copy nothing")
    plugin.dll.DocumentsSavesTestSummary()
    check("first pass: 6 committed revisions in the game folder's saves; 6 copied now, 0 already in the mirror, "
          "0 kept beside it (conflicts), 0 not copied" in plugin.text(), "the summary line")

    # Azimuth commits ae4 (pending -> commit) and drops ae2 (it keeps the newest two).
    st.commit_now(being_written)
    shutil.rmtree(source / "player-1" / "User_90000839FC65C" / "revisions" / "0000000000000ae2")
    check(plugin.run() == 1, "the revision committed meanwhile is copied on the next pass")
    revisions = sorted(p.name for p in (mirror / "player-1" / "User_90000839FC65C" / "revisions").iterdir())
    check(revisions == ["0000000000000ae2", "0000000000000ae3", "0000000000000ae4"],
          f"the mirror keeps the revision Azimuth dropped (nothing is deleted) ({revisions})")
    # A restart: nothing is rewritten.
    stamps = st.snapshot(mirror)
    check(plugin.start(source, mirror) == 1 and plugin.run() == 0, "after a restart nothing is copied again")
    plugin.dll.DocumentsSavesTestSummary()
    check("6 committed revisions in the game folder's saves; 0 copied now, 6 already in the mirror" in plugin.text(),
          "the restart's summary counts them as already in the mirror")
    check(st.snapshot(mirror) == stamps, "no file in the mirror was written again (sizes, times, SHA-256)")
    return source, mirror


# ---------------------------------------------------------------- 4. conflicts

def test_conflicts(plugin, work):
    print("conflicts")
    source, mirror = work / "game" / "saves", work / "Documents" / "Saves" / st.TITLE
    # The mirror already holds revision 5 of a container with other contents (e.g. kept there by
    # an earlier build), and a damaged copy of revision 6 (a blob cut short).
    theirs = st.make_revision(mirror, "player-1", "User_1", 5, {"SaveState": b"documents' own save"})
    st.make_revision(source, "player-1", "User_1", 5, {"SaveState": b"the game folder's save"})
    st.make_revision(source, "player-1", "User_1", 6, {"SaveState": b"x" * 10000})
    st.make_revision(mirror, "player-1", "User_1", 6, {"SaveState": b"x" * 10000})
    (mirror / "player-1" / "User_1" / "revisions" / "0000000000000006" / "data" / "SaveState").write_bytes(b"x" * 10)
    kept = st.snapshot(mirror)
    plugin.start(source, mirror)
    check(plugin.run() == 2, "two revisions go beside the mirror")
    check(st.snapshot(mirror) == kept, "what the mirror held is kept exactly as it was")
    conflicts = mirror.parent / (st.TITLE + ".conflicts")
    got = committed(conflicts)
    names = sorted(got)
    check(len(names) == 2 and all(re.fullmatch(r"player-1/User_1/revisions/000000000000000[56]-[0-9a-f]{12}", n) for n in names),
          f"each goes to <mirror>.conflicts\\...\\revisions\\<revision>-<commit SHA-256> ({names})")
    check(sorted(v["SaveState"] for v in got.values()) == sorted([b"the game folder's save", b"x" * 10000]) and verified(conflicts),
          "the conflict copies are the game folder's revisions, verified")
    check(plugin.text().count("WARNING: a revision is already in the mirror with other contents") == 2, "each is logged")
    check(plugin.start(source, mirror) == 1 and plugin.run() == 0 and len(committed(conflicts)) == 2,
          "after a restart they are not copied again")
    shutil.rmtree(source / "player-1" / "User_1")
    shutil.rmtree(mirror / "player-1" / "User_1")
    shutil.rmtree(conflicts)


# ---------------------------------------------------------------- 5. damaged sources

def test_damaged(plugin, work):
    print("damaged")
    source, mirror = work / "damaged" / "saves", work / "damaged" / "mirror"
    blobs = {"SaveState": os.urandom(9000), "ProfileData": os.urandom(100)}
    st.make_revision(source, "player-1", "User_2", 1, blobs, listed=dict(blobs, SaveState=os.urandom(9000)))
    st.make_revision(source, "player-1", "User_3", 1, {"x": b"1"})
    (source / "player-1" / "User_3" / "revisions" / "0000000000000001" / "commit").write_bytes(
        b"Azimuth complete save revision v2\nblobs 2\n" + b"0" * 64 + b" 1 x\n")
    short = {"SaveState": os.urandom(500)}
    st.make_revision(source, "player-1", "User_4", 1, short)
    (source / "player-1" / "User_4" / "revisions" / "0000000000000001" / "data" / "SaveState").write_bytes(b"cut")
    plugin.start(source, mirror)
    check(plugin.passes(6) == 0, "nothing is copied")
    check(not committed(mirror) and not (mirror.parent / "mirror.staging").exists(), "no copy and no staging left")
    text = plugin.text()
    check("User_2\\revisions\\0000000000000001 not copied: a blob does not match the size and SHA-256 its commit lists (tried 5 times)" in text,
          "a blob that does not match its commit: logged once, after 5 tries")
    check("User_3\\revisions\\0000000000000001 not copied: its commit is not one Azimuth 1.0.4 writes (it lists fewer blobs than its count)" in text,
          "a commit Azimuth would refuse: logged")
    check("User_4\\revisions\\0000000000000001 not copied: a blob's size differs from its commit" in text,
          "a blob cut short: logged")
    check(text.count("ERROR:") == 3, "each logged once")


# ---------------------------------------------------------------- 6. interrupted copies

def test_interrupted(plugin, work):
    print("interrupted")
    source, mirror = work / "cut" / "saves", work / "cut" / "Saves" / st.TITLE
    staging = mirror.parent / (st.TITLE + ".staging")
    st.make_revision(source, "player-1", "User_5", 1, st.profile_blobs(100, seed=5))
    state = {"calls": 0}

    def die_in_blob(step, path):
        state["calls"] += 1
        return 1 if step == 0 and state["calls"] == 3 else 0

    plugin.start(source, mirror, fault=die_in_blob)
    check(plugin.run() == 0, "the game closes in the middle of a blob")
    check(not (mirror / "player-1").exists() and any(staging.glob("m~player-1~User_5~0000000000000001~*")),
          "the mirror has nothing of it; the unfinished copy is in .staging")
    (staging / "not-ours.txt").write_bytes(b"left alone")
    plugin.start(source, mirror)
    check(not any(staging.glob("m~*")) and (staging / "not-ours.txt").read_bytes() == b"left alone",
          "the next start removes the unfinished copy, and only it")
    check("an unfinished copy of player-1\\User_5\\revisions\\0000000000000001 from last time was removed" in plugin.text()
          and "left as it is in the staging folder (not one of this plugin's copies)" in plugin.text(), "both logged")
    check(plugin.run() == 1 and committed(mirror) == committed(source) and verified(mirror), "then it is copied in full")
    (staging / "not-ours.txt").unlink()

    # Closed after the commit was written, before the rename: the copy is complete.
    st.make_revision(source, "player-1", "User_5", 2, st.profile_blobs(100, seed=6))
    plugin.start(source, mirror, fault=lambda step, path: 1 if step == 2 else 0)
    check(plugin.run() == 0 and len(list(staging.glob("m~*"))) == 1, "the game closes just before the rename")
    plugin.start(source, mirror)
    check("was complete (every blob verified): player-1\\User_5\\revisions\\0000000000000002 renamed into place" in plugin.text(),
          "the next start renames the complete copy into place (logged)")
    check(committed(mirror) == committed(source) and not staging.exists(), "the mirror is complete; .staging is gone")
    check(plugin.run() == 0, "nothing is copied again")
    # Closed after the commit, but a damaged staged blob (e.g. the disk): removed, copied again.
    st.make_revision(source, "player-1", "User_5", 3, st.profile_blobs(100, seed=7))
    plugin.start(source, mirror, fault=lambda step, path: 1 if step == 1 else 0)
    plugin.run()
    stage = next(staging.glob("m~*"))
    victim = next((stage / "data").iterdir())
    victim.write_bytes(b"\0" + victim.read_bytes()[1:])
    plugin.start(source, mirror)
    check(not staging.exists() and "an unfinished copy of player-1\\User_5\\revisions\\0000000000000003" in plugin.text(),
          "a staged copy that does not verify is removed, never renamed into place")
    check(plugin.run() == 1 and committed(mirror) == committed(source) and verified(mirror), "then copied again in full")
    # Closed before the rename, and the revision is in the mirror meanwhile: the staged copy is removed.
    st.make_revision(source, "player-1", "User_5", 4, {"x": b"four"})
    plugin.start(source, mirror, fault=lambda step, path: 1 if step == 2 else 0)
    plugin.run()
    st.make_revision(mirror, "player-1", "User_5", 4, {"x": b"four"})
    kept = st.snapshot(mirror)
    plugin.start(source, mirror)
    check(not staging.exists() and st.snapshot(mirror) == kept, "a staged copy whose revision is already there is removed; the mirror untouched")


# ---------------------------------------------------------------- 7. concurrent writes

def test_concurrent(plugin, work):
    print("concurrent")
    source, mirror = work / "busy" / "saves", work / "busy" / "Saves" / st.TITLE
    staging = mirror.parent / (st.TITLE + ".staging")
    folder = st.make_revision(source, "player-1", "User_6", 1, {"SaveState": os.urandom(40000)})
    blob = folder / "data" / "SaveState"
    state = {"done": False}

    def change_blob(step, path):  # a committed blob rewritten in the middle of the copy
        if step == 0 and not state["done"]:
            state["done"] = True
            data = bytearray(blob.read_bytes())
            data[-1] ^= 0xFF
            with open(blob, "r+b") as f:
                f.write(data)
        return 0

    plugin.start(source, mirror, fault=change_blob)
    check(plugin.run() == 0 and not committed(mirror) and not staging.exists(),
          "a blob that changes during the copy: no copy is kept (the staging copy is removed)")
    # Azimuth drops a revision while it is being copied (retention), renaming and deleting open files.
    folder2 = st.make_revision(source, "player-1", "User_7", 1, {"SaveState": os.urandom(40000), "Other": os.urandom(100)})
    outcome = {}

    def drop_revision(step, path):
        if step == 0 and "dropped" not in outcome:
            try:
                for f in (folder2 / "data").iterdir():
                    os.replace(f, f.with_suffix(".old"))
                    os.remove(f.with_suffix(".old"))
                (folder2 / "commit").unlink()
                shutil.rmtree(folder2)
                outcome["dropped"] = True
            except OSError as error:
                outcome["dropped"] = error
        return 0

    plugin.start(source, mirror, fault=drop_revision)
    plugin.run()
    check(outcome.get("dropped") is True, f"Azimuth can rename, delete and remove a revision the plugin is copying ({outcome.get('dropped')})")
    check(not (mirror / "player-1" / "User_7").exists() and not staging.exists(), "nothing of it is kept in the mirror")
    check("User_7" not in plugin.text(), "a revision Azimuth dropped is not an error")
    # A new revision appears (and is committed) while another is being copied.
    folder3 = st.make_revision(source, "player-1", "User_8", 1, {"SaveState": os.urandom(20000)})
    appeared = {}

    def new_revision(step, path):
        if step == 0 and not appeared:
            appeared["folder"] = st.make_revision(source, "player-1", "User_8", 2, {"SaveState": os.urandom(20000)},
                                                  commit=False, pending=True)
        if step == 1 and "folder" in appeared and "committed" not in appeared:
            st.commit_now(appeared["folder"])
            appeared["committed"] = True
        return 0

    plugin.start(source, mirror, fault=new_revision)
    first = plugin.run()
    second = plugin.run()
    got = committed(mirror)
    check(first + second == 2 and "player-1/User_8/revisions/0000000000000001" in got and
          "player-1/User_8/revisions/0000000000000002" in got and verified(mirror),
          f"a revision committed during a copy is copied by the next pass ({first} + {second})")
    del folder3


# ---------------------------------------------------------------- 8. KeepRevisions

def test_keep(plugin, work):
    print("keep")
    source, mirror = work / "keep" / "saves", work / "keep" / "mirror"
    plugin.start(source, mirror, keep=2)
    revisions = mirror / "player-1" / "User_9" / "revisions"
    for n in range(1, 6):
        st.make_revision(source, "player-1", "User_9", n, {"SaveState": os.urandom(1000 + n)})
        if n > 2:
            shutil.rmtree(source / "player-1" / "User_9" / "revisions" / f"{n - 2:016x}")
        plugin.run()
        if n == 2:
            (revisions / f"{1:016x}" / "data" / "mine.txt").write_bytes(b"a file the commit does not list")
    names = sorted(p.name for p in revisions.iterdir())
    check(names == [f"{n:016x}" for n in (1, 4, 5)], f"the newest two are kept, older ones removed ({names})")
    check((revisions / f"{1:016x}" / "data" / "mine.txt").exists() and "holds other files than its commit lists; left as it is" in plugin.text(),
          "a copy holding files its commit does not list is never removed (logged)")
    check(plugin.text().count("KeepRevisions=2: removed") == 2, "each removal is logged")
    stamps = st.snapshot(mirror)
    plugin.start(source, mirror, keep=2)
    check(plugin.run() == 0 and st.snapshot(mirror) == stamps and "removed" not in plugin.text(),
          "after a restart, revisions older than the kept ones are not copied again (no copy-then-remove churn)")
    check(plugin.start(source, mirror, keep=1) == 1, "KeepRevisions=1 is taken as 2")
    st.make_revision(source, "player-1", "User_9", 6, {"SaveState": os.urandom(10)})
    plugin.run()
    names = sorted(p.name for p in revisions.iterdir())
    check(names == [f"{n:016x}" for n in (1, 5, 6)], f"two kept with KeepRevisions=1 ({names})")
    plugin.start(source, mirror, keep=0)
    st.make_revision(source, "player-1", "User_9", 7, {"SaveState": os.urandom(10)})
    plugin.run()
    names = sorted(p.name for p in revisions.iterdir())
    check(names == [f"{n:016x}" for n in (1, 4, 5, 6, 7)],
          f"KeepRevisions=0 removes nothing, and copies every revision the game folder still has ({names})")


# ---------------------------------------------------------------- 9. safety

def test_safety(plugin, work):
    print("safety")
    source, mirror = work / "safe" / "saves", work / "safe" / "mirror"
    st.make_revision(source, "player-1", "User_10", 1, {"SaveState": os.urandom(1000)})
    plugin.start(source, mirror, space=1 << 20)
    check(plugin.passes(5) == 0 and not committed(mirror), "with too little free space nothing is copied")
    check("the disk of the mirror folder has too little free space (tried 5 times)" in plugin.text(), "logged")
    # A link inside the mirror (player-1 pointing elsewhere): refused, nothing written through it.
    elsewhere = work / "safe" / "elsewhere"
    elsewhere.mkdir()
    mirror.mkdir(exist_ok=True)
    if st.junction(mirror / "player-1", elsewhere):
        plugin.start(source, mirror)
        check(plugin.passes(5) == 0 and not any(elsewhere.iterdir()), "a link inside the mirror is never written through")
        check("the mirror's folders cannot be made (or one is a link)" in plugin.text(), "logged")
        os.rmdir(mirror / "player-1")  # the junction itself
    else:
        skip("junctions cannot be made here")
    # Documents holds newer saves than the game folder: warned, both kept, nothing copied back.
    st.make_revision(mirror, "player-1", "User_10", 9, {"SaveState": b"newer, in Documents"})
    before = st.snapshot(source)
    plugin.start(source, mirror)
    plugin.run()
    text = plugin.text()
    check("WARNING: the mirror holds a newer revision of player-1\\User_10 (0000000000000009) than the game folder (0000000000000001)" in text
          and "1 containers are newer in the mirror than in the game folder" in text, "Documents being newer is warned about")
    check(st.snapshot(source) == before and (mirror / "player-1" / "User_10" / "revisions" / f"{9:016x}").exists(),
          "nothing is copied back into the game folder; both are kept")
    # The game folder has no saves at all: said, nothing copied back.
    empty = work / "safe" / "empty-saves"
    empty.mkdir()
    plugin.start(empty, mirror)
    plugin.run()
    check("the game folder has no saves, the mirror has 1 containers" in plugin.text() and not any(empty.iterdir()),
          "an empty game folder is said, and left empty (no automatic restore)")


# ---------------------------------------------------------------- 10. plugin

def initialize(plugin, work, name, ini_text, adapter=1, host=None, saves=None, documents=None, wait=None):
    folder = work / name
    (folder / "plugins").mkdir(parents=True, exist_ok=True)
    dll_path = folder / "plugins" / DLL_NAME
    if ini_text is not None:
        (folder / "plugins" / INI_NAME).write_text(ini_text, encoding="utf-8")
    plugin.lines.clear()
    plugin.hooks()
    host = host if host is not None else st.pe_header()
    saves_text = ctypes.create_unicode_buffer(str(saves)) if saves else None
    documents_text = ctypes.create_unicode_buffer(str(documents)) if documents else None
    plugin.dll.DocumentsSavesTestOverride(ctypes.cast(saves_text, wintypes.LPCWSTR) if saves_text else None,
                                          ctypes.cast(documents_text, wintypes.LPCWSTR) if documents_text else None,
                                          ctypes.cast(host, ctypes.c_void_p), 50, 100)
    folder_text = ctypes.create_unicode_buffer(str(folder / "plugins"))
    path_text = ctypes.create_unicode_buffer(str(dll_path))
    context = Context(ctypes.sizeof(Context), 1, adapter, 0x3A18000, None, None,
                      ctypes.cast(folder_text, ctypes.c_wchar_p), ctypes.cast(path_text, ctypes.c_wchar_p), b"0.4.4", plugin.log)
    plugin._keep.append((context, host, saves_text, documents_text, folder_text, path_text))
    result = plugin.dll.FH2PluginInitialize(ctypes.byref(context))
    if wait:
        deadline = time.time() + 15
        while time.time() < deadline and not wait():
            time.sleep(0.05)
    plugin.dll.DocumentsSavesTestStop()
    return result


def test_plugin(plugin, work):
    print("plugin")
    saves = work / "psaves"
    st.make_revision(saves, "player-1", "User_11", 1, {"SaveState": os.urandom(3000)})
    documents = work / "Documents"
    target = documents / "My Games" / "Forza Horizon 2" / "Saves" / st.TITLE

    def copied(root):
        return lambda: "first pass:" in plugin.text()

    ok = initialize(plugin, work, "default", "[DocumentsSaves]\nEnabled=1\nFolder=\nKeepRevisions=0\n", saves=saves,
                    documents=documents, wait=copied(target))
    check(ok == 1 and committed(target) == committed(saves), "the thread copies into Documents\\My Games\\Forza Horizon 2\\Saves\\1A96545B")
    text = plugin.text()
    check("Azimuth 1.0.4; the saves stay in" in text and "nothing in the mirror is ever deleted" in text and "first pass: 1 committed" in text,
          "the log says the Azimuth build, both folders, the policy and the first pass")
    os.environ["DOCUMENTS_SAVES_TEST_DIR"] = str(work / "Backup")
    initialize(plugin, work, "custom", "[DocumentsSaves]\nFolder=%DOCUMENTS_SAVES_TEST_DIR%\\FH2\n", saves=saves,
               documents=documents, wait=copied(None))
    check(committed(work / "Backup" / "FH2") == committed(saves), "Folder= with an environment variable")
    initialize(plugin, work, "noini", None, saves=saves, documents=work / "Documents2", wait=copied(None))
    check(committed(work / "Documents2" / "My Games" / "Forza Horizon 2" / "Saves" / st.TITLE) == committed(saves),
          "without an INI the defaults apply")

    def nothing(label, ini_text, expect, **kwargs):
        kwargs.setdefault("saves", saves)
        kwargs.setdefault("documents", work / "Nothing")
        result = initialize(plugin, work, label, ini_text, **kwargs)
        time.sleep(0.3)
        check(result == 1 and not (work / "Nothing").exists() and expect in plugin.text(), f"{label}: nothing copied ({expect})")

    nothing("disabled", "[DocumentsSaves]\nEnabled=0\n", "Enabled=0; nothing is copied")
    nothing("mezcal", "", "does not run on Azimuth", adapter=2)
    nothing("unknown-azimuth", "", "is not 1.0.1 to 1.0.4", host=st.pe_header(0x11111111, 0x8A2000))
    nothing("relative-folder", "[DocumentsSaves]\nFolder=Saves\n", "is not a full path")
    nothing("inside-saves", f"[DocumentsSaves]\nFolder={saves}\\player-1\\copy\n", "inside each other")
    nothing("around-saves", f"[DocumentsSaves]\nFolder={work}\n", "inside each other")
    elsewhere = work / "link-target"
    elsewhere.mkdir()
    if st.junction(work / "linked-mirror", elsewhere):
        nothing("linked-folder", f"[DocumentsSaves]\nFolder={work / 'linked-mirror'}\n", "is a link or a file")
        check(not any(elsewhere.iterdir()), "nothing is written through a linked mirror folder")
    else:
        skip("junctions cannot be made here")
    context = Context(8, 1, 1, 0, None, None, None, None, None, plugin.log)
    check(plugin.dll.FH2PluginInitialize(ctypes.byref(context)) == 0, "a context of the wrong size is refused")


# ---------------------------------------------------------------- 11. real

def test_real(plugin, game, work):
    print("real")
    source = st.real_saves(game)
    if not source:
        skip(f"no saves in {game}\\data\\titles\\1A96545B\\saves")
        return
    saves = st.copy_tree(source, work / "real" / "game" / "saves")  # the real folder is only copied from
    before = st.snapshot(saves)
    mirror = work / "real" / "Documents" / "Saves" / st.TITLE
    plugin.start(saves, mirror, chunk=0)
    count = plugin.run()
    want = committed(saves)
    check(count == len(want) and committed(mirror) == want, f"every committed revision of the copy mirrored ({count} of {len(want)})")
    check(verified(mirror), "every mirrored blob has the size and SHA-256 its commit lists")
    after = st.snapshot(saves)
    changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
    check(not changed, f"the copy of the game folder was only read ({changed[:3] or 'every file as it was'})")
    # A restore (what the README and the builder do with the game closed): the mirror copied into an
    # empty saves folder gives every container the same newest committed revision.
    restored = st.copy_tree(mirror, work / "real" / "restored")
    newest = {}
    for key in committed(saves):
        container, revision = key.rsplit("/revisions/", 1)
        newest[container] = max(newest.get(container, revision), revision, key=lambda r: int(r, 16))
    same = all(committed(restored).get(f"{c}/revisions/{r}") == want[f"{c}/revisions/{r}"] for c, r in newest.items())
    check(same, f"a restore from the mirror gives each of the {len(newest)} containers its newest save")


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    parser.add_argument("--game", type=Path, default=Path.home() / "FH2")
    parser.add_argument("--azimuth", type=Path)
    options = parser.parse_args()
    output = options.output.resolve()
    azimuth = options.azimuth or options.game / "azimuth-windows-1.0.4.zip"
    test_package(output)
    print("azimuth")
    st.azimuth_facts(azimuth, check, skip)
    plugin = Plugin(output)
    work = Path(tempfile.mkdtemp(prefix="documents-saves-test-"))
    try:
        test_mirror(plugin, work / "mirror")
        test_conflicts(plugin, work / "mirror")
        test_damaged(plugin, work)
        test_interrupted(plugin, work)
        test_concurrent(plugin, work)
        test_keep(plugin, work)
        test_safety(plugin, work)
        test_plugin(plugin, work / "plugin")
        test_real(plugin, options.game, work)
    finally:
        plugin.hooks()
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        sys.exit(f"{len(failures)} test(s) failed")
    print("all Saves in Documents tests passed")


if __name__ == "__main__":
    main()
