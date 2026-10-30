"""Offline tests of the Language Support plugin (no game run, nothing installed).

Usage: py tests/run_tests.py [--output out] [--zig zig.exe] [--game <FH2 folder>]
                             [--azimuth <azimuth-windows-1.0.4.zip>] [--upstream <Extra Options 0.4.4 checkout>]
                             [--release <Extra Options 0.4.4 release's Azimuth folder>]

Real files are only read (the game's executable, the fork's Ukrainian pack
below Mount\\languages\\uk if present, Azimuth's ZIP and data\\azimuth.ini, the
Extra Options release and source); the game executable is mapped into this
process' own memory and patched there. Defaults: --game ~/FH2,
--azimuth <game>/azimuth-windows-1.0.4.zip, --upstream <game>/src/eo-pr/wt-upstream,
--release <game>/src/eo-pr/upstream-0.4.4/FH2_Extra_Options_0.4.4-azimuth/Azimuth.
A missing input skips the tests that need it.

1. package   mod.json as Extra Options 0.4.4 validates it, the mod layout, the
             DLL (PE32+ x64, the one export, only Windows imports, version
             resource), the INI.
2. azimuth   azimuth.exe 1.0.4 is the build the plugin knows; its azimuth.ini
             keeps the game's language in [title-<game>] language (the keys
             the plugin reads); the real data\\azimuth.ini reads so.
3. release   Extra Options 0.4.4 (release and source): mod folders' media are
             mounted on the game's first file lookup, before the plugin loader
             starts (on a worker); a media-only mod is active at once; the
             plugin loader; its INI refuses unknown keys ([Language] Custom);
             none of its patches touches the voice subtitle sites.
4. game      the game executable passes the guards (five hook sites and
             SetText); every changed site is refused.
5. code      voice_subtitles_code.h: the relocation tables, the hook sites and
             jumps back, as the fork generated it (keystone is not needed).
6. table     VoiceSubtitles.str: synthetic LSB2 tables (loose, stored in a ZIP;
             compressed, bad, truncated and missing refused; ~t times,
             clamping) and the Ukrainian pack's real table.
7. pack      slots, the pack's folder (Pack= or the one with language.ini),
             mods\\loadorder.txt, the launcher's language.
8. tool      tools/make_language_pack.py build and convert: a mod folder the
             plugin and Extra Options take.
9. plugin    FH2PluginInitialize on the mapped (relocated) game: the five
             sites jump into the hook code; the hook code, run here with the
             game's functions stubbed, shows the line through SetText and
             clears it when its time is up; every refusal changes nothing.
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
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
MOD_NAME = "Language Support"
DLL_NAME = "LangSupport.dll"
INI_NAME = "LangSupport.ini"
TEST_DLL = "LangSupportTest.dll"
SYSTEM_DLLS = re.compile(r"^(kernel32|user32|msvcrt|api-ms-win-[a-z0-9-]+)\.dll$", re.I)
GAME_EXE = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/forza_durango_release_final.exe")
UK_TABLE = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/languages/uk/stripped/stringtables/RU/VoiceSubtitles.str")
GAME_SHA256 = "3ec04c6163fb5614e3d351ba0570d39b846041a257715d6340eceb03ccb7f0a3"

IMAGE_SIZE = 0x3A18000
PREFERRED_BASE = 0x140000000
# Hook sites (rva, bytes replaced, the first 16 bytes) and SetText, as src/voice_subtitles.c guards them.
SITES = [(0xF50EE0, 5, "4c894c24204c894424184889542410 48"), (0xF51100, 5, "4c894c24204c894424184889542410 48"),
         (0xF512A0, 5, "4c894c24204c894424184889542410 48"), (0xC3DF10, 5, "405553565741564157488bec4883ec58"),
         (0x85AFA0, 6, "40555657415441554156415748 8dac24")]
SET_TEXT = (0xC3A4B0, "48895c240848897424105748 83ec2048")
CTOR, PREP, PREP3D, UPDATE, PARSER = range(5)
AZIMUTH_104 = (0x6AE393EB, 0x8A2000)
# Cave layout (tools/voice_subtitles/voice_hooks.py).
MB_SEQ, MB_SEEN, MB_UNTIL, COUNT = 0x28, 0x30, 0x38, 0x40
CNT_UPDATE, CNT_CTOR, CNT_PARSER, RING = 0x08, 0x10, 0x18, 0x200
OBJ_A, OBJ_B, OBJ_EMPTY, EMPTY_WSTR, VTABLE = 0x100, 0x140, 0x180, 0x1C0, 0x80

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
kernel32.GetUserDefaultLocaleName.argtypes = [ctypes.c_wchar_p, ctypes.c_int]


def site_bytes(text):
    return bytes.fromhex(text.replace(" ", ""))


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
        result, o = [], self.offset(self.imports_rva) if self.imports_rva else None
        while o is not None:
            name = struct.unpack_from("<IIIII", self.data, o)[3]
            if not name:
                break
            result.append(self.text(name))
            o += 20
        return result

    def relocations(self):
        table = self.read(self.relocations_rva, self.relocations_size) if self.relocations_size else b""
        found, at = [], 0
        while at + 8 <= len(table):
            page, size = struct.unpack_from("<II", table, at)
            if size < 8:
                break
            for (entry,) in struct.iter_unpack("<H", table[at + 8:at + size]):
                if entry >> 12 == 10:  # IMAGE_REL_BASED_DIR64
                    found.append(page + (entry & 0xFFF))
            at += size
        return found

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


def q(address):
    return struct.unpack("<Q", peek(address, 8))[0]


# ----------------------------------------------------------- test data

def lsb2(rows):
    """A game LSB2 string table of (id, text) rows, laid out as src/voice_subtitles.c reads it."""
    blob, entries, names, ids = b"", [], b"", []
    for k, (row_id, text) in enumerate(rows):
        hash16 = 0x100 + k
        entries.append((hash16, len(blob) // 2))
        blob += text.encode("utf-16-le")
        ids.append((hash16, len(names)))
        names += row_id.encode("latin1") + b"\0"
    entries.append((0, len(blob) // 2))
    text_section = 40
    texts = struct.pack("<II", 0, len(rows)) + b"".join(struct.pack("<HI", h, o) for h, o in entries) + blob
    id_section = text_section + len(texts)
    id_part = struct.pack("<II", 0, len(rows)) + b"".join(struct.pack("<HHI", h, 0, o) for h, o in ids) + names
    header = b"LSB2" + bytes(16) + struct.pack("<II", text_section, id_section) + bytes(12)
    return header + texts + id_part


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def ticks(seconds):
    seconds = max(seconds, 2.0 - 0.6)
    seconds = min(seconds, 60.0)
    return int((seconds + 0.6) * 10000000 + 0.5)


ROWS = [("HZ2_VO_Ben_Test_1", "Привіт, водію!"), ("HZ2_VO_Ben_Test_1~t", "4.25"),
        ("HZ2_VO_Ashley_Test_2", "Друга репліка"), ("HZ2_VO_ANNA_Long_3", "Довга"), ("HZ2_VO_ANNA_Long_3~t", "300"),
        ("HZ2_VO_ANNA_Short_4", "Коротка"), ("HZ2_VO_ANNA_Short_4~t", "0,5")]


def write_pack(mods, name="Ukrainian", slot="ru-RU", rows=ROWS, how="loose", archive="RU", ini=True):
    folder = mods / name
    tables = folder / "media" / "stripped" / "stringtables"
    tables.mkdir(parents=True, exist_ok=True)
    if ini:
        (folder / "language.ini").write_text(f"[Language]\nName=Українська\nSlot={slot}\n", encoding="utf-16")
    table = lsb2(rows)
    if how == "loose":
        (tables / archive).mkdir(exist_ok=True)
        (tables / archive / "VoiceSubtitles.str").write_bytes(table)
    elif how in ("stored", "deflated"):
        with zipfile.ZipFile(tables / f"{archive}.zip", "w",
                             zipfile.ZIP_STORED if how == "stored" else zipfile.ZIP_DEFLATED) as z:
            z.writestr("Other.str", b"LSB2" + bytes(60))
            z.writestr("voicesubtitles.str", table)
    return folder


def write_azimuth_ini(launcher, language="ru-RU", layout="title"):
    (launcher / "data").mkdir(parents=True, exist_ok=True)
    if layout == "title":
        text = f"[launcher]\ngame=1a96545b\ndisplay=fullscreen\n[title-1a96545b]\nname=Forza Horizon 2\nlanguage={language}\nspeech=en-US\n"
    else:
        text = f"[launcher]\nlanguage={language}\n"
    (launcher / "data" / "azimuth.ini").write_text(text, encoding="utf-8")


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
    check(manifest.get("format_version") == 1 and manifest.get("id") == "lang-support" and valid_mod_id(manifest["id"]),
          f"mod.json: format_version 1 and the id lang-support ({manifest.get('id')})")
    check(manifest.get("name") == MOD_NAME and isinstance(manifest.get("dependencies"), list) and
          "media" not in manifest and "database" not in manifest,
          "mod.json: Language Support, dependencies list, no media or database of its own")
    files = sorted(str(p.relative_to(mod)).replace("\\", "/") for p in mod.rglob("*") if p.is_file())
    check(files == ["mod.json", "plugins/" + DLL_NAME, "plugins/" + INI_NAME],
          f"out/mods/{MOD_NAME}: mod.json, plugins/{DLL_NAME}, plugins/{INI_NAME} ({files})")
    pe = PE(mod / "plugins" / DLL_NAME)
    check(pe.machine == 0x8664 and pe.magic == 0x20B and pe.characteristics & 0x2000, "the DLL is a PE32+ x64 DLL")
    check(pe.exports() == ["FH2PluginInitialize"], f"the one export is FH2PluginInitialize ({pe.exports()})")
    imports = pe.imports()
    check(all(SYSTEM_DLLS.match(name) for name in imports), f"only Windows DLLs are imported ({', '.join(imports)})")
    version = manifest.get("version", "")
    check(bool(pe.resources_rva) and ("FileVersion\0" + version).encode("utf-16le") in pe.data.replace(b"\0\0\0", b"\0"),
          f"version resource with FileVersion {version}")
    test_pe = PE(output / "tests" / TEST_DLL)
    check("FH2PluginInitialize" in test_pe.exports() and "LangTestTable" in test_pe.exports(),
          "the test DLL has the plugin entry and the test entry points")
    ini = configparser.ConfigParser()
    ini.read(mod / "plugins" / INI_NAME, encoding="utf-8")
    check(ini.get("LangSupport", "Pack", fallback=None) == "" and
          ini.get("LangSupport", "VoiceSubtitles", fallback=None) == "1",
          "LangSupport.ini: [LangSupport] Pack= (the one pack) and VoiceSubtitles=1")
    check(sorted(ini.options("LangSupport")) == ["pack", "voicesubtitles"], "LangSupport.ini: no other keys")


# ---------------------------------------------------------------- 2. azimuth

def test_azimuth(dll, azimuth_zip, game):
    print("azimuth")
    if not azimuth_zip or not azimuth_zip.is_file():
        skip(f"Azimuth ZIP not found ({azimuth_zip})")
        return None
    with zipfile.ZipFile(azimuth_zip) as archive:
        pe = PE(data=archive.read("azimuth.exe"))
    check((pe.timestamp, pe.image_size) == AZIMUTH_104,
          f"azimuth.exe 1.0.4: TimeDateStamp 0x{pe.timestamp:x}, SizeOfImage 0x{pe.image_size:x}")
    host = pe.map()
    check(dll.LangTestBuild(host) == 2, "the plugin knows it as Azimuth 1.0.4 (by its PE header)")
    check(dll.LangTestBuild(None) == 0, "no host image: unknown")

    def wide(text):
        return text.encode("utf-16le") + b"\0\0"
    keys = ["azimuth.ini", "launcher", "title-", "language", "speech", "system", "content"]
    check(all(wide(k) in pe.data for k in keys),
          "its settings keys are there (UTF-16): azimuth.ini, launcher, title-, language, speech, system")
    check(b"azimuth.ini: the game settings under [launcher] became each added game's own (titles " in pe.data,
          "since 1.0.0 the game's settings (its language) are under [title-<game>], not [launcher]")
    check(all(wide(code) in pe.data for code in ("ru-RU", "en-US", "pl-PL", "pt-BR", "zh-TW")),
          "its launcher lists the game language codes the pack slots use (ru-RU, en-US, ...)")
    ini = game / "data" / "azimuth.ini"
    if ini.is_file():
        buffer = ctypes.create_unicode_buffer(85)
        named = dll.LangTestLauncherLanguage(str(ini), buffer, 85)
        config = configparser.ConfigParser(strict=False, interpolation=None)
        config.read_string(ini.read_text(encoding="utf-8", errors="replace"))
        game_id = config.get("launcher", "game", fallback="")
        expected = config.get(f"title-{game_id}", "language", fallback="")
        check(named == (1 if expected and expected.lower() != "system" else 0) and
              (not named or buffer.value == expected),
              f"the real data\\azimuth.ini: [launcher] game={game_id}, [title-{game_id}] language={expected} "
              f"(the plugin reads {buffer.value})")
    else:
        skip(f"no data\\azimuth.ini below {game}")
    return host


# ---------------------------------------------------------------- 3. release

def test_release(release, upstream):
    print("release")
    winmm = release / "winmm.dll" if release else None
    if winmm and winmm.is_file():
        data = winmm.read_bytes()
        check(b"0.4.4\0" in data, "winmm.dll is Extra Options 0.4.4")
        check(b"FILES: mediaoverride > mediapc > mods (last first) > original" in data and b"__fh2_mods__" in data,
              "it mounts mod folders' media below mediaoverride and mediapc (game:\\__fh2_mods__\\NNN\\)")
        check(b"media addon ready" in data and b"FH2PluginInitialize" in data and b"could not start loader" in data,
              "it has media-only mods and the plugin loader (FH2PluginInitialize)")
    else:
        skip(f"Extra Options 0.4.4 release not found ({release})")
    if not upstream or not (upstream / "features" / "media_overrides.c").is_file():
        skip(f"Extra Options 0.4.4 source not found ({upstream})")
        return
    media = (upstream / "features" / "media_overrides.c").read_text(encoding="utf-8", errors="replace")
    resolve = media[media.index("static unsigned char resolve_file("):]
    resolve = resolve[:resolve.index("\n}\n")]
    check(resolve.index("plugins_start();") < resolve.index("ensure_folders_mounted(filesystem);") <
          resolve.index("original_resolve("),
          "source: every native file lookup starts the plugin loader, then mounts the mod folders, then resolves")
    mount = media[media.index("static void ensure_folders_mounted("):]
    mount = mount[:mount.index("\n}\n")]
    check("addons_media_folder(i)" in mount and "0x70000001u + i * 2" in mount and
          'mount_folder(filesystem, "game:\\\\mediapc\\\\", 0x7ffffffd' in mount,
          "source: the mod folders are mounted there, synchronously, at 0x70000001+2i (mediapc 0x7ffffffd above)")
    plugins = (upstream / "features" / "plugins.c").read_text(encoding="utf-8", errors="replace")
    start = plugins[plugins.index("void plugins_start(void)"):]
    check("CreateThread(NULL, 0, load_plugins" in start[:start.index("\n}\n")],
          "source: the plugins load on a worker thread the lookup does not wait for (a plugin may come late)")
    addons = (upstream / "features" / "addons.c").read_text(encoding="utf-8", errors="replace")
    check("if (!addon->database.root && dependencies_ready(addon, 1)) {\n        InterlockedExchange(&addon->active, 1);"
          in addons, "source: a mod without a database is active from discovery (its media serve the first lookup)")
    settings = (upstream / "settings.c").read_text(encoding="utf-8", errors="replace")
    check('(text_equal_ignore_case(key, "Text") || text_equal_ignore_case(key, "Audio"))' in settings and
          "if (!definition || !parse_value(definition, value, &parsed)) {\n            goto invalid;" in settings and
          "REFUSED: invalid INI section, option, or value" in settings,
          "source: FH2_ExtraOptions.ini [Language] knows only the retired Text/Audio; any other key refuses the INI")
    check("languages.c" not in (upstream / "build.py").read_text(encoding="utf-8", errors="replace"),
          "source: its old language override (features/languages.c) is not built")
    patches = json.loads((upstream / "patches.json").read_text(encoding="utf-8"))
    check(patches.get("version") == "0.4.4" and patches.get("original_sha256") == GAME_SHA256,
          "patches.json: 0.4.4, for the game executable these tests read")
    ranges = [(rva, rva + 16) for rva, _, _ in SITES] + [(SET_TEXT[0], SET_TEXT[0] + 16)]
    touched = []
    for group in ("patches", "runtime_hooks", "runtime_data"):
        for p in patches.get(group, []):
            size = max(len(p.get("original", "")) // 2, len(p.get("patched", "")) // 2, int(p.get("size", 8)), 8)
            for key, value in p.items():
                if key.endswith("rva") and isinstance(value, int):
                    if any(value < high and low < value + size for low, high in ranges):
                        touched.append(p.get("id"))
    check(not touched, f"none of its {sum(len(patches.get(g, [])) for g in ('patches', 'runtime_hooks', 'runtime_data'))} "
                       f"patches and hooks touches the five hook sites or SetText ({touched})")


# ------------------------------------------------------------------- test DLL

def load_test_dll(path):
    dll = ctypes.CDLL(str(path))
    dll.LangTestUseHost.argtypes = [ctypes.c_void_p]
    dll.LangTestSlotArchive.argtypes = [ctypes.c_wchar_p]
    dll.LangTestSlotArchive.restype = ctypes.c_wchar_p
    dll.LangTestBuild.argtypes = [ctypes.c_void_p]
    dll.LangTestCheck.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    dll.LangTestCheck.restype = ctypes.c_char_p
    dll.LangTestCave.restype = ctypes.c_void_p
    dll.LangTestLoadOrder.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
    dll.LangTestLauncherLanguage.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint]
    dll.LangTestFindPack.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint]
    dll.LangTestFindPack.restype = ctypes.c_char_p
    dll.LangTestTable.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_char_p)]
    dll.LangTestTable.restype = ctypes.c_uint
    dll.LangTestEntry.argtypes = [ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_void_p),
                                  ctypes.POINTER(ctypes.c_uint)]
    return dll


# ------------------------------------------------------------------- 4. game

def test_game(dll, game_exe):
    print("game")
    if not game_exe.is_file():
        skip(f"game executable not found ({game_exe})")
        return None
    pe = PE(game_exe)
    check(pe.image_size == IMAGE_SIZE and pe.image_base == PREFERRED_BASE,
          f"forza_durango_release_final.exe: image size 0x{pe.image_size:x}, preferred base 0x{pe.image_base:x}")
    for rva, _, guard in SITES + [(SET_TEXT[0], 0, SET_TEXT[1])]:
        check(pe.read(rva, 16) == site_bytes(guard), f"0x{rva:x} begins {site_bytes(guard).hex(' ')}")
    base = pe.map()
    why = dll.LangTestCheck(base, IMAGE_SIZE)
    check(why is None, f"the mapped game passes the plugin's guards ({why})")
    check(dll.LangTestCheck(base, IMAGE_SIZE - 0x1000) is not None, "another image size is refused")
    names = ["0xf50ee0", "0xf51100", "0xf512a0", "0xc3df10", "0x85afa0", "0xc3a4b0"]
    for (rva, _, _), name in zip(SITES + [(SET_TEXT[0], 0, "")], names):
        for offset, value, what in ((0, b"\xe9", "a jump at its first byte"), (9, b"\xcc", "a changed byte")):
            original = peek(base + rva + offset, 1)
            poke(base + rva + offset, value)
            why = dll.LangTestCheck(base, IMAGE_SIZE)
            poke(base + rva + offset, original)
            text = why.decode() if why else ""
            check(name in text and (offset or "another module hooks it" in text), f"{name} with {what}: refused ({text})")
    unmap(base)
    return pe


# ------------------------------------------------------------------- 5. code

def code_header():
    text = (ROOT / "src" / "voice_subtitles_code.h").read_text(encoding="utf-8")
    defines = {m.group(1): int(m.group(2), 0) for m in re.finditer(r"#define (VC_\w+) (0x[0-9A-Fa-f]+|\d+)", text)}

    def array(name, kind=r"unsigned char"):
        body = re.search(rf"static const {kind} {name}\[\d+\] = \{{(.*?)\}};", text, re.S).group(1)
        return [int(v, 0) for v in re.findall(r"0x[0-9A-Fa-f]+|\d+", body)]
    head, code = bytes(array("vc_head")), bytes(array("vc_code"))
    abs64, rel32 = array("vc_rel_abs64", "unsigned"), array("vc_rel_rel32", "unsigned")
    hooks = [(int(m.group(1), 16), int(m.group(2), 16), int(m.group(3)), bytes(int(b, 16) for b in m.group(4).split(", ")))
             for m in re.finditer(r"\{0x([0-9A-F]+)ULL, 0x([0-9A-F]+), (\d), \{([^}]*)\}\}", text)]
    return defines, head, code, abs64, rel32, hooks


def test_code():
    print("code")
    d, head, code, abs64, rel32, hooks = code_header()
    check(d["VC_LINK_CAVE"] == 0x143A20000 and d["VC_CAVE_SIZE"] == 0x52000 and len(head) == 0x200 and
          d["VC_OFF_CODE"] == 0x50200 and d["VC_OFF_TABLE"] == 0x4200 and d["VC_OFF_POOL"] == 0x10200,
          "the hook code is laid out as voice_hooks.py builds it (head 0x200, table 0x4200, pool 0x10200, code 0x50200)")
    check([(PREFERRED_BASE + rva, n, site_bytes(g)[:n]) for rva, n, g in SITES] == [(s, n, o[:n]) for s, _, n, o in hooks],
          "its five hook sites, sizes and replaced bytes are the guarded ones")
    check(head[:4] == b"FH2S", "its header starts FH2S")
    link = d["VC_LINK_CAVE"]
    inside = all(link <= struct.unpack_from("<Q", head if a < 0x200 else code, a if a < 0x200 else a - d["VC_OFF_CODE"])[0]
                 < link + d["VC_CAVE_SIZE"] for a in abs64)
    check(len(abs64) == d["VC_N_REL_ABS64"] and inside,
          f"its {len(abs64)} absolute pointers point into the cave (vtable, objects)")
    targets = sorted(link + o + 4 + struct.unpack_from("<i", code, o - d["VC_OFF_CODE"])[0] for o in rel32)
    expected = sorted([s + n for s, _, n, _ in hooks] + [PREFERRED_BASE + SET_TEXT[0]])
    check(len(rel32) == d["VC_N_REL_REL32"] and targets == expected,
          "its six rel32s are the five jumps back behind the replaced bytes and the call of SetText 0xc3a4b0")
    entries = {}
    for i, (_, hook, n, original) in enumerate(hooks):
        at = code[hook - d["VC_OFF_CODE"]:hook - d["VC_OFF_CODE"] + 8]
        entries[i] = at
    check(all(entries[i].startswith(b"\x4c\x89\x4c\x24\x20") for i in (CTOR, PREP, PREP3D)),
          "the constructor hooks start with the replaced mov [rsp+20h], r9")
    check(entries[UPDATE].startswith(b"\x55\x53\x56\x57") and entries[PARSER].startswith(b"\x55\x56\x57\x41\x54"),
          "the Update and parser hooks start with the replaced pushes")


# ------------------------------------------------------------------- 6. table

def table_of(dll, stringtables, archive="RU"):
    why = ctypes.c_char_p()
    count = dll.LangTestTable(str(stringtables), archive, ctypes.byref(why))
    rows = []
    for i in range(count):
        h, text, duration = ctypes.c_uint64(), ctypes.c_void_p(), ctypes.c_uint()
        dll.LangTestEntry(i, ctypes.byref(h), ctypes.byref(text), ctypes.byref(duration))
        rows.append((h.value, ctypes.wstring_at(text.value), duration.value))
    return rows, (why.value.decode() if why.value else None)


def test_table(dll, work, game):
    print("table")
    expected = [(fnv1a64(b"HZ2_VO_Ben_Test_1"), "Привіт, водію!", ticks(4.25)),
                (fnv1a64(b"HZ2_VO_Ashley_Test_2"), "Друга репліка", ticks(3.0)),
                (fnv1a64(b"HZ2_VO_ANNA_Long_3"), "Довга", ticks(60.0)),
                (fnv1a64(b"HZ2_VO_ANNA_Short_4"), "Коротка", ticks(1.4))]
    for how in ("loose", "stored"):
        folder = write_pack(work / how / "mods", how=how)
        rows, why = table_of(dll, folder / "media" / "stripped" / "stringtables")
        check(rows == expected, f"{how}: 4 lines, FNV-1a 64 of the ids, texts, ~t times (+0.6 s, 2..60 s; 3.6 s "
                                f"without a time) ({why})")
    folder = write_pack(work / "both" / "mods", how="stored")
    (folder / "media/stripped/stringtables/RU").mkdir()
    (folder / "media/stripped/stringtables/RU/VoiceSubtitles.str").write_bytes(lsb2(ROWS[:1]))
    rows, _ = table_of(dll, folder / "media" / "stripped" / "stringtables")
    check(len(rows) == 1, "a loose table goes before the one in the ZIP")
    folder = write_pack(work / "deflated" / "mods", how="deflated")
    rows, why = table_of(dll, folder / "media" / "stripped" / "stringtables")
    check(not rows and why and "compressed" in why, f"a compressed ZIP entry is refused ({why})")
    folder = write_pack(work / "none" / "mods", how="none")
    rows, why = table_of(dll, folder / "media" / "stripped" / "stringtables")
    check(not rows and why and "no VoiceSubtitles.str" in why, f"no table: refused ({why})")
    for label, data, text in (("bad magic", b"LSB1" + lsb2(ROWS)[4:], "not an LSB2"),
                              ("truncated", lsb2(ROWS)[:60], "truncated"),
                              ("only times", lsb2([("A~t", "3")]), "no lines")):
        folder = write_pack(work / label.replace(" ", "-") / "mods")
        (folder / "media/stripped/stringtables/RU/VoiceSubtitles.str").write_bytes(data)
        rows, why = table_of(dll, folder / "media" / "stripped" / "stringtables")
        check(not rows and why and text in why, f"{label}: refused ({why})")
    big = [(f"HZ2_VO_Line_{i}", "x") for i in range(2100)]
    folder = write_pack(work / "full" / "mods", rows=big)
    rows, _ = table_of(dll, folder / "media" / "stripped" / "stringtables")
    check(len(rows) == 2048, f"at most 2048 lines (the hook table's size) ({len(rows)})")
    real = game / UK_TABLE
    if real.is_file():
        rows, why = table_of(dll, real.parent.parent)
        check(len(rows) == 982 and all(text for _, text, _ in rows) and
              all(ticks(1.4) <= d <= ticks(60) for _, _, d in rows),
              f"the Ukrainian pack's VoiceSubtitles.str: {len(rows)} lines (982), each with a text and a time")
    else:
        skip(f"the fork's Ukrainian pack not found ({real})")


# ------------------------------------------------------------------- 7. pack

def test_pack(dll, work):
    print("pack")
    slots = {"en-US": "EN", "en-GB": "GB", "de-DE": "DE", "fr-FR": "FR", "it-IT": "IT", "es-ES": "ES", "es-MX": "MX",
             "pt-BR": "br", "pl-PL": "PL", "ru-RU": "RU", "ja-JP": "JP", "ko-KR": "KO", "zh-TW": "CHT", "zh-HK": "CHT",
             "nl-NL": "nl", "cs-CZ": "cz", "hu-HU": "HU"}
    check(all(dll.LangTestSlotArchive(s) == a and dll.LangTestSlotArchive(s.upper()) == a for s, a in slots.items()),
          "the 17 slots name the game's 15 string table archives (any case)")
    check(dll.LangTestSlotArchive("uk-UA") is None and dll.LangTestSlotArchive("") is None,
          "a language the game has no slot for (uk-UA) is refused")

    def find(mods, wanted=""):
        out = ctypes.create_unicode_buffer(600)
        why = dll.LangTestFindPack(str(mods), wanted, out, 600)
        return (why.decode() if why else None), out.value

    mods = work / "one" / "mods"
    write_pack(mods, "Ukrainian")
    (mods / "Exit Game" / "plugins").mkdir(parents=True)
    check(find(mods) == (None, "Ukrainian|ru-RU|Українська"), "Pack= empty: the one folder with a language.ini")
    check(find(mods, "ukrainian")[0] is None, "Pack=<folder>: that folder (any case)")
    why, _ = find(mods, "Exit Game")
    check(why and "no language.ini" in why, f"Pack= a folder without language.ini: refused ({why})")
    for bad in ("..", "a\\b", "C:x"):
        why, _ = find(mods, bad)
        check(why and "name of a folder" in why, f"Pack={bad}: refused ({why})")
    write_pack(mods, "Second")
    why, _ = find(mods)
    check(why and "several" in why, f"two packs and Pack= empty: refused ({why})")
    check(find(mods, "Second")[0] is None, "two packs: Pack= picks one")
    why, _ = find(work / "none")
    check(why and "mods folder" in why, f"no mods folder: refused ({why})")
    empty = work / "empty" / "mods"
    empty.mkdir(parents=True)
    why, _ = find(empty)
    check(why and "no folder" in why, f"no pack: nothing ({why})")
    write_pack(work / "badslot" / "mods", slot="uk-UA")
    why, _ = find(work / "badslot" / "mods")
    check(why and "Slot" in why, f"a pack with Slot=uk-UA: refused ({why})")
    linked = work / "dot" / "mods"
    write_pack(linked, ".hidden")
    why, _ = find(linked)
    check(why and "no folder" in why, "a dot folder is not a pack (as Extra Options skips it)")

    order = work / "order" / "mods"
    order.mkdir(parents=True)
    check(dll.LangTestLoadOrder(str(order), "Ukrainian") == -1, "no loadorder.txt: not listed (-1)")
    (order / "loadorder.txt").write_bytes("﻿# First\r\nExit Game\r\n  Українська  \r\n#   Ukrainian\r\n".encode("utf-8"))
    check(dll.LangTestLoadOrder(str(order), "Ukrainian") == 0, "'#   Ukrainian': commented out (0)")
    check(dll.LangTestLoadOrder(str(order), "українська") == 1, "a Unicode folder name, any case, CRLF and BOM: listed (1)")
    check(dll.LangTestLoadOrder(str(order), "Other") == -1, "a folder not in it: not listed (-1)")

    launcher = work / "launcher"
    buffer = ctypes.create_unicode_buffer(85)
    write_azimuth_ini(launcher, "ru-RU")
    ini = str(launcher / "data" / "azimuth.ini")
    check(dll.LangTestLauncherLanguage(ini, buffer, 85) == 1 and buffer.value == "ru-RU",
          "azimuth.ini [launcher] game -> [title-<game>] language=ru-RU")
    write_azimuth_ini(launcher, "pl-PL", layout="old")
    check(dll.LangTestLauncherLanguage(ini, buffer, 85) == 1 and buffer.value == "pl-PL",
          "no title section: [launcher] language (before Azimuth 1.0.0)")
    locale = ctypes.create_unicode_buffer(85)
    kernel32.GetUserDefaultLocaleName(locale, 85)
    write_azimuth_ini(launcher, "system")
    check(dll.LangTestLauncherLanguage(ini, buffer, 85) == 0 and buffer.value == locale.value,
          f"language=system: the Windows locale ({locale.value})")
    check(dll.LangTestLauncherLanguage(str(launcher / "missing.ini"), buffer, 85) == 0 and buffer.value == locale.value,
          "no azimuth.ini: the Windows locale")


# ------------------------------------------------------------------- 8. tool

def test_tool(dll, work):
    print("tool")
    tool = ROOT / "tools" / "make_language_pack.py"
    original, modded = work / "original", work / "modded"
    for folder in (original, modded):
        (folder / "stripped" / "stringtables").mkdir(parents=True)
        (folder / "ui").mkdir()
    with zipfile.ZipFile(original / "stripped" / "stringtables" / "RU.zip", "w") as z:
        z.writestr("Same.str", b"same")
        z.writestr("Changed.str", b"old")
    with zipfile.ZipFile(modded / "stripped" / "stringtables" / "RU.zip", "w") as z:
        z.writestr("Same.str", b"same")
        z.writestr("Changed.str", b"new")
        z.writestr("VoiceSubtitles.str", lsb2(ROWS))
    with zipfile.ZipFile(original / "ui" / "Fonts.zip", "w") as z:
        z.writestr("Anthem_A.vfont", b"font")
    with zipfile.ZipFile(modded / "ui" / "Fonts.zip", "w") as z:
        z.writestr("Anthem_A.vfont", b"font with more glyphs")
    for folder in (original, modded):
        (folder / "zipmanifest.xml").write_text("<manifest/>", encoding="utf-8")
        (folder / "ui" / "same.xml").write_text("<a/>", encoding="utf-8")
    (modded / "ui" / "new.xml").write_text("<b/>", encoding="utf-8")
    output = work / "mods" / "Ukrainian"
    run = subprocess.run([sys.executable, "-I", str(tool), "build", str(original), str(modded), str(output),
                          "--slot", "ru-ru", "--name", "Українська", "--id", "uk"],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(run.returncode == 0, f"build: runs ({run.stdout.strip() or run.stderr.strip()})")
    files = sorted(p.relative_to(output).as_posix() for p in output.rglob("*") if p.is_file())
    check(files == ["language.ini", "media/stripped/stringtables/RU/Changed.str",
                    "media/stripped/stringtables/RU/VoiceSubtitles.str", "media/ui/Fonts/Anthem_A.vfont",
                    "media/ui/new.xml", "mod.json"],
          f"build: only what changed, in Extra Options' mod layout (X.zip -> a becomes media/X/a) ({files})")
    manifest = json.loads((output / "mod.json").read_text(encoding="utf-8"))
    check(manifest["format_version"] == 1 and manifest["id"] == "uk" and valid_mod_id(manifest["id"]) and
          manifest["media"] == "media" and manifest["database"] is None,
          "build: mod.json is a media-only mod Extra Options 0.4.4 takes (id uk, database null)")
    out = ctypes.create_unicode_buffer(600)
    why = dll.LangTestFindPack(str(work / "mods"), "", out, 600)
    check(why is None and out.value == "Ukrainian|ru-RU|Українська",
          f"build: the plugin finds the pack and its slot ({out.value or why})")
    rows, _ = table_of(dll, output / "media" / "stripped" / "stringtables")
    check(len(rows) == 4, "build: its VoiceSubtitles.str reads (4 lines)")
    again = subprocess.run([sys.executable, "-I", str(tool), "build", str(original), str(modded), str(output),
                            "--slot", "ru-RU", "--name", "x"], capture_output=True, text=True)
    check(again.returncode != 0 and "not empty" in again.stderr, "build: refuses a folder that is not empty")
    bad = subprocess.run([sys.executable, "-I", str(tool), "build", str(original), str(modded), str(work / "x"),
                          "--slot", "uk-UA", "--name", "x"], capture_output=True, text=True)
    check(bad.returncode != 0 and "not a game language code" in bad.stderr, "build: refuses --slot uk-UA")

    fork = work / "fork" / "uk"
    (fork / "stripped" / "stringtables" / "RU").mkdir(parents=True)
    (fork / "ui" / "Fonts").mkdir(parents=True)
    (fork / "stripped" / "stringtables" / "RU" / "VoiceSubtitles.str").write_bytes(lsb2(ROWS))
    (fork / "ui" / "Fonts" / "verdana.vfont").write_bytes(b"v")
    (fork / "language.ini").write_text("; Custom language pack\n[Language]\nName=Українська\nSlot=ru-RU\n",
                                       encoding="utf-16")
    converted = work / "mods2" / "Ukrainian (uk)"
    run = subprocess.run([sys.executable, "-I", str(tool), "convert", str(fork), str(converted)],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    check(run.returncode == 0, f"convert: runs ({run.stdout.strip() or run.stderr.strip()})")
    files = sorted(p.relative_to(converted).as_posix() for p in converted.rglob("*") if p.is_file())
    check(files == ["language.ini", "media/stripped/stringtables/RU/VoiceSubtitles.str", "media/ui/Fonts/verdana.vfont",
                    "mod.json"], f"convert: the fork's pack below media/, its language.ini beside ({files})")
    manifest = json.loads((converted / "mod.json").read_text(encoding="utf-8"))
    why = dll.LangTestFindPack(str(work / "mods2"), "Ukrainian (uk)", out, 600)
    check(manifest["id"] == "uk" and why is None and out.value == "Ukrainian (uk)|ru-RU|Українська",
          f"convert: mod id from the pack's id (uk); the plugin finds it ({out.value or why})")


# ----------------------------------------------------------------- 9. plugin

LOG = ctypes.CFUNCTYPE(None, ctypes.c_char_p)


class Context(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("api_version", ctypes.c_uint32), ("adapter", ctypes.c_uint32),
                ("game_image_size", ctypes.c_uint32), ("game_image", ctypes.c_void_p),
                ("plugin_module", ctypes.c_void_p), ("plugins_directory", ctypes.c_wchar_p),
                ("plugin_path", ctypes.c_wchar_p), ("extra_options_version", ctypes.c_char_p), ("log_utf8", LOG)]


class Plugin:
    """A fresh copy of the test DLL in <work>\\mods\\Language Support\\plugins, as Extra Options 0.4.4 calls it."""

    def __init__(self, output, work, ini="[LangSupport]\nPack=\nVoiceSubtitles=1\n", host=None):
        folder = work / "mods" / MOD_NAME / "plugins"
        folder.mkdir(parents=True, exist_ok=True)
        shutil.copy2(output / "tests" / TEST_DLL, folder / DLL_NAME)
        (folder / INI_NAME).write_text(ini, encoding="utf-8")
        self.dll = load_test_dll(folder / DLL_NAME)
        self.dll.FH2PluginInitialize.argtypes = [ctypes.POINTER(Context)]
        self.dll.FH2PluginInitialize.restype = wintypes.BOOL
        if host:
            self.dll.LangTestUseHost(host)
        self.lines = []
        self.log = LOG(lambda text: self.lines.append(text.decode("utf-8", "replace")))
        self.folder, self.path = str(folder), str(folder / DLL_NAME)

    def initialize(self, base, adapter=1, size=IMAGE_SIZE, version=b"0.4.4"):
        self.context = Context(ctypes.sizeof(Context), 1, adapter, size, base, None, self.folder, self.path, version,
                               self.log)
        return self.dll.FH2PluginInitialize(ctypes.byref(self.context))

    def said(self, text):
        return any(text in line for line in self.lines)

    def last(self):
        return self.lines[-1].strip() if self.lines else "no log"


def std_string(text):
    """An MSVC std::string (32 bytes) holding `text`; returns (buffer, keep-alive)."""
    data = text.encode("latin1")
    raw = ctypes.create_string_buffer(32)
    keep = None
    if len(data) < 16:
        ctypes.memmove(raw, data, len(data))
        struct.pack_into("<QQ", raw, 16, len(data), 15)
    else:
        keep = ctypes.create_string_buffer(data + b"\0")
        struct.pack_into("<QQQ", raw, 0, ctypes.addressof(keep), 0, 0)
        struct.pack_into("<QQ", raw, 16, len(data), len(data) | 15)
    return raw, keep


def now():
    return ctypes.c_uint64.from_address(0x7FFE0008).value  # KUSER_SHARED_DATA.InterruptTime


def test_plugin(output, pe, host, work):
    print("plugin")
    if not pe or not host:
        skip("the plugin on the mapped game: needs the game executable and Azimuth 1.0.4")
        return
    d, _, code, _, rel32, hooks = code_header()

    def sites_of(base):
        return [peek(base + rva, 8) for rva, _, _ in SITES]

    def scenario(label, ini="[LangSupport]\nPack=\nVoiceSubtitles=1\n", language="ru-RU", pack="loose", order=None,
                 eo_ini=None, host_image=host, adapter=1, version=b"0.4.4", size=IMAGE_SIZE):
        folder = work / label.replace(" ", "-").replace("=", "").replace(".", "")
        if pack:
            write_pack(folder / "mods", how=pack)
        else:
            (folder / "mods").mkdir(parents=True, exist_ok=True)
        write_azimuth_ini(folder, language)
        if order is not None:
            (folder / "mods" / "loadorder.txt").write_text(order, encoding="utf-8")
        if eo_ini is not None:
            (folder / "FH2_ExtraOptions.ini").write_text(eo_ini, encoding="utf-8")
        plugin = Plugin(output, folder, ini, host_image)
        return plugin, plugin.initialize, (adapter, size, version)

    def unchanged(label, expect, damage=None, **options):
        base = pe.map()
        if damage:
            poke(base + damage[0], damage[1])
        before = sites_of(base)
        plugin, initialize, (adapter, size, version) = scenario(label, **options)
        ready = initialize(base, adapter, size, version)
        check(ready == 1 and sites_of(base) == before and plugin.dll.LangTestCave() is None and plugin.said(expect),
              f"{label}: the game is left as it is ({plugin.last()})")
        unmap(base)
        return plugin

    # --- installed, on a game image that is not at its preferred base.
    base = pe.map()
    check(base != PREFERRED_BASE, f"the game image is mapped at 0x{base:x}, not its preferred base")
    plugin, initialize, _ = scenario("installed", order="Exit Game\nUkrainian\n")
    ready = initialize(base)
    cave = plugin.dll.LangTestCave()
    print("    " + "    ".join(plugin.lines).rstrip())
    check(ready == 1 and cave and plugin.said("voice subtitles installed: 4 lines"),
          f"FH2PluginInitialize: voice subtitles installed ({plugin.last()})")
    check(plugin.said("language pack mods\\Ukrainian: Українська in the ru-RU slot (string tables RU)") and
          plugin.said("Extra Options mounts the pack's media\\ as a mod (below mediaoverride and mediapc)") and
          plugin.said("Azimuth 1.0.4 runs the game in ru-RU"),
          "its log names the pack, its slot, the mount and the launcher's language")
    if not cave:
        unmap(base)
        return
    entries = []
    for i, (rva, n, _) in enumerate(SITES):
        at = peek(base + rva, n)
        target = base + rva + 5 + struct.unpack_from("<i", at, 1)[0]
        entries.append(target)
        check(at[0] == 0xE9 and target == cave + hooks[i][1] and at[5:] == b"\x90" * (n - 5),
              f"0x{rva:x}: jmp to the hook code (+0x{hooks[i][1]:x}){' and a nop' if n > 5 else ''}")
    check(abs(cave - base) < 0x7FF00000, f"the hook code is within 2 GB of the image (0x{cave:x})")
    found = sorted(cave + o + 4 + struct.unpack("<i", peek(cave + o, 4))[0] for o in rel32)
    expected = sorted([base + rva + n for rva, n, _ in SITES] + [base + SET_TEXT[0]])
    check(found == expected, "relocated: its jumps back and the SetText call go to the mapped image")
    check(q(cave + COUNT) == 4 and q(cave + OBJ_A) == cave + VTABLE and q(cave + OBJ_EMPTY + 0x38) == cave + EMPTY_WSTR,
          "relocated: the table holds 4 lines; the objects' vtable and the empty text point into the cave")

    # The game's functions behind the hooks, stubbed (the game's code cannot run here).
    record = (ctypes.c_uint64 * 3)()  # SetText's rcx, the object, calls
    set_text = (b"\x48\x89\xc8" + b"\x48\xa3" + struct.pack("<Q", ctypes.addressof(record)) + b"\x48\x8b\x02" +
                b"\x48\xa3" + struct.pack("<Q", ctypes.addressof(record) + 8) + b"\x48\xb8" +
                struct.pack("<Q", ctypes.addressof(record) + 16) + b"\x48\xff\x00\xc3")
    poke(base + SET_TEXT[0], set_text)
    for i in (CTOR, PREP, PREP3D):
        poke(base + SITES[i][0] + 5, b"\xc3")
    poke(base + SITES[UPDATE][0] + 5, b"\x5f\x5e\x5b\x5d\xc3")
    poke(base + SITES[PARSER][0] + 6, b"\x41\x5c\x5f\x5e\x5d\xc3")
    hook = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p)
    update = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_void_p)
    data_source = ctypes.create_string_buffer(0x100)

    def speak(which, name, folder="audio", third="3d"):
        a, ka = std_string(folder)
        b, kb = std_string(name)
        c, kc = std_string(third)
        hook(entries[which])(0x1234, ctypes.addressof(a), ctypes.addressof(b), ctypes.addressof(c))

    def shown():
        obj = record[1]
        getwide = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(q(q(obj) + 7 * 8))
        return obj, ctypes.wstring_at(getwide(obj))

    speak(CTOR, "VO/Ben/HZ2_VO_Ben_Test_1")
    ring = peek(cave + RING, 256)
    check(q(cave + MB_SEQ) == 1 and q(cave + CNT_CTOR) == 1 and ring[:5] == b"audio" and
          ring[32:56] == b"VO/Ben/HZ2_VO_Ben_Test_1" and ring[255] == 1,
          "PlaySpeech hook: the event is logged in the ring and its last path component matches a line")
    until = q(cave + MB_UNTIL) - now()
    check(abs(until - ticks(4.25)) < 10000000, f"it is shown for its ~t time + 0.6 s ({until / 1e7:.2f} s)")
    update(entries[UPDATE])(ctypes.addressof(data_source))
    obj, text = shown()
    check(record[2] == 1 and record[0] == ctypes.addressof(data_source) + 0x38 and obj == cave + OBJ_B and
          text == "Привіт, водію!",
          f"SubtitleDataSource::Update hook: SetText(source+0x38, the line) shows it ({text})")
    update(entries[UPDATE])(ctypes.addressof(data_source))
    check(record[2] == 1 and q(cave + CNT_UPDATE) == 2, "the next frame: nothing new while it is shown")
    speak(PREP, "HZ2_VO_Unknown_9")
    check(q(cave + MB_SEQ) == 1, "PreparePlayVO hook: an event without a line changes nothing")
    speak(PREP3D, "x\\y/HZ2_VO_Ashley_Test_2")
    update(entries[UPDATE])(ctypes.addressof(data_source))
    obj, text = shown()
    check(record[2] == 2 and obj == cave + OBJ_A and text == "Друга репліка" and peek(cave + RING + 2 * 256 + 192, 2) == b"3d"
          and peek(cave + RING + 2 * 256 + 255, 1) == b"\x03",
          f"PreparePlay3DVO hook: the next line is shown through the other object ({text}); its third string is logged")
    poke(cave + MB_UNTIL, struct.pack("<Q", 1))
    update(entries[UPDATE])(ctypes.addressof(data_source))
    obj, text = shown()
    check(record[2] == 3 and obj == cave + OBJ_EMPTY and text == "" and q(cave + MB_UNTIL) == 0,
          "when its time is up, Update shows the empty text once")
    update(entries[UPDATE])(ctypes.addressof(data_source))
    check(record[2] == 3, "and then nothing until the next line")
    hook(entries[PARSER])(0, 0, 0, 0)
    check(q(cave + CNT_PARSER) == 1, "DialogueScript parser hook: counted, then the parser runs on")
    unmap(base)

    plugin, initialize, _ = scenario("stored in RU.zip", pack="stored")
    base = pe.map()
    check(initialize(base) == 1 and plugin.dll.LangTestCave() and plugin.said("voice subtitles installed: 4 lines"),
          f"the table stored in the pack's RU.zip: installed ({plugin.last()})")
    unmap(base)

    unchanged("launcher en-US", "choose ru-RU in the launcher to play in the pack's language; voice subtitles are off",
              language="en-US")
    unchanged("VoiceSubtitles=0", "[LangSupport] VoiceSubtitles=0; voice subtitles are off",
              ini="[LangSupport]\nVoiceSubtitles=0\n")
    unchanged("commented out", "comments the pack out (# Ukrainian)", order="Exit Game\n# Ukrainian\n")
    unchanged("Extra Options 0.4.3", "Extra Options 0.4.3 is not 0.4.4", version=b"0.4.3")
    unchanged("no pack", "no folder in mods\\ holds a language.ini; nothing to do", pack=None)
    unchanged("Pack=Other", "Other: the pack's folder in mods\\ has no language.ini", ini="[LangSupport]\nPack=Other ; x\n")
    unchanged("not Azimuth 1.0.4", "the host is not Azimuth 1.0.4 or 1.0.2", host_image=None)
    unchanged("damaged Update", "SubtitleDataSource::Update 0xc3df10 is not as inspected (another module hooks it)",
              damage=(SITES[UPDATE][0], b"\xe9\0\0\0\0"))
    unchanged("damaged SetText", "ICinematicSubtitlesDisplay::SetText 0xc3a4b0 is not as inspected",
              damage=(SET_TEXT[0] + 3, b"\x90"))
    unchanged("other image size", "image size", size=IMAGE_SIZE + 0x1000)
    unchanged("compressed table", "compressed in the archive", pack="deflated")
    locale = ctypes.create_unicode_buffer(85)
    kernel32.GetUserDefaultLocaleName(locale, 85)
    if locale.value.lower() != "ru-ru":
        unchanged("Mezcal", f"Mezcal: the game runs in the Windows locale, {locale.value}", adapter=2)
    plugin = unchanged("fork INI", "FH2_ExtraOptions.ini has [Language] Custom", language="en-US",
                       eo_ini="[Language]\nCustom=uk\n")
    bad = Context(16, 1, 1, IMAGE_SIZE, None, None, None, None, None, LOG(0))
    check(plugin.dll.FH2PluginInitialize(ctypes.byref(bad)) == 0, "a short context is refused (FALSE)")


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
    work = output / "test-runs"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    test_package(output)
    dll = load_test_dll(output / "tests" / TEST_DLL)
    host = test_azimuth(dll, options.azimuth or game / "azimuth-windows-1.0.4.zip", game)
    test_release(options.release or game / "src/eo-pr/upstream-0.4.4/FH2_Extra_Options_0.4.4-azimuth/Azimuth",
                 options.upstream or game / "src/eo-pr/wt-upstream")
    pe = test_game(dll, game / GAME_EXE)
    test_code()
    test_table(dll, work / "table", game)
    test_pack(dll, work / "pack")
    test_tool(dll, work / "tool")
    test_plugin(output, pe, host, work / "plugin")
    print(f"{len(failures)} failure(s)" if failures else "all tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
