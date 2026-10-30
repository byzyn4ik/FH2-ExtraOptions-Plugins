"""Offline tests of the Keyboard & Mouse plugin (no game run, nothing installed).

Usage: py tests/run_tests.py [--output out] [--zig zig.exe] [--game <FH2 folder>]
                             [--azimuth <azimuth-windows-1.0.4.zip>] [--azimuth102 <azimuth-windows-1.0.2.zip>]
                             [--upstream <Extra Options 0.4.4 checkout>] [--release <its release's Azimuth folder>]
                             [--graphics <Azimuth Graphics Options test DLL>]

Real files are only read: the game's executable and UI.zip, Azimuth's release
ZIPs, Extra Options 0.4.4's release and source. Executables are mapped into
this process' own memory (relocated, RWX) and patched there; their code never
runs. Defaults: --game ~/FH2, --azimuth <game>/azimuth-windows-1.0.4.zip,
--azimuth102 <game>/azimuth-windows-1.0.2.zip, --upstream <game>/src/eo-pr/wt-upstream,
--release <game>/src/eo-pr/upstream-0.4.4/FH2_Extra_Options_0.4.4-azimuth/Azimuth,
--graphics <output>/../azimuth-graphics-options/tests/AzimuthGraphicsOptionsTest.dll (the
top-level build.py's test build of that plugin).
A missing input skips the tests that need it.

 1. package   mod.json as Extra Options 0.4.4 validates it, the mod layout, the
              DLL (PE32+ x64, the one export, Windows imports only, version
              resource), the shipped INI (read by the plugin's own reader).
 2. ini       [KeyboardMouse] and [Controller] parsing (the PR's cases and
              more): defaults, options, key lists, refusals, groups.
 3. hosts     azimuth.exe 1.0.4 and 1.0.2: every host site found again from
              the code (patterns, capstone when present) equals the plugin's
              profile; the guards refuse other builds and changed code; the
              gamepad vibration slot is chained.
 4. release   Extra Options 0.4.4's winmm.dll and INI: what it patches that
              this plugin chains, its own keyboard bindings and the clash
              rules; its source's patch sites never touch a site of ours.
 5. game      the game executable: every guard passes, alone and as Extra
              Options patches it; changed bytes are refused with nothing
              patched; the input hooks, trampolines, vtable/call chains (onto
              the game and onto other modules), mouse pitch, controller pause.
 6. xaml      the real Settings hub and Video page (UI.zip): the tile, the
              KEYBOARD & MOUSE page from the game's page, from Extra Options
              0.4.4's page (its source, built here) and from Azimuth Graphics
              Options' page (its test DLL); readers outside ours leave it.
 7. texts     15 game languages (16 codes, GB = English): tile, page, rows.
 8. page      options and key rebinding round-trip through KeyboardMouse.ini
              (the PR's page test), Extra Options' keys refused.
 9. plugin    FH2PluginInitialize on the mapped game and Azimuth 1.0.4 behind
              simulated Extra Options hooks and another plugin: chains, the
              hub and page through the reader, properties, setters, reset;
              Enabled=0, a bad INI, another host and the shipping DLL.
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
MOD_NAME = "Keyboard & Mouse"
DLL_NAME = "KeyboardMouse.dll"
INI_NAME = "KeyboardMouse.ini"
TEST_DLL = "KeyboardMouseTest.dll"
SYSTEM_DLLS = re.compile(r"^(kernel32|user32|gdi32|msvcrt|api-ms-win-[a-z0-9-]+)\.dll$", re.I)
GAME_EXE = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/forza_durango_release_final.exe")
UI_ZIP = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/media/UI.zip")
TEXTS = json.loads((ROOT / "src" / "translations.json").read_text(encoding="utf-8"))
X = "{http://schemas.microsoft.com/winfx/2006/xaml}"
LOCAL = "{clr-namespace:ForzaUI;assembly=}"

IMAGE_SIZE = 0x3A18000
RESOURCE_CALL, GET_RESOURCE, EO_THUNK = 0x16EBB37, 0xAABEC0, 0x13FE354
RESET_SLOT, RESET_VIDEO = 0x2E60900, 0xEBF060
HUB_INIT_SLOT, HUB_INIT, BIND_BUTTON, VIDEO_TILE = 0x2E15940, 0xDC8A60, 0xD9FC10, 0xDCFCD0
TEXT_TABLES = 0x3662990
# Inline entry hooks (kbm_input.c), in the plugin's order: rva, bytes replaced.
ENTRIES = [(0x1075490, 14), (0x1072500, 16), (0x106F040, 15), (0x106E3E0, 15), (0x106E690, 15), (0x106D640, 15),
           (0x103F9D0, 14), (0x1104E70, 22), (0x154B9E0, 18), (0x14483D0, 17), (0x1105510, 15)]
DIGITAL_BODY = 0x1072510
# Vtable slots and call sites (KbmTestChain order): site, native, kind.
CHAINS = [(0x2ECFBF8, 0x142C5E0, "slot", "UserInterface::Update"), (0x2ECFD58, 0x1425060, "slot", "show popup"),
          (0x2E246C0, 0xCEC3D0, "slot", "PopupMessageBox OnKey"), (0xEA0F66, 0xE7EBC0, "call", "title sign-in"),
          (0x228BD4, 0xB620C0, "call", "disconnect raise 1"), (0x228C65, 0xB620C0, "call", "disconnect raise 2"),
          (0x2ECFC68, 0x14237E0, "slot", "UI set-controller"), (0x2DE3898, 0xCFAA10, "slot", "pairing update"),
          (0x2D16698, 0x8E98E0, "slot", "PauseGame enter"), (0x2D2C728, 0x957170, "slot", "WaitForButtonPressUnsafe"),
          (0x2D2C668, 0x957090, "slot", "WaitForButtonPress"), (0x142BFAA, 0x1408BE0, "call", "navigation vector")]
PITCH_SLOTS = [0x2E703B0, 0x2E70628, 0x2E708A0, 0x2E70B18, 0x2E77C30, 0x2E77EA8, 0x2E78120]  # + 0x260
PAUSE_CALL, PAUSE_FIRE = 0x9648FA, 0x1925260
# Every byte range the plugin may change, for the overlap check with Extra Options.
OUR_RANGES = ([(rva, size, "entry") for rva, size in ENTRIES] +
              [(site, 8 if kind == "slot" else 5, name) for site, _, kind, name in CHAINS] +
              [(v + 0x260, 8, "pitch slot") for v in PITCH_SLOTS] +
              [(PAUSE_CALL, 5, "controller pause call"), (HUB_INIT_SLOT, 8, "Settings hub init"),
               (RESOURCE_CALL, 5, "XAML resource call"), (RESET_SLOT, 8, "Video page reset")])
# Of those, the ones Extra Options 0.4.4 patches itself and the plugin chains.
SHARED = {0x2D16698: "autodrive_pause_entry", RESOURCE_CALL: "drift_video_resource_call",
          RESET_SLOT: "drift_video_reset_options"}
# Azimuth host profiles (kbm_win32.c): window, prompt, cursor, SetVibration slot, SetVibration,
# GetCurrentReading, GetRawCurrentReading.
HOSTS = {"1.0.4": (0x6AE393EB, 0x8A2000, (0x7FCB80, 0x84E80A, 0x7FCBB0, 0x6EDA00, 0x19A500, 0x19A5C0, 0x19A690)),
         "1.0.2": (0x6AE05DD1, 0x9AF000, (0x909D20, 0x95B97A, 0x909D50, 0x801930, 0x2A0590, 0x2A0650, 0x2A0720))}
DEFAULT_KEYS = {"Throttle": "W", "Brake": "S", "SteerLeft": "A", "SteerRight": "D", "Handbrake": "Space",
                "ShiftUp": "E", "ShiftDown": "Q", "Clutch": "LShift", "Camera": "Tab", "Rewind": "R",
                "LookBack": "Down,B", "Activate": "Enter,F,X", "PhotoMode": "P", "RadioPrevious": "Left",
                "RadioNext": "Right", "Map": "M", "Pause": "None", "Confirm": "Enter", "Back": "None", "Up": "Up",
                "Down": "Down", "Left": "Left", "Right": "Right", "TabLeft": "Q", "TabRight": "E", "MenuX": "X",
                "MenuY": "Y", "MenuLeftTrigger": "PageUp", "MenuRightTrigger": "PageDown", "DPadUp": "Numpad8",
                "DPadDown": "Numpad2", "DPadLeft": "Numpad4", "DPadRight": "Numpad6"}
BINDINGS = list(DEFAULT_KEYS)
SCAN = {"W": 0x11, "S": 0x1F, "A": 0x1E, "D": 0x20, "Space": 0x39, "E": 0x12, "Q": 0x10, "LShift": 0x2A, "Tab": 0x0F,
        "R": 0x13, "Down": 0xD0, "B": 0x30, "Enter": 0x1C, "F": 0x21, "X": 0x2D, "P": 0x19, "Left": 0xCB,
        "Right": 0xCD, "M": 0x32, "Up": 0xC8, "Y": 0x15, "PageUp": 0xC9, "PageDown": 0xD1, "Numpad8": 0x48,
        "Numpad2": 0x50, "Numpad4": 0x4B, "Numpad6": 0x4D, "F1": 0x3B, "F3": 0x3D, "F4": 0x3E, "I": 0x17}

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
        self.nt = e
        self.machine, sections, self.stamp, _, _, optional_size, self.characteristics = \
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
        self._relocations = None
        self._image = None

    def offset(self, rva):
        for _, va, vsize, rsize, raw in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError(hex(rva))

    def read(self, rva, size):
        return bytes(self.image()[rva:rva + size])

    def u64(self, rva):
        return struct.unpack_from("<Q", self.image(), rva)[0]

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
        if self._relocations is None:
            o = self.offset(self.relocations_rva)
            table = self.data[o:o + self.relocations_size]
            found, at = [], 0
            while at + 8 <= len(table):
                page, size = struct.unpack_from("<II", table, at)
                if size < 8:
                    break
                for (entry,) in struct.iter_unpack("<H", table[at + 8:at + size]):
                    if entry >> 12 == 10:
                        found.append(page + (entry & 0xFFF))
                at += size
            self._relocations = found
        return self._relocations

    def image(self):
        """The image laid out as the loader maps it (not relocated)."""
        if self._image is None:
            image = bytearray(self.image_size)
            image[:self.headers_size] = self.data[:self.headers_size]
            for _, va, vsize, rsize, raw in self.sections:
                size = min(rsize, vsize) if vsize else rsize
                image[va:va + size] = self.data[raw:raw + size]
            self._image = image
        return self._image

    def map(self):
        """The image laid out and relocated in private RWX memory; returns its address."""
        base = kernel32.VirtualAlloc(None, self.image_size, 0x3000, 0x40)
        if not base:
            raise MemoryError("VirtualAlloc")
        image = bytearray(self.image())
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


def qword(address):
    return struct.unpack("<Q", peek(address, 8))[0]


def call_target(base, rva):
    code = peek(base + rva, 5)
    return base + rva + 5 + struct.unpack_from("<i", code, 1)[0] if code[0] == 0xE8 else None


def jump_target(address):
    """The target of `jmp qword [rip+0]; dq target` at address, or None."""
    code = peek(address, 14)
    return struct.unpack_from("<Q", code, 6)[0] if code[:6] == b"\xff\x25\0\0\0\0" else None


def near(base, size=0x1000):
    """Executable memory within 2 GB of the mapped image (a stand-in module's jump)."""
    for k in range(1, 0x8000):
        at = kernel32.VirtualAlloc(base + IMAGE_SIZE + k * 0x10000, size, 0x3000, 0x40)
        if at:
            return at
    raise MemoryError("no memory near the image")


# ------------------------------------------------------------------- test DLL

def fresh_dll(output, tag):
    """A private copy of the test DLL (its own statics): each scenario installs once."""
    folder = output / "test-runs" / "dll"
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"KeyboardMouseTest-{tag}.dll"
    shutil.copy2(output / "tests" / TEST_DLL, path)
    dll = ctypes.CDLL(str(path))
    signatures = {
        "KbmTestParse": ([ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int),
        "KbmTestReadSettings": ([ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int), ctypes.c_char_p, ctypes.c_size_t,
                                 ctypes.c_void_p], ctypes.c_int),
        "KbmTestHostSites": ([ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)], ctypes.c_int),
        "KbmTestGateRumble": ([ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)], ctypes.c_void_p),
        "KbmTestEoConflict": ([ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_uint], ctypes.c_char_p),
        "KbmTestPageCheck": ([ctypes.c_void_p, ctypes.c_int], ctypes.c_char_p),
        "KbmTestKeyboardXaml": ([ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_size_t),
        "KbmTestHubXaml": ([ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t], ctypes.c_size_t),
        "KbmTestInstall": ([ctypes.c_void_p, ctypes.c_void_p], ctypes.c_int),
        "KbmTestOriginal": ([ctypes.c_uint], ctypes.c_void_p),
        "KbmTestHook": ([ctypes.c_uint], ctypes.c_void_p),
        "KbmTestChain": ([ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p)], ctypes.c_void_p),
        "KbmTestBinding": ([ctypes.c_uint, ctypes.c_uint], ctypes.c_uint),
        "KbmTestPitchPatch": ([ctypes.c_void_p], ctypes.c_int),
        "KbmTestPitchHooks": ([ctypes.c_uint], ctypes.c_void_p),
        "KbmTestPitchStep": ([ctypes.c_int, ctypes.c_int, ctypes.c_long, ctypes.c_int, ctypes.c_uint], ctypes.c_float),
        "KbmTestPauseHook": ([ctypes.POINTER(ctypes.c_void_p)], ctypes.c_void_p),
        "KbmTestMenuGet": ([ctypes.c_uint], ctypes.c_int),
        "KbmTestMenuSet": ([ctypes.c_uint, ctypes.c_int], ctypes.c_int),
        "KbmTestMenuChoice": ([ctypes.c_uint, ctypes.c_uint], ctypes.c_wchar_p),
        "KbmTestMenuNatives": ([ctypes.c_void_p, ctypes.c_void_p], None),
        "KbmTestHubHandlers": ([ctypes.c_int], ctypes.c_void_p),
        "KbmTestRebindCapture": ([ctypes.c_uint, ctypes.c_uint], ctypes.c_int),
        "KbmTestRebindKeys": ([ctypes.c_uint], ctypes.c_uint),
        "KbmTestRebindChoice": ([ctypes.c_uint, ctypes.c_uint], ctypes.c_wchar_p),
        "KbmTestRebindGet": ([ctypes.c_uint], ctypes.c_int),
        "KbmTestLanguage": ([ctypes.c_uint], ctypes.c_char_p),
        "KbmTestTitle": ([ctypes.c_void_p], ctypes.c_char_p),
        "KbmTestPageNatives": ([ctypes.c_void_p], None),
        "KbmTestPageHandlers": ([ctypes.c_int], ctypes.c_void_p),
        "KbmTestPageProperty": ([ctypes.c_uint, ctypes.c_int], ctypes.c_void_p),
        "KbmTestPagePrevious": ([ctypes.c_int], ctypes.c_void_p),
        "KbmTestUseHost": ([ctypes.c_void_p], None),
        "KbmTestNoThread": ([ctypes.c_int], None),
        "KbmTestKeycap": ([ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_void_p], ctypes.c_int),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(dll, name)
        function.argtypes = arguments
        function.restype = result
    dll.KbmTestLanguages.restype = ctypes.c_uint
    dll.KbmTestTextComplete.restype = ctypes.c_int
    dll.KbmTestMenuInstalled.restype = ctypes.c_int
    dll.KbmTestInstalled.restype = ctypes.c_int
    dll.KbmTestRebindReset.restype = None
    dll.KbmTestRebindReload.restype = None
    dll.KbmTestSetIni.argtypes = [ctypes.c_wchar_p]
    dll.KbmTestSetIni.restype = None
    dll.KbmTestSetLog.argtypes = [ctypes.c_void_p]
    dll.KbmTestSetLog.restype = None
    dll.KbmTestPauseInstall.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
    dll.KbmTestPauseInstall.restype = ctypes.c_int
    dll.path = path
    return dll


