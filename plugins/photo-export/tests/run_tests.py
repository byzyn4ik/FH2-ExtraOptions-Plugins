"""Offline tests of the Photo Export plugin (no game run, nothing installed).

Usage: py tests/run_tests.py [--output out] [--game <FH2 folder>] [--azimuth <azimuth-windows-1.0.4.zip>]

Everything is written below a temporary folder: fake save trees in Azimuth
1.0.4's layout, the JPG folder, the plugin's INI. Real files are only read:
azimuth.exe from Azimuth's ZIP (--azimuth, default <game>/azimuth-windows-1.0.4.zip)
and, for the last group, the game folder's saves (<game>/data/titles/1A96545B/saves,
default game ~/FH2), which are copied into the temporary folder first and
never opened in place. A missing input skips the tests that need it.

1. package   mod.json as Extra Options 0.4.4 validates it, the mod layout, the
             DLL (PE32+ x64, the one export, only Windows imports, version
             resource), the INI.
2. azimuth   azimuth.exe 1.0.4: the save layout's names, commit v2, a
             revision published by renaming "pending" to "commit"; the
             plugin recognizes its header and refuses other builds.
3. parts     the commit parser (Azimuth's own refusals), the Folder setting.
4. export    committed JPEGs exported once (newest committed revision,
             local-time names, collision suffix, never overwritten, an
             identical file counts, bad JPEG / SHA-256 refused, a revision
             still being written waits), the list of exported photos (a JPG
             deleted on purpose stays deleted; a damaged list), the saves only
             read, a cut-short export cleaned up.
5. plugin    FH2PluginInitialize: the thread exports into Documents\\My Games\\
             Forza Horizon 2\\Photos or Folder=; Enabled=0, another host, an
             unknown Azimuth, a bad or overlapping Folder change nothing.
6. real      a copy of the game folder's saves: every photo exported, byte for
             byte the image its commit lists.
"""

import argparse
import configparser
import ctypes
from ctypes import wintypes
import datetime
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

MOD_NAME = "Photo Export"
DLL_NAME = "PhotoExport.dll"
INI_NAME = "PhotoExport.ini"
TEST_DLL = "PhotoExportTest.dll"
SYSTEM_DLLS = re.compile(r"^(kernel32|user32|bcrypt|shell32|ole32|msvcrt|api-ms-win-[a-z0-9-]+)\.dll$", re.I)

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


def skip(what):
    print("  skip " + what, flush=True)


def valid_mod_id(value):
    # features/addons.c valid_id() in Extra Options 0.4.4.
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
    check(manifest.get("format_version") == 1 and valid_mod_id(manifest.get("id")) and manifest.get("id") == "photo-export",
          f"mod.json: format_version 1 and the id photo-export ({manifest.get('id')})")
    check(manifest.get("name") == MOD_NAME and manifest.get("version") == "0.0.1", "mod.json: name Photo Export, version 0.0.1")
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
    exports = pe_info(output / "tests" / TEST_DLL)[3]
    check("FH2PluginInitialize" in exports and "PhotoExportTestScan" in exports,
          "the test DLL has the plugin entry and the test entry points")
    ini = configparser.ConfigParser()
    ini.read(mod / "plugins" / INI_NAME, encoding="utf-8")
    check(ini.get("PhotoExport", "Enabled", fallback=None) == "1" and ini.get("PhotoExport", "Folder", fallback=None) == "",
          "PhotoExport.ini: [PhotoExport] Enabled=1, Folder= (Documents\\My Games\\Forza Horizon 2\\Photos)")


# ---------------------------------------------------------------- the test DLL

LOG = ctypes.CFUNCTYPE(None, ctypes.c_char_p)


class Plugin:
    def __init__(self, output):
        self.dll = ctypes.CDLL(str(output / "tests" / TEST_DLL))
        d = self.dll
        d.PhotoExportTestConfigure.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.LPCWSTR, LOG]
        d.PhotoExportTestOverride.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
        d.PhotoExportTestBuild.argtypes = [ctypes.c_void_p]
        d.PhotoExportTestBuild.restype = ctypes.c_char_p
        d.PhotoExportTestCommit.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint)]
        d.PhotoExportTestCommit.restype = ctypes.c_char_p
        d.PhotoExportTestFolder.argtypes = [wintypes.LPCWSTR, ctypes.c_wchar_p]
        d.PhotoExportTestFolder.restype = ctypes.c_size_t
        d.PhotoExportTestDocuments.argtypes = [ctypes.c_wchar_p]
        d.PhotoExportTestDocuments.restype = ctypes.c_size_t
        d.FH2PluginInitialize.argtypes = [ctypes.c_void_p]
        self.lines = []
        self._keep = []
        self.log = LOG(lambda text: self.lines.append(text.decode("utf-8", "replace")))

    def text(self):
        return "".join(self.lines)

    def configure(self, saves, folder, index):
        self.lines.clear()
        return self.dll.PhotoExportTestConfigure(str(saves), str(folder), str(index) if index is not None else "", self.log)

    def scan(self):
        return self.dll.PhotoExportTestScan()


