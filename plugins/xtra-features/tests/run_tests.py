"""Offline tests of the Xtra Features plugin (no game run, nothing installed).

Usage: py tests/run_tests.py [--output out] [--zig zig.exe] [--game <FH2 folder>]
                             [--azimuth <azimuth-windows-1.0.4.zip>]
                             [--release <Extra Options 0.4.4 release's Azimuth folder>]

Real files are only read (the game's executable and UI.zip, Azimuth's ZIP, the
Extra Options release); the game executable is mapped into this process' own
memory and patched there. Defaults: --game ~/FH2,
--azimuth <game>/azimuth-windows-1.0.4.zip,
--release <game>/src/eo-pr/upstream-0.4.4/FH2_Extra_Options_0.4.4-azimuth/Azimuth.
A missing input skips the tests that need it.

1. package   mod.json as Extra Options 0.4.4 validates it, the mod layout, the
             DLL (PE32+ x64, the one export, only Windows imports, version
             resource), the INI.
2. settings  XtraFeatures.ini: defaults, the shipped file, ranges, bad values
             (that feature off, the others on), comments.
3. release   Extra Options 0.4.4's winmm.dll and its bundled plugins do not
             patch any site this plugin writes (their patch data and code hold
             none of its addresses or original bytes); Azimuth 1.0.4's
             azimuth.exe is not needed by any feature (game image only).
4. game      the game executable passes every feature's guards; each changed
             byte the guards cover is refused (that feature only); sites already
             chained by another module are accepted, sites pointing elsewhere
             in the game are refused.
5. patches   chain_call on the two unaligned call sites (8- and 16-byte
             blocks: only the displacement changes), write_code.
6. map zoom  the zoom adjustment on stand-in map controllers (mainland,
             Storm Island, constrained, out of range, far plane).
7. thumbs    the size block and table for scales 1-4 (disassembled).
8. exit game the real pause page with the tile (Exit Game's own page checks),
             captions from the game's language.
9. plugin    FH2PluginInitialize on the mapped game: every site chained (also
             behind another module's hooks, which keep being called with their
             arguments); switches off, another adapter or image size change
             nothing.
"""

import argparse
import configparser
import ctypes
from ctypes import wintypes
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
REPO = ROOT.parents[1]
MOD_NAME = "Xtra Features"
DLL_NAME = "XtraFeatures.dll"
INI_NAME = "XtraFeatures.ini"
TEST_DLL = "XtraFeaturesTest.dll"
SYSTEM_DLLS = re.compile(r"^(kernel32|user32|msvcrt|api-ms-win-[a-z0-9-]+)\.dll$", re.I)
GAME_EXE = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/forza_durango_release_final.exe")
UI_ZIP = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/media/UI.zip")
PAUSE_PAGE = "Scenes/Anthem/A_PauseMenu.xaml"
EXIT_CAPTIONS = json.loads((ROOT / "src" / "exit_game_captions.json").read_text(encoding="utf-8"))
SCAN_CAPTIONS = json.loads((ROOT / "src" / "photo_scan_captions.json").read_text(encoding="utf-8"))

IMAGE_SIZE = 0x3A18000
TEXT_TABLES = 0x3662990
# The sites this plugin writes: {name: (rva, bytes written at most)}.
RESET_VIEW, RESET_VIEW_CALL, MAP_VTABLE = 0xB803F0, 0xD80442, 0x2D9CD48
RENDER_SIZE, STORED_SIZE = 0xAA6A81, 0x1DFDE70
UI_TICK_SLOT, UI_TICK = 0x2ECFBC0, 0x140BA10
RB_SLOT, RB_DO_CALL = 0x2E21B90, 0xDE8070
RB_TEXT_CALL, EMPTY_UI_STRING = 0xDE3735, 0x140BE20
PAUSE_INIT_SLOT, PAUSE_INIT = 0x2E14310, 0xDC6E30
RESOURCE_CALL, GET_RESOURCE, EO_THUNK = 0x16EBB37, 0xAABEC0, 0x13FE354
WRITTEN = {"map zoom call": (RESET_VIEW_CALL, 5), "render size block": (RENDER_SIZE, 38),
           "stored size table": (STORED_SIZE, 32), "UserInterface slot 8": (UI_TICK_SLOT, 8),
           "RB functor slot": (RB_SLOT, 8), "RB help text call": (RB_TEXT_CALL, 5)}
RENDER_ORIGINAL = bytes.fromhex("be3003000041bdcb0100004c8d7f504183fc017511418d75b541bdd80000004c8dbf80000000")
STORED_ORIGINAL = struct.pack("<8i", 0, 0, 816, 459, 0, 0, 192, 108)

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


def skip(what):
    print("  skip " + what, flush=True)


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.VirtualAlloc.restype = ctypes.c_void_p
kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
kernel32.VirtualQuery.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
kernel32.VirtualQuery.restype = ctypes.c_size_t


# ------------------------------------------------------------------ PE reader