class KbmSettings(ctypes.Structure):
    _fields_ = [(n, ctypes.c_int) for n in ("enabled", "mode", "mouse_look", "mouse_capture", "mouse_sensitivity",
                                            "mouse_return_delay", "mouse_return_speed", "steering_speed",
                                            "invert_steering", "trace", "escape", "menu_cursor", "cursor_size",
                                            "menu_arrows_dpad", "map_cursor_speed", "mouse_look_pitch",
                                            "invert_mouse_y")] + [("keys", (ctypes.c_ushort * 3) * 33)]


def read_settings(dll, work, text):
    ini = work / "settings.ini"
    ini.write_text(text, encoding="utf-8")
    pause = ctypes.c_int()
    problem = ctypes.create_string_buffer(512)
    settings = KbmSettings()
    enabled = dll.KbmTestReadSettings(str(ini), ctypes.byref(pause), problem, len(problem), ctypes.byref(settings))
    return enabled, pause.value, problem.value.decode(), settings


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


def test_package(output, dll):
    print("package")
    mod = output / "mods" / MOD_NAME
    manifest = json.loads((mod / "mod.json").read_text(encoding="utf-8"))
    check(manifest.get("format_version") == 1 and manifest.get("id") == "keyboard-mouse" and
          valid_mod_id(manifest.get("id")) and manifest.get("name") == MOD_NAME,
          f"mod.json: format_version 1, id keyboard-mouse, name {MOD_NAME}")
    check(isinstance(manifest.get("dependencies"), list) and "media" not in manifest and "database" not in manifest,
          "mod.json: dependencies list, no media or database")
    files = sorted(str(p.relative_to(mod)).replace("\\", "/") for p in mod.rglob("*") if p.is_file())
    check(files == ["mod.json", "plugins/" + DLL_NAME, "plugins/" + INI_NAME],
          f"out/mods/{MOD_NAME}: mod.json, plugins/{DLL_NAME}, plugins/{INI_NAME} ({files})")
    pe = PE(mod / "plugins" / DLL_NAME)
    check(pe.machine == 0x8664 and pe.magic == 0x20B and pe.characteristics & 0x2000, "the DLL is a PE32+ x64 DLL")
    check(pe.exports() == ["FH2PluginInitialize"], f"the one export is FH2PluginInitialize ({pe.exports()})")
    imports = list(pe.imports())
    check(all(SYSTEM_DLLS.match(name) for name in imports), f"only Windows DLLs are imported ({', '.join(imports)})")
    version = manifest.get("version", "")
    check(bool(pe.resources_rva) and ("FileVersion\0" + version).encode("utf-16le") in pe.data.replace(b"\0\0\0", b"\0"),
          f"version resource with FileVersion {version}")
    test_pe = PE(output / "tests" / TEST_DLL)
    check("FH2PluginInitialize" in test_pe.exports() and "KbmTestInstall" in test_pe.exports(),
          "the test DLL has the plugin entry and the test entry points")
    ini = configparser.ConfigParser(strict=True, inline_comment_prefixes=(";",))
    ini.optionxform = str
    ini.read(mod / "plugins" / INI_NAME, encoding="utf-8")
    check(ini.get("KeyboardMouse", "Enabled", fallback=None) == "1" and
          ini.get("KeyboardMouse", "Mode", fallback=None) == "Auto" and
          ini.get("Controller", "PauseOnlyOnUnplug", fallback=None) == "1",
          "KeyboardMouse.ini: [KeyboardMouse] Enabled=1, Mode=Auto; [Controller] PauseOnlyOnUnplug=1")
    text = (mod / "plugins" / INI_NAME).read_text(encoding="utf-8")
    commented = dict(re.findall(r"^;(\w+)=(.+)$", text, re.M))
    check({k: v for k, v in commented.items() if k in DEFAULT_KEYS} == DEFAULT_KEYS,
          "the INI lists every action with its default key (commented out)")
    work = Path(tempfile.mkdtemp(prefix="ini-", dir=output / "test-runs"))
    enabled, pause, problem, settings = read_settings(dll, work, text)
    check(enabled == 1 and pause == 1 and not problem, f"the plugin reads the shipped INI: on, pause fix on ({problem})")
    uncommented = re.sub(r"^;(\w+=)", r"\1", text, flags=re.M)
    enabled, _, problem, _ = read_settings(dll, work, uncommented)
    check(enabled == 1, f"the INI with every key line uncommented is accepted ({problem})")
    shutil.rmtree(work, ignore_errors=True)


# ------------------------------------------------------------------- 2. ini

def test_ini(output, dll):
    print("ini")
    work = Path(tempfile.mkdtemp(prefix="ini-", dir=output / "test-runs"))
    cases = [
        ("no file content (defaults)", "", 1, 1),
        ("no section", "[Other]\nEnabled=0\n", 1, 1),
        ("off", "[KeyboardMouse]\nEnabled=0\n", 0, 1),
        ("on with options (PR case)", "[KeyboardMouse]\nEnabled=1\nMode=Auto\nMouseLook=1\nMouseSensitivity=80\n"
                                      "Throttle=W\nBrake=S\n", 1, 1),
        ("bad value", "[KeyboardMouse]\nEnabled=2\n", -1, 1),
        ("unknown key", "[KeyboardMouse]\nNoSuchKey=1\n", -1, 1),
        ("two driving actions on one key", "[KeyboardMouse]\nEnabled=1\nThrottle=W\nBrake=W\n", -1, 1),
        ("a driving and a menu action may share a key", "[KeyboardMouse]\nCamera=Tab\nTabLeft=Tab\n", 1, 1),
        ("two menu actions on one key", "[KeyboardMouse]\nMenuX=Y\n", -1, 1),
        ("number out of range", "[KeyboardMouse]\nMouseSensitivity=5000\n", -1, 1),
        ("not a number", "[KeyboardMouse]\nCursorSize=big\n", -1, 1),
        ("mode names, any case", "[KeyboardMouse]\nMode=kbm\nMenuArrows=dpad\n", 1, 1),
        ("bad mode", "[KeyboardMouse]\nMode=Mouse\n", -1, 1),
        ("inline comments", "[KeyboardMouse]\nMouseSensitivity=90 ; slower\nThrottle=W # main\n", 1, 1),
        ("four keys", "[KeyboardMouse]\nThrottle=W,Up,I,K\n", -1, 1),
        ("a key twice in one action", "[KeyboardMouse]\nThrottle=W,W\n", -1, 1),
        ("unknown key name", "[KeyboardMouse]\nThrottle=Esc\n", -1, 1),
        ("F2 is Extra Options' Autodrive", "[KeyboardMouse]\nThrottle=F2\n", -1, 1),
        ("pause fix off", "[Controller]\nPauseOnlyOnUnplug=0\n", 1, 0),
        ("pause fix bad value", "[Controller]\nPauseOnlyOnUnplug=2\n", 1, -1),
        ("pause fix unknown key", "[Controller]\nPauseOnlyOnControllerUnplug=1\n", 1, -1),
    ]
    for name, text, enabled, pause in cases:
        got_enabled, got_pause, problem, _ = read_settings(dll, work, text)
        check((got_enabled, got_pause) == (enabled, pause),
              f"{name}: keyboard & mouse {got_enabled}, pause fix {got_pause}" + (f" ({problem})" if problem else ""))
    _, _, problem, _ = read_settings(dll, work, "[KeyboardMouse]\nThrottle=W\nBrake=W\n")
    check("two actions of the same group" in problem, f"the refusal says why ({problem})")
    _, _, problem, _ = read_settings(dll, work, "[KeyboardMouse]\nMouseSensitivity=5000\n")
    check("MouseSensitivity=5000" in problem, f"the refusal names the line ({problem})")
    _, _, _, s = read_settings(dll, work, "[KeyboardMouse]\nThrottle=W,Up\nBrake=none\nSteerLeft=a\nMouseSensitivity=90 ; x\n")
    check(list(s.keys[0]) == [0x11, 0xC8, 0] and list(s.keys[1]) == [0, 0, 0] and list(s.keys[2]) == [0x1E, 0, 0] and
          s.mouse_sensitivity == 90, "key lists parse to physical scan codes (W,Up / None / lower case)")
    _, _, _, s = read_settings(dll, work, "")
    defaults = [list(s.keys[i]) for i in range(33)]
    expect = [[SCAN[k] for k in v.split(",")] + [0] * (3 - len(v.split(","))) if v != "None" else [0, 0, 0]
              for v in DEFAULT_KEYS.values()]
    check(defaults == expect, "the defaults are the PC Forza keys of the PR (WASD, Space, E/Q, Tab, R, M, P, ...)")
    check((s.enabled, s.mode, s.mouse_look, s.mouse_sensitivity, s.steering_speed, s.escape, s.menu_cursor,
           s.cursor_size, s.map_cursor_speed, s.mouse_look_pitch) == (1, 0, 1, 80, 100, 1, 1, 250, 400, 0),
          "option defaults: on, Auto, mouse look, sensitivity 80, steering 100, Esc, pointer 250 %, map 400 %")
    check(dll.KbmTestParse(b"Throttle", b"W") == 1 and dll.KbmTestParse(b"Brake", b"W") == 0 and
          dll.KbmTestParse(b"MenuArrows", b"Stick") == 1, "single key=value checks (KbmTestParse)")
    shutil.rmtree(work, ignore_errors=True)