class Context(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("api_version", ctypes.c_uint32), ("adapter", ctypes.c_uint32),
                ("game_image_size", ctypes.c_uint32), ("game_image", ctypes.c_void_p),
                ("plugin_module", ctypes.c_void_p), ("plugins_directory", ctypes.c_wchar_p),
                ("plugin_path", ctypes.c_wchar_p), ("extra_options_version", ctypes.c_char_p),
                ("log_utf8", LOG)]


# ---------------------------------------------------------------- 2. azimuth

def test_azimuth(plugin, azimuth_zip):
    print("azimuth")
    data = st.azimuth_facts(azimuth_zip, check, skip)
    d = plugin.dll
    if data:
        header = ctypes.create_string_buffer(data[:0x1000], 0x1000)
        check(d.PhotoExportTestBuild(header) == b"1.0.4", "the plugin recognizes azimuth.exe 1.0.4 by its header")
    check(d.PhotoExportTestBuild(st.pe_header()) == b"1.0.4", "a header with 1.0.4's stamp and size is 1.0.4")
    check(d.PhotoExportTestBuild(st.pe_header(0x6AE05DD1, 0x9AF000)) == b"1.0.2", "1.0.2's is 1.0.2")
    check(d.PhotoExportTestBuild(st.pe_header(0x6AE393EB, 0x9AF000)) is None, "1.0.4's stamp with another size is refused")
    check(d.PhotoExportTestBuild(st.pe_header(0x12345678, 0x8A2000)) is None, "an unknown stamp is refused")
    check(d.PhotoExportTestBuild(None) is None, "no header is refused")


# ---------------------------------------------------------------- 3. parts

def test_parts(plugin):
    print("parts")
    d = plugin.dll
    count = ctypes.c_uint()

    def parse(text):
        return d.PhotoExportTestCommit(text, len(text), ctypes.byref(count))

    blobs = {"header": b"h" * 106, "image": b"i" * 1000, "thumb": b"t" * 10}
    good = st.commit_text(blobs)
    check(parse(good) is None and count.value == 3, "a v2 commit with 3 blobs parses")
    check(parse(good.replace(b"\n", b"\r\n")) is None, "CR LF line ends are accepted")
    check(parse(good[:-1]) is None, "a missing final line break is accepted")
    refused = {
        "another format": st.commit_text(blobs, version=1),
        "not a commit": b"hello\n",
        "no count": b"Azimuth complete save revision v2\n" + good.split(b"\n", 2)[2],
        "count 0": b"Azimuth complete save revision v2\nblobs 0\n",
        "fewer blobs than counted": good.replace(b"blobs 3", b"blobs 4"),
        "more blobs than counted": good.replace(b"blobs 3", b"blobs 2"),
        "a bad hash": good.replace(good.split(b"\n")[2][:4], b"zzzz"),
        "a bad size": good.replace(b" 1000 image", b" 1x00 image"),
        "a path in a name": good.replace(b" image", b" ..\\image"),
        "a name twice": good.replace(b" thumb", b" image"),
        "an empty line": good.replace(b"\nblobs 3\n", b"\nblobs 3\n\n"),
    }
    for what, text in refused.items():
        why = parse(text)
        check(why is not None, f"a commit with {what} is refused ({why.decode() if why else None})")
    out = ctypes.create_unicode_buffer(1024)
    os.environ["PHOTO_EXPORT_TEST_DIR"] = r"D:\Pictures"
    cases = {r"%PHOTO_EXPORT_TEST_DIR%\FH2": r"D:\Pictures\FH2", r"  C:/Photos/FH2\  ": r"C:\Photos\FH2",
             r"\\server\share\FH2": r"\\server\share\FH2", "C:\\": "C:\\"}
    for text, wanted in cases.items():
        length = d.PhotoExportTestFolder(text, out)
        check(length and out.value == wanted, f"Folder={text!r} is {wanted!r} ({out.value if length else 'refused'})")
    for text in ("Photos", r"..\Photos", r"C:Photos", r"C:\Games\..\Photos", r"\\?\C:\Photos", r"\Photos"):
        check(d.PhotoExportTestFolder(text, out) == 0, f"Folder={text!r} is refused")
    documents = ctypes.create_unicode_buffer(1024)
    length = d.PhotoExportTestDocuments(documents)
    check(length > 3 and re.match(r"^([A-Za-z]:\\|\\\\)", documents.value) is not None,
          f"the Documents known folder resolves to a full path ({documents.value})")