class PE:
    def __init__(self, path=None, data=None):
        self.data = data if data is not None else Path(path).read_bytes()
        d = self.data
        e = struct.unpack_from("<I", d, 0x3C)[0]
        if d[:2] != b"MZ" or d[e:e + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        self.machine, sections, self.timestamp, _, _, optional_size, self.characteristics = \
            struct.unpack_from("<HHIIIHH", d, e + 4)
        optional = e + 24
        self.magic = struct.unpack_from("<H", d, optional)[0]
        self.image_base = struct.unpack_from("<Q", d, optional + 24)[0]
        self.image_size, self.headers_size = struct.unpack_from("<II", d, optional + 56)
        self.sections = []
        table = optional + optional_size
        for i in range(sections):
            name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, table + 40 * i)
            self.sections.append((name.rstrip(b"\0").decode(), va, vsize, rsize, raw))
        directories = optional + 112
        self.exports_rva = struct.unpack_from("<I", d, directories)[0]
        self.imports_rva = struct.unpack_from("<I", d, directories + 8)[0]
        self.resources_rva = struct.unpack_from("<I", d, directories + 16)[0]
        self.relocations_rva, self.relocations_size = struct.unpack_from("<II", d, directories + 40)

    def offset(self, rva):
        for _, va, vsize, rsize, raw in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError(hex(rva))

    def read(self, rva, size):
        o = self.offset(rva)
        return self.data[o:o + size]

    def text(self, rva):
        o = self.offset(rva)
        return self.data[o:self.data.index(b"\0", o)].decode("ascii")

    def exports(self):
        if not self.exports_rva:
            return []
        o = self.offset(self.exports_rva)
        count, names = struct.unpack_from("<I", self.data, o + 24)[0], struct.unpack_from("<I", self.data, o + 32)[0]
        return [self.text(struct.unpack_from("<I", self.data, self.offset(names) + 4 * i)[0]) for i in range(count)]

    def imports(self):
        result, o = {}, self.offset(self.imports_rva) if self.imports_rva else None
        while o is not None:
            lookup, _, _, name, first = struct.unpack_from("<IIIII", self.data, o)
            if not name:
                break
            functions, thunk = [], lookup or first
            for i in range(100000):
                value = struct.unpack_from("<Q", self.data, self.offset(thunk + 8 * i))[0]
                if not value:
                    break
                if not value >> 63:
                    functions.append(self.text(value + 2))
            result[self.text(name)] = functions
            o += 20
        return result

    def relocations(self):
        table = self.read(self.relocations_rva, self.relocations_size)
        found, at = [], 0
        while at + 8 <= len(table):
            page, size = struct.unpack_from("<II", table, at)
            if size < 8:
                break
            for (entry,) in struct.iter_unpack("<H", table[at + 8:at + size]):
                if entry >> 12 == 10:
                    found.append(page + (entry & 0xFFF))
            at += size
        return found

    def map(self):
        """The image laid out and relocated as the loader maps it, in private RWX memory."""
        base = kernel32.VirtualAlloc(None, self.image_size, 0x3000, 0x40)
        if not base:
            raise MemoryError("VirtualAlloc")
        image = bytearray(self.image_size)
        image[:self.headers_size] = self.data[:self.headers_size]
        for _, va, vsize, rsize, raw in self.sections:
            size = min(rsize, vsize) if vsize else rsize
            image[va:va + size] = self.data[raw:raw + size]
        delta = base - self.image_base
        for rva in self.relocations():
            value = int.from_bytes(image[rva:rva + 8], "little")
            image[rva:rva + 8] = ((value + delta) & 0xFFFFFFFFFFFFFFFF).to_bytes(8, "little")
        ctypes.memmove(base, bytes(image), self.image_size)
        return base


def unmap(base):
    kernel32.VirtualFree(base, 0, 0x8000)


def peek(address, size):
    return ctypes.string_at(address, size)


def poke(address, data):
    ctypes.memmove(address, data, len(data))


def q(address):
    return struct.unpack("<Q", peek(address, 8))[0]


def call_target(base, rva):
    code = peek(base + rva, 5)
    return base + rva + 5 + struct.unpack_from("<i", code, 1)[0] if code[0] == 0xE8 else None


def jump_target(address):
    """Where "jmp [rip+0]; dq x" (ff 25 00000000) leads; None for anything else."""
    code = peek(address, 14)
    return struct.unpack_from("<Q", code, 6)[0] if code[:6] == b"\xff\x25\0\0\0\0" else None


def near(base, size=0x1000):
    """Executable memory within 2 GB of the mapped image (another module's stand-in jump)."""
    for k in range(1, 0x8000):
        at = kernel32.VirtualAlloc(base + IMAGE_SIZE + k * 0x10000, size, 0x3000, 0x40)
        if at:
            return at
    raise MemoryError("no memory near the image")


