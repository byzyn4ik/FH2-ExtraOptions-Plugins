"""Offline tests of the Custom Radio plugin (no game, nothing installed).

Usage: py tests/run_tests.py [--output out] [--game-exe <forza_durango_release_final.exe>]
                             [--extra-options <folder with winmm.dll and mods\\>]

1. package: mod.json as Extra Options validates it, the plugin layout, the DLL
   (PE32+ x64, exactly the loader's exports, only Windows imports), the INI.
2. sites: src/sites.h is well formed; with --game-exe (or FH2_GAME_EXE) every
   guard byte is checked against the game exe and none of them is relocated.
3. coexistence: the default keys are not Extra Options 0.4.4's; with
   --extra-options (or EO_RELEASE_DIR) its INI bindings and binaries are
   checked too (no default key and no hook site in common).
4. config: CustomRadio.ini parsing (defaults, valid values, bad values and
   unknown keys logged, relative/absolute folders, key and button names).
5. keys: one action per press, only with input enabled, release before rearm.
6. mp3: generated silent MP3s: ID3v2/ID3v1 tags, Xing and CBR lengths, and
   the Windows decode to a 16-bit PCM WAV.
7. plugin: the test DLL loaded with a fake game image (built from sites.h) in
   a child process per scenario: vanilla sites, sites already hooked by
   another module (chained: XML injection, sound/subsound serving, station
   lock, keys through FH2PluginVehicleUpdate), mismatches in the core and in
   an optional group, Enabled=0, UnlockAllStations alone, wrong image size.
"""

import argparse
import configparser
import ctypes
from ctypes import wintypes
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MOD_NAME = "Custom Radio"
DLL_NAME = "CustomRadio.dll"
INI_NAME = "CustomRadio.ini"
GAME_IMAGE_SIZE = 0x3A18000
SYSTEM_DLLS = re.compile(r"^(kernel32|user32|msvcrt|ntdll|api-ms-win-[a-z0-9-]+)\.dll$", re.I)
VK_F1 = 0x70

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)
    return ok


# ------------------------------------------------------------------ PE reader

class PE:
    def __init__(self, path=None, data=None):
        self.data = data if data is not None else Path(path).read_bytes()
        d = self.data
        e = struct.unpack_from("<I", d, 0x3C)[0]
        if d[:2] != b"MZ" or d[e:e + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        self.machine, count, self.timestamp, _, _, optional_size, self.characteristics = \
            struct.unpack_from("<HHIIIHH", d, e + 4)
        optional = e + 24
        self.magic = struct.unpack_from("<H", d, optional)[0]
        self.image_base = struct.unpack_from("<Q", d, optional + 24)[0]
        self.image_size = struct.unpack_from("<I", d, optional + 56)[0]
        self.sections = []
        table = optional + optional_size
        for i in range(count):
            name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, table + 40 * i)
            self.sections.append((va, vsize, rsize, raw))
        directories = optional + 112
        self.directories = [struct.unpack_from("<II", d, directories + 8 * i) for i in range(16)]

    def offset(self, rva):
        for va, vsize, rsize, raw in self.sections:
            if va <= rva < va + max(vsize, rsize):
                if rva - va >= rsize:
                    return None
                return rva - va + raw
        raise ValueError(hex(rva))

    def read(self, rva, size):
        out = bytearray()
        for i in range(size):
            o = self.offset(rva + i)
            out.append(self.data[o] if o is not None else 0)
        return bytes(out)

    def text(self, rva):
        o = self.offset(rva)
        return self.data[o:self.data.index(b"\0", o)].decode("ascii")

    def exports(self):
        rva = self.directories[0][0]
        if not rva:
            return []
        o = self.offset(rva)
        count, names = struct.unpack_from("<I", self.data, o + 24)[0], struct.unpack_from("<I", self.data, o + 32)[0]
        return [self.text(struct.unpack_from("<I", self.data, self.offset(names) + 4 * i)[0]) for i in range(count)]

    def imports(self):
        result = {}
        rva = self.directories[1][0]
        o = self.offset(rva) if rva else None
        while o is not None:
            fields = struct.unpack_from("<IIIII", self.data, o)
            if not fields[3]:
                break
            names, thunk = [], self.offset(fields[0] or fields[4])
            while True:
                value = struct.unpack_from("<Q", self.data, thunk)[0]
                if not value:
                    break
                names.append(self.text(value + 2) if not value >> 63 else f"#{value & 0xffff}")
                thunk += 8
            result[self.text(fields[3])] = names
            o += 20
        return result

    def relocations(self):
        """RVAs of 64-bit relocated fields."""
        rva, size = self.directories[5]
        out = set()
        at = 0
        while at < size:
            page, block = struct.unpack("<II", self.read(rva + at, 8))
            if block < 8:
                break
            entries = self.read(rva + at + 8, block - 8)
            for i in range(0, len(entries), 2):
                value = struct.unpack_from("<H", entries, i)[0]
                if value >> 12 == 10:
                    out.add(page + (value & 0xFFF))
            at += block
        return out


# --------------------------------------------------------------- sites.h

def c_bytes(text):
    return bytes(int(h, 16) for h in re.findall(r"\\x([0-9a-fA-F]{2})", text))


def load_sites():
    source = (ROOT / "src" / "sites.h").read_text(encoding="utf-8")
    functions, slots, calls, data = {}, {}, {}, {}
    for name, rva, body in re.findall(r'X\((FN_\w+),\s*(0x[0-9a-fA-F]+),\s*"([^"]*)"\)', source):
        functions[name] = (int(rva, 16), c_bytes(body))
    for name, rva, function in re.findall(r"X\((SLOT_\w+),\s*(0x[0-9a-fA-F]+),\s*(FN_\w+)\)", source):
        slots[name] = (int(rva, 16), function)
    for name, rva, function, before, after in re.findall(
            r'X\((CALL_\w+),\s*(0x[0-9a-fA-F]+),\s*(FN_\w+),\s*"([^"]*)",\s*"([^"]*)"\)', source):
        calls[name] = (int(rva, 16), function, c_bytes(before), c_bytes(after))
    for name, rva in re.findall(r"#define (RVA_\w+) (0x[0-9a-fA-F]+)u", source):
        data[name] = int(rva, 16)
    return functions, slots, calls, data


