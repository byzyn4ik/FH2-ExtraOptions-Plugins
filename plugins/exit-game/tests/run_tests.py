"""Offline tests of the Exit Game plugin (no game run, nothing installed).

Usage: py tests/run_tests.py [--output out] [--zig zig.exe] [--game <FH2 folder>]
                             [--azimuth <azimuth-windows-1.0.4.zip>] [--upstream <Extra Options 0.4.4 checkout>]
                             [--release <Extra Options 0.4.4 release's Azimuth folder>]

Real files are only read (the game's executable and UI.zip, Azimuth's ZIP, the
Extra Options release and source); the game executable is mapped into this
process' own memory and patched there. Defaults: --game ~/FH2,
--azimuth <game>/azimuth-windows-1.0.4.zip, --upstream <game>/src/eo-pr/wt-upstream,
--release <game>/src/eo-pr/upstream-0.4.4/FH2_Extra_Options_0.4.4-azimuth/Azimuth.
A missing input skips the tests that need it.

1. package   mod.json as Extra Options 0.4.4 validates it, the mod layout, the
             DLL (PE32+ x64, the one export, only Windows imports, version
             resource), the INI.
2. azimuth   azimuth.exe 1.0.4: the game window class is Azimuth.Game.Win32 and
             its window procedure answers WM_CLOSE with DestroyWindow.
3. release   Extra Options 0.4.4's winmm.dll patches the sites this plugin
             chains (resource call, pause page hooks) and moves Cars next to Home.
4. game      the game executable passes the guards, alone and as Extra Options
             patches it; every changed byte the guards cover is refused.
5. xaml      the real pause page (UI.zip), alone and after Extra Options 0.4.4's
             own pause page edit (its source, built here): tile, squares,
             well-formed XML; pages that do not match are left alone.
6. captions  all 15 game languages (16 codes, GB = English), from the game's
             language setting.
7. plugin    FH2PluginInitialize on the mapped game: chains both sites behind
             (simulated) Extra Options and another plugin, the page loads with
             the tile through the chain, the init binds the tile after the
             previous init, the tile posts WM_CLOSE to an Azimuth.Game.Win32
             window; mismatches, Enabled=0 and a non-Azimuth host change nothing.
"""

import argparse
import configparser
import ctypes
from ctypes import wintypes
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MOD_NAME = "Exit Game"
DLL_NAME = "ExitGame.dll"
INI_NAME = "ExitGame.ini"
TEST_DLL = "ExitGameTest.dll"
SYSTEM_DLLS = re.compile(r"^(kernel32|user32|msvcrt|api-ms-win-[a-z0-9-]+)\.dll$", re.I)
GAME_EXE = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/forza_durango_release_final.exe")
UI_ZIP = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/media/UI.zip")
PAUSE_PAGE = "Scenes/Anthem/A_PauseMenu.xaml"
CAPTIONS = json.loads((ROOT / "src" / "translations.json").read_text(encoding="utf-8"))

IMAGE_SIZE = 0x3A18000
PAUSE_INIT_SLOT, PAUSE_INIT = 0x2E14310, 0xDC6E30
RESOURCE_CALL, GET_RESOURCE, EO_THUNK = 0x16EBB37, 0xAABEC0, 0x13FE354
CAR_PACKS_BINDING, BIND_BUTTON = 0xDC8155, 0xD9F9F0
TEXT_TABLES = 0x3662990

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        failures.append(what)


def skip(what):
    print("  skip " + what)


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.VirtualAlloc.restype = ctypes.c_void_p
kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
kernel32.VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
kernel32.GetModuleHandleW.restype = ctypes.c_void_p
kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]


# ------------------------------------------------------------------ PE reader