def module_tests(plugin_id):
    path = REPO / "plugins" / plugin_id / "tests" / "run_tests.py"
    spec = importlib.util.spec_from_file_location(f"tests_{plugin_id.replace('-', '_')}", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# --------------------------------------------------------------- 1. package

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


def test_package(output):
    print("package")
    mod = output / "mods" / MOD_NAME
    manifest = json.loads((mod / "mod.json").read_text(encoding="utf-8"))
    check(manifest.get("format_version") == 1 and valid_mod_id(manifest.get("id")) and
          manifest.get("id") == ROOT.name, f"mod.json: format_version 1 and the id {ROOT.name} ({manifest.get('id')})")
    check(isinstance(manifest.get("dependencies"), list) and "media" not in manifest and "database" not in manifest,
          "mod.json: dependencies list, no media or database")
    files = sorted(str(p.relative_to(mod)).replace("\\", "/") for p in mod.rglob("*") if p.is_file())
    check(files == ["mod.json", "plugins/" + DLL_NAME, "plugins/" + INI_NAME],
          f"out/mods/{MOD_NAME}: mod.json, plugins/{DLL_NAME}, plugins/{INI_NAME} ({files})")
    pe = PE(mod / "plugins" / DLL_NAME)
    check(pe.machine == 0x8664 and pe.magic == 0x20B and pe.characteristics & 0x2000, "the DLL is a PE32+ x64 DLL")
    check(pe.exports() == ["FH2PluginInitialize"], f"the one export is FH2PluginInitialize ({pe.exports()})")
    imports = pe.imports()
    check(all(SYSTEM_DLLS.match(name) for name in imports), f"only Windows DLLs are imported ({', '.join(imports)})")
    check(not any("atomic" in f for functions in imports.values() for f in functions),
          "the 16-byte compare-exchange is inline (cmpxchg16b), no atomic library call")
    version = manifest.get("version", "")
    check(bool(pe.resources_rva) and ("FileVersion\0" + version).encode("utf-16le") in pe.data.replace(b"\0\0\0", b"\0"),
          f"version resource with FileVersion {version}")
    test_pe = PE(output / "tests" / TEST_DLL)
    check("FH2PluginInitialize" in test_pe.exports() and "XtraTestCheck" in test_pe.exports(),
          "the test DLL has the plugin entry and the test entry points")
    ini = configparser.ConfigParser()
    ini.read(mod / "plugins" / INI_NAME, encoding="utf-8")
    shipped = {(s, k): ini.get(s, k, fallback=None) for s, k in
               [("XtraFeatures", "MapZoom"), ("XtraFeatures", "GarageThumbnails"), ("XtraFeatures", "PhotoScan"),
                ("XtraFeatures", "ExitGame"), ("MapZoom", "MapStartZoom"), ("MapZoom", "MapMaxZoomOut"),
                ("MapZoom", "MapIslandStartZoom"), ("MapZoom", "MapIslandMaxZoomOut"),
                ("GarageThumbnails", "CarPreviewScale")]}
    check(list(shipped.values()) == ["1", "1", "1", "1", "150", "225", "100", "175", "3"],
          f"XtraFeatures.ini: every feature on, the fork's map zoom values, CarPreviewScale=3 ({list(shipped.values())})")


# ------------------------------------------------------------- 2. settings

class Settings(ctypes.Structure):
    _fields_ = [("map_zoom", ctypes.c_int), ("thumbnails", ctypes.c_int), ("photo_scan", ctypes.c_int),
                ("exit_game", ctypes.c_int), ("start", ctypes.c_int), ("max", ctypes.c_int),
                ("island_start", ctypes.c_int), ("island_max", ctypes.c_int), ("scale", ctypes.c_int),
                ("problem", (ctypes.c_char * 200) * 4)]


def load_test_dll(output):
    dll = ctypes.CDLL(str(output / "tests" / TEST_DLL))
    dll.XtraTestSettings.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(Settings), ctypes.c_uint]
    dll.XtraTestSettingsSize.restype = ctypes.c_uint
    dll.XtraTestCheck.argtypes = [ctypes.c_void_p, ctypes.c_int]
    dll.XtraTestCheck.restype = ctypes.c_char_p
    dll.XtraTestChainCall.argtypes = [ctypes.c_void_p] * 4 + [ctypes.POINTER(ctypes.c_void_p)]
    dll.XtraTestChainCall.restype = ctypes.c_char_p
    dll.XtraTestWriteCode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
    dll.XtraTestWriteCode.restype = ctypes.c_char_p
    dll.XtraTestMapZoomAdjust.argtypes = [ctypes.c_void_p] + [ctypes.c_int] * 4
    dll.XtraTestMapZoomHook.restype = ctypes.c_void_p
    dll.XtraTestThumbnailSizes.argtypes = [ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
    dll.XtraTestPhotoHandlers.argtypes = [ctypes.c_int]
    dll.XtraTestPhotoHandlers.restype = ctypes.c_void_p
    dll.XtraTestScanCaption.argtypes = [ctypes.c_void_p]
    dll.XtraTestScanCaption.restype = ctypes.c_wchar_p
    dll.XtraTestExitGameXaml.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                                         ctypes.c_char_p]
    dll.XtraTestExitGameXaml.restype = ctypes.c_size_t
    dll.XtraTestExitGameCaption.argtypes = [ctypes.c_char_p]
    dll.XtraTestExitGameCaption.restype = ctypes.c_wchar_p
    dll.XtraTestExitGameGameCaption.argtypes = [ctypes.c_void_p]
    dll.XtraTestExitGameGameCaption.restype = ctypes.c_wchar_p
    return dll


def read_settings(dll, path):
    settings = Settings()
    dll.XtraTestSettings(str(path), ctypes.byref(settings), ctypes.sizeof(settings))
    return settings


def test_settings(dll, work, shipped):
    print("settings")
    check(dll.XtraTestSettingsSize() == ctypes.sizeof(Settings), "the settings layout matches the test's")
    s = read_settings(dll, work / "missing.ini")
    defaults = (1, 1, 1, 1, 150, 225, 100, 175, 3)
    values = lambda s: (s.map_zoom, s.thumbnails, s.photo_scan, s.exit_game, s.start, s.max, s.island_start,
                        s.island_max, s.scale)
    problems = lambda s: [bytes(p).split(b"\0")[0].decode() for p in s.problem]
    check(values(s) == defaults and not any(problems(s)), f"no INI: the defaults {defaults}")
    s = read_settings(dll, shipped)
    check(values(s) == defaults and not any(problems(s)), "the shipped INI: the same values")

    def case(text, expect, problem_index=None, what=""):
        path = work / "case.ini"
        path.write_text(text, encoding="utf-8")
        s = read_settings(dll, path)
        got = values(s)
        ok = all(e is None or e == g for e, g in zip(expect, got))
        said = problems(s)
        if problem_index is None:
            ok = ok and not any(said)
        else:
            ok = ok and bool(said[problem_index]) and all(not p for i, p in enumerate(said) if i != problem_index)
        check(ok, f"{what}: {got} {[p for p in said if p]}")

    case("[XtraFeatures]\nMapZoom=0\nGarageThumbnails=0\nPhotoScan=0\nExitGame=0\n", (0, 0, 0, 0), None,
         "every switch 0")
    case("[XtraFeatures]\nPhotoScan=2\n", (1, 1, 0, 1), 2, "PhotoScan=2 turns the scan off, the rest stay on")
    case("[XtraFeatures]\nExitGame=yes\n", (1, 1, 1, 0), 3, "ExitGame=yes turns Exit Game off")
    case("[MapZoom]\nMapStartZoom=25\nMapMaxZoomOut=400\nMapIslandStartZoom=400\nMapIslandMaxZoomOut=100\n",
         (1, 1, 1, 1, 25, 400, 400, 100), None, "the range ends are taken")
    case("[MapZoom]\nMapStartZoom=24\n", (0, 1, 1, 1), 0, "MapStartZoom=24 turns map zoom off")
    case("[MapZoom]\nMapMaxZoomOut=99\n", (0, 1, 1, 1), 0, "MapMaxZoomOut=99 turns map zoom off")
    case("[MapZoom]\nMapIslandMaxZoomOut=401\n", (0, 1, 1, 1), 0, "MapIslandMaxZoomOut=401 turns map zoom off")
    case("[MapZoom]\nMapIslandStartZoom=1.5\n", (0, 1, 1, 1), 0, "a decimal turns map zoom off")
    case("[MapZoom]\nMapStartZoom= 200 ; farther\n", (1, 1, 1, 1, 200), None, "blanks and a trailing comment")
    case("[XtraFeatures]\nMapZoom=0\n[MapZoom]\nMapStartZoom=9999\n", (0,), None,
         "a feature that is off ignores its settings")
    for scale in (1, 2, 3, 4):
        case(f"[GarageThumbnails]\nCarPreviewScale={scale}\n", (1, 1, 1, 1, None, None, None, None, scale), None,
             f"CarPreviewScale={scale}")
    for bad in ("0", "5", "x", ""):
        case(f"[GarageThumbnails]\nCarPreviewScale={bad}\n", (1, 0, 1, 1), 1,
             f"CarPreviewScale={bad!r} turns the thumbnails off")


