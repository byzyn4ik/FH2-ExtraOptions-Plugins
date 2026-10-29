"""Offline tests of the Azimuth Graphics Options plugin (no game run, nothing installed).

Usage: py tests/run_tests.py [--output out] [--zig zig.exe] [--game <FH2 folder>]
                             [--azimuth <azimuth-windows-1.0.4.zip>] [--azimuth102 <azimuth-windows-1.0.2.zip>]
                             [--eo-release <Extra Options 0.4.4 release folder>] [--eo-source <its source tree>]
                             [--upstream <Extra Options git clone>]

Real files are only read (the game's executable and UI.zip, Azimuth's ZIPs,
Extra Options' release and sources; data\\azimuth.ini is copied to a private
folder). Defaults below --game (default ~/FH2): azimuth-windows-1.0.4.zip,
azimuth-windows-1.0.2.zip, src/eo-pr/upstream-0.4.4/FH2_Extra_Options_0.4.4-azimuth/Azimuth
(the release: winmm.dll), src/eo-pr/wt-upstream (its source, which the release
was built from, read only), src/eo-pr/ForzaHorizon2_ExtraOptions (a clone with
origin/main = 0.3.2 and the fork's combined-10). A missing input skips the
tests that need it.

1. package  mod.json as Extra Options validates it, the plugin layout, the
            DLL (PE32+ x64, exports, only Windows imports), the INI.
2. host     Azimuth 1.0.4's azimuth.exe (and 1.0.2's, and the branded "Forza
            Horizon 2.exe") pass the host guard; every site is checked again
            here from the executable's bytes; the INI keys and values the
            plugin writes are the ones Azimuth reads; the frame rate choices
            match its launcher; other builds are refused.
3. game     the game executable passes the native guards (call site as the
            game has it and as Extra Options patches it).
4. eo       Extra Options 0.4.4: the release's winmm.dll holds every XAML text
            of its source's settings pages code and its patch of the resource
            call (to the thunk 0x13fe354); that code is built to extend the
            real pages.
5. xaml     the real Video and Audio pages (UI.zip), alone and as Extra
            Options 0.4.4 (and 0.3.2, the fork) extend them, then with this
            plugin's rows: well-formed XML, rows, order, list style, captions
            in English, Polish and Ukrainian; other pages are refused.
6. rows     data\\azimuth.ini (a copy): keys written, Azimuth's defaults, other
            lines kept; the caption language.
7. plugin   out/tests/plugin_test.exe drives FH2PluginInitialize and both pages
            (modes nohost, disabled, badhost, badgame, layout, native, eo,
            polish, ukrainian on 1.0.4, native on 1.0.2).
"""

import argparse
import configparser
import ctypes
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MOD_NAME = "Azimuth Graphics Options"
DLL_NAME = "AzimuthGraphicsOptions.dll"
INI_NAME = "AzimuthGraphicsOptions.ini"
SYSTEM_DLLS = re.compile(r"^(kernel32|user32|advapi32|shell32|ole32|ntdll|msvcrt|api-ms-win-[a-z0-9-]+)\.dll$", re.I)
GAME_EXE = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/forza_durango_release_final.exe")
UI_ZIP = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/media/UI.zip")
SHOWN = ["GfxFrameRate", "GfxVSync", "GfxResolution", "GfxDisplay", "GfxPreset", "GfxShadows",
         "GfxTextureFiltering", "GfxAntiAliasing", "GfxHdr"]
AUDIO = ["AzAudioOutput", "AzVolume"]
VIDEO, AUDIO_PAGE = 0, 1
EN, PL, UK = 0, 1, 2
ROW_AUDIO, ROW_VOLUME = 10, 11

# Azimuth builds: the sites the plugin uses (live.h), checked below from the bytes.
HOSTS = {
    "1.0.4": dict(timestamp=0x6ae393eb, image=0x8a2000, rate_list=0x10528, set_rates=0x4ba30, set_call=0x945f,
                  target=0x7d8554, title=0x7d8550, reader=0x4e8e7, toggle=0x6e868, fullscreen=0x7fcbc1,
                  alt_enter=0x6eda7, volume_setter=0xc37f0, volume_call=0x938b, volume=0x7da2e0),
    "1.0.2": dict(timestamp=0x6ae05dd1, image=0x9af000, rate_list=0x105f8, set_rates=0x4cc50, set_call=0x92f3,
                  target=0x8e7540, title=0x8e7544, reader=0x4fae7, toggle=0x6f5f8, fullscreen=0x909d61,
                  alt_enter=0x6fb27, volume_setter=0xbaea0, volume_call=0x921f, volume=0x8e7be8),
}
# The game settings keys Azimuth 1.x reads (its key table, the same in 1.0.2 and 1.0.4).
AZIMUTH_KEYS = ["aspect", "antialiasing", "resolution", "shadow_distance", "shadow_redraw", "graphics",
                "shadow_detail", "car_detail", "particles", "reflections", "scenery_detail", "audio", "volume",
                "motion_blur", "texture_filtering", "hdr", "language", "frame_rate", "vsync"]

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        failures.append(what)


def skip(what):
    print("  skip " + what)


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
            self.sections.append((va, vsize, rsize, raw))
        directories = optional + 112
        self.exports_rva, _ = struct.unpack_from("<II", d, directories)
        self.imports_rva, _ = struct.unpack_from("<II", d, directories + 8)
        self._image = None

    def offset(self, rva):
        for va, vsize, rsize, raw in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError(hex(rva))

    def image(self):
        """The image as the loader lays it out (bytes)."""
        if self._image is None:
            out = bytearray(self.image_size)
            out[:self.headers_size] = self.data[:self.headers_size]
            for va, vsize, rsize, raw in self.sections:
                size = min(rsize, vsize) if vsize else rsize
                out[va:va + size] = self.data[raw:raw + size]
            self._image = bytes(out)
        return self._image

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
        result, o = [], self.offset(self.imports_rva) if self.imports_rva else None
        while o is not None:
            fields = struct.unpack_from("<IIIII", self.data, o)
            if not fields[3]:
                break
            result.append(self.text(fields[3]))
            o += 20
        return result

    def map(self):
        """The image laid out as the loader maps it, in private RWX memory; returns its address."""
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.VirtualAlloc.restype = ctypes.c_void_p
        kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
        base = kernel32.VirtualAlloc(None, self.image_size, 0x3000, 0x40)
        image = self.image()
        ctypes.memmove(base, image, len(image))
        return base