class PE:
    def __init__(self, path=None, data=None):
        self.data = data if data is not None else Path(path).read_bytes()
        d = self.data
        e = struct.unpack_from("<I", d, 0x3C)[0]
        if d[:2] != b"MZ" or d[e:e + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        self.machine, sections, _, _, _, optional_size, self.characteristics = struct.unpack_from("<HHIIIHH", d, e + 4)
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
        self._relocations = None

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

    def section(self, name):
        return next(s for s in self.sections if s[0] == name)

    def exports(self):
        if not self.exports_rva:
            return []
        o = self.offset(self.exports_rva)
        count, names = struct.unpack_from("<I", self.data, o + 24)[0], struct.unpack_from("<I", self.data, o + 32)[0]
        return [self.text(struct.unpack_from("<I", self.data, self.offset(names) + 4 * i)[0]) for i in range(count)]

    def imports(self):
        """{dll: {iat slot rva: function name}}"""
        result, o = {}, self.offset(self.imports_rva) if self.imports_rva else None
        while o is not None:
            lookup, _, _, name, first = struct.unpack_from("<IIIII", self.data, o)
            if not name:
                break
            functions = {}
            thunk = lookup or first
            for i in range(100000):
                value = struct.unpack_from("<Q", self.data, self.offset(thunk + 8 * i))[0]
                if not value:
                    break
                if not value >> 63:
                    functions[first + 8 * i] = self.text(value + 2)
            result[self.text(name)] = functions
            o += 20
        return result

    def relocations(self):
        """RVAs of the image's 64-bit base relocations."""
        if self._relocations is None:
            table = self.read(self.relocations_rva, self.relocations_size)
            found, at = [], 0
            while at + 8 <= len(table):
                page, size = struct.unpack_from("<II", table, at)
                if size < 8:
                    break
                for (entry,) in struct.iter_unpack("<H", table[at + 8:at + size]):
                    if entry >> 12 == 10:  # IMAGE_REL_BASED_DIR64
                        found.append(page + (entry & 0xFFF))
                at += size
            self._relocations = found
        return self._relocations

    def map(self):
        """The image laid out and relocated as the loader maps it, in private RWX memory; returns its address."""
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
    check(manifest.get("format_version") == 1 and valid_mod_id(manifest.get("id")),
          f"mod.json: format_version 1 and a valid id ({manifest.get('id')})")
    check(isinstance(manifest.get("dependencies"), list) and "media" not in manifest and "database" not in manifest,
          "mod.json: dependencies list, no media or database (nothing for the media merge)")
    files = sorted(str(p.relative_to(mod)).replace("\\", "/") for p in mod.rglob("*") if p.is_file())
    check(files == ["mod.json", "plugins/" + DLL_NAME, "plugins/" + INI_NAME],
          f"out/mods/{MOD_NAME}: mod.json, plugins/{DLL_NAME}, plugins/{INI_NAME} ({files})")
    pe = PE(mod / "plugins" / DLL_NAME)
    check(pe.machine == 0x8664 and pe.magic == 0x20B and pe.characteristics & 0x2000, "the DLL is a PE32+ x64 DLL")
    check(pe.exports() == ["FH2PluginInitialize"], f"the one export is FH2PluginInitialize ({pe.exports()})")
    imports = list(pe.imports())
    check(all(SYSTEM_DLLS.match(name) for name in imports), f"only Windows DLLs are imported ({', '.join(imports)})")
    version = manifest.get("version", "")
    blob = pe.data
    check(bool(pe.resources_rva) and ("FileVersion\0" + version).encode("utf-16le") in blob.replace(b"\0\0\0", b"\0"),
          f"version resource with FileVersion {version}")
    test_pe = PE(output / "tests" / TEST_DLL)
    check("FH2PluginInitialize" in test_pe.exports() and "ExitGameTestXaml" in test_pe.exports(),
          "the test DLL has the plugin entry and the test entry points")
    ini = configparser.ConfigParser()
    ini.read(mod / "plugins" / INI_NAME, encoding="utf-8")
    check(ini.get("ExitGame", "Enabled", fallback=None) == "1", "ExitGame.ini: [ExitGame] Enabled=1")


# ---------------------------------------------------------------- 2. azimuth

def rip_target(at, length, data_offset, code):
    return at + length + struct.unpack_from("<i", code, data_offset)[0]


def test_azimuth(azimuth_zip, work):
    print("azimuth")
    if not azimuth_zip or not azimuth_zip.is_file():
        skip(f"Azimuth ZIP not found ({azimuth_zip})")
        return
    with zipfile.ZipFile(azimuth_zip) as archive:
        exe = archive.read("azimuth.exe")
    pe = PE(data=exe)
    _, text_va, text_size, _, _ = pe.section(".text")
    code = pe.read(text_va, text_size)
    iat = {slot: name for functions in pe.imports().values() for slot, name in functions.items()}
    strings = []
    needle = "Azimuth.Game.Win32".encode("utf-16le") + b"\0\0"
    for _, va, vsize, rsize, raw in pe.sections:
        chunk = pe.data[raw:raw + min(vsize, rsize)]
        strings += [va + m.start() for m in re.finditer(re.escape(needle), chunk) if m.start() % 2 == 0]
    check(bool(strings), "azimuth.exe has the UTF-16 string Azimuth.Game.Win32")

    def calls_after(at, limit):
        """IAT functions called (call [rip+x]) in the `limit` bytes after code offset `at`."""
        found = []
        for i in range(at, min(at + limit, len(code) - 6)):
            if code[i] == 0xFF and code[i + 1] == 0x15:
                found.append(iat.get(rip_target(text_va + i, 6, i + 2, code), "?"))
        return found

    register = create = None
    wndproc = None
    for m in re.finditer(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", code):
        i = m.start()
        if rip_target(text_va + i, 7, i + 3, code) not in strings:
            continue
        following = calls_after(i + 7, 24)
        if following[:1] == ["RegisterClassExW"]:
            register = i
            # lpfnWndProc: lea rax,[rip+x]; mov [rsp+78h],rax (WNDCLASSEXW at rsp+70h) before it.
            for j in range(i - 0x100, i):
                if code[j:j + 3] == b"\x48\x8d\x05" and code[j + 7:j + 12] == b"\x48\x89\x44\x24\x78":
                    wndproc = rip_target(text_va + j, 7, j + 3, code) - text_va
        elif following[:1] == ["CreateWindowExW"]:
            create = i
    check(register is not None, "the class Azimuth.Game.Win32 is registered (RegisterClassExW)")
    check(create is not None, "the game window is created with that class (CreateWindowExW)")
    closes = False
    if wndproc is not None:
        body = code[wndproc:wndproc + 0x400]
        m = re.search(rb"\x83\xfb\x10\x0f\x87....\x0f\x84(....)", body, re.S)
        if m:
            target = wndproc + m.end() + struct.unpack("<i", m.group(1))[0]
            closes = code[target:target + 3] == b"\x49\x8b\xcf" and calls_after(target + 3, 6)[:1] == ["DestroyWindow"]
    check(closes, "its window procedure answers WM_CLOSE (0x10) with DestroyWindow(hwnd), as the close button does")


# ---------------------------------------------------------------- 3. release

def test_release(release):
    print("release")
    winmm = release / "winmm.dll" if release else None
    if not winmm or not winmm.is_file():
        skip(f"Extra Options 0.4.4 release not found ({release})")
        return
    data = winmm.read_bytes()
    check(b"0.4.4\0" in data, "winmm.dll is Extra Options 0.4.4")
    call = struct.pack("<i", GET_RESOURCE - (RESOURCE_CALL + 5))
    thunk_call = struct.pack("<i", EO_THUNK - (RESOURCE_CALL + 5))
    check(b"\xe8" + call in data and b"\xe8" + thunk_call in data,
          "its patch data turns the XAML resource call into call 0x13fe354 (its thunk)")
    check(all(s in data for s in (b"drift_video_resource_call", b"drift_video_resource_thunk", b"pause_garage_footer")),
          "it has the resource thunk and the pause page init hook (Return to Garage)")
    check(all(s in data for s in (b'x:Class="ForzaUI.A_PauseMenu"', b'x:Name="PART_GROUP_Autoshow"',
                                  b'x:Name="PART_SCOPE_Autoshow"', b"CarsNextToHome")),
          "it edits the pause page (CarsNextToHome) with the anchors of its source")
    check(b"PART_Button_EOExitGame" not in data and b"PART_Button_HorizonOnline" not in data,
          "it does not touch the Home tab tiles itself")
    ini = configparser.ConfigParser(strict=False)
    ini.read(release / "FH2_ExtraOptions.ini", encoding="utf-8")
    check(ini.get("Frontend", "CarsNextToHome", fallback="?") == "1", "its INI ships CarsNextToHome=1")
    check(b"FH2PluginInitialize" in data, "it loads mod plugins (FH2PluginInitialize)")


# ------------------------------------------------------------------- test DLL

def load_test_dll(output):
    dll = ctypes.CDLL(str(output / "tests" / TEST_DLL))
    dll.ExitGameTestXaml.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                                     ctypes.c_char_p]
    dll.ExitGameTestXaml.restype = ctypes.c_size_t
    dll.ExitGameTestWhy.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_size_t]
    dll.ExitGameTestWhy.restype = ctypes.c_char_p
    dll.ExitGameTestCaption.argtypes = [ctypes.c_char_p]
    dll.ExitGameTestCaption.restype = ctypes.c_wchar_p
    dll.ExitGameTestGameCaption.argtypes = [ctypes.c_void_p]
    dll.ExitGameTestGameCaption.restype = ctypes.c_wchar_p
    dll.ExitGameTestLanguages.restype = ctypes.c_uint
    dll.ExitGameTestLanguage.argtypes = [ctypes.c_uint]
    dll.ExitGameTestLanguage.restype = ctypes.c_char_p
    dll.ExitGameTestCheck.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int]
    dll.ExitGameTestCheck.restype = ctypes.c_char_p
    dll.ExitGameTestHandlers.argtypes = [ctypes.c_int]
    dll.ExitGameTestHandlers.restype = ctypes.c_void_p
    dll.ExitGameTestNatives.argtypes = [ctypes.c_void_p] * 6
    return dll