# ---------------------------------------------------------------- 3. release

def test_release(release, azimuth_zip, game_pe):
    print("release")
    winmm = release / "winmm.dll" if release else None
    if not winmm or not winmm.is_file():
        skip(f"Extra Options 0.4.4 release not found ({release})")
    else:
        data = winmm.read_bytes()
        check(b"0.4.4\0" in data and b"FH2PluginInitialize" in data, "winmm.dll is Extra Options 0.4.4 and loads plugins")
        dlls = [winmm] + sorted((release / "mods").rglob("*.dll"))
        for path in dlls:
            blob = path.read_bytes()
            found = [name for name, (rva, _) in WRITTEN.items() if struct.pack("<I", rva) in blob]
            found += [f"0x{rva:x}" for rva in (RESET_VIEW, UI_TICK, RB_DO_CALL, EMPTY_UI_STRING)
                      if struct.pack("<I", rva) in blob]
            check(not found, f"{path.name} names none of this plugin's sites ({found})")
            if game_pe is not None:
                originals = {name: game_pe.read(rva - 11, 16) for name, (rva, _) in WRITTEN.items()
                             if name.endswith("call")}
                originals["render size block"] = RENDER_ORIGINAL
                hits = [name for name, raw in originals.items() if raw in blob]
                check(not hits, f"{path.name} carries none of their original bytes as patch data ({hits})")
    if azimuth_zip and azimuth_zip.is_file():
        with zipfile.ZipFile(azimuth_zip) as archive:
            exe = PE(data=archive.read("azimuth.exe"))
        check(exe.timestamp == 0x6AE393EB, f"azimuth.exe is 1.0.4 (TimeDateStamp 0x{exe.timestamp:08x}); no "
                                            "feature calls or patches Azimuth code")
    else:
        skip(f"Azimuth ZIP not found ({azimuth_zip})")


# ------------------------------------------------------------------- 4. game

FEATURES = ["map zoom", "thumbnails", "photo scan", "exit game"]


def test_game(dll, game_exe):
    print("game")
    if not game_exe.is_file():
        skip(f"{game_exe} not found")
        return None
    pe = PE(game_exe)
    check(pe.image_size == IMAGE_SIZE, "the game executable is the inspected build (image size 0x3a18000)")
    base = pe.map()
    try:
        for i, name in enumerate(FEATURES):
            problem = dll.XtraTestCheck(base, i)
            check(problem is None, f"stock game: {name} passes its guards ({problem})")

        def refused(feature, rva, data, expect, what):
            old = peek(base + rva, len(data))
            poke(base + rva, data)
            problems = [dll.XtraTestCheck(base, i) for i in range(4)]
            poke(base + rva, old)
            problem = problems[feature]
            others = [FEATURES[i] for i, p in enumerate(problems) if p and i != feature]
            check(problem is not None and expect in problem and not others,
                  f"{FEATURES[feature]}: {what} is refused ({problem.decode() if problem else None}); "
                  f"other features unaffected ({others})")

        flip = lambda rva: bytes([peek(base + rva, 1)[0] ^ 0xFF])
        # Map zoom
        for rva, expect in [(RESET_VIEW + 0x10, b"ResetView at 0xb803f0"),
                            (RESET_VIEW_CALL - 3, b"page setup's ResetView call"),
                            (0xB8DC22 + 4, b"camera distance"), (0xB8D937 + 0x60, b"zoom input"),
                            (0xB8A450 + 6, b"MaxMapDistance setter"), (0xB7BE44 + 9, b"initial zoom"),
                            (0x19440C1 + 4, b"far plane")]:
            refused(0, rva, flip(rva), expect, f"a changed byte at 0x{rva:x}")
        refused(0, RESET_VIEW_CALL, b"\x90", b"ResetView call", "a ResetView site that is no longer a call")
        refused(0, RESET_VIEW_CALL + 1, struct.pack("<i", 0x1000 - (RESET_VIEW_CALL + 5)), b"neither to ResetView",
                "a ResetView call into other game code")
        # Thumbnails
        for rva, expect in [(RENDER_SIZE + 2, b"size block"), (STORED_SIZE + 8, b"size table at"),
                            (0x274121 + 4, b"table's reader"), (0x2D9820 + 3, b"task builder"),
                            (0x126C170 + 1, b"submit"), (0xE1F6D0 + 2, b"capture finish")]:
            refused(1, rva, flip(rva), expect, f"a changed byte at 0x{rva:x}")
        refused(1, 0x2D3BBB8, struct.pack("<Q", base + 0x1000), b"CBackgroundSaveTask", "another background vtable")
        refused(1, UI_TICK_SLOT, struct.pack("<Q", base + 0x1000), b"UserInterface slot",
                "a UserInterface slot pointing elsewhere in the game")
        refused(1, UI_TICK_SLOT, struct.pack("<Q", ctypes.addressof(ctypes.c_int(0))), b"UserInterface slot",
                "a UserInterface slot pointing at data")
        # Photo scan
        for rva, expect in [(RB_DO_CALL + 1, b"RB functor's call"), (0xDB1640 + 15, b"RB handler"),
                            (0x5D8A00 + 4, b"collection credit"), (0xDDF250 + 0x17 + 2, b"opener's check"),
                            (RB_TEXT_CALL + 6, b"after the RB help text call"), (0x5D3A59 + 3, b"car query")]:
            refused(2, rva, flip(rva), expect, f"a changed byte at 0x{rva:x}")
        refused(2, 0x2EB0EA0 + 0x38, struct.pack("<Q", base + 0x1000), b"literal loc string's text",
                "a changed loc string vtable")
        refused(2, RB_SLOT, struct.pack("<Q", base + 0x1000), b"RB functor slot", "an RB slot pointing elsewhere")
        refused(2, RB_TEXT_CALL + 1, struct.pack("<i", 0x1000 - (RB_TEXT_CALL + 5)), b"RB help text call",
                "an RB help text call into other game code")
        # Exit game
        refused(3, PAUSE_INIT, b"\xcc", b"pause page init", "a changed pause page init")
        refused(3, RESOURCE_CALL, b"\x90", b"not a call", "a resource site that is no longer a call")
        # Sites already taken over by another module (code outside the image) are accepted.
        hook = ctypes.cast(dll.XtraTestCheck, ctypes.c_void_p).value
        jump = near(base)
        poke(jump, b"\xff\x25\0\0\0\0" + struct.pack("<Q", hook))
        saved = {rva: peek(base + rva, 8) for rva in (UI_TICK_SLOT, RB_SLOT, RESET_VIEW_CALL, RB_TEXT_CALL)}
        poke(base + UI_TICK_SLOT, struct.pack("<Q", hook))
        poke(base + RB_SLOT, struct.pack("<Q", hook))
        for site in (RESET_VIEW_CALL, RB_TEXT_CALL):
            poke(base + site + 1, struct.pack("<i", jump - (base + site + 5)))
        problems = [dll.XtraTestCheck(base, i) for i in range(3)]
        check(problems == [None, None, None],
              f"sites chained by another module (slots, calls to a jump outside the image) are accepted ({problems})")
        for rva, raw in saved.items():
            poke(base + rva, raw)
        kernel32.VirtualFree(jump, 0, 0x8000)
    finally:
        unmap(base)
    return pe


