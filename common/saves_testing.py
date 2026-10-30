"""Test helpers shared by Photo Export's and Saves in Documents' tests: fake Azimuth
save trees in the 1.0.4 layout, snapshots of folders, and the facts about
azimuth.exe 1.0.4 those plugins rely on (checked on the release ZIP when present).

Layout (azimuth.exe 1.0.1 to 1.0.4):
  <Azimuth folder>\\data\\titles\\1A96545B\\saves\\player-1\\<container>\\revisions\\<16 hex>\\
      data\\<blob> ...
      commit       "Azimuth complete save revision v2\\nblobs <n>\\n<sha256> <size> <name>\\n..."
"""

import ctypes
from ctypes import wintypes
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zipfile

TITLE = "1A96545B"
STAMP_104, SIZE_104 = 0x6AE393EB, 0x8A2000


def commit_text(blobs, version=2):
    """The commit of a revision whose data holds `blobs` ({name: bytes}), in Azimuth's order (sorted by name)."""
    lines = "".join(f"{hashlib.sha256(v).hexdigest()} {len(v)} {k}\n" for k, v in sorted(blobs.items()))
    return f"Azimuth complete save revision v{version}\nblobs {len(blobs)}\n{lines}".encode()


def make_revision(saves, space, container, revision, blobs, commit=True, listed=None, pending=False, when=None):
    """saves/<space>/<container>/revisions/<revision> with data/<blob> and (commit) its commit file.
    listed: the blobs the commit lists instead (to make a damaged revision).
    pending: a "pending" file instead of the commit (Azimuth between its writes and the rename)."""
    if isinstance(revision, int):
        revision = f"{revision:016x}"
    folder = Path(saves) / space / container / "revisions" / revision
    (folder / "data").mkdir(parents=True, exist_ok=True)
    for name, value in blobs.items():
        (folder / "data" / name).write_bytes(value)
        if when:
            os.utime(folder / "data" / name, (when, when))
    text = commit_text(listed if listed is not None else blobs)
    if commit:
        (folder / "commit").write_bytes(text)
    elif pending:
        (folder / "pending").write_bytes(text)
    return folder


def commit_now(folder):
    """Azimuth's last step: pending -> commit."""
    os.replace(Path(folder) / "pending", Path(folder) / "commit")


