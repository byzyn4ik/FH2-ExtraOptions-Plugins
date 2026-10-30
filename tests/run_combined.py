"""All plugins together: Azimuth Graphics Options, Exit Game, Custom Radio, Performance and Keyboard & Mouse.

Usage: py tests/run_combined.py [--zig zig.exe] [--output out] [--game <FH2 folder>] [--orders all|cover]

Needs the plugins built by build.py (their test builds under <output>/build/<id>)
and, below --game (default ~/FH2), the game executable and media/UI.zip and
Azimuth 1.0.4's release ZIP; without them every check skips (as on CI).
Nothing is installed and the game is never run: the game executable and
azimuth.exe are mapped into a test process (tests/combined_test.cpp, built
here) once per install order of the five plugins and per state of the XAML
resource call at 0x16ebb37 (the game's reader; Extra Options 0.4.4's thunk,
with its Video page reset).

Graphics, Exit Game, Performance and Keyboard & Mouse all chain that call;
Custom Radio hooks elsewhere. --orders all (the default) runs all 120 install
orders; --orders cover runs the 24 orders of the four plugins that chain the
call, with Custom Radio put in each of the five places in turn. Each run
checks that the four chain onto each other in that order, that Radio installs
beside them, and that the Video page (Graphics' rows and Performance's FPS
counter row, once each), the Audio page, the pause page (Exit Game's tile),
the Settings hub (Keyboard & Mouse's tile), the standalone KEYBOARD & MOUSE
page (no rows of the others), the Video page reset and the pause page init
come out right, using each plugin's own page checks from its
tests/run_tests.py. Performance's DXGI/D3D12 part is not started here
(ShowFps=0, PerfLog=0; its own tests run it).
"""

import argparse
import importlib.util
import itertools
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "common"))
import build_common as bc  # noqa: E402

GAME_EXE = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/forza_durango_release_final.exe")
UI_ZIP = Path("Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/media/UI.zip")
PAGES = {"video": "Scenes/Anthem/A_VideoOptions.xaml", "audio": "Scenes/Anthem/A_AudioOptions.xaml",
         "pause": "Scenes/Anthem/A_PauseMenu.xaml", "hub": "Scenes/Anthem/A_PauseOptions.xaml"}
# id, letter, mod folder, DLL name in the mod, its test build (below <output>/build/<id>)
PLUGINS = [("azimuth-graphics-options", "g", "Azimuth Graphics Options", "AzimuthGraphicsOptions.dll",
            "tests/AzimuthGraphicsOptionsTest.dll"),
           ("exit-game", "e", "Exit Game", "ExitGame.dll", "tests/ExitGameTest.dll"),
           ("custom-radio", "r", "Custom Radio", "CustomRadio.dll", "obj/CustomRadio-test.dll"),
           ("performance", "p", "Performance", "Performance.dll", "tests/PerformanceTest.dll"),
           ("keyboard-mouse", "k", "Keyboard & Mouse", "KeyboardMouse.dll", "tests/KeyboardMouseTest.dll")]
PAGE_PLUGINS = "gepk"  # the plugins that chain the XAML resource call

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


def skip(what):
    print("  skip " + what, flush=True)


