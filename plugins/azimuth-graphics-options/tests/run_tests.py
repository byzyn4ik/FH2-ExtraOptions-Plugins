"""Offline tests of the Azimuth Graphics Options plugin (no game run, nothing installed).

Usage: py tests/run_tests.py [--output out] [--zig zig.exe] [--game <FH2 folder>]
                             [--azimuth <azimuth-windows-1.0.2.zip>] [--upstream <Extra Options git clone>]

Real files are only read (the game's executable and UI.zip, Azimuth's ZIP,
data\\azimuth.ini is copied to a private folder). Defaults: --game ~/FH2,
--azimuth <game>/azimuth-windows-1.0.2.zip, --upstream
<game>/src/eo-pr/ForzaHorizon2_ExtraOptions (refs origin/main = clean 0.3.2 and
combined-10 when present). A missing input skips the tests that need it.

1. package  mod.json as Extra Options 0.3.2 validates it, the plugin layout, the
            DLL (PE32+ x64, exports, only Windows imports), the INI.
2. host     Azimuth 1.0.2's azimuth.exe and the branded "Forza Horizon 2.exe"
            pass the host guard; the frame rate choices match its launcher.
3. game     the game executable passes the native guards (call site as the
            game has it and as Extra Options 0.3.2 patches it).
4. xaml     the real Video page (UI.zip), alone and as Extra Options' own
            code (origin/main, combined-10) extends it, then with this plugin's
            rows: well-formed XML, rows, order, list style; pages built from it
            are refused.
5. rows     data\\azimuth.ini (a copy): keys written, other lines kept.
6. plugin   out/tests/plugin_test.exe drives FH2PluginInitialize and the page
            (modes nohost, disabled, badhost, badgame, layout, native, eo, polish).
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
        self.image_size, self.headers_size = struct.unpack_from("<II", d, optional + 56)
        self.sections = []
        table = optional + optional_size
        for i in range(sections):
            name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, table + 40 * i)
            self.sections.append((va, vsize, rsize, raw))
        directories = optional + 112
        self.exports_rva, _ = struct.unpack_from("<II", d, directories)
        self.imports_rva, _ = struct.unpack_from("<II", d, directories + 8)

    def offset(self, rva):
        for va, vsize, rsize, raw in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError(hex(rva))

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
        """The image laid out as the loader maps it, in private RWX memory; returns (address, keepalive)."""
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.VirtualAlloc.restype = ctypes.c_void_p
        kernel32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
        base = kernel32.VirtualAlloc(None, self.image_size, 0x3000, 0x40)
        ctypes.memmove(base, self.data[:self.headers_size], self.headers_size)
        for va, vsize, rsize, raw in self.sections:
            size = min(rsize, vsize) if vsize else rsize
            ctypes.memmove(base + va, self.data[raw:raw + size], size)
        return base


# --------------------------------------------------------------- 1. package

def valid_mod_id(value):
    # features/addons.c valid_id() in Extra Options 0.3.2.
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
    expected = {"Graphics": ["Enabled", "LiveApply", "AzimuthIni"], "Logging": ["Enabled"]}
    for section, keys in expected.items():
        check(ini.has_section(section) and all(ini.has_option(section, k) for k in keys),
              f"INI [{section}] has {', '.join(keys)}")
    check(ini.get("Graphics", "Enabled", fallback="") == "1", "INI [Graphics] Enabled defaults to 1")


# ------------------------------------------------------------------ helpers

def load_test_dll(output):
    dll = ctypes.CDLL(str(output / "tests" / "AzimuthGraphicsOptionsTest.dll"))
    dll.AzgTestCheckHost.argtypes = [ctypes.c_void_p]
    dll.AzgTestCheckHost.restype = ctypes.c_char_p
    dll.AzgTestCheckGame.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    dll.AzgTestCheckGame.restype = ctypes.c_char_p
    dll.AzgTestExtendXaml.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                                      ctypes.c_int, ctypes.c_int]
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
    return dll


def extend(dll, xaml, polish=0, live=15):
    out = ctypes.create_string_buffer(len(xaml) + 0x10000)
    n = dll.AzgTestExtendXaml(xaml, len(xaml), out, len(out), polish, live)
    return out.raw[:n] if n else b""


def parse(xaml):
    text = xaml.decode("utf-8-sig")
    ET.fromstring(text)
    return text


# ------------------------------------------------------------------ 2. host

def test_host(dll, azimuth_exe, branded):
    print("host")
    rates = (ctypes.c_int * 16)()
    expected = {60: [30, 60], 75: [30, 60, 75], 144: [30, 60, 90, 120, 144], 165: [30, 60, 90, 120, 144, 165],
                240: [30, 60, 90, 120, 144, 165, 240], 360: [30, 60, 90, 120, 144, 165, 240, 360],
                1: [30, 60], 0: [30, 60], 1200: [30, 60, 90, 120, 144, 165, 240]}
    for refresh, want in expected.items():
        n = dll.AzgTestFrameRates(refresh, rates, 16)
        check(list(rates[:n]) == want, f"Frame rate choices for a {refresh} Hz display: {list(rates[:n])}")
    for label, path in (("azimuth.exe (1.0.2 release)", azimuth_exe), ("branded Forza Horizon 2.exe", branded)):
        if not path or not Path(path).is_file():
            skip(f"{label} not found")
            continue
        pe = PE(path)
        problem = dll.AzgTestCheckHost(pe.map())
        check(problem is None, f"{label} (TimeDateStamp {pe.timestamp:#x}, image {pe.image_size:#x}) is "
                               f"Azimuth 1.0.2{'' if problem is None else ': ' + problem.decode()}")
        data = bytearray(pe.data)
        # The launcher's frame rate table, as the plugin reads it (rva 0x105f8).
        table = pe.offset(0x105f8)
        check([struct.unpack_from("<I", data, table + 4 + 8 * i)[0] for i in range(6)] == [60, 90, 120, 144, 165, 240],
              f"{label}: the launcher's frame rate steps are 60, 90, 120, 144, 165, 240")
        rate_global = struct.unpack_from("<I", data, pe.offset(0x4cc50) + 2)[0] + 0x4cc56
        check(rate_global == 0x8e7540, f"{label}: 0x4cc50 stores the target frame rate at {rate_global:#x}")
    exe = PE(sys.executable)
    check(dll.AzgTestCheckHost(exe.map()) is not None, "another executable is refused")


# ------------------------------------------------------------------ 3. game

def test_game(dll, game_exe):
    print("game")
    if not game_exe.is_file():
        skip(f"{game_exe} not found")
        return
    pe = PE(game_exe)
    base = pe.map()
    target = dll.AzgTestCheckGame(base, pe.image_size)
    check(target is not None and target.startswith(b"the game's resource reader 0xaabec0"),
          f"the game executable passes the guards; the call goes to {target.decode() if target else None}")
    # As Extra Options 0.3.2 patches it: call 0x13fe354, a mov rax/jmp rax thunk there.
    thunk = (ctypes.c_ubyte * 12).from_address(base + 0x13fe354)
    hook = ctypes.cast(dll.AzgTestCheckHost, ctypes.c_void_p).value
    thunk[:] = list(b"\x48\xb8" + struct.pack("<Q", hook) + b"\xff\xe0")
    call = (ctypes.c_ubyte * 5).from_address(base + 0x16ebb37)
    call[:] = list(b"\xe8" + struct.pack("<i", 0x13fe354 - (0x16ebb37 + 5)))
    target = dll.AzgTestCheckGame(base, pe.image_size)
    check(target is not None and target.startswith(b"Extra Options' resource hook (thunk 0x13fe354"),
          f"with Extra Options' patch: {target.decode() if target else None}")
    prologue = (ctypes.c_ubyte * 1).from_address(base + 0xb52170)
    prologue[0] ^= 0xff
    problem = dll.AzgTestCheckGame(base, pe.image_size)
    check(problem is not None and b"InsertItem at 0xb52170 is not as expected" in problem,
          f"a changed native function is refused ({problem.decode() if problem else None})")


# ------------------------------------------------------------------ 4. xaml

def upstream_xaml_dll(zig, upstream, ref, build):
    """Extra Options' own Video page code at `ref`, built as a small DLL."""
    folder = build / ("eo-" + ref.replace("/", "-"))
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    names = subprocess.run(["git", "-C", str(upstream), "ls-tree", "-r", "--name-only", ref], capture_output=True,
                           text=True, check=True).stdout.split()
    for name in names:
        if name.endswith(".h") or name in ("support.c", "features/video_options_xaml.c", "features/video_options_text.c"):
            data = subprocess.run(["git", "-C", str(upstream), "show", f"{ref}:{name}"], capture_output=True,
                                  check=True).stdout
            (folder / Path(name).name).write_bytes(data)
    (folder / "eo_wrapper.c").write_text(
        '#include "video_options_xaml.h"\n'
        "__declspec(dllexport) size_t EoExtend(const char *t, size_t n, char *o, size_t c) {\n"
        "    return video_options_extend_xaml(NULL, t, n, o, c);\n}\n", encoding="utf-8")
    sources = [folder / n for n in ("eo_wrapper.c", "support.c", "video_options_xaml.c", "video_options_text.c")]
    dll = folder / "eo_xaml.dll"
    command = [str(zig), "cc", "-target", "x86_64-windows-gnu", "-O1", "-w", "-shared", "-fms-extensions", "-funwind-tables", "-fno-builtin", "-DFH2_AZIMUTH",
               "-I", str(folder), *map(str, sources), "-o", str(dll)]
    run = subprocess.run(command, capture_output=True, text=True)
    if run.returncode:
        # Functions of other features the file refers to: stubs (never called by EoExtend on the Video page).
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