# ---------------------------------------------------------------- 5. patches

def test_patches(dll, pe):
    print("patches")
    if pe is None:
        skip("needs the game executable")
        return
    base = pe.map()
    try:
        handler = ctypes.cast(dll.XtraTestCheck, ctypes.c_void_p).value
        for site, native, block in ((RESET_VIEW_CALL, RESET_VIEW, 8), (RB_TEXT_CALL, EMPTY_UI_STRING, 16)):
            start = (base + site + 1) & ~(block - 1)
            before = peek(start, 32)
            previous = ctypes.c_void_p()
            problem = dll.XtraTestChainCall(base + site, base + native, base, handler, ctypes.byref(previous))
            after = peek(start, 32)
            changed = [i for i in range(32) if before[i] != after[i]]
            lo, hi = base + site + 1 - start, base + site + 5 - start
            target = call_target(base, site)
            check(problem is None and previous.value == base + native and all(lo <= i < hi for i in changed) and
                  jump_target(target) == handler and abs(target - (base + site)) < 0x7FF00000,
                  f"0x{site:x} (displacement in a {block}-byte block): only the displacement changes, the call "
                  f"goes to a jump to the handler, previous is the game's 0x{native:x} ({problem})")
            again = ctypes.c_void_p()
            problem = dll.XtraTestChainCall(base + site, base + native, base, handler, ctypes.byref(again))
            check(problem is None and again.value == target and jump_target(call_target(base, site)) == handler,
                  f"0x{site:x}: chained a second time (another plugin), the first jump becomes its previous")
        problem = dll.XtraTestChainCall(base + 0xD80437, base + RESET_VIEW, base, handler, ctypes.byref(previous))
        check(problem == b"it is not a call", f"a site that is not a call is refused ({problem})")
        problem = dll.XtraTestChainCall(base + 0xDE06BD, base + 0x1000, base, handler, ctypes.byref(previous))
        check(problem is not None and b"unexpected in the game" in problem,
              f"a call into other game code than expected is refused ({problem})")
        # write_code
        original = peek(base + RENDER_SIZE, 38)
        new = bytes(38)
        problem = dll.XtraTestWriteCode(base + RENDER_SIZE, original, new, 38)
        check(problem is None and peek(base + RENDER_SIZE, 38) == new, f"write_code writes the bytes ({problem})")
        problem = dll.XtraTestWriteCode(base + RENDER_SIZE, original, original, 38)
        check(problem is not None and b"changed" in problem and peek(base + RENDER_SIZE, 38) == new,
              f"write_code refuses bytes that are no longer the expected ones ({problem})")
    finally:
        unmap(base)


# --------------------------------------------------------------- 6. map zoom

def controller(vtable, minimum=900.0, maximum=2500.0, min_zoom=0.0, max_zoom=1.0, initial=1.0, far=10000.0):
    c = ctypes.create_string_buffer(0x400)
    struct.pack_into("<Q", c, 0, vtable)
    struct.pack_into("<f", c, 0xC4, far)
    struct.pack_into("<5f", c, 0x370, minimum, maximum, 0.0, 10.0, 40.0)  # +0x378 unused
    struct.pack_into("<5f", c, 0x384, min_zoom, max_zoom, 3.0, initial, 0.0)
    return c


def fields(c):
    minimum, maximum = struct.unpack_from("<2f", c, 0x370)
    max_speed, = struct.unpack_from("<f", c, 0x380)
    zoom_speed, initial = struct.unpack_from("<2f", c, 0x38C)
    return minimum, maximum, max_speed, zoom_speed, initial


def close(a, b):
    return abs(a - b) < 1e-3 * max(1.0, abs(b))


def test_map_zoom(dll):
    print("map zoom")
    c = controller(0)
    dll.XtraTestMapZoomAdjust(c, 150, 225, 100, 175)
    minimum, maximum, max_speed, zoom_speed, initial = fields(c)
    ratio = (5625 - 900) / (2500 - 900)
    open_at = minimum + (maximum - minimum) * initial
    check(close(maximum, 5625) and close(open_at, 3750) and close(max_speed, 10 + 30 * ratio) and
          close(zoom_speed, 3.0 / ratio),
          f"mainland 150/225: opens at 3750 m, zooms out to 5625 m, pan and zoom speeds scaled "
          f"({open_at:.0f}, {maximum:.0f}, {max_speed:.2f}, {zoom_speed:.3f})")
    dll.XtraTestMapZoomAdjust(c, 150, 225, 100, 175)
    check(fields(c) == (minimum, maximum, max_speed, zoom_speed, initial), "a second ResetView does not scale again")
    c = controller(0, 600.0, 1100.0)
    dll.XtraTestMapZoomAdjust(c, 150, 225, 100, 175)
    minimum, maximum, _, _, initial = fields(c)
    check(close(maximum, 1925) and close(minimum + (maximum - minimum) * initial, 1100),
          f"Storm Island 100/175: opens at 1100 m, zooms out to 1925 m ({maximum:.0f})")
    c = controller(0, min_zoom=0.2)
    before = bytes(c)
    dll.XtraTestMapZoomAdjust(c, 150, 225, 100, 175)
    check(bytes(c) == before, "a zoom range constrained by the game is left alone")
    c = controller(0, far=3000.0)
    dll.XtraTestMapZoomAdjust(c, 400, 400, 100, 100)
    check(close(fields(c)[1], 2700), f"the zoom-out stays at 90% of the far plane ({fields(c)[1]:.0f})")
    c = controller(0, maximum=float("nan"))
    before = bytes(c)
    dll.XtraTestMapZoomAdjust(c, 150, 225, 100, 175)
    check(bytes(c) == before, "values out of the expected range are left alone")
    c = controller(0)
    before = bytes(c)
    dll.XtraTestMapZoomAdjust(c, 100, 100, 150, 225)
    check(bytes(c) == before, "mainland at 100/100: the game's zoom")