def plugin_tests(plugin_id):
    """A plugin's tests/run_tests.py as a module (for its page checks and test data)."""
    path = ROOT / "plugins" / plugin_id / "tests" / "run_tests.py"
    spec = importlib.util.spec_from_file_location(f"tests_{plugin_id.replace('-', '_')}", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def install_orders(which):
    """All 120 install orders, or every order of the page plugins with Radio in each place in turn."""
    if which == "all":
        return ["".join(p) for p in itertools.permutations("gerpk")]
    chosen = []
    for index, page_order in enumerate(itertools.permutations(PAGE_PLUGINS)):
        place = index % 5
        chosen.append("".join(page_order[:place]) + "r" + "".join(page_order[place:]))
    return chosen


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--zig", help="zig.exe (0.15.2)")
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    parser.add_argument("--game", type=Path, default=Path.home() / "FH2")
    parser.add_argument("--azimuth", type=Path, help="Azimuth 1.0.4's release ZIP (default <game>/azimuth-windows-1.0.4.zip)")
    parser.add_argument("--orders", choices=("all", "cover"), default="all", help="install orders to run (default all)")
    options = parser.parse_args()
    output = options.output.resolve()
    game_exe, ui_zip = options.game / GAME_EXE, options.game / UI_ZIP
    azimuth_zip = options.azimuth or options.game / "azimuth-windows-1.0.4.zip"
    print("combined", flush=True)
    missing = [str(p) for p in (game_exe, ui_zip, azimuth_zip) if not p.is_file()]
    missing += [str(output / "build" / i / t) for i, _, _, _, t in PLUGINS if not (output / "build" / i / t).is_file()]
    if missing:
        skip(f"all plugins together: needs {', '.join(missing)}")
        print("all tests passed (skipped)")
        return

    zig = bc.find_zig(options.zig)
    build = output / "build" / "combined"
    runs = build / "runs"
    shutil.rmtree(runs, ignore_errors=True)
    runs.mkdir(parents=True)
    exe = build / "combined_test.exe"
    bc.run([zig, "c++", "-target", "x86_64-windows-gnu", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
            "-DUNICODE", "-D_UNICODE", "-fno-sanitize=all", "-I", bc.INCLUDE, "-municode", "-s",
            HERE / "combined_test.cpp", "-o", exe])
    with zipfile.ZipFile(azimuth_zip) as archive:
        (build / "azimuth.exe").write_bytes(archive.read("azimuth.exe"))
    pages = {}
    with zipfile.ZipFile(ui_zip) as archive:
        for name, entry in PAGES.items():
            pages[name] = archive.read(entry)
            (build / f"{name}.xaml").write_bytes(pages[name])
    graphics, exit_game, radio, performance, keyboard = (plugin_tests(i) for i, *_ in PLUGINS)
    modules = (graphics, exit_game, performance, keyboard)
    music = runs / "Music"
    radio.write_music(music)
    keyboard_ini = (ROOT / "plugins" / "keyboard-mouse" / "mod" / "KeyboardMouse.ini").read_text(encoding="utf-8")

    chosen = install_orders(options.orders)
    print(f"  {len(chosen)} install orders, each with the game's resource reader and with Extra Options' thunk",
          flush=True)
    for variant, order in itertools.product(("stock", "eo"), chosen):
        label = f"{order} ({variant})"
        work = runs / f"{variant}-{order}"
        dlls = {}
        for plugin_id, letter, folder, dll_name, test_dll in PLUGINS:
            plugins = work / "mods" / folder / "plugins"
            plugins.mkdir(parents=True)
            shutil.copy2(output / "build" / plugin_id / test_dll, plugins / dll_name)
            dlls[letter] = plugins / dll_name
        azimuth_ini = work / "azimuth.ini"
        azimuth_ini.write_bytes(graphics.plugin_ini(graphics.azimuth_ini_copy(options.game)))
        (dlls["g"].parent / "AzimuthGraphicsOptions.ini").write_text(
            f"[Graphics]\nEnabled=1\nLiveApply=1\nAzimuthIni={azimuth_ini}\n[Audio]\nEnabled=1\n"
            "[Captions]\nLanguage=auto\n[Logging]\nEnabled=1\n", encoding="utf-8")
        (dlls["e"].parent / "ExitGame.ini").write_text("[ExitGame]\nEnabled=1\n", encoding="utf-8")
        (dlls["r"].parent / "CustomRadio.ini").write_text(
            f"[CustomRadio]\nEnabled=1\nFolder={music}\nPcmDecode=0\n", encoding="utf-8")
        (dlls["p"].parent / "Performance.ini").write_text(
            "[Performance]\nShowFps=0\nPerfLog=0\nPerfLogFile=\nMenuRow=1\n", encoding="utf-8")
        (dlls["k"].parent / "KeyboardMouse.ini").write_text(keyboard_ini, encoding="utf-8")
        args = [exe, order, variant, work, game_exe, build / "azimuth.exe", dlls["g"], dlls["e"], dlls["r"],
                dlls["p"], dlls["k"], build / "video.xaml", build / "audio.xaml", build / "pause.xaml",
                build / "hub.xaml"]
        print(f" [{label}]", flush=True)
        try:
            run = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=300,
                                 encoding="utf-8", errors="replace")
        except subprocess.TimeoutExpired:
            check(False, f"{label}: combined_test finished in time")
            continue
        for line in run.stdout.splitlines():
            if line.startswith("  FAIL ") or line.startswith("  ok "):
                print(line[:7] + f"{label}: " + line[7:])
                if line.startswith("  FAIL "):
                    failures.append(f"{label}: {line[7:]}")
            elif line.strip() and run.returncode:
                print("    " + line)  # the plugins' log, when something failed
        if run.stderr.strip():
            print(run.stderr.rstrip())
        check(run.returncode == 0, f"{label}: combined_test exits cleanly")
        outputs = {name: work / f"{name}_out.xaml" for name in ("video", "audio", "pause", "hub", "keyboard")}
        if not all(path.is_file() and path.stat().st_size for path in outputs.values()):
            check(False, f"{label}: every page came back")
            continue
        # Each plugin's own checks of its pages (their failures are counted below).
        video = outputs["video"].read_bytes()
        graphics_first = order.index("g") < order.index("p")  # the plugin installed first edits the page first
        performance.check_page(video, f"{label} Video page", eo_rows=False,
                               graphics="first" if graphics_first else "second")
        video_text = graphics.parse(video)
        if not graphics_first:
            # Performance made the list 275 px high (as Extra Options does) before Graphics read the page, and
            # Graphics keeps that height; Performance's check above checks it, Graphics' own check wants 440.
            video_text = re.sub(r'(<local:CommonListBox x:Name="OptionsList"[^>]*?)Height="275"', r'\1Height="440"',
                                video_text, count=1)
        graphics.check_video(video_text, f"{label} Video page", False)
        graphics.check_audio(graphics.parse(outputs["audio"].read_bytes()), f"{label} Audio page", False)
        exit_game.check_page(outputs["pause"].read_bytes(), f"{label} pause page", pages["pause"])
        keyboard.check_hub(outputs["hub"].read_bytes(), f"{label} Settings hub")
        page = outputs["keyboard"].read_bytes()
        if b"<!-- EO:StandaloneOptionsPage -->" in page:
            keyboard.check_page(page, f"{label} KEYBOARD & MOUSE page", source=pages["video"])
        else:
            check(False, f"{label}: the KEYBOARD & MOUSE page was built (its checks are left out)")

    failed = len(failures) + sum(len(m.failures) for m in modules)
    print(f"{failed} failure(s)" if failed else "all tests passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