def check_page(text, label, eo_rows):
    rows = re.findall(r'SelectedIndex="\{Binding (Gfx\w+)\}"', text)
    check(rows == SHOWN, f"{label}: the 9 Azimuth rows in order, no Frame pacing ({len(rows)})")
    check(all(text.count(f'x:Key="EO_{n}"') == 1 for n in SHOWN), f"{label}: one TileMenuDataItem per row")
    list_start = text.index('<local:CommonListBox x:Name="OptionsList"')
    opening = text[list_start:text.index(">", list_start)]
    check('A_Style_ListBox_Scrollable' in opening and 'Height="440"' in opening,
          f"{label}: the options list is scrollable, 440 px")
    subtitles = text.index("{Binding Subtitles}")
    first = text.index("{Binding GfxFrameRate}")
    check(subtitles < first, f"{label}: the rows come after the page's own Brightness and Subtitles")
    if eo_rows:
        eo = text.index("{Binding RealtimeShadows}")
        check(text.index("{Binding GfxHdr}") < eo, f"{label}: and before Extra Options' rows (Realtime shadows ...)")
        check(all(f"{{Binding {n}}}" in text for n in ("PhotoSamplesPreset", "LiveryResolution", "LiveryMsaa")),
              f"{label}: Extra Options' Video rows are all kept")