def rip_target(image, at, length):
    """The target of the rip-relative disp32 that ends instruction `at`..`at+length` (disp at the end)."""
    return at + length + struct.unpack_from("<i", image, at + length - 4)[0]


def call_target(image, at):
    return at + 5 + struct.unpack_from("<i", image, at + 1)[0] if image[at] == 0xE8 else None


# --------------------------------------------------------------- 1. package

def valid_mod_id(value):
    # features/addons.c valid_id() in Extra Options.
    if not isinstance(value, str) or not 0 < len(value) <= 128:
        return False
    for i, ch in enumerate(value):
        if not (ch.islower() and ch.isascii() or ch.isdigit() or ch == "-"):
            return False
        if ch == "-" and (i == 0 or i == len(value) - 1 or value[i - 1] == "-"):
            return False
    return True


def test_package(output):
    print("package")
    mod = output / "mods" / MOD_NAME
    manifest = json.loads((mod / "mod.json").read_text(encoding="utf-8"))
    check(manifest.get("format_version") == 1, "mod.json format_version is 1")
    check(valid_mod_id(manifest.get("id")), f"mod.json id {manifest.get('id')!r} is a valid Extra Options mod ID")
    check(isinstance(manifest.get("dependencies", []), list), "mod.json dependencies is a list")
    check(not (mod / "database.json").exists() and not (mod / "strings.json").exists(), "a plugin-only mod")
    top = sorted(p.name for p in (mod / "plugins").iterdir())
    check(top == [DLL_NAME, INI_NAME], f"plugins folder holds the DLL and its INI only ({top})")
    pe = PE(mod / "plugins" / DLL_NAME)
    check(pe.machine == 0x8664 and pe.magic == 0x20B and bool(pe.characteristics & 0x2000), "the DLL is PE32+ x64")
    exports = pe.exports()
    check(exports == ["FH2PluginInitialize"], f"exports FH2PluginInitialize only ({exports})")
    imports = pe.imports()
    check(not [n for n in imports if not SYSTEM_DLLS.match(n)], f"only Windows DLLs imported ({', '.join(imports)})")
    test_exports = PE(output / "tests" / "AzimuthGraphicsOptionsTest.dll").exports()
    check("FH2PluginInitialize" in test_exports and "AzgTestExtendXaml" in test_exports,
          "the test build has the same entry point plus test entry points")
    ini = configparser.ConfigParser(interpolation=None, strict=True)
    ini.optionxform = str
    ini.read(mod / "plugins" / INI_NAME, encoding="utf-8")
    expected = {"Graphics": ["Enabled", "LiveApply", "AzimuthIni"], "Audio": ["Enabled"], "Captions": ["Language"],
                "Logging": ["Enabled"]}
    for section, keys in expected.items():
        check(ini.has_section(section) and all(ini.has_option(section, k) for k in keys),
              f"INI [{section}] has {', '.join(keys)}")
    check(ini.get("Graphics", "Enabled", fallback="") == "1" and ini.get("Audio", "Enabled", fallback="") == "1",
          "INI [Graphics] and [Audio] Enabled default to 1")


# ------------------------------------------------------------------ helpers

def load_test_dll(output):
    dll = ctypes.CDLL(str(output / "tests" / "AzimuthGraphicsOptionsTest.dll"))
    dll.AzgTestCheckHost.argtypes = [ctypes.c_void_p]
    dll.AzgTestCheckHost.restype = ctypes.c_char_p
    dll.AzgTestHostName.argtypes = [ctypes.c_void_p]
    dll.AzgTestHostName.restype = ctypes.c_char_p
    dll.AzgTestHostSite.argtypes = [ctypes.c_void_p, ctypes.c_uint]
    dll.AzgTestHostSite.restype = ctypes.c_uint32
    dll.AzgTestCheckGame.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    dll.AzgTestCheckGame.restype = ctypes.c_char_p
    dll.AzgTestExtendXaml.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p,
                                      ctypes.c_size_t, ctypes.c_int, ctypes.c_int]
    dll.AzgTestExtendXaml.restype = ctypes.c_size_t
    dll.AzgTestRowsFile.argtypes = [ctypes.c_wchar_p]
    dll.AzgTestRowsLayout.restype = ctypes.c_char_p
    dll.AzgTestRowGet.argtypes = [ctypes.c_uint]
    dll.AzgTestRowSet.argtypes = [ctypes.c_uint, ctypes.c_int]
    dll.AzgTestFrameRates.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_int), ctypes.c_int]
    dll.AzgTestSetFrameRates.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.c_int]
    dll.AzgTestRowChoice.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_int]
    dll.AzgTestRowChoice.restype = ctypes.c_wchar_p
    dll.AzgTestRowCount.argtypes = [ctypes.c_uint]
    dll.AzgTestRowCount.restype = ctypes.c_uint
    dll.AzgTestLanguage.argtypes = [ctypes.c_int, ctypes.c_int]
    dll.AzgTestCurrentLanguage.argtypes = [ctypes.c_void_p]
    dll.AzgTestConfigureLanguage.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
    return dll


ALL_LIVE = 31


def extend(dll, xaml, page=VIDEO, language=EN, live=ALL_LIVE):
    out = ctypes.create_string_buffer(len(xaml) + 0x10000)
    n = dll.AzgTestExtendXaml(page, xaml, len(xaml), out, len(out), language, live)
    return out.raw[:n] if n else b""


def parse(xaml):
    text = xaml.decode("utf-8-sig")
    ET.fromstring(text)
    return text


# ------------------------------------------------------------------ 2. host

def find_all(data, needle):
    at, out = data.find(needle), []
    while at >= 0:
        out.append(at)
        at = data.find(needle, at + 1)
    return out


