"""Build (and test) the Forza Horizon 2 Extra Options plugins of this repository.

Usage: py build.py [--zig <zig.exe>] [--plugin <id> ... | --all] [--test] [--game <FH2 folder>]
                   [--version <x.y.z>] [--output out]

Every folder plugins/<id>/ with a build.py and a mod/mod.json is a plugin
(nothing else lists them). Each is built by its own build.py into
<output>/build/<id>/ (its tests, test DLLs and objects stay there), then:

  <output>/mods/<Mod Name>/...                     the mod folder, as installed
  <output>/release/<id>-<mod version>.zip          mods/<Mod Name>/... of one plugin
  <output>/release/FH2-ExtraOptions-Plugins-<version>.zip
                                                   mods/... of every plugin built
<version> is --version, else the VERSION file. Without --plugin every plugin
is built (--all says so explicitly).

--test runs each plugin's offline tests (its build.py --test) and then
tests/run_combined.py, which loads the plugins' test builds together into one
process; a summary of the ok/FAIL/skip counts per plugin follows. Tests that
need the game's files (below --game, default ~/FH2), Azimuth's ZIPs or Extra
Options' release skip themselves when those are missing (as on CI).
Nothing is installed. Zig 0.15.2: --zig, else ZIG, else zig on PATH.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "common"))
import build_common as bc  # noqa: E402

PLUGINS = ROOT / "plugins"
RESULT = re.compile(r"^\s+(ok|FAIL|skip)\s")
ZIP_TIME = (2026, 1, 1, 0, 0, 0)  # fixed, so the same files give the same ZIP


def discover():
    """{id: {"folder", "name", "version"}} for every plugins/<id>/ with build.py and mod/mod.json."""
    found = {}
    for folder in sorted(PLUGINS.iterdir()):
        mod_json = folder / "mod" / "mod.json"
        if not (folder / "build.py").is_file() or not mod_json.is_file():
            continue
        mod = json.loads(mod_json.read_text(encoding="utf-8"))
        if mod.get("id") != folder.name:
            sys.exit(f"plugins/{folder.name}/mod/mod.json: its id is {mod.get('id')!r}, not the folder's name")
        found[folder.name] = {"folder": folder, "name": mod["name"], "version": mod["version"]}
    return found


def run_counted(command, label):
    """Runs a command, echoing its output; returns (exit code, {ok, FAIL, skip})."""
    counts = {"ok": 0, "FAIL": 0, "skip": 0}
    print(f"== {label}", flush=True)
    process = subprocess.Popen([str(part) for part in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, encoding="utf-8", errors="replace",
                               env={**os.environ, "PYTHONIOENCODING": "utf-8", "PYTHONUNBUFFERED": "1"})
    for line in process.stdout:
        print(line, end="", flush=True)
        match = RESULT.match(line)
        if match:
            counts[match.group(1)] += 1
    return process.wait(), counts


def zip_folder(archive, folder, prefix):
    for path in sorted(folder.rglob("*")):
        if path.is_file():
            info = zipfile.ZipInfo(prefix + "/" + path.relative_to(folder).as_posix(), ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, path.read_bytes())


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--zig", help="zig.exe (0.15.2)")
    parser.add_argument("--plugin", action="append", default=[], metavar="ID", help="build this plugin (repeatable)")
    parser.add_argument("--all", action="store_true", help="build every plugin (the default)")
    parser.add_argument("--test", action="store_true", help="run the offline tests")
    parser.add_argument("--game", type=Path, help="tests: the FH2 game folder (default ~/FH2)")
    parser.add_argument("--version", help="the all-in-one ZIP's version (default: the VERSION file)")
    parser.add_argument("--output", type=Path, default=ROOT / "out")
    options = parser.parse_args()
    if options.all and options.plugin:
        parser.error("--all and --plugin exclude each other")
    plugins = discover()
    unknown = [p for p in options.plugin if p not in plugins]
    if unknown:
        parser.error(f"unknown plugin(s) {', '.join(unknown)}; there are: {', '.join(plugins)}")
    chosen = options.plugin or list(plugins)
    zig = bc.find_zig(options.zig)
    output = options.output.resolve()
    version = options.version or (ROOT / "VERSION").read_text(encoding="utf-8").strip()
    mods, release = output / "mods", output / "release"
    release.mkdir(parents=True, exist_ok=True)
    for stale in release.glob("*.zip"):
        stale.unlink()

    results, failed = {}, []
    for plugin_id in chosen:
        plugin = plugins[plugin_id]
        command = [sys.executable, plugin["folder"] / "build.py", "--zig", zig, "--output", output / "build" / plugin_id]
        if options.test:
            command.append("--test")
        if options.game:
            command += ["--game", options.game]
        code, counts = run_counted(command, f"{plugin_id} {plugin['version']}")
        results[plugin_id] = counts
        if code:
            failed.append(plugin_id)
            continue
        built = output / "build" / plugin_id / "mods" / plugin["name"]
        if not (built / "mod.json").is_file():
            sys.exit(f"{plugin_id}: its build.py wrote no {built / 'mod.json'}")
        shutil.rmtree(mods / plugin["name"], ignore_errors=True)
        shutil.copytree(built, mods / plugin["name"])
        with zipfile.ZipFile(release / f"{plugin_id}-{plugin['version']}.zip", "w") as archive:
            zip_folder(archive, mods / plugin["name"], "mods/" + plugin["name"])

    if not failed:
        with zipfile.ZipFile(release / f"FH2-ExtraOptions-Plugins-{version}.zip", "w") as archive:
            for plugin_id in chosen:
                name = plugins[plugin_id]["name"]
                zip_folder(archive, mods / name, "mods/" + name)

    if options.test and not failed:
        command = [sys.executable, ROOT / "tests" / "run_combined.py", "--zig", zig, "--output", output]
        if options.game:
            command += ["--game", options.game]
        code, counts = run_counted(command, "all plugins together")
        results["(combined)"] = counts
        if code:
            failed.append("(combined)")

    print("\n== summary")
    for name, counts in results.items():
        print(f"  {name:28} {counts['ok']:4} ok  {counts['FAIL']:3} FAIL  {counts['skip']:3} skip")
    for path in sorted(release.glob("*.zip")):
        print(f"  {path}")
    if failed:
        sys.exit(f"failed: {', '.join(failed)}")


if __name__ == "__main__":
    main()
