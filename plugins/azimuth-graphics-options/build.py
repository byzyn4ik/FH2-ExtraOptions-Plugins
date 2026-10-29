"""Build the Azimuth Graphics Options plugin for Forza Horizon 2 Extra Options.

Usage: py build.py [--zig <zig.exe>] [--output out] [--test] [test options]

Writes:
  <output>/mods/Azimuth Graphics Options/mod.json
  <output>/mods/Azimuth Graphics Options/plugins/AzimuthGraphicsOptions.dll
  <output>/mods/Azimuth Graphics Options/plugins/AzimuthGraphicsOptions.ini
  <output>/tests/AzimuthGraphicsOptionsTest.dll  (the same code with test entry points)
  <output>/tests/plugin_test.exe                  (offline plugin test)
--test also runs tests/run_tests.py on the result (it passes --game, --azimuth,
--azimuth102, --eo-release, --eo-source and --upstream through). Nothing is installed.
Zig 0.15.2: --zig, else the ZIG environment variable, else zig on PATH.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
SRC = HERE / "src"
MOD_NAME = "Azimuth Graphics Options"
DLL_NAME = "AzimuthGraphicsOptions.dll"
INI_NAME = "AzimuthGraphicsOptions.ini"
PLUGIN = ["common.cpp", "rows.cpp", "language.cpp", "live.cpp", "call_chain.cpp", "settings_pages.cpp", "plugin.cpp"]


def find_zig(given):
    for candidate in (given, os.environ.get("ZIG"), shutil.which("zig")):
        if candidate and Path(candidate).is_file():
            return Path(candidate).resolve()
    sys.exit("zig not found: pass --zig <zig.exe> (Zig 0.15.2) or set ZIG")


def run(command):
    print("zig", *[Path(str(part)).name if str(part).endswith((".cpp", ".exe", ".dll")) else str(part)
                   for part in command[1:]])
    subprocess.run([str(part) for part in command], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--zig", help="zig.exe (0.15.2)")
    parser.add_argument("--output", type=Path, default=HERE / "out")
    parser.add_argument("--test", action="store_true", help="run the offline tests after building")
    parser.add_argument("--game", type=Path, help="tests: the FH2 game folder (default ~/FH2)")
    parser.add_argument("--azimuth", type=Path, help="tests: the Azimuth 1.0.4 release ZIP")
    parser.add_argument("--azimuth102", type=Path, help="tests: the Azimuth 1.0.2 release ZIP")
    parser.add_argument("--eo-release", type=Path, help="tests: the Extra Options 0.4.4 release folder (winmm.dll)")
    parser.add_argument("--eo-source", type=Path, help="tests: the Extra Options 0.4.4 source tree")
    parser.add_argument("--upstream", type=Path, help="tests: an Extra Options git clone (origin/main)")
    options = parser.parse_args()
    zig = find_zig(options.zig)
    output = options.output.resolve()
    build = output / "obj"
    mod = output / "mods" / MOD_NAME
    plugins = mod / "plugins"
    tests = output / "tests"
    for folder in (plugins, tests, build):
        folder.mkdir(parents=True, exist_ok=True)

    common = [zig, "c++", "-target", "x86_64-windows-gnu", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
              "-DUNICODE", "-D_UNICODE", "-fno-sanitize=all"]
    sources = [SRC / name for name in PLUGIN]
    run([*common, "-shared", "-s", *sources, "-luser32", "-Wl,--out-implib," + str(build / "plugin.lib"),
         "-o", plugins / DLL_NAME])
    run([*common, "-shared", "-s", "-DAZG_TEST_EXPORTS", *sources, "-luser32",
         "-Wl,--out-implib," + str(build / "plugin-test.lib"), "-o", tests / "AzimuthGraphicsOptionsTest.dll"])
    run([*common, "-municode", "-s", HERE / "tests" / "plugin_test.cpp", "-o", tests / "plugin_test.exe"])
    for folder in (plugins, tests):
        for stale in folder.glob("*.lib"):
            stale.unlink()
    shutil.copy2(HERE / "mod" / "mod.json", mod / "mod.json")
    shutil.copy2(HERE / "mod" / INI_NAME, plugins / INI_NAME)
    for path in (mod / "mod.json", plugins / DLL_NAME, plugins / INI_NAME, tests / "AzimuthGraphicsOptionsTest.dll",
                 tests / "plugin_test.exe"):
        print(path)
    if options.test:
        command = [sys.executable, str(HERE / "tests" / "run_tests.py"), "--output", str(output), "--zig", str(zig)]
        for name in ("game", "azimuth", "azimuth102", "eo_release", "eo_source", "upstream"):
            if getattr(options, name):
                command += ["--" + name.replace("_", "-"), str(getattr(options, name))]
        subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