def check_host_sites(name, pe, label):
    """Every site of the plugin's table for this build, from the executable's own bytes."""
    s, img = HOSTS[name], pe.image()
    steps = [struct.unpack_from("<I", img, s["rate_list"] + 4 + 8 * i)[0] for i in range(6)]
    check(steps == [60, 90, 120, 144, 165, 240] and b"\xc7\x44\x24\x20\x1e\x00\x00\x00" in
          img[s["rate_list"] - 0x40:s["rate_list"]],
          f"{label}: the launcher's frame rate steps (30 first, then {steps})")
    tail = img[s["rate_list"]:s["rate_list"] + 0xa0]
    check(b"\x3b\x72\xfc\x76" in tail and b"\x81\xfe\xe8\x03\x00\x00" in tail and b"\xbe\x3c\x00\x00\x00" in
          img[s["rate_list"] - 0x40:s["rate_list"]],
          f"{label}: then the display's refresh rate when higher (at most 1000), 60 when unknown")
    check(img[s["set_rates"]:s["set_rates"] + 2] == b"\x89\x0d" and rip_target(img, s["set_rates"], 6) == s["target"]
          and img[s["set_rates"] + 6:s["set_rates"] + 8] == b"\x89\x15"
          and rip_target(img, s["set_rates"] + 6, 6) == s["title"] and img[s["set_rates"] + 12] == 0xC3,
          f"{label}: {s['set_rates']:#x} stores the target rate at {s['target']:#x} (and the title's at {s['title']:#x})")
    check(call_target(img, s["set_call"]) == s["set_rates"]
          and img[s["set_call"] - 7:s["set_call"] - 4] == b"\x8b\x8c\x24",
          f"{label}: startup calls it at {s['set_call']:#x} with the title's frame_rate")
    check(img[s["reader"]:s["reader"] + 2] == b"\x39\x1d" and rip_target(img, s["reader"], 6) == s["target"]
          and img[s["reader"] - 8:s["reader"] - 3] == b"\xbb\x3c\x00\x00\x00",
          f"{label}: the pacing reads the target at {s['reader']:#x} for every frame")
    check(struct.unpack_from("<I", img, s["target"])[0] == 30 and struct.unpack_from("<I", img, s["volume"])[0] == 100,
          f"{label}: the target rate starts at 30 and the volume at 100")
    check(img[s["toggle"]:s["toggle"] + 2] == b"\x80\x3d" and img[s["toggle"] + 6] == 0
          and s["toggle"] + 7 + struct.unpack_from("<i", img, s["toggle"] + 2)[0] == s["fullscreen"],
          f"{label}: the Alt+Enter toggle reads the fullscreen byte {s['fullscreen']:#x}")
    check(img[s["alt_enter"]:s["alt_enter"] + 11] == bytes.fromhex("4883ff0d751d480fbae61d")
          and img[s["alt_enter"] + 11:s["alt_enter"] + 13] == b"\x73\x16",
          f"{label}: WM_SYSKEYDOWN VK_RETURN with Alt at {s['alt_enter']:#x}")
    v = s["volume_setter"]
    check(call_target(img, s["volume_call"]) == v and img[v + 0x42:v + 0x4c] == bytes.fromhex("b8640000003bd80f47d8")
          and img[v + 0x4c:v + 0x4e] == b"\x87\x1d" and rip_target(img, v + 0x4c, 6) == s["volume"]
          and img[s["volume_call"] - 7:s["volume_call"] - 4] == b"\x8b\x8c\x24",
          f"{label}: the volume setter {v:#x} clamps to 100 and stores {s['volume']:#x}; startup calls it at {s['volume_call']:#x}")
    # The in-game menu of Azimuth calls the same setter too.
    check(len([a for a in find_all(img, b"\xe8") if 0x1000 <= a < len(img) - 5 and call_target(img, a) == v][:3]) >= 2,
          f"{label}: Azimuth calls the volume setter from more than its startup (its in-game menu)")


def check_ini_reader(name, pe, label):
    """The data\\azimuth.ini keys and values the plugin writes, as Azimuth reads them."""
    img = pe.image()
    keys = []
    for key in AZIMUTH_KEYS + ["frame_rate", "audio", "volume", "display", "launcher"]:
        if (key.encode("utf-16-le") + b"\0\0") not in img:
            keys.append(key)
    check(not keys, f"{label}: Azimuth's key table has every game key and [launcher] display (missing {keys})")
    values = [v for v in ("stereo", "system", "multichannel", "msaax2", "msaax4", "windowed", "fullscreen", "custom",
                          "16x") if v.encode("utf-16-le") not in img and v.encode() not in img]
    check(not values, f"{label}: the values the rows write are Azimuth's (missing {values})")
    # frame_rate: 1..4 digits, 30..1000 ("add eax,-1Eh; cmp eax,3CAh"); volume: 0..100 ("cmp eax,64h").
    check(img.find(b"\x83\xc0\xe2\x3d\xca\x03\x00\x00") > 0 and img.find(b"\x83\xf8\x64\x77") > 0,
          f"{label}: frame_rate is read as 30..1000 and volume as 0..100")
    # The default audio output (1 = stereo) and volume (100) of the settings object.
    check(img.find(b"\xc7\x81\x80\x00\x00\x00\x64\x00\x00\x00") > 0 and img.find(b"\xc7\x41\x7c\x01\x00\x00\x00") > 0,
          f"{label}: Azimuth's defaults: audio stereo, volume 100")
    title = img.find("1a96545b".encode("utf-16-le"))
    record = img.find(struct.pack("<Q", pe.image_base + title)) if title > 0 else -1
    rate = struct.unpack_from("<I", img, record + 0x1c)[0] if record > 0 else None
    check(rate == 30, f"{label}: FH2's own frame rate in Azimuth's title table is {rate} (the default without frame_rate)")