# ---------------------------------------------------------------- 4. export

def local_name(stamp, suffix=""):
    when = datetime.datetime.strptime(stamp, "%Y%m%d%H%M%S").replace(tzinfo=datetime.timezone.utc)
    return "FH2 " + when.astimezone().strftime("%Y-%m-%d %H-%M-%S") + suffix + ".jpg"


def jpgs(folder):
    return {p.name: p.read_bytes() for p in Path(folder).glob("*.jpg")}


def test_export(plugin, work):
    print("export")
    saves, photos = work / "saves", work / "Photos"
    index = photos / "PhotoExport.list"

    first = st.jpeg(200_000)
    st.make_revision(saves, "player-1", "Photo_0000_20261015203217", 1, st.photo_blobs(first), when=1_760_000_000)
    same_second = st.jpeg(150_000)
    st.make_revision(saves, "player-1", "Photo_0008_20261015203217", 1, st.photo_blobs(same_second))
    pending = st.jpeg(90_000)
    pending_folder = st.make_revision(saves, "player-1", "Photo_0001_20261016101500", 1, st.photo_blobs(pending),
                                      commit=False, pending=True)
    old_rev, new_rev = st.jpeg(80_000), st.jpeg(81_000)
    st.make_revision(saves, "player-1", "Photo_0002_20261017120000", 1, st.photo_blobs(old_rev))
    st.make_revision(saves, "player-1", "Photo_0002_20261017120000", 2, st.photo_blobs(new_rev))
    st.make_revision(saves, "player-1", "Photo_0002_20261017120000", 3, st.photo_blobs(st.jpeg(82_000)), commit=False)
    st.make_revision(saves, "player-1", "Photo_0003_20261018000000", 1, st.photo_blobs(b"\x89PNG" + os.urandom(5000)))
    bad = st.photo_blobs(st.jpeg(5000))
    st.make_revision(saves, "player-1", "Photo_0004_20261018000001", 1, bad, listed=dict(bad, image=st.jpeg(5000)))
    second_player = st.jpeg(70_000)
    st.make_revision(saves, "player-2", "Photo_0000_20260101000000", 1, st.photo_blobs(second_player))
    st.make_revision(saves, "player-1", "User_9000000000000", 1, st.profile_blobs(scale=100))
    st.make_revision(saves, "machine", "Photo_0000_20260101000001", 1, st.photo_blobs(st.jpeg(1000)))
    photos.mkdir(parents=True)
    (photos / "FH2 old.jpg.photo-export.tmp").write_bytes(b"cut short")
    (photos / "notes.txt.photo-export.tmp").write_bytes(b"not ours: not a .jpg temporary")
    before = st.snapshot(saves)

    check(plugin.configure(saves, photos, index) == 1, "configured")
    check(not (photos / "FH2 old.jpg.photo-export.tmp").exists() and (photos / "notes.txt.photo-export.tmp").exists(),
          "a cut-short export's temporary file is deleted, nothing else")
    check(plugin.scan() == 4, "the first scan exports 4 photos")
    plugin.dll.PhotoExportTestSummary()
    expected = {local_name("20261015203217"): first,
                local_name("20261015203217", " (0008)"): same_second,
                local_name("20261017120000"): new_rev,
                local_name("20260101000000"): second_player}
    exported = jpgs(photos)
    check(exported == expected, f"each photo once, by local time, the newest committed revision ({sorted(exported)})")
    check(int((photos / local_name("20261015203217")).stat().st_mtime) == 1_760_000_000, "the JPG keeps the image's time")
    check(not any(p.suffix == ".tmp" for p in photos.iterdir() if p.name != "notes.txt.photo-export.tmp"),
          "no temporary file is left")
    check(plugin.scan() == 0, "the next scan exports nothing (remembered)")
    text = plugin.text()
    check(text.count("PHOTO EXPORT: exported ") == 4, "one log line per exported photo")
    check("first scan: 4 photos, 4 exported now, 0 exported before" in text, "the summary line")
    check("ERROR" not in text, "an error is not logged at its first check")
    check("machine" not in text, "the machine space holds no photos of a player")
    lines = index.read_text(encoding="utf-8").splitlines() if index.exists() else []
    check(len(lines) == 4 and all(re.fullmatch(r"player-\d\\Photo_\d{4}_\d{14} [0-9a-f]{16} [0-9a-f]{64}", l) for l in lines),
          f"PhotoExport.list lists the 4 ({len(lines)} lines)")
    check(bool(ctypes.windll.kernel32.GetFileAttributesW(str(index)) & 2), "PhotoExport.list is hidden")

    # The revision being written is committed (pending -> commit): the next pass exports it.
    st.commit_now(pending_folder)
    check(plugin.scan() == 1 and (photos / local_name("20261016101500")).read_bytes() == pending,
          "a photo whose commit appears later is exported then")
    # A newer revision of an exported photo, committed after the folder's time last changed.
    newest = st.jpeg(83_000)
    folder3 = saves / "player-1" / "Photo_0002_20261017120000" / "revisions" / "0000000000000003"
    shutil.rmtree(folder3)
    rev4 = st.make_revision(saves, "player-1", "Photo_0002_20261017120000", 4, st.photo_blobs(newest), commit=False,
                            pending=True)
    check(plugin.scan() == 0, "a newer revision without its commit waits")
    st.commit_now(rev4)
    check(plugin.scan() == 1 and (photos / local_name("20261017120000", " (0002)")).read_bytes() == newest,
          "its commit appearing later (no change of the revisions folder) still exports it, under the next name")
    for _ in range(3):
        plugin.scan()
    check(plugin.text().count("PHOTO EXPORT: exported ") == 6, "nothing is exported twice")
    text = plugin.text()
    check("Photo_0003_20261018000000 not exported: data\\image is not a JPEG" in text,
          "a PNG image is refused (logged once, after 5 checks)")
    check("Photo_0004_20261018000001 not exported: data\\image does not match the SHA-256" in text,
          "an image that does not match its commit is refused (logged once, after 5 checks)")
    check(text.count("PHOTO EXPORT: ERROR:") == 2, "two errors, each logged once")

    # A restart: the list says what was exported; nothing is written again.
    stamps = {p.name: p.stat().st_mtime_ns for p in photos.iterdir()}
    check(plugin.configure(saves, photos, index) == 1 and plugin.scan() == 0, "after a restart nothing is exported again")
    plugin.dll.PhotoExportTestSummary()
    check("first scan: 5 photos, 0 exported now, 5 exported before (6 listed" in plugin.text(), "the restart's summary")
    check({p.name: p.stat().st_mtime_ns for p in photos.iterdir()} == stamps, "no file in the JPG folder changed")
    # A JPG deleted on purpose stays deleted.
    (photos / local_name("20261015203217")).unlink()
    check(plugin.configure(saves, photos, index) == 1 and plugin.scan() == 0 and
          not (photos / local_name("20261015203217")).exists(), "a JPG deleted on purpose is not exported again")
    # Without the list: a JPG already there with the same bytes counts; a different file keeps its name.
    index.unlink()
    (photos / local_name("20260101000000")).write_bytes(b"mine")
    check(plugin.configure(saves, photos, index) == 1, "configured without a list")
    count = plugin.scan()
    check(count == 2, f"without the list, the deleted JPG and the one whose name is taken are exported ({count})")
    check((photos / local_name("20260101000000")).read_bytes() == b"mine", "a different file is never overwritten")
    check((photos / local_name("20260101000000", " (0000)")).read_bytes() == second_player, "it gets the next name")
    check((photos / local_name("20261015203217", " (0008)")).read_bytes() == same_second and
          not (photos / local_name("20261015203217", " (0008-2)")).exists(), "an identical JPG already there counts as exported")
    check(len(index.read_text(encoding="utf-8").splitlines()) == 5, "the list is written again (one line per photo's newest revision)")
    # Every name taken by different files: refused, logged, nothing overwritten.
    third = st.jpeg(1234)
    st.make_revision(saves, "player-1", "Photo_0009_20261019000000", 1, st.photo_blobs(third))
    taken = [local_name("20261019000000")] + [local_name("20261019000000", f" (0009{'' if i == 1 else f'-{i}'})") for i in range(1, 10)]
    for name in taken:
        (photos / name).write_bytes(b"taken")
    for _ in range(5):
        plugin.scan()
    check(all((photos / n).read_bytes() == b"taken" for n in taken) and "every file name for it is taken" in plugin.text(),
          "when all 10 names are taken by other files, nothing is overwritten (logged)")
    # A list that cannot be read (a folder of that name): exports still never duplicate.
    index.unlink()
    index.mkdir()
    check(plugin.configure(saves, photos, index) == 1 and plugin.scan() == 0, "an unreadable list: nothing exported twice")
    check("cannot be read" in plugin.text(), "an unreadable list is logged")
    index.rmdir()
    # The saves were only read; deleting a photo in game keeps its JPG.
    after = st.snapshot(saves)
    untouched = {k: v for k, v in before.items() if "Photo_0001" not in k and "Photo_0002" not in k}
    check(all(after.get(k) == v for k, v in untouched.items()),
          "the saves were only read (every file the test did not change is as it was: size, time, SHA-256)")
    kept = (photos / local_name("20261015203217", " (0008)")).read_bytes()
    shutil.rmtree(saves / "player-1" / "Photo_0008_20261015203217")
    plugin.scan()
    check((photos / local_name("20261015203217", " (0008)")).read_bytes() == kept, "a photo deleted in game keeps its JPG")
    # Reads do not block Azimuth: an image the plugin has open can still be deleted and renamed.
    image = saves / "player-2" / "Photo_0000_20260101000000" / "revisions" / "0000000000000001" / "data" / "image"
    handle = st.open_shared(image)
    try:
        os.replace(image, image.with_name("image2"))
        os.remove(image.with_name("image2"))
        moved = True
    except OSError:
        moved = False
    st.close(handle)
    check(moved, "a save file opened as the plugin opens it can be renamed and deleted (FILE_SHARE_DELETE)")