# ------------------------------------------------------------------ 3. hosts

def find_host_sites(pe):
    """The host sites from the code itself, independent of the plugin's table."""
    _, text_va, text_size, _, _ = pe.section(".text")
    code = pe.read(text_va, text_size)
    iat = {slot: name for functions in pe.imports().values() for slot, name in functions.items()}

    def rip(at, length):  # target of the rip-relative operand ending the instruction at code offset `at`
        return text_va + at + length + struct.unpack_from("<i", code, at + length - 4)[0]

    found = {}
    # PostMessageW(window, WM_CLOSE, 0, 0): sub rsp,28; mov rcx,[window]; xor r9d,r9d; xor r8d,r8d; mov edx,10h
    hits = [m.start() for m in re.finditer(rb"\x48\x83\xec\x28\x48\x8b\x0d....\x45\x33\xc9\x45\x33\xc0"
                                           rb"\xba\x10\x00\x00\x00\x90?\xff\x15", code, re.S)]
    if len(hits) == 1:
        at = hits[0] + 4
        call = hits[0] + (23 if code[hits[0] + 22] == 0x90 else 22)
        found["window"] = (text_va + at, rip(at, 7), iat.get(rip(call, 6)))
    # The gamepad reading routine (called by GetRawCurrentReading) checks the prompt flag.
    hits = [m.start() for m in re.finditer(rb"\x48\x8b\x51\x20\x48\x8d\x4c\x24\x20\xe8....\x0f\x10\x08", code, re.S)]
    if len(hits) == 1:
        reading = rip(hits[0] + 9, 5)
        found["reading"] = reading
        found["raw"] = text_va + hits[0] - 0x19
    hits = [m.start() for m in re.finditer(rb"\x48\x3b\xc1\x75.\x80\x3d....\x00\x75.\x48\x8b\x5b\x08", code, re.S)]
    if len(hits) == 1:
        at = hits[0] + 5
        found["prompt"] = (text_va + at, rip(at, 6) + 1)
    # WM_SETCURSOR: cmp si,1; jne; cmp [cursor],r14b; jne; cmp [hide],r14b; ... LoadCursorW(0, IDC_ARROW)
    hits = [m.start() for m in re.finditer(rb"\x66\x83\xfe\x01\x0f\x85....\x44\x38\x35....\x75.\x44\x38\x35", code, re.S)]
    if len(hits) == 1:
        at = hits[0] + 10
        tail = code[at:at + 0x40]
        arrow = tail.find(b"\xba\x00\x7f\x00\x00")
        load = iat.get(rip(at + arrow + 7, 6)) if arrow >= 0 and tail[arrow + 7:arrow + 9] == b"\xff\x15" else None
        found["cursor"] = (text_va + at, rip(at, 7), load)
    # The window global is the HWND: IsWindow(window) / DestroyWindow(window) / xchg [window],rsi.
    if "window" in found:
        window = found["window"][1]
        uses = []
        for m in re.finditer(rb"\x48\x8b\x0d....\x90\xff\x15", code, re.S):
            if rip(m.start(), 7) == window:
                uses.append(iat.get(rip(m.start() + 8, 6)))
        found["window_calls"] = sorted(set(u for u in uses if u))
    # IGamepad vtable: the slot holding GetRawCurrentReading; SetVibration two slots before.
    if "raw" in found:
        target = struct.pack("<Q", pe.image_base + found["raw"])
        _, rdata_va, rdata_size, _, _ = pe.section(".rdata")
        rdata = pe.read(rdata_va, rdata_size)
        slots = [rdata_va + m.start() for m in re.finditer(re.escape(target), rdata) if m.start() % 8 == 0]
        if len(slots) == 1:
            slot = slots[0] - 16
            found["vtable"] = (slot, pe.u64(slot) - pe.image_base, pe.u64(slot + 8) - pe.image_base, found["raw"])
    return found


def capstone_view(pe, rva, count):
    try:
        import capstone
    except ImportError:
        return None
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    return [f"{i.mnemonic} {i.op_str}" for i in md.disasm(pe.read(rva, 16 * count), rva)][:count]


def test_hosts(output, dll, zips):
    print("hosts")
    out = (ctypes.c_uint * 7)()
    reserved = kernel32.VirtualAlloc(None, 0x10000, 0x2000, 0x04)  # reserved, not committed: reading faults
    check(reserved and dll.KbmTestHostSites(reserved, out) == 0,
          "an image that cannot be read is refused (the fault is caught, nothing crashes)")
    kernel32.VirtualFree(reserved, 0, 0x8000)
    executables = {}
    for version, path in zips.items():
        if not path or not path.is_file():
            skip(f"Azimuth {version} ZIP not found ({path})")
            continue
        with zipfile.ZipFile(path) as archive:
            executables[version] = PE(data=archive.read("azimuth.exe"))
    for version, pe in executables.items():
        stamp, size, profile = HOSTS[version]
        window, prompt, cursor, slot, vibration, reading_get, raw_get = profile
        check(pe.stamp == stamp and pe.image_size == size,
              f"{version}: azimuth.exe TimeDateStamp 0x{pe.stamp:08x}, SizeOfImage 0x{pe.image_size:x}")
        found = find_host_sites(pe)
        check(found.get("window", (0, 0, 0))[1:] == (window, "PostMessageW"),
              f"{version}: the window global 0x{window:x} is what PostMessageW(window, WM_CLOSE) posts to "
              f"(use 0x{found.get('window', (0,))[0]:x})")
        check(set(found.get("window_calls", [])) >= {"IsWindow", "DestroyWindow"},
              f"{version}: the game window is checked and destroyed through it ({found.get('window_calls')})")
        check(found.get("prompt", (0, 0))[1] == prompt and
              0 < found["prompt"][0] - found.get("reading", 0) < 0x200,
              f"{version}: the prompt flag 0x{prompt:x} is checked in the gamepad reading routine "
              f"0x{found.get('reading', 0):x} (use 0x{found.get('prompt', (0,))[0]:x})")
        check(found.get("cursor", (0, 0, 0))[1:] == (cursor, "LoadCursorW"),
              f"{version}: the cursor flag 0x{cursor:x} decides WM_SETCURSOR's arrow (LoadCursorW(0, IDC_ARROW))")
        check(found.get("vtable") == (slot, vibration, reading_get, raw_get),
              f"{version}: IGamepad vtable slot 0x{slot:x}: SetVibration 0x{vibration:x}, GetCurrentReading "
              f"0x{reading_get:x}, GetRawCurrentReading 0x{raw_get:x} (calls the reading routine)")
        view = capstone_view(pe, vibration, 16)
        if view is None:
            skip(f"{version}: capstone not installed (instruction view)")
        else:
            check(any(v.startswith("movups xmm0, xmmword ptr [rdx]") for v in view),
                  f"{version}: SetVibration reads the four motor levels from its argument (capstone: "
                  f"{next((v for v in view if 'rdx]' in v), '?')})")
            use = capstone_view(pe, found["window"][0], 1)
            check(use and use[0].startswith("mov rcx, qword ptr [rip"), f"{version}: capstone at the window use: {use}")
        base = pe.map()
        try:
            out = (ctypes.c_uint * 7)()
            build = dll.KbmTestHostSites(base, out)
            check(build == (2 if version == "1.0.4" else 1) and tuple(out) == profile,
                  f"{version}: the plugin picks its {version} profile and every global checks out")
            nt = base + pe.nt
            original = peek(nt + 8, 4)
            poke(nt + 8, struct.pack("<I", stamp ^ 1))
            check(dll.KbmTestHostSites(base, out) == 0, f"{version}: another TimeDateStamp is refused")
            poke(nt + 8, original)
            size_at = nt + 24 + 56
            poke(size_at, struct.pack("<I", size + 0x10000))
            check(dll.KbmTestHostSites(base, out) == (2 if version == "1.0.4" else 1),
                  f"{version}: a branded copy (a larger SizeOfImage) is accepted")
            poke(size_at, struct.pack("<I", size - 0x1000))
            check(dll.KbmTestHostSites(base, out) == 0, f"{version}: a smaller SizeOfImage is refused")
            poke(size_at, struct.pack("<I", size))
            for name, at, what in (("window use", found["window"][0], b"\x90"), ("prompt use", found["prompt"][0], b"\x90"),
                                   ("cursor use", found["cursor"][0] + 3, b"\x00")):
                old = peek(base + at, 1)
                poke(base + at, what)
                check(dll.KbmTestHostSites(base, out) == 0, f"{version}: a changed {name} is refused")
                poke(base + at, old)
            # Gamepad vibration: the slot is chained (onto the game's function, then onto another module's hook).
            previous = ctypes.c_void_p()
            muted = MUTED(lambda: 1)
            hook = dll.KbmTestGateRumble(base, ctypes.cast(muted, ctypes.c_void_p), ctypes.byref(previous))
            check(hook and qword(base + slot) == hook and previous.value == base + vibration,
                  f"{version}: SetVibration slot chained onto Azimuth's own function")
            other = SETVIB(lambda gamepad, levels: 0)
            poke(base + slot, struct.pack("<Q", ctypes.cast(other, ctypes.c_void_p).value))
            hook = dll.KbmTestGateRumble(base, ctypes.cast(muted, ctypes.c_void_p), ctypes.byref(previous))
            check(hook and previous.value == ctypes.cast(other, ctypes.c_void_p).value,
                  f"{version}: ... and onto another module's hook already there")
            poke(base + slot + 8, b"\0" * 8)
            check(not dll.KbmTestGateRumble(base, ctypes.cast(muted, ctypes.c_void_p), ctypes.byref(previous)),
                  f"{version}: a vtable without GetCurrentReading next to it is left alone")
        finally:
            unmap(base)
    return executables