def test_host(dll, executables):
    print("host")
    rates = (ctypes.c_int * 16)()
    expected = {60: [30, 60], 75: [30, 60, 75], 144: [30, 60, 90, 120, 144], 165: [30, 60, 90, 120, 144, 165],
                240: [30, 60, 90, 120, 144, 165, 240], 360: [30, 60, 90, 120, 144, 165, 240, 360],
                1: [30, 60], 0: [30, 60], 1200: [30, 60, 90, 120, 144, 165, 240]}
    for refresh, want in expected.items():
        n = dll.AzgTestFrameRates(refresh, rates, 16)
        check(list(rates[:n]) == want, f"Frame rate choices for a {refresh} Hz display: {list(rates[:n])}")
    keys = {}
    for name, label, path in executables:
        if not path or not Path(path).is_file():
            skip(f"{label} not found")
            continue
        pe = PE(path)
        base = pe.map()
        problem = dll.AzgTestCheckHost(base)
        got = dll.AzgTestHostName(base)
        check(problem is None and got == name.encode(),
              f"{label} (TimeDateStamp {pe.timestamp:#x}, image {pe.image_size:#x}) is Azimuth {name}"
              f"{'' if problem is None else ': ' + problem.decode()}")
        sites = HOSTS[name]
        plugin_sites = [dll.AzgTestHostSite(base, i) for i in range(5)]
        check(plugin_sites == [sites["target"], sites["volume_setter"], sites["fullscreen"], sites["rate_list"],
                               sites["volume"]], f"{label}: the plugin's sites are the ones checked here")
        check_host_sites(name, pe, label)
        check_ini_reader(name, pe, label)
        img = pe.image()
        table = img.find("frame_rate".encode("utf-16-le") + b"\0\0")
        keys[name] = sorted(set(re.findall(rb"(?:[a-z_]\x00){3,}\x00\x00", img[table - 0x200:table + 0x80])))
        # A changed launcher table, or another timestamp: refused.
        changed = bytearray(pe.data)
        changed[pe.offset(sites["rate_list"] + 4)] = 0x3D
        problem = dll.AzgTestCheckHost(PE(data=bytes(changed)).map())
        check(problem is not None and b"unknown build" in problem, f"{label} with a changed launcher list is refused")
        changed = bytearray(pe.data)
        e = struct.unpack_from("<I", changed, 0x3C)[0]
        struct.pack_into("<I", changed, e + 8, pe.timestamp + 1)
        problem = dll.AzgTestCheckHost(PE(data=bytes(changed)).map())
        check(problem is not None and b"is not Azimuth 1.0.4 or 1.0.2" in problem,
              f"another TimeDateStamp is refused ({problem.decode() if problem else None})")
    if len(keys) == 2:
        check(keys["1.0.4"] == keys["1.0.2"], "1.0.4 reads the same azimuth.ini keys as 1.0.2 (none new or renamed)")
    exe = PE(sys.executable)
    check(dll.AzgTestCheckHost(exe.map()) is not None, "another executable is refused")


# ------------------------------------------------------------------ 3. game

def test_game(dll, game_exe):
    print("game")
    if not game_exe.is_file():
        skip(f"{game_exe} not found")
        return None
    pe = PE(game_exe)
    base = pe.map()
    target = dll.AzgTestCheckGame(base, pe.image_size)
    check(target is not None and target.startswith(b"the game's resource reader 0xaabec0"),
          f"the game executable passes the guards; the call goes to {target.decode() if target else None}")
    # As Extra Options patches it: call 0x13fe354, a mov rax/jmp rax thunk there.
    thunk = (ctypes.c_ubyte * 12).from_address(base + 0x13fe354)
    hook = ctypes.cast(dll.AzgTestCheckHost, ctypes.c_void_p).value
    thunk[:] = list(b"\x48\xb8" + struct.pack("<Q", hook) + b"\xff\xe0")
    call = (ctypes.c_ubyte * 5).from_address(base + 0x16ebb37)
    call[:] = list(b"\xe8" + struct.pack("<i", 0x13fe354 - (0x16ebb37 + 5)))
    target = dll.AzgTestCheckGame(base, pe.image_size)
    check(target is not None and target.startswith(b"Extra Options' resource hook (thunk 0x13fe354"),
          f"with Extra Options' patch: {target.decode() if target else None}")
    for rva, what in ((0xb52170, b"InsertItem at 0xb52170 is not as expected"),
                      (0xc803c0, b"RegisterAudioType at 0xc803c0 is not as expected"),
                      (0xc804b9, b"Audio page type is not as expected"),
                      (0xed6efe, b"Gamma property template is not as expected")):
        byte = (ctypes.c_ubyte * 1).from_address(base + rva)
        byte[0] ^= 0xff
        problem = dll.AzgTestCheckGame(base, pe.image_size)
        byte[0] ^= 0xff
        check(problem is not None and what in problem, f"a changed game is refused ({problem.decode() if problem else None})")
    return base


# ------------------------------------------------------------------ 4. Extra Options

def build_eo_xaml(zig, files, folder):
    """Extra Options' settings pages code (support.c, video_options_xaml.c, video_options_text.c and headers) as a DLL."""
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    for name, data in files.items():
        (folder / name).write_bytes(data)
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
        # Functions of other features the file refers to: stubs (never called on these pages).
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


def c_literals(text):
    """C string literals (adjacent ones joined, escapes decoded) outside comments."""
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    out = []
    for m in re.finditer(r'(?:"(?:[^"\\\n]|\\.)*"\s*)+', text):
        raw = "".join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', m.group()))
        out.append(raw.encode("latin-1", "backslashreplace").decode("unicode_escape").encode("latin-1"))
    return out


def eo_extend(library, xaml):
    out = ctypes.create_string_buffer(len(xaml) + 0x20000)
    n = library.EoExtend(xaml, len(xaml), out, len(out))
    return out.raw[:n]


def test_eo(zig, release, source, output):
    """Extra Options 0.4.4: its release and the source it was built from; returns the XAML code (or None)."""
    print("eo")
    winmm = release / "winmm.dll" if release else None
    if not winmm or not winmm.is_file():
        skip("the Extra Options 0.4.4 release (winmm.dll) not found (--eo-release)")
        return None
    data = winmm.read_bytes()
    check(b"0.4.4" in data and b"supports the inspected Azimuth 1.0.1, 1.0.2, 1.0.3 and 1.0.4" in data,
          "the release is Extra Options 0.4.4 for Azimuth 1.0.1 ... 1.0.4")
    # Its patch of the resource call: original "call 0xaabec0", patched "call 0x13fe354" (the thunk).
    original = b"\xe8" + struct.pack("<i", 0xaabec0 - (0x16ebb37 + 5))
    patched = b"\xe8" + struct.pack("<i", 0x13fe354 - (0x16ebb37 + 5))
    check(original + patched in data, "its patch table points the resource call at 0x16ebb37 to the thunk 0x13fe354")
    check(b"<!-- EO:StandaloneOptionsPage -->" in data, "it skips plugins' standalone pages marked EO:StandaloneOptionsPage")
    if not source or not (source / "features" / "video_options_xaml.c").is_file():
        skip("the Extra Options 0.4.4 source not found (--eo-source)")
        return None
    xaml_c = (source / "features" / "video_options_xaml.c").read_text(encoding="utf-8")
    literals = [l for l in c_literals(xaml_c) if len(l) >= 12 and not l.endswith(b".h")]
    missing = [l for l in literals if l not in data]
    check(literals and not missing, f"the release holds all {len(literals)} XAML texts of the source's settings pages "
                                    f"code{'' if not missing else ' (missing ' + repr(missing[:3]) + ')'}")
    for name in (b"Master Volume", b"Ben's Photo Mode Dialogue", b"Checkpoint Sound Effect", b"Real-time Shadows"):
        check(name in data, f"the release has the caption {name.decode()!r}")
    files = {}
    for folder in (source, source / "features", source / "adapters"):
        for header in folder.glob("*.h"):
            files[header.name] = header.read_bytes()
    for name in ("support.c", "features/video_options_xaml.c", "features/video_options_text.c"):
        files[Path(name).name] = (source / name).read_bytes()
    library = build_eo_xaml(zig, files, output / "obj" / "eo-0.4.4")
    check(library is not None, "Extra Options 0.4.4's settings pages code builds for the test")
    return library