def jpeg(size, seed=None):
    body = os.urandom(size) if seed is None else hashlib.sha256(str(seed).encode()).digest() * (size // 32 + 1)
    return b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01" + body[:size] + b"\xff\xd9"


def profile_blobs(scale=1, seed=None):
    """The blobs of a User_ container (sizes as in a real profile, scaled down)."""
    def data(n):
        return os.urandom(n) if seed is None else (hashlib.sha256(f"{seed}-{n}".encode()).digest() * (n // 32 + 1))[:n]
    return {"BinarySaveState": data(225155 // scale), "PlayerDatabase": data(6291464 // scale),
            "ProfileData": data(20488 // scale), "SaveState": data(122843 // scale),
            "TransactionLogFile": data(8), "VersionFlags": data(4)}


def photo_blobs(image):
    return {"header": os.urandom(106), "image": image, "metadata": os.urandom(60), "thumb": jpeg(500)}


def snapshot(root):
    """{relative path: (size, mtime_ns, sha256)} of every file below root (folders as None)."""
    root = Path(root)
    result = {}
    if not root.exists():
        return result
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if path.is_dir():
            result[relative + "/"] = None
        else:
            stat = path.stat()
            result[relative] = (stat.st_size, stat.st_mtime_ns, hashlib.sha256(path.read_bytes()).hexdigest())
    return result


def junction(link, target):
    """A directory junction (no admin rights needed); True when made."""
    result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(target)], capture_output=True)
    return result.returncode == 0


def pe_header(stamp=STAMP_104, size=SIZE_104):
    """A minimal PE32+ header (what the plugins read to tell the Azimuth build)."""
    data = bytearray(0x400)
    data[0:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHI", data, 0x84, 0x8664, 1, stamp)
    struct.pack_into("<H", data, 0x94, 240)          # SizeOfOptionalHeader
    struct.pack_into("<H", data, 0x98, 0x20B)        # PE32+
    struct.pack_into("<I", data, 0x98 + 56, size)    # SizeOfImage
    return ctypes.create_string_buffer(bytes(data), len(data))


# ---------------------------------------------------------------- azimuth.exe 1.0.4

class PE:
    def __init__(self, data):
        self.data = d = data
        e = struct.unpack_from("<I", d, 0x3C)[0]
        self.stamp = struct.unpack_from("<I", d, e + 8)[0]
        count, optional_size = struct.unpack_from("<H", d, e + 6)[0], struct.unpack_from("<H", d, e + 20)[0]
        optional = e + 24
        self.image_size = struct.unpack_from("<I", d, optional + 56)[0]
        self.sections = []
        for i in range(count):
            s = optional + optional_size + 40 * i
            name = d[s:s + 8].rstrip(b"\0").decode()
            vsize, va, rsize, raw = struct.unpack_from("<IIII", d, s + 8)
            self.sections.append((name, va, vsize, rsize, raw))
        self.imports_rva = struct.unpack_from("<I", d, optional + 112 + 8)[0]

    def offset(self, rva):
        for _, va, vsize, rsize, raw in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError(hex(rva))

    def rva(self, offset):
        for _, va, vsize, rsize, raw in self.sections:
            if raw <= offset < raw + rsize:
                return offset - raw + va
        return None

    def cstr(self, rva):
        o = self.offset(rva)
        return self.data[o:self.data.index(b"\0", o)].decode("latin1")

    def iat(self):
        """{slot rva: function name} of every named import."""
        result, o = {}, self.offset(self.imports_rva)
        while True:
            lookup, _, _, name, first = struct.unpack_from("<IIIII", self.data, o)
            if not name:
                return result
            thunk = lookup or first
            for i in range(100000):
                value = struct.unpack_from("<Q", self.data, self.offset(thunk + 8 * i))[0]
                if not value:
                    break
                if not value >> 63:
                    result[first + 8 * i] = self.cstr(value + 2)
            o += 20

    def text(self):
        name, va, vsize, rsize, raw = next(s for s in self.sections if s[0] == ".text")
        return va, self.data[raw:raw + min(vsize, rsize)]


def azimuth_exe(azimuth_zip):
    with zipfile.ZipFile(azimuth_zip) as archive:
        return archive.read("azimuth.exe")


def wide_strings(pe, text):
    """RVAs of the NUL-terminated UTF-16 string `text`."""
    needle = text.encode("utf-16le") + b"\0\0"
    return [pe.rva(m.start()) for m in re.finditer(re.escape(needle), pe.data) if m.start() % 2 == 0]


def ascii_strings(pe, text, ends=b"\0"):
    """RVAs of the ASCII string `text` followed by one of the bytes in `ends`."""
    return [pe.rva(m.start()) for m in re.finditer(re.escape(text.encode()) + b"[" + re.escape(ends) + b"]", pe.data)]


def code_events(pe, start, end, strings, iat):
    """[(rva, what)] in code [start, end): rip-relative references to `strings` ({rva: label}) and calls of IAT functions."""
    va, code = pe.text()
    events = []
    for i in range(start - va, end - va - 7):
        if code[i] in (0x48, 0x4C) and code[i + 1] == 0x8D and code[i + 2] & 0xC7 == 5:
            target = va + i + 7 + struct.unpack_from("<i", code, i + 3)[0]
            if target in strings:
                events.append((va + i, strings[target]))
        if code[i] == 0xFF and code[i + 1] == 0x15:
            target = va + i + 6 + struct.unpack_from("<i", code, i + 2)[0]
            if target in iat:
                events.append((va + i, iat[target]))
        if code[i] == 0x48 and code[i + 1] == 0x8B and code[i + 2] & 0xC7 == 5:  # mov rax,[rip+IAT]
            target = va + i + 7 + struct.unpack_from("<i", code, i + 3)[0]
            if target in iat:
                events.append((va + i, iat[target]))
    return events


def azimuth_facts(azimuth_zip, check, skip):
    """The facts about azimuth.exe 1.0.4 the save plugins rely on."""
    if not azimuth_zip or not Path(azimuth_zip).is_file():
        skip(f"azimuth.exe 1.0.4 facts: Azimuth ZIP not found ({azimuth_zip})")
        return None
    data = azimuth_exe(azimuth_zip)
    pe = PE(data)
    check(pe.stamp == STAMP_104 and pe.image_size == SIZE_104,
          f"azimuth.exe is 1.0.4 (TimeDateStamp {pe.stamp:#x}, SizeOfImage {pe.image_size:#x})")
    for text in ("titles", "saves", "player-1", "revisions", "data", "commit", "pending"):
        check(bool(wide_strings(pe, text)), f"azimuth.exe 1.0.4 has the path name L\"{text}\"")
    check(bool(ascii_strings(pe, "Azimuth complete save revision v2", b"\n\0")),
          "azimuth.exe 1.0.4 writes commit markers of format v2")
    # The publish: "pending" is written, then renamed to "commit" (MoveFileExW), in the
    # function that also fails with "Cannot publish a complete save revision".
    iat = pe.iat()
    labels = {}
    for text in ("revisions", "data", "pending", "commit"):
        for rva in wide_strings(pe, text):
            labels[rva] = text
    for rva in ascii_strings(pe, "Cannot publish a complete save revision"):
        labels[rva] = "publish-error"
    va, code = pe.text()
    publish = [e for e in code_events(pe, va, va + len(code), {k: v for k, v in labels.items() if v == "publish-error"}, {})]
    check(len(publish) == 1, f"one function fails with \"Cannot publish a complete save revision\" ({len(publish)})")
    if publish:
        site = publish[0][0]
        events = code_events(pe, site - 0xD40, site, labels, iat)
        order = [what for _, what in events if what in ("revisions", "CreateDirectoryW", "data", "pending", "commit", "MoveFileExW")]
        compact = [w for i, w in enumerate(order) if i == 0 or order[i - 1] != w]
        wanted = ["revisions", "CreateDirectoryW", "data", "pending", "commit", "MoveFileExW"]
        at = 0
        for what in compact:
            if at < len(wanted) and what == wanted[at]:
                at += 1
        check(at == len(wanted), "a revision is published as: revisions\\<n> created, data\\ written, "
                                 f"\"pending\" written, then renamed to \"commit\" ({' > '.join(compact)})")
    return data


def real_saves(game):
    """The game folder's saves (only ever copied from), or None."""
    folder = Path(game) / "data" / "titles" / TITLE / "saves"
    return folder if folder.is_dir() and any(folder.iterdir()) else None


def copy_tree(source, target):
    """A copy of a saves folder (the source is only read)."""
    shutil.copytree(source, target, symlinks=True)
    return Path(target)


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)


def open_shared(path):
    """Opens a file the way the plugins read saves (share read, write, delete); returns the handle."""
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                     wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    return kernel32.CreateFileW(str(path), 0x80000000, 7, None, 3, 0, None)


def close(handle):
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle(handle)