def test_xaml(dll, zig, game, upstream, output):
    print("xaml")
    ui = game / UI_ZIP
    if not ui.is_file():
        skip(f"{ui} not found")
        return None
    with zipfile.ZipFile(ui) as archive:
        native = archive.read("Scenes/Anthem/A_VideoOptions.xaml")
    ours = extend(dll, native)
    check(bool(ours) and ours.startswith(b"\xef\xbb\xbf"), "the game's Video page gets the rows (UTF-8 with BOM)")
    text = parse(ours)
    check_page(text, "game page", False)
    check(not extend(dll, ours), "a page that already has the rows is left alone")
    check('Description="Target frame rate. Applies at once."' in text, "live descriptions say what applies at once")
    restart = parse(extend(dll, native, live=0))
    check("Target frame rate. Restart the game to apply." in restart, "without live apply they say restart")
    polish = parse(extend(dll, native, polish=1))
    check('Label="Liczba klatek"' in polish and "Pełny ekran" not in polish and 'Label="Cienie"' in polish,
          "Polish captions")
    built = native.replace(b'Content="{local:Loc ScreenTitles.IDS_VideoOptions}"', b'Content="KEYBOARD"')
    check(not extend(dll, built), "a page built from the Video page (other title) is refused")
    hidden = native.replace(b'<StackPanel Margin="0, 0, 0, 6" Orientation="Horizontal"',
                            b'<StackPanel Visibility="Collapsed" Margin="0, 0, 0, 6" Orientation="Horizontal"')
    check(hidden != native and not extend(dll, hidden),
          "a page with the brightness preview collapsed (Extra Options' World/KBM pages) is refused")
    check(not extend(dll, b'<Page x:Class="ForzaUI.A_HudOptions"/>'), "another page is refused")
    eo_page = None
    if not upstream or not (upstream / ".git").exists():
        skip("Extra Options clone not found (--upstream)")
        return native
    for ref in ("origin/main", "combined-10"):
        have = subprocess.run(["git", "-C", str(upstream), "rev-parse", "--verify", "-q", ref], capture_output=True)
        if have.returncode:
            skip(f"{ref} not in {upstream}")
            continue
        library = upstream_xaml_dll(zig, upstream, ref, output / "obj")
        if not library:
            check(False, f"Extra Options' Video page code at {ref} builds for the test")
            continue
        out = ctypes.create_string_buffer(len(native) + 0x20000)
        n = library.EoExtend(native, len(native), out, len(out))
        check(n > 0, f"{ref}: Extra Options extends the game's Video page")
        extended = out.raw[:n]
        parse(extended)
        both = extend(dll, extended)
        check(bool(both), f"{ref}: the plugin adds its rows to Extra Options' page")
        if both:
            check_page(parse(both), f"{ref} page", True)
        n = library.EoExtend(both, len(both), out, len(out)) if both else 0
        check(n == 0, f"{ref}: Extra Options leaves an already extended page alone (the plugin runs after it)")
        if ref == "origin/main":
            eo_page = extended
    if eo_page:
        (output / "test-runs").mkdir(parents=True, exist_ok=True)
        (output / "test-runs" / "eo_video.xaml").write_bytes(eo_page)
    return native