# --------------------------------------------------------- 7. thumbnails

def test_thumbnails(dll):
    print("thumbnails")
    try:
        import capstone
    except ImportError:
        capstone = None
    for scale in (1, 2, 3, 4):
        render = ctypes.create_string_buffer(38)
        stored = ctypes.create_string_buffer(32)
        dll.XtraTestThumbnailSizes(scale, render, stored)
        table = struct.unpack("<8i", stored.raw)
        if scale == 1:
            check(render.raw == RENDER_ORIGINAL and stored.raw == STORED_ORIGINAL, "scale 1: the game's own bytes")
            continue
        check(table == (0, 0, 816 * scale, 459 * scale, 0, 0, 192 * scale, 108 * scale),
              f"scale {scale}: the stored sizes {table[2]}x{table[3]} and {table[6]}x{table[7]}")
        if capstone is None:
            skip("capstone is not installed: the size block is not disassembled")
            continue
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        text = [f"{i.mnemonic} {i.op_str}" for i in md.disasm(render.raw, 0x140000000 + RENDER_SIZE)]
        want = [f"mov esi, 0x{816 * scale:x}", f"mov r13d, 0x{459 * scale:x}", "lea r15, [rdi + 0x50]",
                "cmp r12d, 1", f"jne 0x{0x140000000 + RENDER_SIZE + 36:x}", f"mov esi, 0x{192 * scale:x}",
                f"mov r13d, 0x{108 * scale:x}", "add r15, 0x30", "nop ", "nop "]
        check(text == want and sum(1 for _ in md.disasm(render.raw, 0)) == 10,
              f"scale {scale}: the size block disassembles to the intended 38 bytes, ending where it began")
    if capstone is not None:
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        text = [f"{i.mnemonic} {i.op_str}" for i in md.disasm(RENDER_ORIGINAL, 0x140000000 + RENDER_SIZE)]
        check(text[-1] == "lea r15, [rdi + 0x80]" and text[4].startswith("jne 0x"),
              "the game's block: lea r15,[rdi+0x80] (rdi+0x50+0x30) is what add r15,0x30 replaces")


# --------------------------------------------------------- 8. exit game

def language_manager(code):
    manager = ctypes.create_string_buffer(0x60)
    raw = code.encode()
    ctypes.memmove(ctypes.addressof(manager) + 0x30, raw + b"\0", len(raw) + 1)
    struct.pack_into("<QQ", manager, 0x40, len(raw), 15)
    return manager


def test_exit_game(dll, game):
    print("exit game")
    exit_tests = module_tests("exit-game")
    check(EXIT_CAPTIONS == exit_tests.CAPTIONS, "the captions are the Exit Game plugin's (16 codes)")
    cell = ctypes.c_void_p()
    fake_game = ctypes.addressof(cell) - TEXT_TABLES
    for code, caption in EXIT_CAPTIONS.items():
        cell.value = ctypes.addressof(manager := language_manager(code))
        check(dll.XtraTestExitGameGameCaption(fake_game) == caption and
              dll.XtraTestExitGameCaption(code.lower().encode()) == caption, f"{code}: Exit Game caption {caption}")
        scan = SCAN_CAPTIONS.get(code, SCAN_CAPTIONS["EN"])
        check(dll.XtraTestScanCaption(fake_game) == scan, f"{code}: photo scan help text {scan}")
        del manager
    cell.value = None
    check(dll.XtraTestExitGameGameCaption(fake_game) == EXIT_CAPTIONS["EN"] and
          dll.XtraTestScanCaption(fake_game) == "Scan cars", "unknown language: English")
    ui_zip = game / UI_ZIP
    if not ui_zip.is_file():
        skip(f"{ui_zip} not found")
        return
    with zipfile.ZipFile(ui_zip) as archive:
        native = archive.read(PAUSE_PAGE)
    out = ctypes.create_string_buffer(len(native) + 0x4000)
    n = dll.XtraTestExitGameXaml(native, len(native), out, len(out), b"EN")
    page = out.raw[:n]
    exit_tests.check_page(page, "the real pause page", native)
    n2 = dll.XtraTestExitGameXaml(page, len(page), out, len(out), b"EN")
    check(n2 == 0, "a page that already has the tile (the Exit Game plugin's too) is left as it is: one tile")
    failures.extend(exit_tests.failures)


# ------------------------------------------------------------------ 9. plugin

LOG = ctypes.CFUNCTYPE(None, ctypes.c_char_p)
FOUR = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64)
RB = ctypes.CFUNCTYPE(ctypes.c_uint8, ctypes.c_void_p, ctypes.c_void_p)
RESET = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)


class Context(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("api_version", ctypes.c_uint32), ("adapter", ctypes.c_uint32),
                ("game_image_size", ctypes.c_uint32), ("game_image", ctypes.c_void_p),
                ("plugin_module", ctypes.c_void_p), ("plugins_directory", ctypes.c_wchar_p),
                ("plugin_path", ctypes.c_wchar_p), ("extra_options_version", ctypes.c_char_p), ("log_utf8", LOG)]