def test_sites(game_exe):
    print("sites")
    functions, slots, calls, data = load_sites()
    check(len(functions) == 14 and len(slots) == 3 and len(calls) == 8 and len(data) == 7,
          f"sites.h: {len(functions)} functions, {len(slots)} slots, {len(calls)} calls, {len(data)} data RVAs")
    check(all(len(b) >= 10 for _, b in functions.values()), "every function guard has at least 10 bytes")
    check(all(len(b) == 8 and len(a) == 8 for _, _, b, a in calls.values()), "every call guard has 8 bytes on each side")
    check(all(f in functions for _, f in slots.values()) and all(c[1] in functions for c in calls.values()),
          "slots and calls name known functions")
    if not game_exe:
        print("  skip game exe check (pass --game-exe or set FH2_GAME_EXE)")
        return
    exe = PE(game_exe)
    check(exe.image_size == GAME_IMAGE_SIZE, f"game exe image size {exe.image_size:#x}")
    relocated = exe.relocations()

    def relocated_in(rva, size):
        return any(r <= x < r + 8 for r in relocated for x in range(rva, rva + size) if r <= x)

    for name, (rva, guard) in functions.items():
        check(exe.read(rva, len(guard)) == guard and not relocated_in(rva, len(guard)),
              f"{name} {rva:#x}: first {len(guard)} bytes match, none relocated")
    for name, (rva, function) in slots.items():
        value = struct.unpack("<Q", exe.read(rva, 8))[0]
        check(value - exe.image_base == functions[function][0] and rva in relocated,
              f"{name} {rva:#x} holds {function} (a relocated pointer)")
    for name, (rva, function, before, after) in calls.items():
        code = exe.read(rva - 8, 21)
        target = rva + 5 + struct.unpack("<i", code[9:13])[0]
        check(code[:8] == before and code[8] == 0xE8 and code[13:] == after and
              target == functions[function][0] and not relocated_in(rva - 8, 21),
              f"{name} {rva:#x}: call {function} between the guard bytes, none relocated")
    for name, rva in data.items():
        if name.endswith("_VTABLE"):
            check(rva in relocated, f"{name} {rva:#x} is a virtual table (relocated pointer)")


# --------------------------------------------------------------- 1. package

def valid_mod_id(value):
    # features/addons.c valid_id() in Extra Options 0.4.4.
    if not isinstance(value, str) or not 0 < len(value) <= 128:
        return False
    for i, ch in enumerate(value):
        if not ((ch.isascii() and ch.islower()) or ch.isdigit() or ch == "-"):
            return False
        if ch == "-" and (i == 0 or i == len(value) - 1 or value[i - 1] == "-"):
            return False
    return True


def test_package(output):
    print("package")
    mod = output / "mods" / MOD_NAME
    manifest = json.loads((mod / "mod.json").read_text(encoding="utf-8"))
    check(manifest.get("format_version") == 1, "mod.json format_version is 1")
    check(valid_mod_id(manifest.get("id")), f"mod.json id {manifest.get('id')!r} is a valid mod ID")
    check(manifest.get("dependencies") == [], "mod.json has no dependencies")
    check(all(key not in manifest for key in ("database", "media", "strings")),
          "mod.json names no database, media or strings (a plugin-only mod)")
    top = sorted(p.name for p in mod.iterdir())
    check(top == ["mod.json", "plugins"], f"the mod folder holds mod.json and plugins only ({top})")
    files = sorted(p.name for p in (mod / "plugins").iterdir())
    check(files == [DLL_NAME, INI_NAME], f"plugins holds the DLL and its INI only ({files})")
    pe = PE(mod / "plugins" / DLL_NAME)
    check(pe.machine == 0x8664 and pe.magic == 0x20B and pe.characteristics & 0x2000,
          "CustomRadio.dll is a PE32+ x64 DLL")
    check(sorted(pe.exports()) == ["FH2PluginInitialize", "FH2PluginVehicleUpdate"],
          f"exports exactly FH2PluginInitialize and FH2PluginVehicleUpdate ({pe.exports()})")
    imports = pe.imports()
    check(all(SYSTEM_DLLS.match(name) for name in imports), f"imports only Windows DLLs ({sorted(imports)})")
    check(not any(name.startswith("Fh2Test") or name.startswith("CustomRadioTest") for name in pe.exports()),
          "no test exports in the release DLL")
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    ini.read(mod / "plugins" / INI_NAME, encoding="utf-8")
    radio, keys = ini["CustomRadio"], ini["Keys"]
    check(radio.get("Enabled") == "1" and radio.get("UnlockAllStations") == "0" and radio.get("PcmDecode") == "1"
          and radio.get("Volume") == "100" and radio.get("Folder") == "Music",
          "INI: station on, UnlockAllStations off, PcmDecode on, Volume 100, Folder Music")
    check([keys.get(f"Radio{a}Keyboard") for a in ("Station", "Next", "Restart")] == ["F9", "F7", "F6"]
          and all(keys.get(f"Radio{a}Xbox") == "None" for a in ("Station", "Next", "Restart")),
          "INI: keys F9/F7/F6, no Xbox buttons")
    return ini


# ---------------------------------------------------------- 3. coexistence

# Extra Options 0.4.4's [Bindings] keyboard defaults (FH2_ExtraOptions.ini):
# Autodrive F2, Headlights F3, Roof F4, ToggleUI F1, JumpToManufacturer and
# RimJumpToManufacturer Tab; everything else None.
EO_044_KEYBOARD = {"F1", "F2", "F3", "F4", "Tab"}


def test_coexistence(ini, eo_dir):
    print("coexistence")
    ours = {ini["Keys"].get(f"Radio{a}Keyboard") for a in ("Station", "Next", "Restart")}
    check(not ours & EO_044_KEYBOARD, f"default keys {sorted(ours)} are not Extra Options 0.4.4's {sorted(EO_044_KEYBOARD)}")
    if not eo_dir:
        print("  skip release check (pass --extra-options or set EO_RELEASE_DIR)")
        return
    eo = Path(eo_dir)
    release = configparser.ConfigParser(interpolation=None, strict=False)
    release.optionxform = str
    release.read(eo / "FH2_ExtraOptions.ini", encoding="utf-8-sig")
    bound = {value.strip() for key, value in release["Bindings"].items() if key.endswith("Keyboard")}
    check(not ours & {b for b in bound if b.lower() != "none"},
          f"no key in common with {eo / 'FH2_ExtraOptions.ini'} [Bindings] ({sorted(bound)})")
    functions, slots, calls, _ = load_sites()
    sites = [rva for rva, _ in slots.values()] + [c[0] for c in calls.values()]
    binaries = [eo / "winmm.dll"] + sorted((eo / "mods").glob("*/plugins/*.dll"))
    for binary in binaries:
        data = binary.read_bytes()
        hits = [hex(rva) for rva in sites if struct.pack("<I", rva) in data]
        check(not hits, f"{binary.name} names none of the hooked sites ({hits})")
    for binary in binaries:
        if binary.name.lower() == "winmm.dll":
            continue
        for path in binary.parent.glob("*.ini"):
            text = path.read_text(encoding="utf-8", errors="replace")
            numbers = {int(v) for v in re.findall(r"^\s*\w*Keyboard\s*=\s*(\d+)", text, re.M)}
            ours_vk = {VK_F1 + int(k[1:]) - 1 for k in ours}
            check(not numbers & ours_vk, f"{path.name}: its keyboard settings do not use our keys")