# ------------------------------------------------------------------ 5. xaml

def check_video(text, label, eo_rows):
    rows = re.findall(r'SelectedIndex="\{Binding (Gfx\w+)\}"', text)
    check(rows == SHOWN, f"{label}: the 9 Azimuth rows in order, no Frame pacing ({len(rows)})")
    check(all(text.count(f'x:Key="AZG_{n}"') == 1 for n in SHOWN), f"{label}: one TileMenuDataItem per row")
    list_start = text.index('<local:CommonListBox x:Name="OptionsList"')
    opening = text[list_start:text.index(">", list_start)]
    check('A_Style_ListBox_Scrollable' in opening and 'Height="440"' in opening,
          f"{label}: the options list is scrollable, 440 px")
    check(text.index("{Binding Subtitles}") < text.index("{Binding GfxFrameRate}"),
          f"{label}: the rows come after the page's own Brightness and Subtitles")
    if eo_rows:
        eo = text.index("{Binding RealtimeShadows}")
        check(text.index("{Binding GfxHdr}") < eo, f"{label}: and before Extra Options' rows (Realtime shadows ...)")
        check(all(f"{{Binding {n}}}" in text for n in ("PhotoSamplesPreset", "LiveryResolution", "LiveryMsaa")),
              f"{label}: Extra Options' Video rows are all kept")
    check("AzAudioOutput" not in text, f"{label}: no audio rows on the Video page")


def check_audio(text, label, eo_rows):
    list_start = text.index('<local:CommonListBox x:Name="OptionsList"')
    list_end = text.index("</local:CommonListBox>")
    body = text[list_start:list_end]
    bindings = re.findall(r'(?:SelectedIndex|Value)="\{Binding (\w+)\}"', body)
    check(bindings[-2:] == AUDIO, f"{label}: audio output and volume are the last rows ({bindings[-3:]})")
    check(all(text.count(f'x:Key="AZG_{n}"') == 1 for n in AUDIO) and 'x:Key="AZG_VolumeSlider"' in text,
          f"{label}: one TileMenuDataItem per row and the volume slider style")
    check('<Slider Value="{Binding AzVolume}" Tag="{StaticResource AZG_AzVolume}" '
          'Style="{StaticResource AZG_VolumeSlider}"/>' in body, f"{label}: volume is a slider")
    opening = text[list_start:text.index(">", list_start)]
    check('A_Style_ListBox_Scrollable' in opening and 'Height="550"' in opening,
          f"{label}: the options list is scrollable, 550 px")
    native = ["AudioPreset", "RadioVolume", "SFXVolume", "VOVolume", "CarVolume", "TireVolume", "GPSVoice", "RadioDJ",
              "XboxMusic"]
    check(all(n in bindings for n in native), f"{label}: the page's own rows are kept")
    if eo_rows:
        eo = ["MasterVolume", "BenPhotoDialogue", "AshleyGarageDialogue", "CheckpointSound"]
        check(all(n in bindings for n in eo) and bindings.index("CheckpointSound") < bindings.index("AzAudioOutput"),
              f"{label}: Extra Options' Audio rows are kept, ours after them")
    check("GfxFrameRate" not in text, f"{label}: no video rows on the Audio page")


