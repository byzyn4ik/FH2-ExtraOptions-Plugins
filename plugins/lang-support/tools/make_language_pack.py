"""Build a language pack for the Language Support plugin: an Extra Options mod folder.

usage:
  py -I make_language_pack.py build <original media> <mod media> <output folder>
        --slot ru-RU --name "Українська" [--id uk] [--version 1.0.0] [--whole-zips]
  py -I make_language_pack.py convert <fork pack folder> <output folder> [--id uk] [--version 1.0.0]

build: <original media> is the game's untouched Mount\\media, <mod media> a
translation mod's replacement files laid out the same way (zips and loose
files). Every zip entry and loose file whose data differs from the original
goes into <output folder>\\media\\ in Extra Options' mod layout (a zip member
media\\X.zip -> a\\b becomes media\\X\\a\\b), so the pack ships only what the
translation changes. --whole-zips copies each changed zip whole instead (Extra
Options registers zips in a mod's media\\ with the game's own zip reader).
zipmanifest.xml is never copied: the original archives stay as they are.

convert: <fork pack folder> is a pack of the Extra Options fork
(<game>\\Mount\\languages\\<id>\\ with language.ini): its files are copied to
<output folder>\\media\\ and its language.ini beside them.

The output folder is a mod folder: mod.json (media only, no database),
language.ini ([Language] Name and Slot, read by the plugin) and media\\.
Install it as <Azimuth>\\mods\\<output folder name>\\ beside
mods\\Language Support\\, then choose the slot's language in the Azimuth
launcher. A spoken-line subtitle table goes in
media\\stripped\\stringtables\\<archive>\\VoiceSubtitles.str (loose or stored
in <archive>.zip), as the fork's packs have it.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import sys
import zipfile

# Azimuth language code -> the game's string table archive (src/pack.c slots[]).
SLOTS = {"en-US": "EN", "en-GB": "GB", "de-DE": "DE", "fr-FR": "FR", "it-IT": "IT", "es-ES": "ES", "es-MX": "MX",
         "pt-BR": "br", "pl-PL": "PL", "ru-RU": "RU", "ja-JP": "JP", "ko-KR": "KO", "zh-TW": "CHT", "zh-HK": "CHT",
         "nl-NL": "nl", "cs-CZ": "cz", "hu-HU": "HU"}
MOD_ID = re.compile(r"^[a-z0-9]+(-[a-z0-9]+)*$")  # Extra Options 0.4.4 features/addons.c valid_id()


def digest(data):
    return hashlib.sha1(data).digest()


def file_digest(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.digest()


def safe_parts(name):
    parts = PurePosixPath(name.replace("\\", "/")).parts
    if not parts or any(p in ("", ".", "..") or ":" in p for p in parts):
        sys.exit(f"refusing unsafe archive path {name!r}")
    return parts


def changed_entries(original, modded):
    """[(entry name, data)] of modded entries that are new or differ from the original."""
    with zipfile.ZipFile(original) as zo:
        known = {i.filename.lower(): (i.file_size, i.CRC, i.filename) for i in zo.infolist() if not i.is_dir()}
        out = []
        with zipfile.ZipFile(modded) as zm:
            for info in zm.infolist():
                if info.is_dir():
                    continue
                data = zm.read(info)
                old = known.get(info.filename.lower())
                if old and old[:2] == (info.file_size, info.CRC) and digest(zo.read(old[2])) == digest(data):
                    continue
                out.append((info.filename, data))
    return out


def slot_of(slot):
    for known in SLOTS:
        if known.lower() == slot.lower():
            return known
    sys.exit(f"--slot {slot!r} is not a game language code ({', '.join(SLOTS)})")


def mod_id(given, output):
    value = given if given is not None else output.name.lower().replace(" ", "-").replace("_", "-")
    if not MOD_ID.match(value) or len(value) > 128:
        sys.exit(f"mod id {value!r}: lower-case letters, digits and single hyphens (give one with --id)")
    return value


def prepare(output):
    if output.exists() and any(output.iterdir()):
        sys.exit(f"{output} is not empty")
    (output / "media").mkdir(parents=True, exist_ok=True)


def write_manifest(output, identifier, name, slot, version):
    manifest = {"format_version": 1, "id": identifier, "name": f"{name} (language pack)", "version": version,
                "description": f"{name} in the game's {slot} slot, for the Language Support plugin.",
                "media": "media", "database": None, "dependencies": []}
    (output / "mod.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    # UTF-16 (with BOM) so GetPrivateProfileString reads any language name.
    (output / "language.ini").write_text(
        "; Language pack for the Language Support plugin (Forza Horizon 2 Extra Options).\n"
        f"; Install this folder as <Azimuth>\\mods\\{output.name}\\; choose {slot} in the Azimuth launcher.\n"
        f"[Language]\nName={name}\nSlot={slot}\n", encoding="utf-16")


def build(options):
    if not options.original.is_dir() or not options.modded.is_dir():
        sys.exit("both media folders must exist")
    slot = slot_of(options.slot)
    identifier = mod_id(options.id, options.output)
    prepare(options.output)
    media = options.output / "media"
    entries = files = size = 0
    for source in sorted(p for p in options.modded.rglob("*") if p.is_file()):
        relative = source.relative_to(options.modded)
        if relative.as_posix().lower() == "zipmanifest.xml":
            continue
        original = options.original / relative
        if source.suffix.lower() == ".zip" and original.is_file() and not options.whole_zips:
            for name, data in changed_entries(original, source):
                target = media / relative.with_suffix("") / Path(*safe_parts(name))
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                entries += 1
                size += len(data)
            continue
        if original.is_file() and file_digest(original) == file_digest(source):
            continue
        target = media / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        files += 1
        size += source.stat().st_size
    write_manifest(options.output, identifier, options.name, slot, options.version)
    print(f"{entries} changed zip entries, {files} whole files, {size / 1e6:.2f} MB -> {options.output} "
          f"(mod id {identifier}, slot {slot})")


def read_fork_ini(path):
    data = path.read_bytes()
    for encoding in ("utf-16",) if data[:2] in (b"\xff\xfe", b"\xfe\xff") else ("utf-8-sig", "cp1252"):
        try:
            text = data.decode(encoding)
            break
        except UnicodeDecodeError:
            continue
    else:
        sys.exit(f"{path}: cannot read it")
    values, section = {}, None
    for line in text.splitlines():
        line = line.split(";", 1)[0].strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
        elif "=" in line and section == "language":
            key, value = line.split("=", 1)
            values[key.strip().lower()] = value.strip()
    return values


def convert(options):
    ini = options.pack / "language.ini"
    if not ini.is_file():
        sys.exit(f"{options.pack} has no language.ini (not a pack of the Extra Options fork)")
    values = read_fork_ini(ini)
    slot = slot_of(values.get("slot", ""))
    name = values.get("name") or options.pack.name
    identifier = mod_id(options.id if options.id is not None else options.pack.name.lower(), options.output)
    prepare(options.output)
    count = size = 0
    for source in sorted(p for p in options.pack.rglob("*") if p.is_file()):
        relative = source.relative_to(options.pack)
        if relative.as_posix().lower() in ("language.ini", "zipmanifest.xml"):
            continue
        safe_parts(relative.as_posix())
        target = options.output / "media" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        count += 1
        size += source.stat().st_size
    write_manifest(options.output, identifier, name, slot, options.version)
    print(f"{count} files, {size / 1e6:.2f} MB -> {options.output} (mod id {identifier}, slot {slot})")


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    make = commands.add_parser("build", help="a pack from a translation mod's media")
    make.add_argument("original", type=Path)
    make.add_argument("modded", type=Path)
    make.add_argument("output", type=Path)
    make.add_argument("--slot", required=True, help="game language slot the pack takes over, e.g. ru-RU")
    make.add_argument("--name", required=True, help="the language's name (log, mod.json)")
    make.add_argument("--whole-zips", action="store_true")
    old = commands.add_parser("convert", help="a pack of the Extra Options fork as a mod folder")
    old.add_argument("pack", type=Path)
    old.add_argument("output", type=Path)
    for command in (make, old):
        command.add_argument("--id", help="the mod id (default: from the folder name)")
        command.add_argument("--version", default="1.0.0", help="the pack's version in mod.json")
    options = parser.parse_args()
    (build if options.command == "build" else convert)(options)


if __name__ == "__main__":
    main()