MUTED = ctypes.CFUNCTYPE(ctypes.c_int)
SETVIB = ctypes.CFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p)


# ---------------------------------------------------------------- 4. release

def eo_source_constants(upstream):
    """Every hex constant in the game image range in Extra Options' C sources: {value: [files]}."""
    found = {}
    for path in list(upstream.glob("*.c")) + list((upstream / "features").glob("*.c")) + \
            list((upstream / "adapters").glob("*.c")) + list(upstream.glob("*.h")) + list((upstream / "features").glob("*.h")):
        for m in re.finditer(r"0x([0-9a-fA-F]{5,8})\b", path.read_text(encoding="utf-8", errors="replace")):
            value = int(m.group(1), 16)
            if 0x1000 <= value < IMAGE_SIZE:
                found.setdefault(value, set()).add(path.name)
    return found


def test_release(dll, release, upstream, output):
    print("release")
    winmm = release / "winmm.dll" if release else None
    if not winmm or not winmm.is_file():
        skip(f"Extra Options 0.4.4 release not found ({release})")
    else:
        data = winmm.read_bytes()
        check(b"0.4.4" in data and b"FH2PluginInitialize" in data, "winmm.dll is Extra Options 0.4.4 with its plugin loader")
        call = b"\xe8" + struct.pack("<i", GET_RESOURCE - (RESOURCE_CALL + 5))
        thunk_call = b"\xe8" + struct.pack("<i", EO_THUNK - (RESOURCE_CALL + 5))
        check(call + thunk_call in data, "it points the XAML resource call 0x16ebb37 at its thunk 0x13fe354 (chained)")
        check(b"drift_video_reset_options" in data and b"autodrive_pause_entry" in data,
              "it hooks the Video page reset slot and PauseGame enter (both chained)")
        check(b"<!-- EO:StandaloneOptionsPage -->" in data,
              "it leaves pages marked EO:StandaloneOptionsPage alone (the KEYBOARD & MOUSE page is marked)")
        check(b"A_PauseOptions" not in data and b"PART_Button_Controls" not in data and b"KbmMode" not in data,
              "it does not edit the Settings hub nor use the page's property names")
        check(b"RegisterRawInputDevices" not in data and b"GetAsyncKeyState" in data,
              "it polls keys with GetAsyncKeyState and owns no Raw Input device (ours registers its own)")
        eo_ini = release / "FH2_ExtraOptions.ini"
        ini = configparser.ConfigParser(strict=False, inline_comment_prefixes=(";",))
        ini.optionxform = str
        ini.read(eo_ini, encoding="utf-8")
        keyboard = {k[:-8]: v for k, v in ini["Bindings"].items() if k.endswith("Keyboard")} if ini.has_section("Bindings") else {}
        check(keyboard.get("Autodrive") == "F2" and keyboard.get("ToggleUI") == "F1" and
              keyboard.get("Headlights") == "F3" and keyboard.get("Roof") == "F4" and
              keyboard.get("JumpToManufacturer") == "Tab", f"its INI's keyboard bindings: F1 F2 F3 F4 Tab ({keyboard})")
        clashes = [(b, k) for b, keys in DEFAULT_KEYS.items() if keys != "None" for k in keys.split(",")
                   if dll.KbmTestEoConflict(str(eo_ini), BINDINGS.index(b), SCAN[k])]
        check(not clashes, f"no default key of ours does what one of its bindings does ({clashes})")
        check(dll.KbmTestEoConflict(str(eo_ini), BINDINGS.index("Throttle"), SCAN["F3"]) == b"Headlights",
              "Throttle=F3 would clash with its Headlights (driving)")
        check(dll.KbmTestEoConflict(str(eo_ini), BINDINGS.index("MenuX"), SCAN["Tab"]) == b"JumpToManufacturer",
              "MenuX=Tab would clash with its JumpToManufacturer (menus)")
        check(dll.KbmTestEoConflict(str(eo_ini), BINDINGS.index("Camera"), SCAN["Tab"]) is None,
              "Camera=Tab (driving) and its Tab (car lists) do not clash")
        check(dll.KbmTestEoConflict(str(eo_ini), BINDINGS.index("Map"), SCAN["F1"]) == b"ToggleUI" and
              dll.KbmTestEoConflict(str(eo_ini), BINDINGS.index("Rewind"), SCAN["F1"]) == b"ToggleUI",
              "F1 (its ToggleUI, everywhere) clashes in both groups")
        work = Path(tempfile.mkdtemp(prefix="eo-ini-", dir=output / "test-runs"))
        custom = work / "FH2_ExtraOptions.ini"
        custom.write_text("[Bindings]\nAutodriveKeyboard=0x57\nHeadlightsKeyboard=None\n", encoding="utf-8")
        check(dll.KbmTestEoConflict(str(custom), BINDINGS.index("Throttle"), SCAN["W"]) == b"Autodrive" and
              dll.KbmTestEoConflict(str(custom), BINDINGS.index("Throttle"), SCAN["F3"]) is None and
              dll.KbmTestEoConflict(str(custom), BINDINGS.index("Map"), SCAN["Tab"]) == b"JumpToManufacturer",
              "a custom [Bindings] (0x57, None, missing keys = its source defaults) is read as it reads it")
        custom.unlink()
        check(dll.KbmTestEoConflict(str(custom), BINDINGS.index("Throttle"), SCAN["F3"]) is None and
              dll.KbmTestEoConflict(str(custom), BINDINGS.index("Throttle"), 0x3C) == b"Autodrive",
              "without its INI its source defaults apply (F2 Autodrive)")
        shutil.rmtree(work, ignore_errors=True)
    if not upstream or not (upstream / "patches.json").is_file():
        skip(f"Extra Options 0.4.4 source not found ({upstream})")
        return
    patches = json.loads((upstream / "patches.json").read_text(encoding="utf-8"))
    eo_ranges = [(p["rva"], len(p["original"]) // 2, p["id"]) for p in patches["patches"]]
    for hook in patches["runtime_hooks"]:
        size = len(hook["original"]) // 2 if "original" in hook else 8
        eo_ranges.append((hook["rva"], size, hook["id"]))
    overlaps = [(name, eo_id) for rva, size, name in OUR_RANGES for start, length, eo_id in eo_ranges
                if start < rva + size and rva < start + length and SHARED.get(rva) != eo_id]
    check(not overlaps, f"no patch or runtime hook of its patches.json ({len(eo_ranges)}) overlaps a site of ours "
                        f"except the three it shares on purpose ({overlaps})")
    shared = {rva: any(start == rva and eo_id == name for start, _, eo_id in eo_ranges) for rva, name in SHARED.items()}
    check(all(shared.values()), f"the shared sites are exactly its {', '.join(SHARED.values())} ({shared})")
    constants = eo_source_constants(upstream)
    inside = sorted((hex(value), name, sorted(files)) for value, files in constants.items()
                    for rva, size, name in OUR_RANGES if rva < value < rva + size)
    check(not inside, f"no address in its C sources points inside a byte range of ours ({inside})")
    exact = {hex(rva): (name, sorted(constants[rva])) for rva, size, name in OUR_RANGES
             if rva in constants and rva not in SHARED}
    called = {r: v for r, v in exact.items() if v[0] == "entry"}
    other = {r: v for r, v in exact.items() if v[0] != "entry"}
    check(not other, f"its sources name none of our slots or call sites but the shared ones ({other})")
    eo_starts = {start for start, _, _ in eo_ranges}
    check(all(int(r, 16) not in eo_starts for r in called),
          f"functions of ours it only calls (not patches): {', '.join(f'{r} ({v[1][0]})' for r, v in called.items())}")


# ------------------------------------------------------------------- 5. game

PAGE_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
VOID2 = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
VOID3 = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)


def eo_patch(base, reader_address, reset_address=None, pause_enter=None):
    """What Extra Options 0.4.4 patches on the sites this plugin shares: its thunk and the resource call, the
    Video page reset slot, PauseGame enter (Autodrive)."""
    poke(base + EO_THUNK, b"\x48\xb8" + struct.pack("<Q", reader_address) + b"\xff\xe0")
    poke(base + RESOURCE_CALL, b"\xe8" + struct.pack("<i", EO_THUNK - (RESOURCE_CALL + 5)))
    if reset_address:
        poke(base + RESET_SLOT, struct.pack("<Q", reset_address))
    if pause_enter:
        poke(base + 0x2D16698, struct.pack("<Q", pause_enter))


def snapshot(base):
    return {name: peek(base + rva, size) for rva, size, name in OUR_RANGES}


def test_game(output, game_exe):
    print("game")
    if not game_exe.is_file():
        skip(f"{game_exe} not found")
        return None
    pe = PE(game_exe)
    check(pe.image_size == IMAGE_SIZE, "the game executable is the inspected build (image size 0x3a18000)")
    dll = fresh_dll(output, "guards")
    base = pe.map()
    keep = []
    try:
        resource = dll.KbmTestPageCheck(base, 0)
        reset = dll.KbmTestPageCheck(base, 1)
        check(resource == b"the game's resource reader 0xaabec0" and reset == b"the game's Video page reset 0xebf060",
              f"stock game: settings page guards pass ({resource}; {reset})")
        hook = ctypes.cast(dll.KbmTestPageHandlers, ctypes.c_void_p).value  # any executable code outside the image
        eo_patch(base, hook, hook)
        resource = dll.KbmTestPageCheck(base, 0)
        reset = dll.KbmTestPageCheck(base, 1)
        check(resource and resource.startswith(b"Extra Options' resource thunk 0x13fe354 -> KeyboardMouseTest-guards.dll+0x")
              and reset and reset.startswith(b"a hook at KeyboardMouseTest-guards.dll+0x"),
              f"with Extra Options' hooks: the resource call and the reset slot are recognised ({resource}; {reset})")

        def refused(rva, data, expect, what):
            old = peek(base + rva, len(data))
            poke(base + rva, data)
            problem = dll.KbmTestPageCheck(base, 0)
            poke(base + rva, old)
            check(problem is not None and expect in problem, f"{what} is refused ({problem.decode() if problem else None})")

        flip = lambda rva: bytes([peek(base + rva, 1)[0] ^ 0xFF])
        refused(0x155EE30, b"\xcc", b"register property at 0x155ee30", "a changed property registration")
        refused(0xED825B + 3, b"\0\0\0\0", b"Subtitles and OnOffOptions", "an int template naming another property")
        refused(0xECA808 + 3, flip(0xECA808 + 3), b"0x35470d0", "the VideoOptions type registering elsewhere")
        refused(0xB52170, b"\xcc", b"insert item", "a changed list insert")
        refused(RESOURCE_CALL + 5, b"\x90", b"after the XAML resource call", "changed code after the resource call")
        refused(EO_THUNK, b"\x90\x90", b"not Extra Options' thunk", "a resource call to a changed thunk")
        refused(RESOURCE_CALL, b"\x90", b"not a call", "a resource site that is no longer a call")
        refused(RESET_SLOT, struct.pack("<Q", base + 0x1000), b"reset slot", "a reset slot pointing elsewhere in the game")
    finally:
        unmap(base)

    # The input hooks on the stock game.
    dll = fresh_dll(output, "install")
    base = pe.map()
    try:
        before = snapshot(base)
        check(dll.KbmTestInstall(base, None) == 1, "the input hooks install on the stock game")
        after = snapshot(base)
        for index, (rva, size) in enumerate(ENTRIES):
            hook = dll.KbmTestHook(index)
            original = dll.KbmTestOriginal(index)
            code = peek(base + rva, size)
            target = jump_target(base + rva)
            fill = code[14:]
            ok = target == hook and fill == (b"\xcc" if rva == 0x1072500 else b"\x90") * (size - 14)
            if rva == 0x1072500:
                ok = ok and original == base + DIGITAL_BODY
            else:
                trampoline = peek(original, size + 14) if original else b""
                ok = ok and trampoline[:size] == pe.read(rva, size) and jump_target(original + size) == base + rva + size
            check(ok, f"entry 0x{rva:x}: absolute jump to its hook ({size} bytes), "
                      f"{'the thunk goes to its body 0x1072510' if rva == 0x1072500 else 'the trampoline runs the moved bytes and jumps back'}")
        for index, (site, native, kind, name) in enumerate(CHAINS):
            previous = ctypes.c_void_p()
            handler = dll.KbmTestChain(index, ctypes.byref(previous))
            now = qword(base + site) if kind == "slot" else jump_target(call_target(base, site) or 0)
            check(handler and now == handler and previous.value == base + native,
                  f"{name} ({kind} 0x{site:x}) chained onto the game's 0x{native:x}")
        call = call_target(base, CHAINS[3][0])
        check(call is not None and abs(call - base) < 0x7FF00000 and not (base <= call < base + IMAGE_SIZE),
              "call sites go to a jump allocated within 2 GB of the game, outside it (no int3 padding used)")
        check(dll.KbmTestBinding(0, 0x00) == 1 and dll.KbmTestBinding(0, 0x16) == BINDINGS.index("Map") + 1 and
              dll.KbmTestBinding(7, 0x54) == BINDINGS.index("Pause") + 1 and dll.KbmTestBinding(7, 0x4A) ==
              BINDINGS.index("Confirm") + 1 and dll.KbmTestBinding(0, 0x02) == BINDINGS.index("SteerLeft") + 1,
              "key bindings map to the game's actions (Gas 0, map 0x16 in the world, START 0x54, OK 0x4a, steering)")
        check(dll.KbmTestPitchPatch(base) == 7 and
              all(qword(base + v + 0x260) in (dll.KbmTestPitchHooks(0), dll.KbmTestPitchHooks(1)) for v in PITCH_SLOTS),
              "mouse pitch: the look direction slot +0x260 of the 7 attached cameras is chained")
        previous = ctypes.c_void_p()
        check(dll.KbmTestPauseInstall(base, 1, None) == 1 and
              jump_target(call_target(base, PAUSE_CALL)) == dll.KbmTestPauseHook(ctypes.byref(previous)) and
              previous.value == base + PAUSE_FIRE, "controller pause: the onPause call 0x9648fa chained onto 0x1925260")
        untouched = [name for name in before if before[name] == after[name] and name not in
                     ("Settings hub init", "XAML resource call", "Video page reset", "pitch slot", "controller pause call")]
        check(not untouched, f"every input site changed ({untouched})")
    finally:
        unmap(base)

    # Behind other modules' hooks: the plugin chains onto them.
    dll = fresh_dll(output, "chained")
    base = pe.map()
    try:
        others = {0: PAGE_FN(lambda p: None), 8: PAGE_FN(lambda p: None), 6: VOID2(lambda a, b: None)}
        keep += list(others.values())
        for index, callback in others.items():
            poke(base + CHAINS[index][0], struct.pack("<Q", ctypes.cast(callback, ctypes.c_void_p).value))
        nav = CHAINS[11][0]
        stub = near(base)
        nav_other = VOID2(lambda a, b: None)
        keep.append(nav_other)
        poke(stub, b"\xff\x25\0\0\0\0" + struct.pack("<Q", ctypes.cast(nav_other, ctypes.c_void_p).value))
        poke(base + nav + 1, struct.pack("<i", stub - (base + nav + 5)))
        check(dll.KbmTestInstall(base, None) == 1, "the hooks install behind other modules' hooks")
        for index in list(others) + [11]:
            previous = ctypes.c_void_p()
            handler = dll.KbmTestChain(index, ctypes.byref(previous))
            expect = stub if index == 11 else ctypes.cast(others[index], ctypes.c_void_p).value
            check(previous.value == expect, f"{CHAINS[index][3]}: chained onto the other module's hook")
    finally:
        unmap(base)

    # A changed entry: nothing at all is patched.
    dll = fresh_dll(output, "refused")
    base = pe.map()
    try:
        poke(base + 0x106E690 + 3, b"\x90")
        before = snapshot(base)
        check(dll.KbmTestInstall(base, None) == 0, "a changed action query entry refuses the input hooks")
        check(snapshot(base) == before, "... and no site is changed")
    finally:
        unmap(base)
    dll = fresh_dll(output, "pad")
    base = pe.map()
    try:
        read = fresh_dll(output, "pad-settings")
        work = Path(tempfile.mkdtemp(prefix="pad-", dir=output / "test-runs"))
        _, _, _, settings = read_settings(read, work, "[KeyboardMouse]\nMode=PAD\n")
        before = snapshot(base)
        check(dll.KbmTestInstall(base, ctypes.byref(settings)) == 0 and snapshot(base) == before,
              "Mode=PAD: no input hook is installed")
        shutil.rmtree(work, ignore_errors=True)
    finally:
        unmap(base)
    return pe



# ------------------------------------------------------------------- 6. xaml

def parse(data, label):
    try:
        return ET.fromstring(data.decode("utf-8-sig"))
    except (ET.ParseError, UnicodeDecodeError) as error:
        check(False, f"{label}: well-formed XML ({error})")
        return None


def build(function, text, room=0x40000):
    out = ctypes.create_string_buffer(len(text) + room)
    n = function(text, len(text), out, len(out))
    return out.raw[:n] if n else b""


def check_hub(hub, label, caption="KEYBOARD & MOUSE"):
    root = parse(hub, label)
    if root is None:
        return
    buttons = {b.get(X + "Name"): b for b in root.iter() if b.tag.endswith("Button")}
    tile, controls = buttons.get("PART_Button_EOKeyboard"), buttons.get("PART_Button_Controls")
    nav = lambda e, k: next((v for a, v in e.attrib.items() if a.endswith("TileMenuNavigationManager." + k)), None)
    check(tile is not None and (nav(tile, "GridX"), nav(tile, "GridY")) == ("2", "0"),
          f"{label}: the KEYBOARD & MOUSE tile takes column 2 of the top row")
    check(controls is not None and "Tile_1x1" in controls.get("Style", "") and
          controls.get("Template") == "{StaticResource EO_ControlTemplate_RightHalfTile}",
          f"{label}: Controls is cut to 1x1 (the right half of its picture)")
    heading = [e.get("Heading") for e in root.iter() if e.get(X + "Key") == "EO_MenuDataItem_Keyboard"]
    check(heading == [caption], f"{label}: the tile's caption {heading}")
    check(hub[:3] == b"\xef\xbb\xbf", f"{label}: UTF-8 with its byte-order mark")


def check_page(page, label, title="KEYBOARD & MOUSE", source=None):
    root = parse(page, label)
    if root is None:
        return None
    check(page.startswith(b"\xef\xbb\xbf<!-- EO:StandaloneOptionsPage -->"),
          f"{label}: starts with the BOM and the EO:StandaloneOptionsPage marker")
    lists = [e for e in root.iter() if e.get(X + "Name") == "OptionsList"]
    rows = list(lists[0]) if lists else []
    names = [r.get("SelectedIndex") for r in rows]
    check(len(rows) == 9 + 32 + 1, f"{label}: 9 options, 32 key rows and Reset ({len(rows)} rows)")
    check(all(r.tag.endswith("SpinnerControl") for r in rows), f"{label}: every row is a native spinner")
    check(names[:2] == ["{Binding KbmMode}", "{Binding KbmMouseLook}"] and names[-1] == "{Binding KbmKeyReset}",
          f"{label}: row order Mode, MouseLook ... Reset")
    check(rows and rows[0].get(X + "Name") == "FirstItem", f"{label}: the first row is FirstItem (initial focus)")
    check(lists and lists[0].get("SelectedIndex") == "{Binding KbmKeyListIndex, Mode=OneWayToSource}" and
          lists[0].get("Height") == "440", f"{label}: the list tracks the focused row and is 440 px high")
    keys = {e.get(X + "Key") for e in root.iter() if e.get(X + "Key")}
    check(all("EO_" + r.get("SelectedIndex")[9:-1] in keys for r in rows), f"{label}: every row has its description")
    contents = [e.get("Content") for e in root.iter() if e.get("Content") and "IDS_VideoOptions" not in e.get("Content", "")
                and e.get("Content") == title]
    check(contents == [title] and b"ScreenTitles.IDS_VideoOptions" not in page, f"{label}: the title is {title!r}")
    panels = [e for e in root.iter() if e.tag.endswith("StackPanel") and e.get("Background") == "Black"]
    check(panels and all(p.get("Visibility") == "Collapsed" for p in panels), f"{label}: the brightness preview is hidden")
    body = page[page.index(b'x:Name="OptionsList"'):page.index(b"</local:CommonListBox>")]
    foreign = re.findall(rb"\{Binding (?!Kbm)(\w+)", body)
    check(not foreign, f"{label}: no other page's rows are left in the list ({sorted(set(foreign))[:6]})")
    if source is not None:
        native = [e.get(X + "Name") for e in parse(source, label + " source").iter() if e.get(X + "Name")]
        after = [e.get(X + "Name") for e in root.iter() if e.get(X + "Name") and e.get(X + "Name") != "FirstItem"]
        lost = [n for n in native if n not in after and n != "FirstItem"]
        check(not lost or all(n.startswith(("EO_", "AZG", "Gfx")) for n in lost),
              f"{label}: the page's own named elements outside the list are kept ({lost[:5]})")
    return root


def build_eo_xaml(zig, upstream, folder):
    """Extra Options 0.4.4's settings page code (support.c, video_options_xaml.c, video_options_text.c) as a DLL."""
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    for sub in (upstream, upstream / "features", upstream / "adapters"):
        for header in sub.glob("*.h"):
            (folder / header.name).write_bytes(header.read_bytes())
    for name in ("support.c", "features/video_options_xaml.c", "features/video_options_text.c"):
        (folder / Path(name).name).write_bytes((upstream / name).read_bytes())
    (folder / "eo_wrapper.c").write_text(
        '#include "video_options_xaml.h"\n'
        "__declspec(dllexport) size_t EoExtend(const char *t, size_t n, char *o, size_t c) {\n"
        "    return video_options_extend_xaml(NULL, t, n, o, c);\n}\n", encoding="utf-8")
    sources = [folder / n for n in ("eo_wrapper.c", "support.c", "video_options_xaml.c", "video_options_text.c")]
    dll = folder / "eo_xaml.dll"
    command = [str(zig), "cc", "-target", "x86_64-windows-gnu", "-O1", "-w", "-shared", "-fms-extensions",
               "-funwind-tables", "-fno-builtin", "-DFH2_AZIMUTH", "-I", str(folder), *map(str, sources), "-o", str(dll)]
    run = subprocess.run(command, capture_output=True, text=True)
    if run.returncode:
        missing = sorted(set(re.findall(r"undefined symbol: (\w+)", run.stderr)))
        if not missing:
            print(run.stderr)
            return None
        (folder / "stubs.c").write_text("".join(f"void *{n}(void) {{ return 0; }}\n" for n in missing))
        run = subprocess.run(command[:-2] + [str(folder / "stubs.c")] + command[-2:], capture_output=True, text=True)
        if run.returncode:
            print(run.stderr)
            return None
    library = ctypes.CDLL(str(dll))
    library.EoExtend.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    library.EoExtend.restype = ctypes.c_size_t
    return library


def graphics_extend(graphics, xaml):
    out = ctypes.create_string_buffer(len(xaml) + 0x10000)
    n = graphics.AzgTestExtendXaml(0, xaml, len(xaml), out, len(out), 0, 31)
    return out.raw[:n] if n else b""


def test_xaml(output, dll, zig, game, upstream, graphics_path):
    print("xaml")
    ui = game / UI_ZIP
    if not ui.is_file():
        skip(f"{ui} not found")
        return None, None, None
    with zipfile.ZipFile(ui) as archive:
        hub_native = archive.read("Scenes/Anthem/A_PauseOptions.xaml")
        video = archive.read("Scenes/Anthem/A_VideoOptions.xaml")
        defined = {k for n in archive.namelist() if n.lower().endswith(".xaml")
                   for k in re.findall(rb'x:Key="([^"]+)"', archive.read(n))}
    hub = build(dll.KbmTestHubXaml, hub_native)
    check(bool(hub), "the game's Settings hub gets the tile")
    if hub:
        check_hub(hub, "Settings hub")
        added = set(re.findall(rb"StaticResource (\w+)", hub)) - set(re.findall(rb"StaticResource (\w+)", hub_native))
        missing = sorted(k.decode() for k in added if k not in defined and not k.startswith(b"EO_"))
        check(added and not missing, f"every resource the tile uses is the game's own or the plugin's ({missing})")
        check(not build(dll.KbmTestHubXaml, hub), "a hub that has the tile is left alone (read through two chains)")
        check(not build(dll.KbmTestHubXaml, video), "another page is not taken for the hub")
    (output / "test-runs").mkdir(parents=True, exist_ok=True)
    page = build(dll.KbmTestKeyboardXaml, video)
    check(bool(page), "the KEYBOARD & MOUSE page is built from the game's Video page")
    if page:
        check_page(page, "page from the game's Video page", source=video)
        (output / "test-runs" / "keyboard_page_game.xaml").write_bytes(page)
        added = set(re.findall(rb"StaticResource (\w+)", page)) - set(re.findall(rb"StaticResource (\w+)", video))
        missing = sorted(k.decode() for k in added if k not in defined and not k.startswith(b"EO_"))
        check(not missing, f"every resource the rows use is the game's own or the page's ({missing})")
        check(not build(dll.KbmTestKeyboardXaml, page), "the KEYBOARD & MOUSE page is not rebuilt from itself")
        check(not build(dll.KbmTestKeyboardXaml, hub_native), "another page is not taken for the Video page")
        marked = b"<!-- EO:StandaloneOptionsPage -->" + video
        check(not build(dll.KbmTestKeyboardXaml, marked), "a plugin's standalone page made from the Video page is left alone")
        short = ctypes.create_string_buffer(len(page))
        check(dll.KbmTestKeyboardXaml(video, len(video), short, len(page)) == 0, "a buffer one byte short is refused")
    if hub:
        (output / "test-runs" / "settings_hub.xaml").write_bytes(hub)

    eo_page = None
    if not upstream or not (upstream / "features" / "video_options_xaml.c").is_file():
        skip(f"Extra Options 0.4.4 source not found ({upstream})")
    else:
        library = build_eo_xaml(zig, upstream, output / "obj" / "eo-0.4.4")
        check(library is not None, "Extra Options 0.4.4's settings page code builds for the test")
        if library:
            out = ctypes.create_string_buffer(len(video) + 0x20000)
            n = library.EoExtend(video, len(video), out, len(out))
            eo_page = out.raw[:n]
            check(bool(eo_page) and b"RealtimeShadows" in eo_page, "Extra Options 0.4.4 adds its rows to the Video page")
            ours = build(dll.KbmTestKeyboardXaml, eo_page)
            check(bool(ours), "the KEYBOARD & MOUSE page is built from Extra Options' Video page (it reads it first)")
            if ours:
                check_page(ours, "page from Extra Options' Video page", source=eo_page)
                (output / "test-runs" / "keyboard_page_eo_0.4.4.xaml").write_bytes(ours)
                n = library.EoExtend(ours, len(ours), out, len(out))
                check(n == 0, "Extra Options 0.4.4 leaves the KEYBOARD & MOUSE page alone (StandaloneOptionsPage)")
    graphics = None
    if not graphics_path or not graphics_path.is_file():
        skip(f"Azimuth Graphics Options test DLL not found ({graphics_path})")
    else:
        copy = output / "test-runs" / "dll" / "AzimuthGraphicsOptionsTest.dll"
        copy.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(graphics_path, copy)
        graphics = ctypes.CDLL(str(copy))
        graphics.AzgTestExtendXaml.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p,
                                               ctypes.c_size_t, ctypes.c_int, ctypes.c_int]
        graphics.AzgTestExtendXaml.restype = ctypes.c_size_t
        source = eo_page or video
        inner = graphics_extend(graphics, source)
        check(bool(inner) and b"GfxFrameRate" in inner,
              "Azimuth Graphics Options adds its rows to the Video page (as when it reads before this plugin)")
        if inner:
            ours = build(dll.KbmTestKeyboardXaml, inner)
            check(bool(ours), "the KEYBOARD & MOUSE page is built from that page (its list style and height replaced)")
            if ours:
                check_page(ours, "page from Graphics Options' Video page", source=inner)
                check(not graphics_extend(graphics, ours),
                      "Azimuth Graphics Options leaves the KEYBOARD & MOUSE page alone (when it reads after this plugin)")
    return hub_native, video, eo_page