def test_xaml(dll, zig, game, eo044, upstream, output):
    print("xaml")
    ui = game / UI_ZIP
    if not ui.is_file():
        skip(f"{ui} not found")
        return None, None, None
    with zipfile.ZipFile(ui) as archive:
        video = archive.read("Scenes/Anthem/A_VideoOptions.xaml")
        audio = archive.read("Scenes/Anthem/A_AudioOptions.xaml")
    ours = extend(dll, video)
    check(bool(ours) and ours.startswith(b"\xef\xbb\xbf"), "the game's Video page gets the rows (UTF-8 with BOM)")
    text = parse(ours)
    check_video(text, "game Video page", False)
    check(not extend(dll, ours), "a Video page that already has the rows is left alone")
    check('Description="Target frame rate. Applies at once."' in text, "live descriptions say what applies at once")
    restart = parse(extend(dll, video, live=0))
    check("Target frame rate. Restart the game to apply." in restart, "without live apply they say restart")
    polish = parse(extend(dll, video, language=PL))
    check('Label="Liczba klatek"' in polish and 'Label="Cienie"' in polish, "Polish Video captions")
    ukrainian = parse(extend(dll, video, language=UK))
    check('Label="Частота кадрів"' in ukrainian and 'Label="Тіні"' in ukrainian and "Діє одразу." in ukrainian,
          "Ukrainian Video captions")
    built = video.replace(b'Content="{local:Loc ScreenTitles.IDS_VideoOptions}"', b'Content="KEYBOARD"')
    check(not extend(dll, built), "a page built from the Video page (other title) is refused")
    hidden = video.replace(b'<StackPanel Margin="0, 0, 0, 6" Orientation="Horizontal"',
                           b'<StackPanel Visibility="Collapsed" Margin="0, 0, 0, 6" Orientation="Horizontal"')
    check(hidden != video and not extend(dll, hidden),
          "a page with the brightness preview collapsed (the fork's World/KBM pages) is refused")
    check(not extend(dll, b"<!-- EO:StandaloneOptionsPage -->" + video), "a plugin's standalone Video-class page is refused")
    check(not extend(dll, b'<Page x:Class="ForzaUI.A_HudOptions"/>'), "another page is refused")
    check(not extend(dll, audio), "the Audio page is not the Video page")

    ours = extend(dll, audio, page=AUDIO_PAGE)
    check(bool(ours) and ours.startswith(b"\xef\xbb\xbf"), "the game's Audio page gets the rows (UTF-8 with BOM)")
    text = parse(ours)
    check_audio(text, "game Audio page", False)
    check(not extend(dll, ours, page=AUDIO_PAGE), "an Audio page that already has the rows is left alone")
    check("Restart the game to apply." in text and "Applies at once." in text,
          "audio output says restart, volume says at once")
    check("Volume (Azimuth)" in text and "Audio output (Azimuth)" in text, "English Audio captions")
    check("Wyjście audio (Azimuth)" in parse(extend(dll, audio, page=AUDIO_PAGE, language=PL)), "Polish Audio captions")
    check("Аудіовихід (Azimuth)" in parse(extend(dll, audio, page=AUDIO_PAGE, language=UK)), "Ukrainian Audio captions")
    check(not extend(dll, b"<!-- EO:StandaloneOptionsPage -->" + audio, page=AUDIO_PAGE),
          "a plugin's standalone Audio-class page is refused")
    check(not extend(dll, video, page=AUDIO_PAGE), "the Video page is not the Audio page")

    pages = {}
    builds = []
    if eo044:
        builds.append(("Extra Options 0.4.4", eo044))
    else:
        skip("Extra Options 0.4.4's pages (no release/source)")
    if upstream and (upstream / ".git").exists():
        for ref in ("origin/main", "combined-10"):
            have = subprocess.run(["git", "-C", str(upstream), "rev-parse", "--verify", "-q", ref], capture_output=True)
            if have.returncode:
                skip(f"{ref} not in {upstream}")
                continue
            names = subprocess.run(["git", "-C", str(upstream), "ls-tree", "-r", "--name-only", ref],
                                   capture_output=True, text=True, check=True).stdout.split()
            files = {}
            for name in names:
                if name.endswith(".h") or name in ("support.c", "features/video_options_xaml.c",
                                                   "features/video_options_text.c"):
                    files[Path(name).name] = subprocess.run(["git", "-C", str(upstream), "show", f"{ref}:{name}"],
                                                            capture_output=True, check=True).stdout
            library = build_eo_xaml(zig, files, output / "obj" / ("eo-" + ref.replace("/", "-")))
            check(library is not None, f"Extra Options' settings pages code at {ref} builds for the test")
            if library:
                builds.append((f"Extra Options {ref}", library))
    else:
        skip("Extra Options git clone not found (--upstream)")
    for label, library in builds:
        for page, native, checker, name in ((VIDEO, video, check_video, "Video"), (AUDIO_PAGE, audio, check_audio, "Audio")):
            extended = eo_extend(library, native)
            if not extended:
                if page == AUDIO_PAGE and label != "Extra Options 0.4.4":
                    continue  # older Extra Options leaves the Audio page alone
                check(False, f"{label} extends the game's {name} page")
                continue
            check(True, f"{label} extends the game's {name} page")
            parse(extended)
            both = extend(dll, extended, page=page)
            check(bool(both), f"{label}: the plugin adds its rows to its {name} page")
            if both:
                checker(parse(both), f"{label} {name} page", page == VIDEO or label == "Extra Options 0.4.4")
                check(not eo_extend(library, both), f"{label} leaves an already extended {name} page alone "
                                                    "(the plugin runs after it)")
            if label == "Extra Options 0.4.4":
                pages[page] = extended
    (output / "test-runs").mkdir(parents=True, exist_ok=True)
    for page, name in ((VIDEO, "eo_video.xaml"), (AUDIO_PAGE, "eo_audio.xaml")):
        if page in pages:
            (output / "test-runs" / name).write_bytes(pages[page])
    return video, audio, pages


# ------------------------------------------------------------------ 6. rows

SAMPLE_INI = (b"[launcher]\ngame=1a96545b\ndisplay=fullscreen\nhdr_peak=1000\n"
              b"[title-1a96545b]\nfolder=Anthem\nresolution=2160\nantialiasing=msaax4\ngraphics=ultra\n"
              b"shadow_detail=ultra\nshadow_distance=ultra\nshadow_redraw=ultra\ntexture_filtering=16x\n"
              b"audio=stereo\nframe_rate=120\nvsync=on\nhdr=on\nlanguage=en-US\n")


def azimuth_ini_copy(game):
    real = game / "data" / "azimuth.ini"
    return real.read_bytes() if real.is_file() else SAMPLE_INI


def set_key(data, key, value):
    if re.search(rb"(?m)^" + key + rb"=", data):
        return re.sub(rb"(?m)^" + key + rb"=[^\r\n]*", key + b"=" + value, data)
    return data.replace(b"[title-1a96545b]", b"[title-1a96545b]\r\n" + key + b"=" + value, 1)


def drop_key(data, key):
    return re.sub(rb"(?m)^" + key + rb"=[^\r\n]*\r?\n", b"", data)