def extend(dll, xaml, code=b"EN", room=0x4000):
    out = ctypes.create_string_buffer(len(xaml) + room)
    n = dll.ExitGameTestXaml(xaml, len(xaml), out, len(out), code)
    return out.raw[:n] if n else b""


def why(dll, xaml, room=0x4000):
    text = dll.ExitGameTestWhy(xaml, len(xaml), len(xaml) + room)
    return text.decode() if text else None


# ------------------------------------------------------------------- 4. game

def eo_patch(base, reader_address):
    """Extra Options 0.4.4's resource patch: its thunk at 0x13fe354 and the call pointed at it."""
    poke(base + EO_THUNK, b"\x48\xb8" + struct.pack("<Q", reader_address) + b"\xff\xe0")
    poke(base + RESOURCE_CALL, b"\xe8" + struct.pack("<i", EO_THUNK - (RESOURCE_CALL + 5)))


def test_game(dll, game_exe):
    print("game")
    if not game_exe.is_file():
        skip(f"{game_exe} not found")
        return None
    pe = PE(game_exe)
    check(pe.image_size == IMAGE_SIZE, "the game executable is the inspected build (image size 0x3a18000)")
    base = pe.map()
    try:
        init = dll.ExitGameTestCheck(base, IMAGE_SIZE, 0)
        resource = dll.ExitGameTestCheck(base, IMAGE_SIZE, 1)
        check(init == b"the game's pause page init 0xdc6e30", f"stock game: the init slot holds {init}")
        check(resource == b"the game's resource reader 0xaabec0", f"stock game: the resource call goes to {resource}")
        # As Extra Options 0.4.4 patches it (any executable code outside the image will do).
        hook = ctypes.cast(dll.ExitGameTestCaption, ctypes.c_void_p).value
        eo_patch(base, hook)
        poke(base + PAUSE_INIT_SLOT, struct.pack("<Q", hook))
        init = dll.ExitGameTestCheck(base, IMAGE_SIZE, 0)
        resource = dll.ExitGameTestCheck(base, IMAGE_SIZE, 1)
        check(init is not None and init.startswith(b"a hook at ExitGameTest.dll+0x"),
              f"with Extra Options' hooks: the init slot holds {init}")
        check(resource is not None and resource.startswith(b"Extra Options' resource thunk 0x13fe354 -> ExitGameTest.dll+0x"),
              f"with Extra Options' hooks: the resource call goes to {resource}")

        def refused(rva, data, expect, what):
            old = peek(base + rva, len(data))
            poke(base + rva, data)
            problem = dll.ExitGameTestCheck(base, IMAGE_SIZE, 0)
            poke(base + rva, old)
            check(problem is not None and expect in problem, f"{what} is refused ({problem.decode() if problem else None})")

        flip = lambda rva: bytes([peek(base + rva, 1)[0] ^ 0xFF])
        refused(BIND_BUTTON + 3, flip(BIND_BUTTON + 3), b"tile binder at 0xd9f9f0", "a changed tile binder")
        refused(CAR_PACKS_BINDING + 0x31, flip(CAR_PACKS_BINDING + 0x31), b"Car Packs tile binding",
                "a Car Packs binding calling elsewhere")
        refused(CAR_PACKS_BINDING + 0x29, b"\0\0\0\0", b"Car Packs tile binding", "a Car Packs binding with another name")
        refused(PAUSE_INIT, b"\xcc", b"pause page init at 0xdc6e30", "a changed pause page init")
        refused(RESOURCE_CALL + 5, b"\x90", b"after the XAML resource call", "changed code after the resource call")
        refused(0x3229C0, b"\xcc", b"allocate", "a changed native allocator")
        refused(PAUSE_INIT_SLOT, struct.pack("<Q", base + 0x1000), b"init slot 0x2e14310",
                "an init slot pointing elsewhere in the game")
        refused(PAUSE_INIT_SLOT, struct.pack("<Q", ctypes.addressof(ctypes.c_int(0))), b"init slot 0x2e14310",
                "an init slot pointing at data")
        refused(EO_THUNK, b"\x90\x90", b"not Extra Options' thunk", "a resource call to a changed thunk")
        refused(RESOURCE_CALL, b"\x90", b"not a call", "a resource site that is no longer a call")
        refused(RESOURCE_CALL + 1, struct.pack("<i", 0x1000 - (RESOURCE_CALL + 5)), b"somewhere unexpected",
                "a resource call into other game code")
        problem = dll.ExitGameTestCheck(base, IMAGE_SIZE - 0x1000, 0)
        check(problem is not None and b"supported size" in problem, "another image size is refused")
    finally:
        unmap(base)
    return pe


# ------------------------------------------------------------------- 5. xaml

def parse(data, label):
    try:
        return ET.fromstring(data.decode("utf-8-sig"))
    except (ET.ParseError, UnicodeDecodeError) as error:
        check(False, f"{label}: well-formed XML ({error})")
        return None