# ------------------------------------------------------------------ 5. rows

SAMPLE_INI = (b"[launcher]\ngame=1a96545b\ndisplay=fullscreen\nhdr_peak=1000\n"
              b"[title-1a96545b]\nfolder=Anthem\nresolution=2160\nantialiasing=msaax4\ngraphics=ultra\n"
              b"shadow_detail=ultra\nshadow_distance=ultra\nshadow_redraw=ultra\ntexture_filtering=16x\n"
              b"frame_rate=120\nvsync=on\nhdr=on\nlanguage=en-US\n")


def azimuth_ini_copy(game):
    real = game / "data" / "azimuth.ini"
    return real.read_bytes() if real.is_file() else SAMPLE_INI


def test_rows(dll, game, output):
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
    check(dll.AzgTestRowCount(0) == 4 and dll.AzgTestRowChoice(0, 3, 0) == "120 FPS", "Frame rate labels")
    lines = original.decode("utf-8", "replace").splitlines()
    frame = next((l.split("=", 1)[1] for l in lines if l.startswith("frame_rate=")), "60")
    check(dll.AzgTestRowGet(0) == max(i for i, r in enumerate(rates) if r <= int(frame)),
          f"Frame rate shows the saved {frame} (or the step below it)")
    ini.write_bytes(original.replace(f"frame_rate={frame}".encode(), b"frame_rate=100"))
    check(dll.AzgTestRowGet(0) == 2, "a saved 100 shows as 90, as Azimuth's launcher does")
    check(dll.AzgTestRowSet(0, 2) == 1 and b"frame_rate=90" in ini.read_bytes(),
          "choosing 90 then writes 90 (the saved 100 was not a choice)")
    check(dll.AzgTestRowSet(0, 2) == 0, "choosing 90 again writes nothing")
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
    (folder / "old.ini").write_bytes(b"[launcher]\nframe_rate=60\n")
    dll.AzgTestRowsFile(str(folder / "old.ini"))
    check(b"title-1a96545b" in dll.AzgTestRowsLayout(), "an azimuth.ini without the title section is refused")


# ----------------------------------------------------------------- 6. plugin