# ---------------------------------------------------------------- 5. plugin

def initialize(plugin, work, name, ini_text, adapter=1, host=None, saves=None, documents=None, wait=None):
    folder = work / name
    (folder / "plugins").mkdir(parents=True, exist_ok=True)
    dll_path = folder / "plugins" / DLL_NAME
    if ini_text is not None:
        (folder / "plugins" / INI_NAME).write_text(ini_text, encoding="utf-8")
    plugin.lines.clear()
    host = host if host is not None else st.pe_header()
    # The DLL keeps these pointers: buffers that live as long as the test.
    saves_text = ctypes.create_unicode_buffer(str(saves)) if saves else None
    documents_text = ctypes.create_unicode_buffer(str(documents)) if documents else None
    plugin.dll.PhotoExportTestOverride(ctypes.cast(saves_text, wintypes.LPCWSTR) if saves_text else None,
                                       ctypes.cast(documents_text, wintypes.LPCWSTR) if documents_text else None,
                                       ctypes.cast(host, ctypes.c_void_p), 50, 100)
    folder_text = ctypes.create_unicode_buffer(str(folder / "plugins"))
    path_text = ctypes.create_unicode_buffer(str(dll_path))
    context = Context(ctypes.sizeof(Context), 1, adapter, 0x3A18000, None, None,
                      ctypes.cast(folder_text, ctypes.c_wchar_p), ctypes.cast(path_text, ctypes.c_wchar_p), b"0.4.4", plugin.log)
    plugin._keep.append((context, host, saves_text, documents_text, folder_text, path_text))
    result = plugin.dll.FH2PluginInitialize(ctypes.byref(context))
    if wait:
        deadline = time.time() + 10
        while time.time() < deadline and not wait():
            time.sleep(0.05)
    plugin.dll.PhotoExportTestStop()
    return result