def element_names(root):
    return [e.get("{http://schemas.microsoft.com/winfx/2006/xaml}Name") for e in root.iter()]


def check_page(text, label, native, caption="EXIT GAME"):
    root = parse(text, label)
    if root is None:
        return
    x = "{http://schemas.microsoft.com/winfx/2006/xaml}"
    home = next((e for e in root.iter() if e.get(x + "Name") == "PART_SCOPE_Home"), None)
    tiles = {e.get(x + "Name"): e for e in home} if home is not None else {}
    tile = tiles.get("PART_Button_EOExitGame")
    grid = "{clr-namespace:ForzaUI;assembly=}"
    nav = lambda e, k: e.get(grid + "TileMenuNavigationManager." + k) if e is not None else None
    check(tile is not None, f"{label}: PART_Button_EOExitGame is a tile of the Home tab")
    check(tile is not None and (nav(tile, "GridX"), nav(tile, "GridY"), nav(tile, "TileState")) == ("1", "1", "Enabled")
          and tile.get("Style") == "{StaticResource A_Style_Button_Tile_1x1}"
          and tile.get("Template") == "{StaticResource A_ControlTemplate_IconTile}"
          and tile.get("DataContext") == "{StaticResource EO_MenuDataItem_ExitGame}",
          f"{label}: a 1x1 icon tile at grid (1,1), enabled, bound to EO_MenuDataItem_ExitGame")
    squares = [n for n in ("PART_Button_HorizonOnline", "PART_Button_HorizonSolo")
               if n in tiles and tiles[n].get("Style") == "{StaticResource A_Style_Button_Tile_1x1}"
               and tiles[n].get("Template") == "{StaticResource EO_ControlTemplate_SquareImageTile}"]
    check(len(squares) == 2, f"{label}: Online Road Trip and Horizon Solo are square tiles ({squares})")
    resources = [e for e in root.iter() if e.tag.endswith("ResourceDictionary")]
    keys = {e.get(x + "Key"): e for r in resources[:1] for e in r}
    item = keys.get("EO_MenuDataItem_ExitGame")
    check("EO_ControlTemplate_SquareImageTile" in keys, f"{label}: the square tile template is a page resource")
    check(item is not None and item.get("Heading") == caption
          and item.get("Icon") == "/ForzaUI;component/Textures/Anthem/Icons/PauseMenu/Quit.png",
          f"{label}: the tile's item has the caption {caption!r} and the game's Quit icon")
    # Nothing else changed: the same elements in the same order, plus ours.
    template = keys.get("EO_ControlTemplate_SquareImageTile")
    ours = {id(e) for e in template.iter()} if template is not None else set()
    before = [n for n in element_names(parse(native, label + " (input)")) if n]
    after = [e.get(x + "Name") for e in root.iter()
             if e.get(x + "Name") and e.get(x + "Name") != "PART_Button_EOExitGame" and id(e) not in ours]
    check(before == after, f"{label}: every other named element is kept, in order ({len(before)})")
    return root


def upstream_pause_dll(zig, upstream, build):
    """Extra Options 0.4.4's own pause page edit (features/pause_menu.c), built as a small DLL."""
    folder = build / "eo-pause-menu"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    revision = subprocess.run(["git", "-C", str(upstream), "rev-parse", "HEAD"], capture_output=True, text=True,
                              check=True).stdout.strip()
    for name in ("extra_options.h", "support.h", "support.c", "features/pause_menu.h", "features/pause_menu.c"):
        data = subprocess.run(["git", "-C", str(upstream), "show", f"{revision}:{name}"], capture_output=True,
                              check=True).stdout
        (folder / Path(name).name).write_bytes(data)
    (folder / "wrapper.c").write_text(
        '#include "pause_menu.h"\n'
        "__declspec(dllexport) size_t EoPauseReorder(const char *t, size_t n, char *o, size_t c) {\n"
        "    pause_menu_configure(1);\n    return pause_menu_reorder(t, n, o, c);\n}\n", encoding="utf-8")
    sources = [str(folder / n) for n in ("wrapper.c", "support.c", "pause_menu.c")]
    dll = folder / "eo_pause.dll"
    command = [str(zig), "cc", "-target", "x86_64-windows-gnu", "-O1", "-w", "-shared", "-fms-extensions",
               "-fno-builtin", "-DFH2_AZIMUTH", "-I", str(folder), *sources, "-o", str(dll)]
    run = subprocess.run(command, capture_output=True, text=True)
    if run.returncode:
        missing = sorted(set(re.findall(r"undefined symbol: (\w+)", run.stderr)))
        if not missing:
            print(run.stderr)
            return None, revision
        (folder / "stubs.c").write_text("".join(f"void *{n}(void) {{ return 0; }}\n" for n in missing))
        run = subprocess.run(command[:-2] + [str(folder / "stubs.c")] + command[-2:], capture_output=True, text=True)
        if run.returncode:
            print(run.stderr)
            return None, revision
    library = ctypes.CDLL(str(dll))
    library.EoPauseReorder.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    library.EoPauseReorder.restype = ctypes.c_size_t
    return library, revision


def eo_reorder(library, xaml):
    out = ctypes.create_string_buffer(len(xaml) + 1)  # Extra Options' own capacity for this page
    n = library.EoPauseReorder(xaml, len(xaml), out, len(out))
    return out.raw[:n] if n else b""


def group_order(text):
    return re.findall(rb'x:Name="(PART_(?:GROUP|SCOPE)_\w+)"', text)