def test_plugin(output, game_exe, azimuth_exe, native, game):
    print("plugin")
    exe = output / "tests" / "plugin_test.exe"
    eo_page = output / "test-runs" / "eo_video.xaml"
    native_page = output / "test-runs" / "native_video.xaml"
    if native:
        native_page.write_bytes(native)
    for mode in ("nohost", "disabled", "badhost", "badgame", "layout", "native", "eo", "polish"):
        shipping = mode in ("nohost", "disabled")
        if not shipping and not (azimuth_exe and game_exe.is_file()):
            skip(f"plugin_test {mode}: needs the game and azimuth executables")
            continue
        if mode in ("native", "eo", "polish") and not (native and (mode != "eo" or eo_page.is_file())):
            skip(f"plugin_test {mode}: needs the Video page XAML")
            continue
        work = output / "test-runs" / mode
        shutil.rmtree(work, ignore_errors=True)
        plugins = work / "mods" / MOD_NAME / "plugins"
        plugins.mkdir(parents=True)
        dll = output / ("mods/" + MOD_NAME + "/plugins/" + DLL_NAME if shipping else "tests/AzimuthGraphicsOptionsTest.dll")
        shutil.copy2(dll, plugins / DLL_NAME)
        ini = work / "azimuth.ini"
        data = azimuth_ini_copy(game)
        # plugin_test expects a saved 120 FPS.
        data = re.sub(rb"(?m)^frame_rate=\d+", b"frame_rate=120", data)
        if b"frame_rate=120" not in data:
            data = data.replace(b"[title-1a96545b]\n", b"[title-1a96545b]\nframe_rate=120\n")
        if mode == "layout":
            data = b"[launcher]\ndisplay=fullscreen\n"
        ini.write_bytes(data)
        (plugins / INI_NAME).write_text(
            f"[Graphics]\nEnabled={0 if mode == 'disabled' else 1}\nLiveApply=1\nAzimuthIni={ini}\n"
            "[Logging]\nEnabled=1\n", encoding="utf-8")
        args = [str(exe), mode, str(plugins / DLL_NAME), str(work)]
        if not shipping:
            args += [str(game_exe), str(azimuth_exe), str(eo_page if mode == "eo" else native_page)]
        try:
            run = subprocess.run(args, capture_output=True, text=True, timeout=300)
            print(run.stdout.rstrip())
            if run.stderr:
                print(run.stderr.rstrip())
            check(run.returncode == 0, f"plugin_test {mode}")
        except subprocess.TimeoutExpired:
            check(False, f"plugin_test {mode} finished in time")
        own_log = plugins / "AzimuthGraphicsOptions.log"
        check(own_log.is_file(), f"{mode}: AzimuthGraphicsOptions.log is written")
        if mode in ("native", "eo", "polish") and (work / "video_out.xaml").is_file():
            text = parse((work / "video_out.xaml").read_bytes())
            check_page(text, f"{mode} (through the hook)", mode == "eo")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    parser.add_argument("--zig", type=Path, default=Path(shutil.which("zig") or "zig"))
    parser.add_argument("--game", type=Path, default=Path.home() / "FH2")
    parser.add_argument("--azimuth", type=Path)
    parser.add_argument("--upstream", type=Path)
    options = parser.parse_args()
    output = options.output.resolve()
    game = options.game
    azimuth_zip = options.azimuth or game / "azimuth-windows-1.0.2.zip"
    upstream = options.upstream or game / "src" / "eo-pr" / "ForzaHorizon2_ExtraOptions"
    azimuth_exe = None
    if azimuth_zip.is_file():
        azimuth_exe = output / "test-runs" / "azimuth-1.0.2" / "azimuth.exe"
        azimuth_exe.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(azimuth_zip) as archive:
            azimuth_exe.write_bytes(archive.read("azimuth.exe"))
    test_package(output)
    dll = load_test_dll(output)
    test_host(dll, azimuth_exe, game / "Forza Horizon 2.exe")
    test_game(dll, game / GAME_EXE)
    native = test_xaml(dll, options.zig, game, upstream, output)
    test_rows(dll, game, output)
    test_plugin(output, game / GAME_EXE, azimuth_exe, native, game)
    print(f"{len(failures)} failure(s)" if failures else "all tests passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