# ------------------------------------------------------------- DLL helpers

class RadioConfig(ctypes.Structure):
    _fields_ = [("enabled", ctypes.c_int), ("unlock_all", ctypes.c_int), ("pcm_decode", ctypes.c_int),
                ("volume", ctypes.c_int), ("folder", ctypes.c_wchar * 1024), ("name", ctypes.c_char * 256),
                ("keyboard", ctypes.c_uint * 3), ("xbox", ctypes.c_uint * 3)]


LOG_FN = ctypes.CFUNCTYPE(None, ctypes.c_char_p)


def load_test_dll(output, log):
    dll = ctypes.CDLL(str(output / "obj" / "CustomRadio-test.dll"))
    sink = LOG_FN(lambda line: log.append(line.decode("utf-8", "replace")))
    dll.CustomRadioTestBindLog.argtypes = [LOG_FN]
    dll.CustomRadioTestBindLog(sink)
    dll._sink = sink
    dll.CustomRadioTestConfig.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.POINTER(RadioConfig), ctypes.c_uint]
    dll.CustomRadioTestParseKey.argtypes = [ctypes.c_wchar_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint)]
    dll.CustomRadioTestKeyEdges.argtypes = [ctypes.c_uint, ctypes.c_int]
    dll.CustomRadioTestKeyEdges.restype = ctypes.c_uint
    dll.CustomRadioTestDescribe.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint)]
    dll.CustomRadioTestDecode.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint)]
    return dll


# ------------------------------------------------------------- 4. config

def test_config(dll, log, work, shipped_ini):
    print("config")
    check(dll.CustomRadioTestConfigSize() == ctypes.sizeof(RadioConfig), "RadioConfig layout matches")
    base = str(work / "Azimuth")

    def load(text):
        path = work / "CustomRadio.ini"
        path.write_text(text, encoding="utf-16")
        log.clear()
        config = RadioConfig()
        dll.CustomRadioTestConfig(str(path), base, ctypes.byref(config), ctypes.sizeof(config))
        return config, list(log)

    config, lines = load("")
    check(config.enabled == 1 and config.unlock_all == 0 and config.pcm_decode == 1 and config.volume == 100,
          "no INI: station on, UnlockAllStations off, PcmDecode on, Volume 100")
    check(config.folder == base + "\\Music", f"no INI: folder is Music beside the launcher ({config.folder})")
    check(list(config.keyboard) == [0x78, 0x76, 0x75] and list(config.xbox) == [0, 0, 0] and config.name == b"",
          "no INI: keys F9/F7/F6, no buttons, default name")
    check(not lines, "no INI: nothing logged")

    shipped = Path(shipped_ini).read_text(encoding="utf-8")
    config, lines = load(shipped)
    check(config.enabled == 1 and config.volume == 100 and list(config.keyboard) == [0x78, 0x76, 0x75]
          and config.folder == base + "\\Music" and not lines,
          "the shipped CustomRadio.ini gives the defaults without a warning")

    config, lines = load("[CustomRadio]\nEnabled=0\nFolder=..\\Songs\\\nName=MY TUNES & MORE\nVolume=150\n"
                         "UnlockAllStations=1\nPcmDecode=0\n[Keys]\nRadioStationKeyboard=F10\n"
                         "RadioNextKeyboard=0x41\nRadioRestartKeyboard=None\nRadioNextXbox=LB+Y\n"
                         "RadioStationXbox=dpadup\n")
    check(config.enabled == 0 and config.unlock_all == 1 and config.pcm_decode == 0 and config.volume == 150,
          "valid values: Enabled=0, UnlockAllStations=1, PcmDecode=0, Volume=150")
    check(config.folder == str(work / "Songs"), f"relative folder with .. is resolved ({config.folder})")
    check(config.name == "MY TUNES & MORE".encode(), "Name is kept")
    check(list(config.keyboard) == [0x79, 0x41, 0] and list(config.xbox) == [0x1, 0x100 | 0x8000, 0],
          "keys F10 / 0x41 / None, buttons DPadUp / LB+Y")
    check(not lines, f"valid values: nothing logged ({lines})")

    config, _ = load("[CustomRadio]\nFolder=D:\\My Music\n")
    check(config.folder == "D:\\My Music", "absolute folder is kept")
    config, _ = load("[CustomRadio]\nFolder=\\\\server\\share\\mp3\n")
    check(config.folder == "\\\\server\\share\\mp3", "UNC folder is kept")

    config, lines = load("[CustomRadio]\nEnabled=yes\nVolume=401\nPcmDecode=2\n[Keys]\n"
                         "RadioStationKeyboard=F25\nRadioNextXbox=LB+Q\n")
    check(config.enabled == 1 and config.volume == 100 and config.pcm_decode == 1 and
          config.keyboard[0] == 0x78 and config.xbox[1] == 0, "bad values: defaults used")
    check(sum("is not valid" in line for line in lines) == 5, f"bad values: each logged ({len(lines)} lines)")
    check(any("[CustomRadio] Volume=401 is not valid; default used: 100" in line for line in lines),
          "the log names the setting, value and default")
    config, lines = load("[CustomRadio]\nVolume=-5\n")
    check(config.volume == 100 and len(lines) == 1, "negative volume refused")
    config, lines = load("[CustomRadio]\nVolume=0x10\n")
    check(config.volume == 100 and len(lines) == 1, "hex volume refused")
    config, lines = load("[CustomRadio]\nSingleEntry=1\nEnabled=1\n[Keys]\nRadioPause=F8\n")
    check(sum("unknown key ignored" in line for line in lines) == 2 and config.enabled == 1,
          "unknown keys are logged and ignored")

    def parse(text, xbox=0):
        value = ctypes.c_uint(0xFFFF)
        ok = dll.CustomRadioTestParseKey(text, xbox, ctypes.byref(value))
        return value.value if ok else None

    good = {"F1": 0x70, "F24": 0x87, "f6": 0x75, " F9 ": 0x78, "A": 0x41, "z": 0x5A, "7": 0x37, "Esc": 0x1B,
            "PageDown": 0x22, "0x41": 0x41, "120": 120, "None": 0, "none": 0}
    for text, value in good.items():
        check(parse(text) == value, f"keyboard {text!r} -> {value:#x}")
    for text in ("F0", "F25", "F01", "0x100", "255", "Ctrl+A", "", "Q2"):
        check(parse(text) is None, f"keyboard {text!r} refused")
    pads = {"A": 0x1000, "LB+Y": 0x8100, "View": 0x20, "back": 0x20, "LS + RS": 0xC0, "None": 0, "DPadDown": 2}
    for text, value in pads.items():
        check(parse(text, 1) == value, f"xbox {text!r} -> {value:#x}")
    for text in ("LT", "RT", "LB+", "Q", "A++B"):
        check(parse(text, 1) is None, f"xbox {text!r} refused")