def test_xaml(dll, zig, game, upstream, output):
    print("xaml")
    ui = game / UI_ZIP
    if not ui.is_file():
        skip(f"{ui} not found")
        return None, None
    with zipfile.ZipFile(ui) as archive:
        native = archive.read(PAUSE_PAGE)
        video = archive.read("Scenes/Anthem/A_VideoOptions.xaml")
    ours = extend(dll, native)
    check(bool(ours), "the game's pause page gets the tile")
    if not ours:
        print("    why:", why(dll, native))
        return native, None
    check_page(ours, "game page", native)
    with zipfile.ZipFile(ui) as archive:
        defined = {k for n in archive.namelist() if n.lower().endswith(".xaml")
                   for k in re.findall(rb'x:Key="([^"]+)"', archive.read(n))}
    added = set(re.findall(rb"StaticResource (\w+)", ours)) - set(re.findall(rb"StaticResource (\w+)", native))
    missing = sorted(k.decode() for k in added if k not in defined and not k.startswith(b"EO_"))
    check(added and not missing, f"every resource the tile uses is the game's own (UI.zip) or the plugin's ({missing})")
    x = "{http://schemas.microsoft.com/winfx/2006/xaml}"
    icons = {e.get(x + "Key"): e.get("Icon") for e in parse(ours, "game page").iter() if e.get("Icon")}
    check(icons.get("A_MenuDataItem_Quit") and icons.get("EO_MenuDataItem_ExitGame") == icons["A_MenuDataItem_Quit"],
          f"the tile's icon is the one of the game's own Quit tile ({icons.get('A_MenuDataItem_Quit')})")
    check(max(ours) < 0x80, "the page stays ASCII (captions as character references)")
    check(len(ours) - len(native) < 0x4000, f"it grows by {len(ours) - len(native)} bytes (room: 0x4000)")
    check(why(dll, ours) == "it already has the Exit Game tile", "a page that has the tile is left alone")
    check(not extend(dll, video) and why(dll, video) == "not the pause menu page", "another page is left alone")
    out = ctypes.create_string_buffer(len(ours))
    check(dll.ExitGameTestXaml(native, len(native), out, len(ours), b"EN") == 0,
          "a buffer one byte short (no room for the NUL) is refused")
    out = ctypes.create_string_buffer(len(ours) + 1)
    check(dll.ExitGameTestXaml(native, len(native), out, len(ours) + 1, b"EN") == len(ours), "an exact buffer works")
    no_solo = native.replace(b'x:Name="PART_Button_HorizonSolo"', b'x:Name="PART_Button_Other"')
    check(not extend(dll, no_solo), f"a Home tab without Horizon Solo is left alone ({why(dll, no_solo)})")
    taken = native.replace(b'local:TileMenuNavigationManager.GridX="0"\r\n                                local:'
                           b'TileMenuNavigationManager.GridY="1"\r\n                                local:'
                           b'UIPageTransitionManager.TransitionIndex="2"\r\n                                '
                           b'x:Name="PART_Button_HorizonSolo"',
                           b'local:TileMenuNavigationManager.GridX="1"\r\n                                local:'
                           b'TileMenuNavigationManager.GridY="1"\r\n                                local:'
                           b'UIPageTransitionManager.TransitionIndex="2"\r\n                                '
                           b'x:Name="PART_Button_HorizonSolo"', 1)
    check(taken != native and not extend(dll, taken),
          f"a Home tab with something in column 1 is left alone ({why(dll, taken)})")
    narrow = native.replace(b'Template="{StaticResource A_ControlTemplate_HorizontalImageTile}"\r\n'
                            b'                                local:TileMenuNavigationManager.TileState="Disabled"\r\n'
                            b'                                local:TileMenuNavigationManager.GridX="0"\r\n'
                            b'                                local:TileMenuNavigationManager.GridY="1"\r\n'
                            b'                                local:UIPageTransitionManager.TransitionIndex="2"\r\n'
                            b'                                x:Name="PART_Button_HorizonOnline"',
                            b'Template="{StaticResource A_ControlTemplate_IconTile}"\r\n'
                            b'                                local:TileMenuNavigationManager.TileState="Disabled"\r\n'
                            b'                                local:TileMenuNavigationManager.GridX="0"\r\n'
                            b'                                local:TileMenuNavigationManager.GridY="1"\r\n'
                            b'                                local:UIPageTransitionManager.TransitionIndex="2"\r\n'
                            b'                                x:Name="PART_Button_HorizonOnline"', 1)
    check(narrow != native and not extend(dll, narrow),
          f"an Online Road Trip tile that is not the wide image tile is left alone ({why(dll, narrow)})")
    (output / "test-runs").mkdir(parents=True, exist_ok=True)
    (output / "test-runs" / "pause_game.xaml").write_bytes(ours)

    eo_page = None
    if not upstream or not (upstream / ".git").exists():
        skip(f"Extra Options 0.4.4 source not found ({upstream})")
        return native, None
    library, revision = upstream_pause_dll(zig, upstream, output / "obj")
    print(f"    Extra Options source {revision[:7]} (features/pause_menu.c)")
    if not library:
        check(False, "Extra Options' pause page edit builds for the test")
        return native, None
    eo_page = eo_reorder(library, native)
    check(bool(eo_page) and group_order(eo_page) != group_order(native),
          "Extra Options 0.4.4 (CarsNextToHome=1) moves the Cars tab next to Home on the game's page")
    both = extend(dll, eo_page)
    check(bool(both), "the plugin adds the tile to Extra Options' page (it runs after Extra Options)")
    if both:
        check_page(both, "Extra Options' page", eo_page)
        check(group_order(both) == group_order(eo_page), "Extra Options' tab order (Cars next to Home) is kept")
        check(eo_reorder(library, ours) == both,
              "the two edits are independent: Extra Options' edit of the page with the tile gives the same page")
        (output / "test-runs" / "pause_eo_0.4.4.xaml").write_bytes(both)
    return native, eo_page


# ---------------------------------------------------------------- 6. captions

def language_manager(code):
    """A stand-in for the game's text manager: the language code as a short std::string at +0x30."""
    manager = ctypes.create_string_buffer(0x60)
    raw = code.encode()
    ctypes.memmove(ctypes.addressof(manager) + 0x30, raw + b"\0", len(raw) + 1)
    struct.pack_into("<QQ", manager, 0x40, len(raw), 15)
    return manager