def test_plugin(plugin, work):
    print("plugin")
    saves = work / "psaves"
    image = st.jpeg(5000)
    st.make_revision(saves, "player-1", "Photo_0000_20261015203217", 1, st.photo_blobs(image))
    documents = work / "Documents"
    documents.mkdir()
    target = documents / "My Games" / "Forza Horizon 2" / "Photos"
    ok = initialize(plugin, work, "default", "[PhotoExport]\nEnabled=1\nFolder=\n", saves=saves, documents=documents,
                    wait=lambda: (target / local_name("20261015203217")).exists())
    check(ok == 1 and jpgs(target) == {local_name("20261015203217"): image},
          "the thread exports into Documents\\My Games\\Forza Horizon 2\\Photos (created)")
    text = plugin.text()
    check("Azimuth 1.0.4; photos in" in text and "first scan: 1 photos, 1 exported now" in text,
          "the log says the Azimuth build, the folders and the first scan")
    os.environ["PHOTO_EXPORT_TEST_PICTURES"] = str(work / "Pictures")
    custom = work / "Pictures" / "Forza Horizon 2"
    initialize(plugin, work, "custom", "[PhotoExport]\nFolder=%PHOTO_EXPORT_TEST_PICTURES%\\Forza Horizon 2\n",
               saves=saves, documents=documents, wait=lambda: (custom / local_name("20261015203217")).exists())
    check(jpgs(custom) == {local_name("20261015203217"): image}, "Folder= with an environment variable")
    check(initialize(plugin, work, "noini", None, saves=saves, documents=work / "Documents2",
                     wait=lambda: (work / "Documents2" / "My Games" / "Forza Horizon 2" / "Photos" / local_name("20261015203217")).exists()) == 1
          and (work / "Documents2" / "My Games" / "Forza Horizon 2" / "Photos" / local_name("20261015203217")).exists(),
          "without an INI the defaults apply (enabled, Documents)")

    def nothing(label, ini_text, expect, **kwargs):
        kwargs.setdefault("saves", saves)
        kwargs.setdefault("documents", work / "Nothing")
        result = initialize(plugin, work, label, ini_text, **kwargs)
        time.sleep(0.3)
        check(result == 1 and not (work / "Nothing").exists() and expect in plugin.text(),
              f"{label}: nothing exported ({expect})")

    nothing("disabled", "[PhotoExport]\nEnabled=0\n", "Enabled=0; nothing is exported")
    nothing("mezcal", "", "does not run on Azimuth", adapter=2)
    nothing("unknown-azimuth", "", "is not 1.0.1 to 1.0.4", host=st.pe_header(0x11111111, 0x8A2000))
    nothing("relative-folder", "[PhotoExport]\nFolder=Photos\n", "is not a full path")
    nothing("inside-saves", f"[PhotoExport]\nFolder={saves}\\player-1\\jpg\n", "inside each other")
    context = Context(8, 1, 1, 0, None, None, None, None, None, plugin.log)
    check(plugin.dll.FH2PluginInitialize(ctypes.byref(context)) == 0, "a context of the wrong size is refused")