# --------------------------------------------------------------- 5. keys

def test_keys(dll):
    print("keys")
    steps = [  # (down, enabled, expected pressed)
        (0b010, 1, 0b010, "a press while driving fires"),
        (0b010, 1, 0, "holding does not repeat"),
        (0, 1, 0, "release fires nothing"),
        (0b010, 1, 0b010, "the next press fires again"),
        (0, 1, 0, ""),
        (0b001, 0, 0, "a press in a menu does not fire"),
        (0b001, 1, 0, "a key held from the menu does not fire when driving resumes"),
        (0, 1, 0, ""),
        (0b001, 1, 0b001, "after release it fires"),
        (0b111, 1, 0b110, "only the newly pressed actions fire"),
    ]
    for down, enabled, expected, what in steps:
        got = dll.CustomRadioTestKeyEdges(down, enabled)
        if what:
            check(got == expected, f"{what} ({got:#05b})")


# ---------------------------------------------------------------- 6. mp3

FRAME_HEADER = bytes([0xFF, 0xFB, 0x94, 0x00])  # MPEG-1 Layer III, 128 kbps, 48 kHz, stereo
FRAME_SIZE = 384                                 # 144 * 128000 / 48000, no padding


def silent_frame():
    # Zero side information: no Huffman data, so every granule decodes to silence.
    return FRAME_HEADER + bytes(FRAME_SIZE - 4)


def info_frame(frames):
    frame = bytearray(silent_frame())
    frame[36:40] = b"Info"
    frame[40:44] = struct.pack(">I", 1)  # frame count present
    frame[44:48] = struct.pack(">I", frames)
    return bytes(frame)


def id3v2(title=None, artist=None):
    def frame(name, payload):
        return name + struct.pack(">I", len(payload)) + b"\0\0" + payload
    body = b""
    if title is not None:
        body += frame(b"TIT2", b"\x01\xff\xfe" + title.encode("utf-16-le") + b"\0\0")
    if artist is not None:
        body += frame(b"TPE1", b"\x00" + artist.encode("latin-1"))
    size = len(body)
    syncsafe = bytes([(size >> 21) & 0x7F, (size >> 14) & 0x7F, (size >> 7) & 0x7F, size & 0x7F])
    return b"ID3\x03\x00\x00" + syncsafe + body


def id3v1(title, artist):
    return (b"TAG" + title.encode("latin-1").ljust(30, b"\0") + artist.encode("latin-1").ljust(30, b"\0")
            + bytes(30) + b"2026" + bytes(30) + b"\xff")


def make_mp3(seconds, xing=True, tag=b"", tail=b""):
    frames = round(seconds * 48000 / 1152)
    return tag + (info_frame(frames) if xing else b"") + silent_frame() * frames + tail, frames


def write_music(folder):
    """The test music folder; returns the expected playable tracks in folder order."""
    folder.mkdir(parents=True, exist_ok=True)
    data, frames = make_mp3(5, tag=id3v2('T\u00ebst & "S\u00f3ng"', "Tester <1>"))
    (folder / "01 tagged.mp3").write_bytes(data)
    expected = [("T\u00ebst & 'S\u00f3ng'", "Tester (1)", frames * 24)]
    data, frames = make_mp3(4, xing=False)
    (folder / "02 plain.mp3").write_bytes(data)
    expected.append(("02 plain", "", frames * FRAME_SIZE * 8 // 128))
    data, frames = make_mp3(6, tail=id3v1("Old Tag", "V1 Artist"))
    (folder / "03 v1.mp3").write_bytes(data)
    expected.append(("Old Tag", "V1 Artist", frames * 24))
    (folder / "04 short.mp3").write_bytes(make_mp3(1)[0])
    (folder / "notes.txt").write_text("not music")
    return expected


def test_mp3(dll, work):
    print("mp3")
    music = work / "Music"
    expected = write_music(music)
    title, artist, duration = ctypes.create_string_buffer(256), ctypes.create_string_buffer(256), ctypes.c_uint()
    for name, (want_title, want_artist, want_length) in zip(("01 tagged.mp3", "02 plain.mp3", "03 v1.mp3"), expected):
        dll.CustomRadioTestDescribe(str(music / name), title, artist, ctypes.byref(duration))
        got = (title.value.decode("utf-8"), artist.value.decode("utf-8"), duration.value)
        raw = ('T\u00ebst & "S\u00f3ng"', "Tester <1>") if name.startswith("01") else (want_title, want_artist)
        check(got == (raw[0], raw[1], want_length), f"{name}: title/artist/length {got}")
    dll.CustomRadioTestDescribe(str(music / "04 short.mp3"), title, artist, ctypes.byref(duration))
    check(duration.value < 3000, f"04 short.mp3 is under 3 s ({duration.value} ms)")
    for name, frames in (("01 tagged.mp3", 208), ("02 plain.mp3", 167)):
        data = (music / name).read_bytes()
        header, size = ctypes.create_string_buffer(44), ctypes.c_uint()
        result = dll.CustomRadioTestDecode(data, len(data), header, ctypes.byref(size))
        if result == -1:
            if os.environ.get("CI"):
                print("  skip decode: this CI image has no Windows MP3 codec (msacm32)")
            else:
                check(False, "Windows MP3 codec (msacm32) available")
            continue
        (riff, _, wave, _, _, tag, channels, rate, byte_rate, align, bits, data_id,
         data_size) = struct.unpack("<4sI4s4sIHHIIHH4sI", header.raw)
        samples = data_size // 4
        check(result == 1 and riff == b"RIFF" and wave == b"WAVE" and data_id == b"data" and tag == 1 and
              channels == 2 and rate == 48000 and bits == 16 and align == 4 and byte_rate == 192000 and
              size.value == 44 + data_size,
              f"{name}: decoded by Windows to a 48 kHz 16-bit stereo WAV")
        check(abs(samples - frames * 1152) <= 2 * 1152,
              f"{name}: {samples} samples for {frames} frames ({frames * 1152})")
    data = bytes(4096)
    header, size = ctypes.create_string_buffer(44), ctypes.c_uint()
    check(dll.CustomRadioTestDecode(data, len(data), header, ctypes.byref(size)) == 0,
          "a file without MP3 frames is not decoded (served as is)")


# ------------------------------------------------------ 7. plugin scenarios

def run_scenarios(output, work):
    print("plugin")
    scenarios = ["vanilla", "chained", "core-mismatch", "optional-mismatch", "disabled", "unlock-only",
                 "slot-not-code", "wrong-size"]
    for scenario in scenarios:
        folder = work / f"scenario-{scenario}"
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--scenario", scenario,
                                 "--output", str(output), "--work", str(folder)],
                                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300,
                                env={**os.environ, "PYTHONIOENCODING": "utf-8"})
        print(f" [{scenario}]")
        for line in result.stdout.splitlines():
            print(line)
            if line.startswith("  FAIL "):
                failures.append(f"{scenario}: {line[7:]}")
        if result.returncode != 0 and not any(line.startswith("  FAIL ") for line in result.stdout.splitlines()):
            check(False, f"scenario {scenario} exited with {result.returncode}: {result.stderr.strip()[-400:]}")