def test_captions(dll, native):
    print("captions")
    codes = [dll.ExitGameTestLanguage(i).decode() for i in range(dll.ExitGameTestLanguages())]
    check(codes == list(CAPTIONS), f"the plugin has every code of translations.json ({len(codes)})")
    languages = {code for code in codes if code != "GB"}
    check(len(languages) == 15 and CAPTIONS["GB"] == CAPTIONS["EN"],
          "15 game languages (EN BR DE FR ES MX IT JP KO CHT CZ HU PL RU NL), GB = English")
    fork = {"EN": "EXIT GAME", "BR": "SAIR DO JOGO", "DE": "SPIEL BEENDEN", "FR": "QUITTER LE JEU",
            "ES": "SALIR DEL JUEGO", "MX": "SALIR DEL JUEGO", "IT": "ESCI DAL GIOCO", "JP": "ゲームを終了",
            "KO": "게임 종료", "CHT": "結束遊戲", "CZ": "UKONČIT HRU", "HU": "KILÉPÉS A JÁTÉKBÓL",
            "PL": "WYJDŹ Z GRY", "RU": "ВЫЙТИ ИЗ ИГРЫ", "NL": "SPEL AFSLUITEN"}
    check(all(CAPTIONS[c] == t for c, t in fork.items()), "the captions are those of the Exit Game branch (PR #111)")
    cell = ctypes.c_void_p()
    fake_game = ctypes.addressof(cell) - TEXT_TABLES  # exit_caption() reads game+0x3662990
    for code in codes:
        caption = CAPTIONS[code]
        check(dll.ExitGameTestCaption(code.encode()) == caption and
              dll.ExitGameTestCaption(code.lower().encode()) == caption, f"{code}: caption {caption}")
        manager = language_manager(code)
        cell.value = ctypes.addressof(manager)
        check(dll.ExitGameTestGameCaption(fake_game) == caption, f"{code}: chosen from the game's language setting")
        if native:
            page = extend(dll, native, code.encode())
            root = parse(page, code) if page else None
            heading = next((e.get("Heading") for e in root.iter()
                            if e.get("{http://schemas.microsoft.com/winfx/2006/xaml}Key") == "EO_MenuDataItem_ExitGame"),
                           None) if root is not None else None
            check(heading == caption and max(page) < 0x80, f"{code}: the pause page shows {heading}")
    for bad in ("", "XX", "ENGLISH"):
        manager = language_manager(bad) if bad else None
        cell.value = ctypes.addressof(manager) if manager else None
        check(dll.ExitGameTestGameCaption(fake_game) == CAPTIONS["EN"], f"unknown language {bad!r}: English")
    check(dll.ExitGameTestGameCaption(None) == CAPTIONS["EN"], "no game image: English")


# ----------------------------------------------------------------- 7. plugin

LOG = ctypes.CFUNCTYPE(None, ctypes.c_char_p)
PAGE_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
READER = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
ALLOC = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_size_t)
BIND = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)


class Context(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("api_version", ctypes.c_uint32), ("adapter", ctypes.c_uint32),
                ("game_image_size", ctypes.c_uint32), ("game_image", ctypes.c_void_p),
                ("plugin_module", ctypes.c_void_p), ("plugins_directory", ctypes.c_wchar_p),
                ("plugin_path", ctypes.c_wchar_p), ("extra_options_version", ctypes.c_char_p), ("log_utf8", LOG)]


class Plugin:
    """FH2PluginInitialize of one of the DLLs, as Extra Options 0.4.4 calls it."""

    def __init__(self, path, work, enabled=1):
        self.dll = ctypes.CDLL(str(path))
        self.dll.FH2PluginInitialize.argtypes = [ctypes.POINTER(Context)]
        self.dll.FH2PluginInitialize.restype = wintypes.BOOL
        self.lines = []
        self.log = LOG(lambda text: self.lines.append(text.decode("utf-8", "replace")))
        folder = work / "mods" / MOD_NAME / "plugins"
        folder.mkdir(parents=True, exist_ok=True)
        (folder / INI_NAME).write_text(f"[ExitGame]\nEnabled={enabled}\n", encoding="utf-8")
        self.folder, self.path = str(folder), str(folder / DLL_NAME)

    def initialize(self, base, adapter=1, size=IMAGE_SIZE):
        context = Context(ctypes.sizeof(Context), 1, adapter, size, base, None, self.folder, self.path, b"0.4.4",
                          self.log)
        self.context = context
        return self.dll.FH2PluginInitialize(ctypes.byref(context))

    def said(self, text):
        return any(text in line for line in self.lines)


def near(base, size=0x1000):
    """Executable memory within 2 GB of the mapped image (for a stand-in plugin's jump)."""
    for k in range(1, 0x8000):
        at = kernel32.VirtualAlloc(base + IMAGE_SIZE + k * 0x10000, size, 0x3000, 0x40)
        if at:
            return at
    raise MemoryError("no memory near the image")


def call_target(base, rva):
    code = peek(base + rva, 5)
    return base + rva + 5 + struct.unpack_from("<i", code, 1)[0] if code[0] == 0xE8 else None


class Window:
    """A hidden window of Azimuth's game window class in this process."""

    def __init__(self):
        u = ctypes.WinDLL("user32", use_last_error=True)
        self.u = u

        class WNDCLASSEXW(ctypes.Structure):
            _fields_ = [("cbSize", ctypes.c_uint), ("style", ctypes.c_uint), ("lpfnWndProc", ctypes.c_void_p),
                        ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int), ("hInstance", ctypes.c_void_p),
                        ("hIcon", ctypes.c_void_p), ("hCursor", ctypes.c_void_p), ("hbrBackground", ctypes.c_void_p),
                        ("lpszMenuName", ctypes.c_wchar_p), ("lpszClassName", ctypes.c_wchar_p),
                        ("hIconSm", ctypes.c_void_p)]
        u.RegisterClassExW.argtypes = [ctypes.POINTER(WNDCLASSEXW)]
        u.RegisterClassExW.restype = ctypes.c_uint16
        u.CreateWindowExW.argtypes = [ctypes.c_uint32, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32] + \
            [ctypes.c_int] * 4 + [ctypes.c_void_p] * 4
        u.CreateWindowExW.restype = ctypes.c_void_p
        u.DestroyWindow.argtypes = [ctypes.c_void_p]
        u.UnregisterClassW.argtypes = [ctypes.c_wchar_p, ctypes.c_void_p]
        u.PeekMessageW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
        self.instance = kernel32.GetModuleHandleW(None)
        self.cls = WNDCLASSEXW(ctypes.sizeof(WNDCLASSEXW), 0, ctypes.cast(u.DefWindowProcW, ctypes.c_void_p).value,
                               0, 0, self.instance, None, None, None, None, "Azimuth.Game.Win32", None)
        self.atom = u.RegisterClassExW(ctypes.byref(self.cls))
        self.hwnd = u.CreateWindowExW(0, "Azimuth.Game.Win32", "Exit Game test", 0, 0, 0, 0, 0, None, None,
                                      self.instance, None)

    def messages(self):
        msg = wintypes.MSG()
        seen = []
        while self.u.PeekMessageW(ctypes.byref(msg), self.hwnd, 0, 0, 1):
            seen.append(msg.message)
        return seen

    def close(self):
        if self.hwnd:
            self.u.DestroyWindow(self.hwnd)
            self.messages()
        self.u.UnregisterClassW("Azimuth.Game.Win32", self.instance)