# ------------------------------------------------------------------ 7. texts

def language_manager(code):
    manager = ctypes.create_string_buffer(0x60)
    raw = code.encode()
    ctypes.memmove(ctypes.addressof(manager) + 0x30, raw + b"\0", len(raw) + 1)
    struct.pack_into("<QQ", manager, 0x40, len(raw), 15)
    return manager


def test_texts(dll, hub_native, video):
    print("texts")
    codes = [dll.KbmTestLanguage(i).decode() for i in range(dll.KbmTestLanguages())]
    check(codes == list(TEXTS), f"the plugin has every code of translations.json ({len(codes)})")
    check(len(set(codes) - {"GB"}) == 15 and TEXTS["GB"] == TEXTS["EN"],
          "15 game languages (EN BR DE FR ES MX IT JP KO CHT CZ HU PL RU NL), GB = English")
    check(dll.KbmTestTextComplete() == 1, "every text of every language is there")
    pr = {"EN": "KEYBOARD & MOUSE", "PL": "KLAWIATURA I MYSZ", "DE": "TASTATUR UND MAUS", "FR": "CLAVIER ET SOURIS",
          "BR": "TECLADO E MOUSE", "ES": "TECLADO Y RATÓN"}
    check(all(TEXTS[c]["title"] == t for c, t in pr.items()), "the titles are those of the keyboard & mouse branch (PR #101)")
    cell = ctypes.c_void_p()
    fake_game = ctypes.addressof(cell) - TEXT_TABLES
    for code in codes:
        title = TEXTS[code]["title"]
        manager = language_manager(code)
        cell.value = ctypes.addressof(manager)
        check(dll.KbmTestTitle(fake_game).decode("utf-8") == title, f"{code}: {title} (from the game's language)")
    for bad in ("XX", "ENGLISH"):
        manager = language_manager(bad)
        cell.value = ctypes.addressof(manager)
        check(dll.KbmTestTitle(fake_game).decode() == TEXTS["EN"]["title"], f"unknown language {bad!r}: English")
    check(dll.KbmTestTitle(None).decode() == TEXTS["EN"]["title"], "no game image: English")
    check(dll.KbmTestMenuChoice(0, 1) == "Keyboard & Mouse" and dll.KbmTestMenuChoice(1, 1) == "On" and
          dll.KbmTestMenuChoice(1, 0) == "Off" and dll.KbmTestMenuChoice(4, 2) == "100%",
          "option choice labels (mode, On/Off, percent)")
    rgba = ctypes.create_string_buffer(64 * 64 * 4)
    check(dll.KbmTestKeycap("W", 64, rgba) == 1 and any(rgba.raw[3::4]) and rgba.raw[3] == 0,
          "key prompts: a keycap image is drawn (opaque key, transparent corner)")