# ---------------------------------------------------------------- 6. real

def test_real(plugin, game, work):
    print("real")
    source = st.real_saves(game)
    if not source:
        skip(f"no saves in {game}\\data\\titles\\1A96545B\\saves")
        return
    saves = st.copy_tree(source, work / "real-saves")  # the real folder is only copied from
    before = st.snapshot(saves)
    photos = work / "real-photos"
    plugin.configure(saves, photos, photos / "PhotoExport.list")
    count = plugin.scan()
    containers = sorted(p for p in saves.glob("player-*/Photo_*") if p.is_dir())
    expected = 0
    for container in containers:
        revisions = sorted((r for r in (container / "revisions").iterdir() if (r / "commit").is_file()), key=lambda r: int(r.name, 16))
        if not revisions:
            continue
        newest = revisions[-1]
        listed = {line.split(" ", 2)[2]: line.split(" ", 2)[0] for line in (newest / "commit").read_text().splitlines()[2:]}
        image = (newest / "data" / "image").read_bytes()
        if hashlib.sha256(image).hexdigest() == listed.get("image") and image[:3] == b"\xff\xd8\xff":
            expected += 1
            check(image in jpgs(photos).values(), f"{container.name}: exported, byte for byte its image")
    check(count == expected and len(jpgs(photos)) == expected, f"every committed photo of the copy exported ({count} of {expected})")
    check(st.snapshot(saves) == before, "the copy was only read (every file as it was)")


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
    plugin = Plugin(output)
    work = Path(tempfile.mkdtemp(prefix="photo-export-test-"))
    try:
        test_azimuth(plugin, azimuth)
        test_parts(plugin)
        test_export(plugin, work / "export")
        test_plugin(plugin, work / "plugin")
        test_real(plugin, options.game, work / "real")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        sys.exit(f"{len(failures)} test(s) failed")
    print("all Photo Export tests passed")


if __name__ == "__main__":
    main()