class Plugin:
    """FH2PluginInitialize of a fresh copy of the test DLL, as Extra Options 0.4.4 calls it."""

    def __init__(self, output, work, ini_text):
        folder = work / "mods" / MOD_NAME / "plugins"
        folder.mkdir(parents=True, exist_ok=True)
        shutil.copy2(output / "tests" / TEST_DLL, folder / DLL_NAME)
        (folder / INI_NAME).write_text(ini_text, encoding="utf-8")
        self.dll = ctypes.CDLL(str(folder / DLL_NAME))
        self.dll.FH2PluginInitialize.argtypes = [ctypes.POINTER(Context)]
        self.dll.FH2PluginInitialize.restype = wintypes.BOOL
        self.dll.XtraTestMapZoomHook.restype = ctypes.c_void_p
        self.dll.XtraTestPhotoHandlers.argtypes = [ctypes.c_int]
        self.dll.XtraTestPhotoHandlers.restype = ctypes.c_void_p
        self.lines = []
        self.log = LOG(lambda text: self.lines.append(text.decode("utf-8", "replace")))
        self.folder, self.path = str(folder), str(folder / DLL_NAME)
        self.base = ctypes.c_void_p(self.dll._handle).value
        self.size = PE(folder / DLL_NAME).image_size

    def initialize(self, game, adapter=1, size=IMAGE_SIZE):
        self.context = Context(ctypes.sizeof(Context), 1, adapter, size, game, None, self.folder, self.path,
                               b"0.4.4", self.log)
        return self.dll.FH2PluginInitialize(ctypes.byref(self.context))

    def owns(self, address):
        return address is not None and self.base <= address < self.base + self.size

    def said(self, text):
        return any(text in line for line in self.lines)


SITES = [(RESET_VIEW_CALL, 16), (RENDER_SIZE, 38), (STORED_SIZE, 32), (UI_TICK_SLOT, 8), (RB_SLOT, 8),
         (RB_TEXT_CALL, 16), (PAUSE_INIT_SLOT, 8), (RESOURCE_CALL, 8)]


def snapshot(base):
    return {rva: peek(base + rva - 8, size + 16) for rva, size in SITES}