# ------------------------------------------------------------------- 8. page

def ini_value(ini, key):
    for line in ini.read_text(encoding="utf-8").splitlines():
        if line.split("=")[0].strip() == key:
            return line.split("=", 1)[1].strip()
    return None


def test_page(output):
    print("page")
    dll = fresh_dll(output, "page")
    work = Path(tempfile.mkdtemp(prefix="kbm-page-", dir=output / "test-runs"))
    ini = work / INI_NAME
    ini.write_text("[KeyboardMouse]\nEnabled=1\nMouseSensitivity=90 ; comment\nThrottle=W\n", encoding="utf-8")
    dll.KbmTestSetIni(str(ini))
    dll.KbmTestRebindReload()
    eo_ini = work / "FH2_ExtraOptions.ini"
    eo_ini.write_text("[Bindings]\nHeadlightsKeyboard=F3\nJumpToManufacturerKeyboard=Tab\n", encoding="utf-8")
    dll.KbmTestEoConflict(str(eo_ini), 0, 0)  # loads Extra Options' bindings from this file

    def accepted():
        enabled, _, problem, _ = read_settings(dll, work, ini.read_text(encoding="utf-8"))
        return enabled == 1, problem

    check(dll.KbmTestMenuGet(4) == 2, "MouseSensitivity=90 shows the nearest choice (100%)")
    check(dll.KbmTestMenuGet(1) == 1, "MouseLook shows its default (On) when the INI has no line")
    check(dll.KbmTestMenuSet(4, 5) and ini_value(ini, "MouseSensitivity") == "160", "mouse sensitivity is written")
    check(dll.KbmTestMenuSet(0, 1) and ini_value(ini, "Mode") == "KBM", "the input mode is written")
    check(not dll.KbmTestMenuSet(0, 1), "an unchanged value is not written again")
    check(not dll.KbmTestMenuSet(0, 7), "a choice out of range is refused")
    ok, problem = accepted()
    check(ok, f"the plugin accepts the INI the page wrote ({problem})")
    dll.KbmTestRebindCapture(0, SCAN["I"])  # Throttle: I
    check(ini_value(ini, "Throttle") == "I" and dll.KbmTestRebindKeys(0) == SCAN["I"], "Throttle rebound to I")
    dll.KbmTestRebindCapture(1, SCAN["I"])  # Brake: I is taken by Throttle
    check(ini_value(ini, "Brake") is None and dll.KbmTestRebindKeys(1) == SCAN["S"], "a key another driving action uses is refused")
    label = dll.KbmTestRebindChoice(1, dll.KbmTestRebindGet(1))
    check(label and label.startswith("In use") and "Throttle" in label, f"... and the row says so ({label})")
    dll.KbmTestRebindCapture(2, 0x0E)  # Steer left: Backspace clears
    check(ini_value(ini, "SteerLeft") == "None" and dll.KbmTestRebindKeys(2) == 0, "Backspace clears a binding")
    dll.KbmTestRebindCapture(3, 0x01)  # Steer right: Esc cancels
    check(ini_value(ini, "SteerRight") is None and dll.KbmTestRebindKeys(3) == SCAN["D"], "Esc cancels the capture")
    dll.KbmTestRebindCapture(0, SCAN["F3"])  # Throttle: F3 is Extra Options' headlights
    label = dll.KbmTestRebindChoice(0, dll.KbmTestRebindGet(0))
    check(ini_value(ini, "Throttle") == "I" and label == "In use (Extra Options)",
          f"a key Extra Options binds for driving is refused for a driving action ({label})")
    menu_x = 24  # rebind item of MenuX
    dll.KbmTestRebindCapture(menu_x, SCAN["Tab"])
    check(ini_value(ini, "MenuX") is None, "Tab (its jump to manufacturer in car lists) is refused for a menu action")
    dll.KbmTestRebindCapture(8, 0x3F)  # Camera: F5
    check(ini_value(ini, "Camera") == "F5", "a free key is taken (Camera=F5)")
    dll.KbmTestRebindCapture(9, 0x3C)  # Rewind: F2 cannot be bound
    check(ini_value(ini, "Rewind") is None, "F2 (Autodrive) cannot be bound: the capture keeps waiting")
    ok, problem = accepted()
    check(ok, f"the plugin accepts the rebound INI ({problem})")
    dll.KbmTestRebindReload()
    check(dll.KbmTestRebindKeys(0) == SCAN["I"], "the rebound key survives a reload")
    check(dll.KbmTestRebindChoice(0, dll.KbmTestRebindGet(0)) == "I", "the Throttle row shows I")
    dll.KbmTestRebindReset()
    check(ini_value(ini, "Throttle") is None and ini_value(ini, "SteerLeft") is None and
          ini_value(ini, "Camera") is None and dll.KbmTestRebindKeys(0) == SCAN["W"],
          "Reset Key Bindings restores the defaults (the key lines are removed)")
    check(ini_value(ini, "Mode") == "KBM" and ini_value(ini, "MouseSensitivity") == "160",
          "... and leaves the options alone")
    ok, problem = accepted()
    check(ok, f"the plugin accepts the INI after Reset ({problem})")
    dll.KbmTestSetIni("")
    shutil.rmtree(work, ignore_errors=True)