def test_rows(dll, game, output, game_base):
    print("rows")
    folder = output / "test-runs" / "rows"
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    ini = folder / "azimuth.ini"
    original = azimuth_ini_copy(game)
    ini.write_bytes(original)
    print(f"  (a copy of {'the real data/azimuth.ini' if original != SAMPLE_INI else 'a sample azimuth.ini'})")
    dll.AzgTestRowsFile(str(ini))
    check(dll.AzgTestRowsLayout() == b"", "Azimuth 1.x layout ([launcher] and [title-1a96545b])")
    rates = (ctypes.c_int * 4)(30, 60, 90, 120)
    dll.AzgTestSetFrameRates(rates, 4)
    check(dll.AzgTestRowCount(0) == 4 and dll.AzgTestRowChoice(0, 3, EN) == "120 FPS", "Frame rate labels")
    lines = original.decode("utf-8", "replace").splitlines()
    frame = next((l.split("=", 1)[1] for l in lines if l.startswith("frame_rate=")), "30")
    check(dll.AzgTestRowGet(0) == max(i for i, r in enumerate(rates) if r <= int(frame)),
          f"Frame rate shows the saved {frame} (or the step below it)")
    ini.write_bytes(set_key(original, b"frame_rate", b"100"))
    check(dll.AzgTestRowGet(0) == 2, "a saved 100 shows as 90, as Azimuth's launcher does")
    check(dll.AzgTestRowSet(0, 2) == 1 and b"frame_rate=90" in ini.read_bytes(),
          "choosing 90 then writes 90 (the saved 100 was not a choice)")
    check(dll.AzgTestRowSet(0, 2) == 0, "choosing 90 again writes nothing")
    ini.write_bytes(drop_key(original, b"frame_rate"))
    check(dll.AzgTestRowGet(0) == 0, "without frame_rate: 30 FPS, the title's own rate (Azimuth's default)")
    ini.write_bytes(set_key(original, b"frame_rate", b"12345"))
    check(dll.AzgTestRowGet(0) == 0, "a frame_rate Azimuth refuses (over 4 digits): 30 FPS")
    ini.write_bytes(original)
    check(dll.AzgTestRowSet(5, 2) == 1 and dll.AzgTestRowSet(6, 0) == 1 and dll.AzgTestRowSet(4, 0) == 1,
          "Graphics High, Shadows Low, Display Windowed")
    now = ini.read_bytes().decode("utf-8", "replace")
    section = now[now.index("[title-1a96545b]"):]
    check("shadow_detail=low" in section and "shadow_distance=low" in section and "shadow_redraw=low" in section
          and "graphics=custom" in section, "Shadows writes the three shadow keys and Graphics = Custom")
    check("display=windowed" in now[:now.index("[title-1a96545b]")], "Display mode is in [launcher]")
    changed = {"shadow_detail", "shadow_distance", "shadow_redraw", "graphics", "display"}
    kept = [l for l in lines if l.split("=", 1)[0] not in changed]
    check(all(l in now.splitlines() for l in kept), "every other line of azimuth.ini is kept")
    check(dll.AzgTestRowSet(2, 0) == 0, "Frame pacing (not in Azimuth 1.x) cannot be set")

    # Audio output and volume.
    check(dll.AzgTestRowCount(ROW_AUDIO) == 3 and [dll.AzgTestRowChoice(ROW_AUDIO, i, EN) for i in range(3)] ==
          ["Stereo", "System (Windows)", "Multichannel"], "Audio output choices")
    for value, index in ((b"stereo", 0), (b"system", 1), (b"multichannel", 2), (b"surround", 0)):
        ini.write_bytes(set_key(original, b"audio", value))
        check(dll.AzgTestRowGet(ROW_AUDIO) == index, f"audio={value.decode()} shows choice {index}")
    ini.write_bytes(drop_key(original, b"audio"))
    check(dll.AzgTestRowGet(ROW_AUDIO) == 0, "without audio: Stereo (Azimuth's default)")
    for index, value in ((2, "multichannel"), (1, "system"), (0, "stereo")):
        check(dll.AzgTestRowSet(ROW_AUDIO, index) == 1 and f"audio={value}" in ini.read_text(encoding="utf-8"),
              f"choice {index} writes audio={value}")
    check(dll.AzgTestRowSet(ROW_AUDIO, 3) == 0, "an audio choice out of range is refused")
    ini.write_bytes(drop_key(original, b"volume"))
    check(dll.AzgTestRowGet(ROW_VOLUME) == 100, "without volume: 100 (Azimuth's default)")
    check(dll.AzgTestRowSet(ROW_VOLUME, 35) == 1 and "volume=35" in ini.read_text(encoding="utf-8")
          and dll.AzgTestRowGet(ROW_VOLUME) == 35, "volume 35 is written and read back")
    check(dll.AzgTestRowSet(ROW_VOLUME, 35) == 0 and dll.AzgTestRowSet(ROW_VOLUME, 101) == 0
          and dll.AzgTestRowSet(ROW_VOLUME, -1) == 0, "the same volume, 101 and -1 write nothing")
    ini.write_bytes(set_key(original, b"volume", b"250"))
    check(dll.AzgTestRowGet(ROW_VOLUME) == 100, "a volume Azimuth refuses (over 100) shows 100")
    after = ini.read_bytes()
    check(dll.AzgTestRowSet(ROW_VOLUME, 100) == 1 and b"volume=100" in ini.read_bytes() and after != ini.read_bytes(),
          "choosing 100 over a refused value writes 100")
    check(dll.AzgTestRowChoice(ROW_VOLUME, 0, EN) is None, "volume is a slider (no choices)")
    (folder / "old.ini").write_bytes(b"[launcher]\nframe_rate=60\n")
    dll.AzgTestRowsFile(str(folder / "old.ini"))
    check(b"title-1a96545b" in dll.AzgTestRowsLayout(), "an azimuth.ini without the title section is refused")

    print("language")
    if game_base:
        manager = ctypes.create_string_buffer(0x60)
        slot = (ctypes.c_uint64).from_address(game_base + 0x3662990)
        slot.value = ctypes.addressof(manager)

        def game_text(code):
            ctypes.memset(ctypes.addressof(manager) + 0x30, 0, 16)
            ctypes.memmove(ctypes.addressof(manager) + 0x30, code, len(code))
            struct.pack_into("<QQ", manager, 0x40, len(code), 15)

        cases = [(b"EN", 0, 0, EN), (b"PL", 0, 0, PL), (b"RU", 0, 0, EN), (b"RU", 0, 1, UK), (b"DE", 0, 1, EN),
                 (b"PL", 0, 1, PL), (b"EN", 3, 0, UK), (b"PL", 1, 0, EN), (b"RU", 2, 1, PL)]
        for code, choice, pack, want in cases:
            game_text(code)
            dll.AzgTestLanguage(choice, pack)
            got = dll.AzgTestCurrentLanguage(game_base)
            check(got == want, f"game text {code.decode()}, [Captions] {['auto', 'en', 'pl', 'uk'][choice]}, "
                               f"{'a' if pack else 'no'} Ukrainian pack: {['English', 'Polish', 'Ukrainian'][got]}")
        dll.AzgTestLanguage(0, 0)
        slot.value = 0
    else:
        skip("the caption language from the game's text (needs the game executable)")
    launcher, mount = folder / "launcher", folder / "Mount"
    (mount / "languages" / "uk").mkdir(parents=True)
    launcher.mkdir()
    (launcher / "FH2_ExtraOptions.ini").write_text("[Language]\nCustom=uk\n", encoding="utf-16")
    (mount / "languages" / "uk" / "language.ini").write_text("[Language]\nName=Українська\nSlot=ru-RU\n",
                                                            encoding="utf-16")
    check(dll.AzgTestConfigureLanguage(str(launcher), str(mount)) == 1,
          "the fork's custom language uk in the ru-RU slot is found (Ui defaults to the id)")
    (mount / "languages" / "uk" / "language.ini").write_text("[Language]\nSlot=pl-PL\n", encoding="utf-16")
    check(dll.AzgTestConfigureLanguage(str(launcher), str(mount)) == 0, "a pack in another slot is not Ukrainian")
    (launcher / "FH2_ExtraOptions.ini").write_text("[Language]\nCustom=\n", encoding="utf-16")
    check(dll.AzgTestConfigureLanguage(str(launcher), str(mount)) == 0, "no custom language: not Ukrainian")
    dll.AzgTestLanguage(0, 0)