# The child side: one scenario per process (the plugin's hooks are process-wide).

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.VirtualAlloc.restype = ctypes.c_void_p
kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
kernel32.HeapAlloc.restype = ctypes.c_void_p
kernel32.HeapAlloc.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_size_t]
kernel32.GetProcessHeap.restype = ctypes.c_void_p
kernel32.VirtualQuery.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
kernel32.VirtualQuery.restype = ctypes.c_size_t


class Context(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("api_version", ctypes.c_uint32), ("adapter", ctypes.c_uint32),
                ("game_image_size", ctypes.c_uint32), ("game_image", ctypes.c_void_p),
                ("plugin_module", ctypes.c_void_p), ("plugins_directory", ctypes.c_wchar_p),
                ("plugin_path", ctypes.c_wchar_p), ("extra_options_version", ctypes.c_char_p),
                ("log_utf8", LOG_FN)]


class Frame(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("input_enabled", ctypes.c_uint32), ("car", ctypes.c_void_p),
                ("buttons", ctypes.c_uint32), ("delta_seconds", ctypes.c_float), ("flash", ctypes.c_void_p)]


KEY_STATE_FN = ctypes.WINFUNCTYPE(ctypes.c_short, ctypes.c_int)
SELECT_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)

# Foreign hooks (another module's code) placed in the fake image's first page.
STUBS = {
    "xml": (0x2000, bytes([0xB0, 0x01, 0xC3])),                                  # mov al,1; ret
    "update": (0x2010, bytes([0xC7, 0x41, 0x3C, 0x55, 0x55, 0, 0, 0xC3])),       # mov dword [rcx+3Ch],5555h
    "next": (0x2020, bytes([0x48, 0x89, 0xD0, 0xC3])),                           # mov rax,rdx
    "sound": (0x2030, bytes([0xB8, 0x77, 0, 0, 0, 0xC3])),                       # mov eax,77h
    "subsound": (0x2040, bytes([0xB8, 0x55, 0, 0, 0, 0xC3])),                    # mov eax,55h
    "find": (0x2050, bytes([0x48, 0x89, 0xC8, 0xC3])),                           # mov rax,rcx
    "locked": (0x2060, bytes([0xB0, 0x01, 0xC3])),                               # every station locked
    "change": (0x2070, bytes([0xC7, 0x01, 0x34, 0x12, 0, 0, 0xC3])),             # mov dword [rcx],1234h
}
CHAINED_SLOTS = {"SLOT_XML_READ": "xml", "SLOT_PLAYER_UPDATE": "update", "SLOT_PLAYLIST_NEXT": "next"}
CHAINED_CALLS = {"CALL_SOUND_INFO": "sound", "CALL_SUBSOUND": "subsound", "CALL_HISTORY_FIND": "find",
                 "CALL_LOCKED_1": "locked", "CALL_LOCKED_2": "locked", "CALL_LOCKED_3": "locked",
                 "CALL_CHANGE_1": "change", "CALL_CHANGE_2": "change"}

RADIO_INFO = (b'<?xml version="1.0" encoding="utf-8"?>\r\n<RadioStations>\r\n'
              b'  <RadioStation Name="HORIZON PULSE" Number="1" RecentlyPlayedListSize="5">\r\n'
              b'  </RadioStation>\r\n  <RadioStation Name="HORIZON BASS ARENA" Number="7">\r\n'
              b'  </RadioStation>\r\n</RadioStations>\r\n')


class FakeGame:
    def __init__(self):
        self.base = kernel32.VirtualAlloc(None, GAME_IMAGE_SIZE, 0x3000, 0x40)  # RWX
        if not self.base:
            raise OSError("VirtualAlloc failed")
        self.functions, self.slots, self.calls, self.data = load_sites()
        for rva, guard in self.functions.values():
            self.write(rva, guard)
        for rva, function in self.slots.values():
            self.write(rva, struct.pack("<Q", self.base + self.functions[function][0]))
        for rva, function, before, after in self.calls.values():
            self.write(rva - 8, before + b"\xE8" + struct.pack("<i", self.functions[function][0] - rva - 5) + after)

    def write(self, rva, data):
        ctypes.memmove(self.base + rva, data, len(data))

    def read(self, rva, size):
        return ctypes.string_at(self.base + rva, size)

    def snapshot(self):
        return ctypes.string_at(self.base, GAME_IMAGE_SIZE)

    def call_target(self, name):
        rva = self.calls[name][0]
        return self.base + rva + 5 + struct.unpack("<i", self.read(rva + 1, 4))[0]

    def slot_value(self, name):
        return struct.unpack("<Q", self.read(self.slots[name][0], 8))[0]

    def chain(self, slots=CHAINED_SLOTS, calls=CHAINED_CALLS):
        for name, (rva, code) in STUBS.items():
            self.write(rva, code)
        for slot, stub in slots.items():
            self.write(self.slots[slot][0], struct.pack("<Q", self.base + STUBS[stub][0]))
        for call, stub in calls.items():
            rva = self.calls[call][0]
            self.write(rva + 1, struct.pack("<i", STUBS[stub][0] - rva - 5))


def changed_offsets(before, after):
    out = []
    for start in range(0, len(before), 65536):
        if before[start:start + 65536] != after[start:start + 65536]:
            out.extend(i for i in range(start, min(start + 65536, len(before))) if before[i] != after[i])
    return out


def module_range(handle):
    pe_offset = struct.unpack("<I", ctypes.string_at(handle + 0x3C, 4))[0]
    size = struct.unpack("<I", ctypes.string_at(handle + pe_offset + 24 + 56, 4))[0]
    return handle, handle + size