# ----------------------------------------------------------------- 9. plugin

LOG = ctypes.CFUNCTYPE(None, ctypes.c_char_p)
READER = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p))
ALLOC = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_size_t)
VOID0 = ctypes.CFUNCTYPE(None)
PTR0 = ctypes.CFUNCTYPE(ctypes.c_void_p)
BOX = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
TYPED = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p)
TYPE = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)
STRING = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p)
INSERT = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p)
BIND = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)
GETTER = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
SETTER = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)


class Context(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("api_version", ctypes.c_uint32), ("adapter", ctypes.c_uint32),
                ("game_image_size", ctypes.c_uint32), ("game_image", ctypes.c_void_p),
                ("plugin_module", ctypes.c_void_p), ("plugins_directory", ctypes.c_wchar_p),
                ("plugin_path", ctypes.c_wchar_p), ("extra_options_version", ctypes.c_char_p), ("log_utf8", LOG)]


class Natives(ctypes.Structure):
    _fields_ = [(n, ctypes.c_void_p) for n in (
        "allocate", "free_memory", "object_init", "object_cleanup", "delete_object", "type_init", "int_template",
        "list_template", "register_property", "box_int", "property_changed", "collection_type", "allocate_typed",
        "collection_init", "make_string", "insert_item", "registry")]


class Plugin:
    """FH2PluginInitialize of a DLL, as Extra Options 0.4.4 calls it, with KeyboardMouse.ini beside it."""

    def __init__(self, dll, work, ini_text):
        self.dll = dll
        self.dll.FH2PluginInitialize.argtypes = [ctypes.POINTER(Context)]
        self.dll.FH2PluginInitialize.restype = wintypes.BOOL
        self.lines = []
        self.log = LOG(lambda text: self.lines.append(text.decode("utf-8", "replace")))
        folder = work / "mods" / MOD_NAME / "plugins"
        folder.mkdir(parents=True, exist_ok=True)
        self.ini = folder / INI_NAME
        self.ini.write_text(ini_text, encoding="utf-8")
        self.folder, self.path = str(folder), str(folder / DLL_NAME)

    def initialize(self, base, adapter=1, size=IMAGE_SIZE):
        self.context = Context(ctypes.sizeof(Context), 1, adapter, size, base, None, self.folder, self.path, b"0.4.4",
                               self.log)
        return self.dll.FH2PluginInitialize(ctypes.byref(self.context))

    def said(self, text):
        return any(text in line for line in self.lines)


class FakeGame:
    """Stand-ins for the game's object and property functions (the mapped game's code cannot run)."""

    def __init__(self):
        self.keep, self.objects, self.events, self.registered, self.boxed = [], {}, [], [], []
        self.strings, self.items = {}, {}

        def new(size, refs=1):
            buffer = ctypes.create_string_buffer(max(size, 0x30))
            struct.pack_into("<i", buffer, 0x10, refs)
            self.objects[ctypes.addressof(buffer)] = buffer
            return ctypes.addressof(buffer)

        self.new = new

        def template(name):
            prop = ctypes.create_string_buffer(0x58)
            label = ctypes.create_string_buffer(name.encode() + b"\0")
            vtable = ctypes.create_string_buffer(0x40)
            struct.pack_into("<Q", prop, 0, ctypes.addressof(vtable))
            struct.pack_into("<QQQ", prop, 0x40, ctypes.addressof(label), 0x7777, 0x1234)
            self.keep += [prop, label, vtable]
            return ctypes.addressof(prop)

        int_template, list_template = template("Subtitles"), template("OnOffOptions")

        def box(out, value):
            obj = new(0x30)
            struct.pack_into("<i", self.objects[obj], 0x28, ctypes.c_int.from_address(value).value)
            self.boxed.append(ctypes.c_int.from_address(value).value)
            ctypes.c_void_p.from_address(out).value = obj
            return out

        def make_string(out, text):
            obj = new(0x30)
            self.strings[obj] = text
            ctypes.c_void_p.from_address(out).value = obj
            return out

        def insert(items, index, label):
            ctypes.c_int.from_address(label + 0x10).value += 1
            self.items.setdefault(items, []).append(self.strings.get(label))

        def register(registry, link):
            prop = ctypes.c_void_p.from_address(link).value
            name = ctypes.string_at(ctypes.c_void_p.from_address(prop + 0x40).value).decode()
            self.registered.append((registry, prop, name))

        self.callbacks = [
            ALLOC(lambda size: new(size)), PAGE_FN(lambda p: self.events.append(("free", p))),
            PAGE_FN(lambda p: None), PAGE_FN(lambda p: self.events.append(("cleanup", p))),
            PAGE_FN(lambda p: self.events.append(("delete", p))), VOID0(lambda: self.events.append("type init")),
            PTR0(lambda: int_template), PTR0(lambda: list_template), VOID2(register), BOX(box),
            VOID2(lambda page, prop: self.events.append(("changed", page, prop))), TYPE(lambda x: 0x5151),
            TYPED(lambda size, t: new(size)), PAGE_FN(lambda items: None), STRING(make_string), INSERT(insert)]
        self.registry = ctypes.create_string_buffer(0x60)
        self.natives = Natives(*[ctypes.cast(c, ctypes.c_void_p).value for c in self.callbacks],
                               ctypes.addressof(self.registry))


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


def resource_text(address):
    raw = peek(address, 0x38)
    bytes_at, length, _ = struct.unpack_from("<Qii", raw, 0x28)
    return peek(bytes_at, length)