# ----------------------------------------------------------------- 7. plugin

def plugin_ini(data):
    data = set_key(data, b"frame_rate", b"120")  # plugin_test expects 120 FPS, stereo, volume 80
    data = set_key(data, b"audio", b"stereo")
    return set_key(data, b"volume", b"80")


def test_plugin(output, game_exe, hosts, video, audio, eo_pages, game):
    print("plugin")
    exe = output / "tests" / "plugin_test.exe"
    runs = output / "test-runs"
    native_video, native_audio = runs / "native_video.xaml", runs / "native_audio.xaml"
    if video:
        native_video.write_bytes(video)
        native_audio.write_bytes(audio)
    plan = [(m, "1.0.4") for m in ("nohost", "disabled", "badhost", "badgame", "layout", "native", "eo", "polish",
                                   "ukrainian")]
    plan += [("native", "1.0.2"), ("badhost", "1.0.2")]
    for mode, version in plan:
        shipping = mode in ("nohost", "disabled")
        host = hosts.get(version)
        name = mode if version == "1.0.4" else f"{mode}-{version}"
        if not shipping and not (host and game_exe.is_file()):
            skip(f"plugin_test {name}: needs the game and Azimuth {version} executables")
            continue
        flow = mode in ("native", "eo", "polish", "ukrainian")
        if flow and not (video and (mode != "eo" or (VIDEO in eo_pages and AUDIO_PAGE in eo_pages))):
            skip(f"plugin_test {name}: needs the {'Extra Options 0.4.4 ' if mode == 'eo' else ''}pages")
            continue
        work = runs / name
        shutil.rmtree(work, ignore_errors=True)
        plugins = work / "mods" / MOD_NAME / "plugins"
        plugins.mkdir(parents=True)
        dll = output / ("mods/" + MOD_NAME + "/plugins/" + DLL_NAME if shipping else "tests/AzimuthGraphicsOptionsTest.dll")
        shutil.copy2(dll, plugins / DLL_NAME)
        ini = work / "azimuth.ini"
        data = plugin_ini(azimuth_ini_copy(game))
        if mode == "layout":
            data = b"[launcher]\ndisplay=fullscreen\n"
        ini.write_bytes(data)
        enabled = 0 if mode == "disabled" else 1
        (plugins / INI_NAME).write_text(
            f"[Graphics]\nEnabled={enabled}\nLiveApply=1\nAzimuthIni={ini}\n[Audio]\nEnabled={enabled}\n"
            f"[Captions]\nLanguage={'uk' if mode == 'ukrainian' else 'auto'}\n[Logging]\nEnabled=1\n", encoding="utf-8")
        args = [str(exe), mode, str(plugins / DLL_NAME), str(work)]
        if not shipping:
            args += [str(game_exe), str(host)]
            if flow:
                if mode == "eo":
                    (work / "eo_video.xaml").write_bytes(eo_pages[VIDEO])
                    (work / "eo_audio.xaml").write_bytes(eo_pages[AUDIO_PAGE])
                    args += [str(work / "eo_video.xaml"), str(work / "eo_audio.xaml")]
                else:
                    args += [str(native_video), str(native_audio)]
        try:
            run = subprocess.run(args, capture_output=True, text=True, timeout=300, encoding="utf-8", errors="replace")
            print(run.stdout.rstrip())
            if run.stderr:
                print(run.stderr.rstrip())
            check(run.returncode == 0, f"plugin_test {name}")
        except subprocess.TimeoutExpired:
            check(False, f"plugin_test {name} finished in time")
        own_log = plugins / "AzimuthGraphicsOptions.log"
        check(own_log.is_file(), f"{name}: AzimuthGraphicsOptions.log is written")
        if flow and (work / "video_out.xaml").is_file() and (work / "audio_out.xaml").is_file():
            check_video(parse((work / "video_out.xaml").read_bytes()), f"{name} (through the hook)", mode == "eo")
            check_audio(parse((work / "audio_out.xaml").read_bytes()), f"{name} (through the hook)", mode == "eo")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    parser.add_argument("--zig", type=Path, default=Path(shutil.which("zig") or "zig"))
    parser.add_argument("--game", type=Path, default=Path.home() / "FH2")
    parser.add_argument("--azimuth", type=Path, help="Azimuth 1.0.4's release ZIP")
    parser.add_argument("--azimuth102", type=Path, help="Azimuth 1.0.2's release ZIP")
    parser.add_argument("--eo-release", type=Path, help="Extra Options 0.4.4's release folder (with winmm.dll)")
    parser.add_argument("--eo-source", type=Path, help="Extra Options 0.4.4's source tree")
    parser.add_argument("--upstream", type=Path, help="an Extra Options git clone (origin/main = 0.3.2)")
    options = parser.parse_args()
    output = options.output.resolve()
    game = options.game
    eo_pr = game / "src" / "eo-pr"
    zips = {"1.0.4": options.azimuth or game / "azimuth-windows-1.0.4.zip",
            "1.0.2": options.azimuth102 or game / "azimuth-windows-1.0.2.zip"}
    release = options.eo_release or eo_pr / "upstream-0.4.4" / "FH2_Extra_Options_0.4.4-azimuth" / "Azimuth"
    source = options.eo_source or eo_pr / "wt-upstream"
    upstream = options.upstream or eo_pr / "ForzaHorizon2_ExtraOptions"
    hosts = {}
    for version, path in zips.items():
        if path.is_file():
            exe = output / "test-runs" / f"azimuth-{version}" / "azimuth.exe"
            exe.parent.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(path) as archive:
                exe.write_bytes(archive.read("azimuth.exe"))
            hosts[version] = exe
    test_package(output)
    dll = load_test_dll(output)
    test_host(dll, [("1.0.4", "azimuth.exe (1.0.4 release)", hosts.get("1.0.4")),
                    ("1.0.2", "azimuth.exe (1.0.2 release)", hosts.get("1.0.2")),
                    ("1.0.2", "branded Forza Horizon 2.exe (1.0.2)", game / "Forza Horizon 2.exe")])
    game_base = test_game(dll, game / GAME_EXE)
    eo044 = test_eo(options.zig, release, source, output)
    video, audio, eo_pages = test_xaml(dll, options.zig, game, eo044, upstream, output)
    test_rows(dll, game, output, game_base)
    test_plugin(output, game / GAME_EXE, hosts, video, audio, eo_pages or {}, game)
    print(f"{len(failures)} failure(s)" if failures else "all tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