def scenario(name, output, work):
    if work.exists():
        shutil.rmtree(work)
    plugins = work / "mods" / MOD_NAME / "plugins"
    plugins.mkdir(parents=True)
    shutil.copy2(output / "obj" / "CustomRadio-test.dll", plugins / DLL_NAME)
    expected_tracks = write_music(work / "Music")
    settings = {"vanilla": "Enabled=1\nPcmDecode=1\n", "chained": "Enabled=1\nPcmDecode=1\n",
                "core-mismatch": "Enabled=1\n", "optional-mismatch": "Enabled=1\n", "disabled": "Enabled=0\n",
                "unlock-only": "Enabled=0\nUnlockAllStations=1\n", "slot-not-code": "Enabled=1\n",
                "wrong-size": "Enabled=1\n"}[name]
    (plugins / INI_NAME).write_text(f"[CustomRadio]\n{settings}Folder={work / 'Music'}\n", encoding="utf-8")

    game = FakeGame()
    if name == "chained":
        game.chain()
        audio = ctypes.create_string_buffer(64)  # the audio manager SetStation checks for
        game.write(game.data["RVA_AUDIO_MANAGER"], struct.pack("<Q", ctypes.addressof(audio)))
        game._keep = audio
    if name == "core-mismatch":
        rva = game.functions["FN_SUBSOUND"][0]
        game.write(rva + 3, b"\x90")
    if name == "optional-mismatch":
        rva = game.calls["CALL_CHANGE_2"][0]
        game.write(rva + 6, b"\x90")
    if name == "slot-not-code":
        game.write(game.slots["SLOT_XML_READ"][0], struct.pack("<Q", 0x1234))
    if name == "vanilla":
        # A radio system that already has stations: the plugin started late.
        system = ctypes.create_string_buffer(0x40)
        stations = ctypes.create_string_buffer(32)
        ctypes.memmove(system, struct.pack("<Q", game.base + game.data["RVA_RADIO_SYSTEM_VTABLE"]), 8)
        ctypes.memmove(ctypes.addressof(system) + 0x30,
                       struct.pack("<QQ", ctypes.addressof(stations), ctypes.addressof(stations) + 16), 16)
        game.write(game.data["RVA_RADIO_SYSTEM_GLOBAL"], struct.pack("<Q", ctypes.addressof(system)))
        game._keep = (system, stations)
    before = game.snapshot()

    log = []
    sink = LOG_FN(lambda line: log.append(line.decode("utf-8", "replace")))
    dll = ctypes.CDLL(str(plugins / DLL_NAME))
    dll.FH2PluginInitialize.argtypes = [ctypes.POINTER(Context)]
    dll.FH2PluginVehicleUpdate.argtypes = [ctypes.POINTER(Frame)]
    context = Context(ctypes.sizeof(Context), 1, 1, GAME_IMAGE_SIZE if name != "wrong-size" else 0x1000,
                      game.base, dll._handle, str(plugins), str(plugins / DLL_NAME), b"0.4.4", sink)
    check(ctypes.sizeof(Context) == 64, "the loader context is 64 bytes")
    result = dll.FH2PluginInitialize(ctypes.byref(context))
    after = game.snapshot()
    text = "".join(log)
    changed = changed_offsets(before, after)
    dll_low, dll_high = module_range(dll._handle)

    def slot_hooked(slot):
        return dll_low <= game.slot_value(slot) < dll_high

    def call_hooked(call):
        target = game.call_target(call)
        code = ctypes.string_at(target, 14)
        destination = struct.unpack("<Q", code[6:14])[0]
        return (not game.base <= target < game.base + GAME_IMAGE_SIZE and code[:6] == b"\xFF\x25\0\0\0\0" and
                dll_low <= destination < dll_high)

    def allowed(sites_slots, sites_calls):
        ranges = [range(game.slots[s][0], game.slots[s][0] + 8) for s in sites_slots]
        ranges += [range(game.calls[c][0] + 1, game.calls[c][0] + 5) for c in sites_calls]
        return all(any(o in r for r in ranges) for o in changed)

    core_slots = ["SLOT_XML_READ", "SLOT_PLAYER_UPDATE"]
    core_calls = ["CALL_SOUND_INFO", "CALL_SUBSOUND"]
    lock_calls = ["CALL_LOCKED_1", "CALL_LOCKED_2", "CALL_LOCKED_3"]
    change_calls = ["CALL_CHANGE_1", "CALL_CHANGE_2"]
    all_slots = list(game.slots)
    all_calls = list(game.calls)

    if name in ("vanilla", "chained"):
        check(result == 1, "FH2PluginInitialize returns TRUE")
        check(all(slot_hooked(s) for s in all_slots), "all 3 slots point into the plugin")
        check(all(call_hooked(c) for c in all_calls),
              "all 8 calls go through a jump of the plugin's own (not in the game image) into the plugin")
        check(allowed(all_slots, all_calls), f"nothing else in the image changed ({len(changed)} bytes changed)")
        for line in ("custom station hooks installed", "station lock hook ready (UnlockAllStations=0)",
                     "D-pad cycle hook installed", "RadioStation key tunes straight",
                     "folder order with wrap-around installed", "decoded to PCM by Windows"):
            check(line in text, f"log: {line}")
        check(("already hooked by another module" in text) == (name == "chained"),
              "chaining is logged only when another hook was there")
    if name == "vanilla":
        check("stations were already loaded" in text, "log: a late start (stations already loaded) is reported")
    if name == "chained":
        check(text.count("already hooked by another module; chained") == 11, "log: all 11 sites chained")
        chained_checks(dll, game, log, expected_tracks)
    if name == "core-mismatch":
        check(result == 0, "FH2PluginInitialize returns FALSE")
        check(not changed, "the game image is untouched")
        check("SUBSOUND: the called game function's first bytes are not as expected" in text and
              "game code did not match; custom station disabled" in text, "log: the mismatch and its site")
    if name == "optional-mismatch":
        check(result == 1, "FH2PluginInitialize returns TRUE")
        check(allowed(all_slots, [c for c in all_calls if c not in change_calls]),
              "the D-pad calls (both) are untouched; the rest is hooked")
        check(all(call_hooked(c) for c in core_calls + lock_calls) and all(slot_hooked(s) for s in core_slots),
              "the station's core and lock hooks are installed")
        check("D-pad cycle hook did not match; native cycle only" in text and
              "CHANGE_2: the code around the call is not as expected" in text, "log: the D-pad group refused")
    if name == "disabled":
        check(result == 1 and not changed, "Enabled=0: TRUE and the game image is untouched")
        check("custom station is off (Enabled=0)" in text, "log: the station is off")
    if name == "unlock-only":
        check(result == 1 and all(call_hooked(c) for c in lock_calls) and allowed([], lock_calls),
              "UnlockAllStations=1 alone hooks only the three lock calls")
        check("all radio stations unlocked" in text, "log: all stations unlocked")
    if name == "slot-not-code":
        check(result == 0 and not changed, "a slot that holds no code: FALSE, nothing written")
        check("XML_READ: the slot holds neither the game function nor code" in text, "log: the slot refused")
    if name == "wrong-size":
        check(result == 0 and not changed, "another image size: FALSE, nothing written")
    os._exit(1 if failures else 0)