def test_plugin(output, pe, hosts, hub_native, video, eo_page):
    print("plugin")
    if pe is None or "1.0.4" not in hosts:
        skip("the plugin tests need the game executable and Azimuth 1.0.4")
        return
    shipped = (ROOT / "mod" / INI_NAME).read_text(encoding="utf-8")
    work = Path(tempfile.mkdtemp(prefix="plugin-", dir=output / "test-runs"))
    host_pe = hosts["1.0.4"]

    # --- A: behind simulated Extra Options and other plugins, on Azimuth 1.0.4.
    if hub_native is None:
        skip("plugin chain: needs the game's UI.zip")
    else:
        dll = fresh_dll(output, "plugin")
        plugin = Plugin(dll, work / "chain", shipped)
        base, host = pe.map(), host_pe.map()
        keep, events = [], []
        fake = FakeGame()
        dll.KbmTestPageNatives(ctypes.byref(fake.natives))
        dll.KbmTestUseHost(host)
        dll.KbmTestNoThread(1)
        resources = {}

        def eo_reader(reader, out):
            events.append("extra options reader")
            out[0] = resources[reader]
            return ctypes.cast(out, ctypes.c_void_p).value

        eo = READER(eo_reader)
        eo_reset = PAGE_FN(lambda page: events.append(("extra options reset", page)))
        eo_pause = PAGE_FN(lambda state: events.append(("extra options pause enter", state)))
        other_hub = PAGE_FN(lambda page: events.append(("other plugin hub init", page)))
        eo_patch(base, ctypes.cast(eo, ctypes.c_void_p).value, ctypes.cast(eo_reset, ctypes.c_void_p).value,
                 ctypes.cast(eo_pause, ctypes.c_void_p).value)
        poke(base + HUB_INIT_SLOT, struct.pack("<Q", ctypes.cast(other_hub, ctypes.c_void_p).value))
        thunk_reader = READER(base + EO_THUNK)

        def other_plugin(reader, out):  # e.g. Azimuth Graphics Options or Exit Game, chained before this plugin
            events.append("other plugin reader")
            return thunk_reader(reader, out)

        other = READER(other_plugin)
        jump = near(base)
        poke(jump, b"\xff\x25\0\0\0\0" + struct.pack("<Q", ctypes.cast(other, ctypes.c_void_p).value))
        poke(base + RESOURCE_CALL + 1, struct.pack("<i", jump - (base + RESOURCE_CALL + 5)))
        keep += [eo, eo_reset, eo_pause, other_hub, thunk_reader, other, fake]

        ready = plugin.initialize(base)
        check(ready == 1, "FH2PluginInitialize returns TRUE")
        for line in plugin.lines:
            print("    log:", line.rstrip()[:150])
        check(plugin.said("KBM: Keyboard & Mouse 0.0.1 for Extra Options 0.4.4") and
              plugin.said("KBM: host Azimuth 1.0.4 (TimeDateStamp 0x6ae393eb, SizeOfImage 0x8a2000)"),
              "it logs its version, Extra Options' and the host")
        check(plugin.said("KBM: native keyboard & mouse hooks installed") and dll.KbmTestInstalled() == 1,
              "the input hooks are in")
        check(plugin.said("gamepad vibration follows the input source (Azimuth 1.0.4"), "vibration gate on 1.0.4")
        check(plugin.said("XAML resource call 0x16ebb37 chained onto another module's hook at") and
              plugin.said("Video page reset chained onto a hook at"), "the log names what the shared sites held")
        check(plugin.said("PauseGame enter vtable slot 0x2d16698 chained onto a hook at"),
              "PauseGame enter is chained behind Extra Options' Autodrive hook")
        check(plugin.said("Settings hub init (slot 0x2e15940) chained onto a hook at"),
              "the Settings hub init is chained behind another plugin's tile hook")
        check(plugin.said("CONTROLLER PAUSE: the onPause call 0x9648fa chained onto the game's 0x1925260; the "
                          "controller-missing pause now needs a real unplug"), "the controller pause option is on")
        check(qword(base + HUB_INIT_SLOT) == dll.KbmTestHubHandlers(0) and
              qword(base + RESET_SLOT) == dll.KbmTestPageHandlers(1) and
              jump_target(call_target(base, RESOURCE_CALL)) == dll.KbmTestPageHandlers(0),
              "the hub init slot, the reset slot and the resource call now lead to the plugin")
        check(dll.KbmTestPagePrevious(0) == jump and
              dll.KbmTestPagePrevious(1) == ctypes.cast(eo_reset, ctypes.c_void_p).value,
              "and remember the other plugin's jump and Extra Options' reset")
        bound = []
        video_tile = PAGE_FN(lambda page: events.append(("video tile", page)))
        binder = BIND(lambda page, name, handler: bound.append((page, name, struct.unpack_from("<Qi", peek(handler, 12)))))
        keep += [video_tile, binder]
        dll.KbmTestMenuNatives(ctypes.cast(binder, ctypes.c_void_p).value, ctypes.cast(video_tile, ctypes.c_void_p).value)
        call = READER(call_target(base, RESOURCE_CALL))

        def read(key, text):
            resources[key] = make_resource(keep, text)
            out = ctypes.c_void_p()
            events.clear()
            call(key, ctypes.byref(out))
            return out.value

        out = read(1, hub_native)
        check(events[:2] == ["other plugin reader", "extra options reader"], f"the previous readers run first ({events[:2]})")
        check(out != resources[1], "the Settings hub comes back as a new resource")
        if out != resources[1]:
            check_hub(resource_text(out), "the hub through the chain")
        check(read(2, video) == resources[2], "the Video page passes through untouched without the tile's request")
        hub_init = PAGE_FN(qword(base + HUB_INIT_SLOT))
        events.clear()
        hub_init(0x5150)
        check(events[:1] == [("other plugin hub init", 0x5150)], "the other plugin's hub init runs first")
        check(len(bound) == 1 and bound[0][:2] == (0x5150, b"PART_Button_EOKeyboard") and
              bound[0][2] == (dll.KbmTestHubHandlers(1), 0), "then PART_Button_EOKeyboard is bound to the tile")
        if bound:
            PAGE_FN(bound[0][2][0])(0x5150)
            check(("video tile", 0x5150) in events, "choosing the tile starts the Video page's transition")
        source = eo_page or video
        page_out = read(3, source)
        check(page_out != resources[3], "right after the tile, the Video page comes back as the KEYBOARD & MOUSE page")
        if page_out != resources[3]:
            check_page(resource_text(page_out), "the page through the chain", source=source)
        names = [n for _, _, n in fake.registered]
        check(len(names) == 85 and names[0] == "KbmMode" and "KbmModeChoices" in names and "KbmKeyListIndex" in names
              and "KbmKeyListIndexChoices" not in names and "type init" in [e for e in fake.events if isinstance(e, str)],
              f"85 properties registered on the VideoOptions type after its set-up ({len(names)})")
        check(all(r == ctypes.addressof(fake.registry) for r, _, _ in fake.registered),
              "all on the VideoOptions registry")
        check(read(4, source) == resources[4], "a later Video page (no new tile choice) is the plain Video page")
        props = {n: p for _, p, n in fake.registered}
        page = fake.new(0x40, 1)
        mode = props["KbmMode"]
        getter = GETTER(ctypes.c_void_p.from_address(mode + 0x28).value)
        out = ctypes.c_void_p()
        getter(ctypes.addressof(out), mode, page)
        check(fake.boxed[-1:] == [0], "the Mode row reads Auto from KeyboardMouse.ini")
        check(ctypes.c_int.from_address(page + 0x10).value == 2, "the page object is held while the plugin tracks it")
        choices = GETTER(ctypes.c_void_p.from_address(props["KbmModeChoices"] + 0x28).value)
        getter_out = ctypes.c_void_p()
        choices(ctypes.addressof(getter_out), props["KbmModeChoices"], page)
        check(fake.items.get(getter_out.value) == ["Auto", "Keyboard & Mouse", "Gamepad"],
              f"the Mode row's choices ({fake.items.get(getter_out.value)})")
        setter = SETTER(ctypes.c_void_p.from_address(mode + 0x30).value)
        boxed = fake.new(0x30)
        struct.pack_into("<i", fake.objects[boxed], 0x28, 1)
        fake.events.clear()
        setter(mode, page, boxed)
        check(ini_value(plugin.ini, "Mode") == "KBM" and ("changed", page, mode) in fake.events,
              "choosing Keyboard & Mouse writes Mode=KBM and the row is read again")
        check(plugin.said("KBM: setting changed in the menu: Mode=KBM."), "and applies it at once")
        reset = PAGE_FN(qword(base + RESET_SLOT))
        fake.events.clear()
        events.clear()
        reset(page)
        check(ini_value(plugin.ini, "Mode") == "Auto" and not [e for e in events if e[0] == "extra options reset"] and
              len([e for e in fake.events if e[0] == "changed"]) == 9,
              "Reset on the KEYBOARD & MOUSE page resets its 9 options, not the Video settings")
        events.clear()
        reset(0x6160)
        check(events == [("extra options reset", 0x6160)], "Reset on any other page goes to Extra Options' reset")
        pause_enter = PAGE_FN(qword(base + 0x2D16698))
        events.clear()
        pause_enter(0x7170)
        check(events == [("extra options pause enter", 0x7170)], "PauseGame enter still runs Extra Options' Autodrive hook")
        dll.KbmTestUseHost(None)
        unmap(base)
        unmap(host)

    def scenario(label, ini_text, adapter=1, host_change=None, dll_path=None, expect="", eo_ini=None):
        dll = ctypes.CDLL(str(dll_path)) if dll_path else fresh_dll(output, label.replace(" ", "-").replace("=", ""))
        folder = work / label.replace(" ", "-").replace("=", "")
        plugin = Plugin(dll, folder, ini_text)
        if eo_ini is not None:
            (folder / "FH2_ExtraOptions.ini").write_text(eo_ini, encoding="utf-8")
        base = pe.map()
        host = host_pe.map()
        if not dll_path:
            if host_change:
                poke(host + host_change[0], host_change[1])
            dll.KbmTestUseHost(host)
            dll.KbmTestNoThread(1)
        before = snapshot(base)
        ready = plugin.initialize(base, adapter)
        after = snapshot(base)
        changed = sorted(name for name in before if before[name] != after[name])
        if not dll_path:
            dll.KbmTestUseHost(None)
        unmap(base)
        unmap(host)
        said = plugin.said(expect)
        if not said:
            for line in plugin.lines:
                print("    log:", line.rstrip()[:150])
        return ready, changed, said, dll

    ready, changed, said, _ = scenario("Enabled=0", "[KeyboardMouse]\nEnabled=0\n[Controller]\nPauseOnlyOnUnplug=0\n",
                                       expect="keyboard & mouse is off ([KeyboardMouse] Enabled=0)")
    check(ready == 1 and not changed and said, f"Enabled=0 and no pause fix: the game is left as it is ({changed})")
    ready, changed, said, _ = scenario("bad ini", "[KeyboardMouse]\nBogus=1\n",
                                       expect="KBM: REFUSED: [KeyboardMouse] Bogus=1: unknown key or invalid value")
    check(ready == 1 and changed == ["controller pause call"] and said,
          f"a refused [KeyboardMouse]: only the pause fix is installed, the log names the line ({changed})")
    stamp_at = host_pe.nt + 8
    ready, changed, said, _ = scenario("other host", shipped, host_change=(stamp_at, b"\x00\x00\x00\x00"),
                                       expect="the host is not Azimuth 1.0.4 or 1.0.2 (TimeDateStamp 0x00000000")
    check(ready == 1 and changed == ["controller pause call"] and said,
          f"another Azimuth build: keyboard & mouse refused with a log line, the pause fix stays ({changed})")
    window_use = 0x6DA54
    ready, changed, said, _ = scenario("changed host", shipped, host_change=(window_use, b"\x90"),
                                       expect="globals are not as inspected")
    check(ready == 1 and changed == ["controller pause call"] and said, f"a changed Azimuth 1.0.4 is refused ({changed})")
    ready, changed, said, _ = scenario("mezcal", shipped, adapter=2, expect="not an Azimuth adapter")
    check(ready == 1 and changed == ["controller pause call"] and said, f"a non-Azimuth adapter is refused ({changed})")
    ready, changed, said, _ = scenario("mode pad", shipped.replace("Mode=Auto", "Mode=PAD"),
                                       expect="Mode=PAD; the gamepad path is untouched")
    check(ready == 1 and "entry" not in changed and "Settings hub init" in changed and "XAML resource call" in changed,
          f"Mode=PAD: no input hook, but the tile and page (to switch back) ({changed})")
    ready, changed, said, _ = scenario("eo clash", "[KeyboardMouse]\nThrottle=F3\nTabRight=F4\n",
                                       eo_ini="[Bindings]\nHeadlightsKeyboard=F3\nRoofKeyboard=None\n",
                                       expect="KBM: warning: Throttle=F3 is also Extra Options' HeadlightsKeyboard")
    check(ready == 1 and said and "entry" in changed,
          "a key of ours that Extra Options' FH2_ExtraOptions.ini (found above the mods folder) also binds is warned about")
    shipping = output / "mods" / MOD_NAME / "plugins" / DLL_NAME
    ready, changed, said, dll = scenario("shipping", shipped, dll_path=shipping,
                                         expect="the host is not Azimuth 1.0.4 or 1.0.2")
    check(ready == 1 and changed == ["controller pause call"] and said,
          f"the shipping DLL in a process that is not Azimuth: keyboard & mouse refused, the pause fix on ({changed})")
    bad = Context(16, 1, 1, IMAGE_SIZE, None, None, None, None, None, LOG(0))
    dll.FH2PluginInitialize.argtypes = [ctypes.POINTER(Context)]
    check(dll.FH2PluginInitialize(ctypes.byref(bad)) == 0, "a short context is refused (FALSE)")
    shutil.rmtree(work, ignore_errors=True)


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    parser.add_argument("--zig", type=Path, default=Path(shutil.which("zig") or "zig"))
    parser.add_argument("--game", type=Path, default=Path.home() / "FH2")
    parser.add_argument("--azimuth", type=Path)
    parser.add_argument("--azimuth102", type=Path)
    parser.add_argument("--upstream", type=Path)
    parser.add_argument("--release", type=Path)
    parser.add_argument("--graphics", type=Path)
    options = parser.parse_args()
    output = options.output.resolve()
    # The top-level build.py builds Azimuth Graphics Options first, into out/build/azimuth-graphics-options.
    options.graphics = options.graphics or output.parent / "azimuth-graphics-options" / "tests" / "AzimuthGraphicsOptionsTest.dll"
    game = options.game
    runs = output / "test-runs"
    shutil.rmtree(runs, ignore_errors=True)
    runs.mkdir(parents=True, exist_ok=True)
    dll = fresh_dll(output, "main")
    test_package(output, dll)
    test_ini(output, dll)
    hosts = test_hosts(output, dll, {"1.0.4": options.azimuth or game / "azimuth-windows-1.0.4.zip",
                                     "1.0.2": options.azimuth102 or game / "azimuth-windows-1.0.2.zip"})
    test_release(dll, options.release or game / "src/eo-pr/upstream-0.4.4/FH2_Extra_Options_0.4.4-azimuth/Azimuth",
                 options.upstream or game / "src/eo-pr/wt-upstream", output)
    pe = test_game(output, game / GAME_EXE)
    hub, video, eo_page = test_xaml(output, dll, options.zig, game, options.upstream or game / "src/eo-pr/wt-upstream",
                                    options.graphics)
    test_texts(dll, hub, video)
    test_page(output)
    test_plugin(output, pe, hosts, hub, video, eo_page)
    print(f"{len(failures)} failure(s)" if failures else "all tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