def make_resource(keep, text):
    """A resource object like the game's (refcount 1), with a vtable whose slot 0 is never called."""
    vtable = (ctypes.c_void_p * 8)(0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888)
    data = ctypes.create_string_buffer(text, len(text) + 1)
    resource = ctypes.create_string_buffer(0x38)
    struct.pack_into("<Q", resource, 0, ctypes.addressof(vtable) + 8)
    struct.pack_into("<i", resource, 0x10, 1)
    struct.pack_into("<Qii", resource, 0x28, ctypes.addressof(data), len(text), len(text) + 1)
    keep += [vtable, data, resource]
    return ctypes.addressof(resource)


def read_resource(address):
    raw = peek(address, 0x38)
    bytes_at, length, capacity = struct.unpack_from("<Qii", raw, 0x28)
    return peek(bytes_at, length), struct.unpack_from("<Q", raw, 0)[0], capacity


def test_plugin(output, pe, native, eo_page):
    print("plugin")
    if pe is None:
        skip("the plugin tests need the game executable")
        return
    work = Path(tempfile.mkdtemp(prefix="exit-game-", dir=output / "test-runs"))
    test_path = output / "tests" / TEST_DLL
    shipping_path = output / "mods" / MOD_NAME / "plugins" / DLL_NAME

    # --- A: behind Extra Options 0.4.4 and another plugin, through the hooks.
    if native is None:
        skip("plugin chain: needs the pause page XAML (UI.zip)")
    else:
        plugin = Plugin(test_path, work / "chain")
        dll = load_test_dll(output)
        base = pe.map()
        keep, events = [], []
        page_xaml = eo_page or native
        allocations = {}

        def allocate(size):
            buffer = ctypes.create_string_buffer(size)
            allocations[ctypes.addressof(buffer)] = buffer
            events.append("allocate")
            return ctypes.addressof(buffer)

        callbacks = [
            ALLOC(allocate),
            PAGE_FN(lambda p: (allocations.pop(p, None), events.append("free"))),
            PAGE_FN(lambda p: events.append("object init")),
            PAGE_FN(lambda p: events.append("object cleanup")),
            PAGE_FN(lambda p: events.append(("delete", p))),
            BIND(lambda page, name, handler: events.append(
                ("bind", page, name, struct.unpack_from("<Qi", peek(handler, 12))))),
        ]
        dll.ExitGameTestNatives(*[ctypes.cast(c, ctypes.c_void_p).value for c in callbacks])
        resources = {}

        def eo_reader(reader, out):
            events.append("extra options reader")
            out[0] = resources[reader]
            return ctypes.cast(out, ctypes.c_void_p).value

        eo = READER(eo_reader)
        eo_init = PAGE_FN(lambda page: events.append(("extra options init", page)))
        eo_patch(base, ctypes.cast(eo, ctypes.c_void_p).value)
        poke(base + PAUSE_INIT_SLOT, struct.pack("<Q", ctypes.cast(eo_init, ctypes.c_void_p).value))
        # Another plugin chained before this one (as Azimuth Graphics Options does): its jump near the image.
        thunk_reader = READER(base + EO_THUNK)

        def other_plugin(reader, out):
            events.append("other plugin")
            return thunk_reader(reader, out)

        other = READER(other_plugin)
        jump = near(base)
        poke(jump, b"\xff\x25\0\0\0\0" + struct.pack("<Q", ctypes.cast(other, ctypes.c_void_p).value))
        poke(base + RESOURCE_CALL + 1, struct.pack("<i", jump - (base + RESOURCE_CALL + 5)))
        manager = language_manager("PL")
        poke(base + TEXT_TABLES, struct.pack("<Q", ctypes.addressof(manager)))
        keep += callbacks + [eo, eo_init, other, thunk_reader, manager]

        ready = plugin.initialize(base)
        check(ready == 1, "FH2PluginInitialize returns TRUE")
        for line in plugin.lines:
            print("    log:", line.rstrip())
        check(plugin.said("EXIT GAME: plugin 1.0.0 for Extra Options 0.4.4"), "it logs its version and the host's")
        slot = struct.unpack("<Q", peek(base + PAUSE_INIT_SLOT, 8))[0]
        check(slot == dll.ExitGameTestHandlers(0), "the pause page init slot now holds the plugin's init")
        target = call_target(base, RESOURCE_CALL)
        check(target is not None and abs(target - base) < 0x7FF00000 and
              peek(target, 6) == b"\xff\x25\0\0\0\0" and
              struct.unpack("<Q", peek(target + 6, 8))[0] == dll.ExitGameTestHandlers(1),
              "the resource call now goes through a near jump to the plugin's reader")
        check(plugin.said("chained onto a hook at") and plugin.said("chained onto another module's hook at"),
              "the log says what both sites called before")

        call = READER(target)
        # The pause page: the game's, as Extra Options and the other plugin leave it.
        resources[1] = make_resource(keep, page_xaml)
        out = ctypes.c_void_p()
        events.clear()
        call(1, ctypes.byref(out))
        check(events[:2] == ["other plugin", "extra options reader"], f"the previous readers run first ({events[:2]})")
        check(out.value != resources[1], "the pause page comes back as a new resource")
        if out.value != resources[1]:
            text, vtable, capacity = read_resource(out.value)
            check_page(text, "through the hooks (Polish)", page_xaml, CAPTIONS["PL"])
            bytes_at = struct.unpack_from("<Q", peek(out.value, 0x30), 0x28)[0]
            check(capacity == len(page_xaml) + 0x4000 and peek(bytes_at + len(text), 1) == b"\0",
                  "its buffer is NUL-terminated, with the plugin's room")
            slots = struct.unpack("<7Q", peek(vtable - 8, 56))
            check(slots[0] == 0x1111 and slots[2:] == (0x3333, 0x4444, 0x5555, 0x6666, 0x7777) and slots[1] not in (0x2222, 0),
                  "its vtable is the original's with the plugin's delete in slot 0")
            check(events.count("object init") == 1 and ("delete", resources[1]) in events,
                  "the original resource is released (its last reference: deleted)")
        check(plugin.said("Exit Game tile added to the pause menu's Home tab"), "the log says the tile was added")
        # Another page passes through untouched.
        resources[2] = make_resource(keep, b'<Page x:Class="ForzaUI.A_VideoOptions"/>')
        out = ctypes.c_void_p()
        events.clear()
        call(2, ctypes.byref(out))
        check(out.value == resources[2] and "allocate" not in events, "another page passes through untouched")
        # A page that already has the tile (loaded through a second chain) is kept.
        resources[3] = make_resource(keep, extend(dll, page_xaml))
        out = ctypes.c_void_p()
        events.clear()
        call(3, ctypes.byref(out))
        check(out.value == resources[3] and events.count("free") == 1, "a page that has the tile is kept as it is")

        # The pause page init: the previous init, then the tile is bound.
        init = PAGE_FN(slot)
        window = Window()
        try:
            events.clear()
            init(0x5150)
            check(events[:1] == [("extra options init", 0x5150)], "the previous pause page init runs first")
            binds = [e for e in events if e[0] == "bind"]
            check(len(binds) == 1 and binds[0][1] == 0x5150 and binds[0][2] == b"PART_Button_EOExitGame"
                  and binds[0][3][0] == dll.ExitGameTestHandlers(2) and binds[0][3][1] == 0,
                  "then PART_Button_EOExitGame is bound to the plugin's handler (adjust 0)")
            if binds:
                handler = PAGE_FN(binds[0][3][0])
                window.messages()
                handler(0x5150)
                seen = window.messages()
                check(0x10 in seen, f"choosing the tile posts WM_CLOSE to the Azimuth.Game.Win32 window ({seen})")
                check(plugin.said("Exit Game chosen in the pause menu; closing the game window"), "and logs it")
                window.close()
                window.hwnd = None
                handler(0x5150)
                check(plugin.said("was not found; the game keeps running"), "without that window it only logs")
        finally:
            window.close()
        # A pause page that does not match: no tile, and no binding afterwards.
        resources[4] = make_resource(keep, page_xaml.replace(b'x:Name="PART_Button_HorizonSolo"', b'x:Name="X"'))
        out = ctypes.c_void_p()
        call(4, ctypes.byref(out))
        check(out.value == resources[4] and plugin.said("the pause menu is not as expected"),
              "a pause page that does not match is kept as it is (logged)")
        events.clear()
        init(0x5151)
        check(not [e for e in events if e[0] == "bind"], "and the init binds nothing for it")
        unmap(base)

    def unchanged(label, path, enabled=1, adapter=1, damage=None, expect=""):
        base = pe.map()
        if damage:
            poke(base + damage[0], damage[1])
        before = (peek(base + PAUSE_INIT_SLOT, 8), peek(base + RESOURCE_CALL, 5))
        plugin = Plugin(path, work / label.replace(" ", "-"), enabled)
        ready = plugin.initialize(base, adapter)
        after = (peek(base + PAUSE_INIT_SLOT, 8), peek(base + RESOURCE_CALL, 5))
        check(ready == 1 and before == after and plugin.said(expect),
              f"{label}: the game is left as it is ({plugin.lines[-1].strip() if plugin.lines else 'no log'})")
        unmap(base)

    unchanged("Enabled=0", shipping_path, enabled=0, expect="[ExitGame] Enabled=0; nothing is installed")
    unchanged("not Azimuth", shipping_path, adapter=2, expect="does not run on Azimuth")
    unchanged("changed binder", shipping_path, damage=(BIND_BUTTON, b"\xcc"),
              expect="tile binder at 0xd9f9f0 is not as expected; nothing is installed")

    # --- the shipping DLL on the stock game: both sites chained.
    base = pe.map()
    plugin = Plugin(shipping_path, work / "shipping")
    ready = plugin.initialize(base)
    module = kernel32.GetModuleHandleW(str(shipping_path))
    slot = struct.unpack("<Q", peek(base + PAUSE_INIT_SLOT, 8))[0]
    size = PE(shipping_path).image_size
    target = call_target(base, RESOURCE_CALL)
    check(ready == 1 and module and module <= slot < module + size, "shipping DLL: the init slot holds its init")
    check(target is not None and peek(target, 2) == b"\xff\x25" and
          module <= struct.unpack("<Q", peek(target + 6, 8))[0] < module + size,
          "shipping DLL: the resource call goes to its reader")
    check(plugin.said("chained onto the game's pause page init 0xdc6e30") and
          plugin.said("chained onto the game's resource reader 0xaabec0"), "shipping DLL: the log names the game's code")
    unmap(base)
    bad = Context(16, 1, 1, IMAGE_SIZE, None, None, None, None, None, LOG(0))
    check(plugin.dll.FH2PluginInitialize(ctypes.byref(bad)) == 0, "a short context is refused (FALSE)")
    shutil.rmtree(work, ignore_errors=True)


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    parser.add_argument("--zig", type=Path, default=Path(shutil.which("zig") or "zig"))
    parser.add_argument("--game", type=Path, default=Path.home() / "FH2")
    parser.add_argument("--azimuth", type=Path)
    parser.add_argument("--upstream", type=Path)
    parser.add_argument("--release", type=Path)
    options = parser.parse_args()
    output = options.output.resolve()
    game = options.game
    (output / "test-runs").mkdir(parents=True, exist_ok=True)
    test_package(output)
    test_azimuth(options.azimuth or game / "azimuth-windows-1.0.4.zip", output / "test-runs")
    test_release(options.release or game / "src/eo-pr/upstream-0.4.4/FH2_Extra_Options_0.4.4-azimuth/Azimuth")
    dll = load_test_dll(output)
    pe = test_game(dll, game / GAME_EXE)
    native, eo_page = test_xaml(dll, options.zig, game, options.upstream or game / "src/eo-pr/wt-upstream", output)
    test_captions(dll, native)
    test_plugin(output, pe, native, eo_page)
    print(f"{len(failures)} failure(s)" if failures else "all tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