def game_string(text):
    """An MSVC std::string (32 bytes) holding `text`."""
    data = text.encode("utf-8")
    buffer = ctypes.create_string_buffer(32)
    if len(data) < 16:
        ctypes.memmove(buffer, data, len(data))
        ctypes.memmove(ctypes.addressof(buffer) + 16, struct.pack("<QQ", len(data), 15), 16)
        return buffer, None
    heap = ctypes.create_string_buffer(data + b"\0")
    ctypes.memmove(buffer, struct.pack("<Q", ctypes.addressof(heap)), 8)
    ctypes.memmove(ctypes.addressof(buffer) + 16, struct.pack("<QQ", len(data), len(data)), 16)
    return buffer, heap


def read_game_string(address):
    raw = ctypes.string_at(address, 32)
    size, capacity = struct.unpack_from("<QQ", raw, 16)
    if capacity >= 16:
        return ctypes.string_at(struct.unpack_from("<Q", raw, 0)[0], size).decode("utf-8")
    return raw[:size].decode("utf-8")


def chained_checks(dll, game, log, expected_tracks):
    selected = []
    select = SELECT_FN(lambda _, name: selected.append(read_game_string(name)))
    dll.CustomRadioTestUseFakes.argtypes = [SELECT_FN]
    dll.CustomRadioTestUseFakes(select)
    dll.CustomRadioTestStation.argtypes = [ctypes.c_char_p, ctypes.c_uint]

    # RadioInfo through the hooked read slot (the foreign read returns 1 first).
    resource = ctypes.create_string_buffer(0x100)
    name, name_heap = game_string("media\\audio\\RadioInfo_EN.xml")
    ctypes.memmove(ctypes.addressof(resource) + 0xB8, name, 32)
    heap = kernel32.GetProcessHeap()
    xml = kernel32.HeapAlloc(heap, 0, len(RADIO_INFO) + 1)
    ctypes.memmove(xml, RADIO_INFO + b"\0", len(RADIO_INFO) + 1)
    ctypes.memmove(ctypes.addressof(resource) + 0xE0, struct.pack("<QI", xml, len(RADIO_INFO)), 12)
    read = ctypes.CFUNCTYPE(ctypes.c_ubyte, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(
        game.slot_value("SLOT_XML_READ"))
    check(read(ctypes.addressof(resource), None, None, None) == 1, "XML read: the chained read's result comes back")
    buffer, length = struct.unpack_from("<QI", resource.raw, 0xE0)
    text = ctypes.string_at(buffer, length).decode("utf-8")
    check(buffer != xml and text.startswith(RADIO_INFO[:-20].decode()) and text.endswith("</RadioStations>\r\n"),
          "XML read: the text is replaced, original stations kept")
    station = re.search(r'    <RadioStation Name="([^"]*)" Number="(\d+)" RecentlyPlayedListSize="-1">', text)
    check(station is not None and station.group(1) == "MY MUSIC" and station.group(2) == "8",
          "XML read: MY MUSIC added as number 8 (after the highest, 7)")
    entries = re.findall(r'<Entry EventName="/Master/Radio/Track" SoundName="FH2MUSIC_(\d+)" SampleLength="(\d+)" '
                         r'DisplayName="([^"]*)" Artist="([^"]*)"', text)
    freeroam = entries[:len(entries) // 2]
    check(len(entries) == 6 and [e[0] for e in freeroam] == ["0", "1", "2"],
          f"XML read: 3 playable tracks in folder order, in both track playlists ({len(entries)} entries)")
    check([(e[2], e[3], int(e[1])) for e in freeroam] == expected_tracks,
          f"XML read: names (with & kept, quote and <> replaced) and lengths ({freeroam})")
    check(text.count('SoundName="FH2MUSIC_SILENCE"') == 2 and '<Playlist Type="DJ">' in text,
          "XML read: silent stingers and an empty DJ list")
    check(text.index("<RadioStation Name=\"MY MUSIC\"") < text.index("</RadioStations>"),
          "XML read: the station is inside RadioStations")
    station_name = ctypes.create_string_buffer(64)
    check(dll.CustomRadioTestStation(station_name, 64) == 8 and station_name.value == b"MY MUSIC",
          "the plugin knows its station (8, MY MUSIC)")
    other = ctypes.create_string_buffer(0x100)
    other_name, other_heap = game_string("media\\ui\\Something.xml")
    ctypes.memmove(ctypes.addressof(other) + 0xB8, other_name, 32)
    ctypes.memmove(ctypes.addressof(other) + 0xE0, struct.pack("<QI", xml, 10), 12)
    read(ctypes.addressof(other), None, None, None)
    check(struct.unpack_from("<Q", other.raw, 0xE0)[0] == xml, "XML read: other files are left alone")

    # getSoundInfo through the hooked call.
    sound_info = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)(
        game.call_target("CALL_SOUND_INFO"))
    info = ctypes.create_string_buffer(0xF0)
    check(sound_info(None, b"FH2MUSIC_0", info) == 0, "sound info FH2MUSIC_0: answered by the plugin")
    data, mode = struct.unpack_from("<QI", info.raw, 0)
    exinfo, size = struct.unpack_from("<II", info.raw, 0x10)
    marker = struct.unpack_from("<I", info.raw, 0xE8)[0]
    check(mode == 0x10004000 and exinfo == 0xD8 and marker == 0x46480000,
          "sound info: OPENMEMORY_POINT|ACCURATETIME, exinfo size, subsound marker 0")
    dll.CustomRadioTestDecode.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_char_p, ctypes.c_void_p]
    codec = dll.CustomRadioTestDecode(b"", 0, ctypes.create_string_buffer(44), None) != -1
    if codec or not os.environ.get("CI"):
        check(ctypes.string_at(data, 4) == b"RIFF" and
              size == 44 + struct.unpack("<I", ctypes.string_at(data + 40, 4))[0],
              f"sound info: the track as a PCM WAV in memory ({size} bytes)")
    else:
        check(ctypes.string_at(data, 3) == b"ID3", "sound info: without a Windows MP3 codec the MP3 is served as is")
    check(sound_info(None, b"FH2MUSIC_SILENCE", info) == 0 and ctypes.string_at(struct.unpack_from("<Q", info.raw)[0], 4) == b"RIFF",
          "sound info FH2MUSIC_SILENCE: the silent stinger")
    check(sound_info(None, b"music_horizon_pulse_01", info) == 0x77, "sound info: other keys go to the chained code")
    check(sound_info(None, b"FH2MUSIC_9", info) == 0x77, "sound info: an unknown track number goes on too")

    # getSubSound through the hooked call.
    subsound = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p)(
        game.call_target("CALL_SUBSOUND"))
    player = ctypes.create_string_buffer(0x40)
    instance = ctypes.create_string_buffer(16)
    ctypes.memmove(ctypes.addressof(player) + 0x10, struct.pack("<Q", ctypes.addressof(instance)), 8)
    ctypes.memmove(ctypes.addressof(player) + 0x20, struct.pack("<I", 3), 4)
    ctypes.memmove(ctypes.addressof(player) + 0x2C, struct.pack("<i", 1500), 4)
    request = ctypes.create_string_buffer(0x28)
    ctypes.memmove(ctypes.addressof(request) + 0x10, struct.pack("<Q", ctypes.addressof(player)), 8)
    check(subsound(0xABCD000, 0x46480000, ctypes.addressof(request)) == 0 and
          struct.unpack_from("<Q", request.raw)[0] == 0xABCD000, "subsound: our marker returns the stream itself")
    check(subsound(0xABCD000, 3, ctypes.addressof(request)) == 0x55, "subsound: other indices go to the chained code")
    subsound(0xABCD000, 0x46480000, ctypes.addressof(request))  # the player plays track 0 again

    # IsStationLocked through the hooked calls (the chained code says "locked").
    locked = ctypes.CFUNCTYPE(ctypes.c_ubyte, ctypes.c_void_p, ctypes.c_void_p)(game.call_target("CALL_LOCKED_2"))
    mine, _ = game_string("MY MUSIC")
    theirs, _ = game_string("HORIZON PULSE")
    check(locked(None, ctypes.addressof(mine)) == 0, "station lock: the custom station is never locked")
    check(locked(None, ctypes.addressof(theirs)) == 1, "station lock: other stations follow the chained code")

    # ChangeStation and the playlist hooks fall through to the chained code here.
    change = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int)(game.call_target("CALL_CHANGE_1"))
    module = ctypes.create_string_buffer(0x80)
    change(ctypes.addressof(module), 1)
    check(struct.unpack_from("<I", module.raw)[0] == 0x1234, "D-pad: an unreadable station map leaves it to the chained code")
    next_pick = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int)(
        game.slot_value("SLOT_PLAYLIST_NEXT"))
    check(next_pick(0x1000, 0x2222, 0) == 0x2222, "playlist next: other playlists go to the chained code")
    find = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)(
        game.call_target("CALL_HISTORY_FIND"))
    found = ctypes.create_string_buffer(8)
    check(find(ctypes.addressof(found), 1, 2, 3) == ctypes.addressof(found), "recently played: chained code runs")

    # Keys through FH2PluginVehicleUpdate.
    held = set()
    key_state = KEY_STATE_FN(lambda key: -32768 if key in held else 0)
    dll.CustomRadioTestInput.argtypes = [KEY_STATE_FN, ctypes.c_int]
    dll.CustomRadioTestRequests.restype = ctypes.c_uint
    car = ctypes.create_string_buffer(16)

    def tick(enabled=1, buttons=0):
        frame = Frame(ctypes.sizeof(Frame), enabled, ctypes.addressof(car), buttons, 0.016, None)
        dll.FH2PluginVehicleUpdate(ctypes.byref(frame))

    check(ctypes.sizeof(Frame) == 32, "the vehicle frame is 32 bytes")
    dll.CustomRadioTestInput(key_state, 1)
    held.add(0x78)  # F9
    tick()
    check(selected == ["MY MUSIC"], f"F9 while driving tunes to MY MUSIC ({selected})")
    tick()
    check(len(selected) == 1, "F9 held: no repeat")
    held.clear()
    tick()
    held.add(0x78)
    tick(enabled=0)
    check(len(selected) == 1, "F9 in a menu (input disabled): nothing")
    held.clear()
    tick(enabled=0)
    dll.CustomRadioTestInput(key_state, 0)
    held.add(0x78)
    tick()
    check(len(selected) == 1, "F9 with another window focused: nothing")
    held.clear()
    tick()
    dll.CustomRadioTestInput(key_state, 1)
    held.add(0x76)  # F7
    tick()
    check(dll.CustomRadioTestRequests() == 0b010, "F7 while driving: a next-track request for the radio thread")
    tick(enabled=0)
    check(dll.CustomRadioTestRequests() == 0, "input disabled: the pending request is dropped")
    held.clear()
    tick()
    held.add(0x75)  # F6
    tick()
    check(dll.CustomRadioTestRequests() == 0b100, "F6 while driving: a restart request")
    held.clear()
    tick()

    # The radio thread: the playing custom player's update takes the request.
    update = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_float)(game.slot_value("SLOT_PLAYER_UPDATE"))
    log.clear()
    other_player = ctypes.create_string_buffer(0x40)
    update(ctypes.addressof(other_player), 0.016)
    check(dll.CustomRadioTestRequests() == 0b100, "another player's update leaves the request")
    check(struct.unpack_from("<I", other_player.raw, 0x3C)[0] == 0x5555, "player update: the chained update runs")
    update(ctypes.addressof(player), 0.016)
    check(dll.CustomRadioTestRequests() == 0, "the custom player's update takes the request")
    check(any("station clock not found; key ignored" in line for line in log),
          "with no radio system it is logged and ignored (no crash)")


# ------------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    parser.add_argument("--game-exe", default=os.environ.get("FH2_GAME_EXE"))
    parser.add_argument("--extra-options", default=os.environ.get("EO_RELEASE_DIR"))
    parser.add_argument("--scenario")
    parser.add_argument("--work", type=Path)
    options = parser.parse_args()
    sys.stdout.reconfigure(errors="backslashreplace")
    output = options.output.resolve()
    if options.scenario:
        scenario(options.scenario, output, options.work)
        return
    ini = test_package(output)
    test_sites(options.game_exe)
    test_coexistence(ini, options.extra_options)
    with tempfile.TemporaryDirectory(prefix="custom-radio-") as temporary:
        work = Path(temporary)
        log = []
        dll = load_test_dll(output, log)
        test_config(dll, log, work, output / "mods" / MOD_NAME / "plugins" / INI_NAME)
        test_keys(dll)
        test_mp3(dll, work)
        run_scenarios(output, work)
    print()
    if failures:
        print(f"{len(failures)} FAILED:")
        for failure in failures:
            print("  " + failure)
        sys.exit(1)
    print("all tests passed")


if __name__ == "__main__":
    main()