def test_plugin(output, pe):
    print("plugin")
    if pe is None:
        skip("the plugin tests need the game executable")
        return
    work = Path(tempfile.mkdtemp(prefix="xtra-features-", dir=output / "test-runs"))
    ini = (ROOT / "mod" / INI_NAME).read_text(encoding="utf-8")

    # --- A: the stock game, every feature on.
    base = pe.map()
    plugin = Plugin(output, work / "stock", ini)
    ok = plugin.initialize(base)
    log = "".join(plugin.lines)
    check(ok and all(plugin.said(t) for t in ("MapZoom: ResetView call 0xd80442 chained onto the game's ResetView",
                                              "GarageThumbnails: new garage thumbnails at 3x",
                                              "GarageThumbnails: UserInterface slot 0x2ecfbc0 chained onto the game's",
                                              "PhotoScan: RB functor slot 0x2e21b90 chained onto the game's",
                                              "PhotoScan: RB help text call 0xde3735 chained",
                                              "ExitGame: pause page init chained onto the game's pause page init")),
          "stock game: all four features installed, each says where it chained")
    check(all(line.startswith("XTRA FEATURES: ") and line.endswith("\r\n") for line in plugin.lines),
          "every log line is 'XTRA FEATURES: ...' with a line break")
    check(jump_target(call_target(base, RESET_VIEW_CALL)) == plugin.dll.XtraTestMapZoomHook(),
          "the ResetView call goes (through a jump) to the map zoom hook")
    render = ctypes.create_string_buffer(38)
    stored = ctypes.create_string_buffer(32)
    plugin.dll.XtraTestThumbnailSizes(3, render, stored)
    check(peek(base + RENDER_SIZE, 38) == render.raw and peek(base + STORED_SIZE, 32) == stored.raw,
          "the size block and the size table hold scale 3 (the shipped INI)")
    stub = q(base + UI_TICK_SLOT)
    check(not (base <= stub < base + IMAGE_SIZE) and q(stub + 0x56) == base + UI_TICK and peek(stub + 0x50, 6) ==
          b"\xff\x25\0\0\0\0", "UserInterface slot 8 holds a stub that ends in a jump to the game's 0x140ba10")
    check(q(base + RB_SLOT) == plugin.dll.XtraTestPhotoHandlers(0), "the RB functor slot holds the scan handler")
    check(jump_target(call_target(base, RB_TEXT_CALL)) == plugin.dll.XtraTestPhotoHandlers(1),
          "the RB help text call goes (through a jump) to the help text handler")
    check(plugin.owns(q(base + PAUSE_INIT_SLOT)) and plugin.owns(jump_target(call_target(base, RESOURCE_CALL))),
          "the pause page init slot and the XAML resource call lead into the plugin (Exit Game)")
    unmap(base)

    # --- B: behind another module on every site (Extra Options' resource thunk too).
    base = pe.map()
    calls = []
    other_tick = FOUR(lambda a, b, c, d: calls.append(("tick", a, b, c, d)) or 0x1234)
    other_rb = RB(lambda functor, argument: calls.append(("rb", functor, argument)) or 7)
    other_reset = RESET(lambda c, centre: calls.append(("reset", c, centre)))
    addresses = {name: ctypes.cast(f, ctypes.c_void_p).value for name, f in
                 (("tick", other_tick), ("rb", other_rb), ("reset", other_reset))}
    jumps = near(base)
    poke(jumps, b"\xff\x25\0\0\0\0" + struct.pack("<Q", addresses["reset"]))
    poke(base + RESET_VIEW_CALL + 1, struct.pack("<i", jumps - (base + RESET_VIEW_CALL + 5)))
    poke(base + UI_TICK_SLOT, struct.pack("<Q", addresses["tick"]))
    poke(base + RB_SLOT, struct.pack("<Q", addresses["rb"]))
    eo_reader = ctypes.cast(plugin.dll.XtraTestCheck, ctypes.c_void_p).value
    poke(base + EO_THUNK, b"\x48\xb8" + struct.pack("<Q", eo_reader) + b"\xff\xe0")
    poke(base + RESOURCE_CALL + 1, struct.pack("<i", EO_THUNK - (RESOURCE_CALL + 5)))
    chained = Plugin(output, work / "chained", ini)
    ok = chained.initialize(base)
    check(ok and chained.said("MapZoom: ResetView call 0xd80442 chained onto ") and not chained.said("onto the game's ResetView") and
          chained.said("UserInterface slot 0x2ecfbc0 chained onto ") and not chained.said("onto the game's 0x140ba10"),
          "behind another module: map zoom and the F8 hook chain onto it")
    check(chained.said("PhotoScan: RB functor slot 0x2e21b90 chained onto") and
          chained.said("Extra Options' resource thunk 0x13fe354"),
          "behind another module: the RB slot and Exit Game (behind Extra Options' thunk) chain onto them")
    # The other modules' hooks keep being called, with their arguments.
    stub = q(base + UI_TICK_SLOT)
    result = FOUR(stub)(11, 22, 33, 44)
    check(calls[-1:] == [("tick", 11, 22, 33, 44)] and result == 0x1234,
          "the F8 stub runs its tick and then the previous slot, with rcx, rdx, r8, r9 and its result")
    functor = ctypes.create_string_buffer(16)
    page = ctypes.create_string_buffer(0x1200)
    struct.pack_into("<Q", functor, 8, ctypes.addressof(page))
    struct.pack_into("<i", page, 0x1194, 1)  # effects panel: RB is the game's (vignette)
    argument = ctypes.c_int(0x14)
    rb = RB(q(base + RB_SLOT))
    value = rb(ctypes.addressof(functor), ctypes.addressof(argument))
    check(value == 7 and calls[-1] == ("rb", ctypes.addressof(functor), ctypes.addressof(argument)),
          "RB in the effects panel goes to the previous handler, with its arguments and result")
    struct.pack_into("<i", page, 0x1194, 3)
    rb(ctypes.addressof(functor), ctypes.addressof(argument))
    check(calls[-1][0] == "rb", "RB before saving (watermark) goes to the previous handler")
    reset = RESET(call_target(base, RESET_VIEW_CALL))
    fake = controller(0x1111)
    before = bytes(fake)
    reset(ctypes.addressof(fake), 0x5678)
    check(calls[-1] == ("reset", ctypes.addressof(fake), 0x5678) and bytes(fake) == before,
          "ResetView of another object: passed on unchanged to the previous target")
    fake = controller(base + MAP_VTABLE)
    reset(ctypes.addressof(fake), 0x9ABC)
    check(calls[-1] == ("reset", ctypes.addressof(fake), 0x9ABC) and close(fields(fake)[1], 5625),
          "ResetView of the pause map: zoom-out raised to 225% before the previous target runs")
    check(chained.said("MapZoom: pause map opened; opening distance 3750 m (game 2500 m), farthest zoom-out 5625 m"),
          "the first pause map logs its distances")
    unmap(base)
    kernel32.VirtualFree(jumps, 0, 0x8000)

    # --- C: nothing changes when off, on another adapter or another image size.
    for label, text, adapter, size, expect in [
            ("every switch 0", "[XtraFeatures]\nMapZoom=0\nGarageThumbnails=0\nPhotoScan=0\nExitGame=0\n", 1,
             IMAGE_SIZE, "is off ([XtraFeatures]"),
            ("Mezcal", ini, 2, IMAGE_SIZE, "does not run on Azimuth"),
            ("another image size", ini, 1, IMAGE_SIZE - 0x1000, "not the supported Forza Horizon 2 build"),
            ("map zoom at 100%", "[MapZoom]\nMapStartZoom=100\nMapMaxZoomOut=100\nMapIslandStartZoom=100\n"
             "MapIslandMaxZoomOut=100\n[XtraFeatures]\nGarageThumbnails=0\nPhotoScan=0\nExitGame=0\n", 1, IMAGE_SIZE,
             "every [MapZoom] percentage is 100")]:
        base = pe.map()
        before = snapshot(base)
        p = Plugin(output, work / label.replace(" ", "-").replace("%", ""), text)
        ok = p.initialize(base, adapter, size)
        check(ok and snapshot(base) == before and p.said(expect), f"{label}: nothing is changed ({expect})")
        unmap(base)

    # --- D: a changed game: that feature only is left out.
    base = pe.map()
    poke(base + RESET_VIEW + 0x10, b"\xcc")
    before = peek(base + RESET_VIEW_CALL, 5)
    p = Plugin(output, work / "changed", ini)
    p.initialize(base)
    check(peek(base + RESET_VIEW_CALL, 5) == before and p.said("MapZoom: ResetView at 0xb803f0 is not as inspected") and
          p.said("PhotoScan: RB functor slot") and p.said("new garage thumbnails"),
          "a changed ResetView: map zoom is left out and says why, the others install")
    unmap(base)

    bad = Context(8, 1, 1, IMAGE_SIZE, None, None, None, None, None, LOG(0))
    check(plugin.dll.FH2PluginInitialize(ctypes.byref(bad)) == 0, "a short context is refused (FALSE)")
    shutil.rmtree(work, ignore_errors=True)


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    parser.add_argument("--zig", type=Path, default=Path(shutil.which("zig") or "zig"))
    parser.add_argument("--game", type=Path, default=Path.home() / "FH2")
    parser.add_argument("--azimuth", type=Path)
    parser.add_argument("--release", type=Path)
    options = parser.parse_args()
    output = options.output.resolve()
    game = options.game
    (output / "test-runs").mkdir(parents=True, exist_ok=True)
    test_package(output)
    dll = load_test_dll(output)
    work = Path(tempfile.mkdtemp(prefix="xtra-settings-", dir=output / "test-runs"))
    test_settings(dll, work, ROOT / "mod" / INI_NAME)
    shutil.rmtree(work, ignore_errors=True)
    pe = PE(game / GAME_EXE) if (game / GAME_EXE).is_file() else None
    test_release(options.release or game / "src/eo-pr/upstream-0.4.4/FH2_Extra_Options_0.4.4-azimuth/Azimuth",
                 options.azimuth or game / "azimuth-windows-1.0.4.zip", pe)
    pe = test_game(dll, game / GAME_EXE)
    test_patches(dll, pe)
    test_map_zoom(dll)
    test_thumbnails(dll)
    test_exit_game(dll, game)
    test_plugin(output, pe)
    print(f"{len(failures)} failure(s)" if failures else "all tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
