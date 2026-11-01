#!/usr/bin/env python3
"""Identify and unpack a Skate 3 Xbox 360 disc image.

    python -I tools/s3inspect.py info    <iso-or-folder>
    python -I tools/s3inspect.py ls      <iso> [dir]
    python -I tools/s3inspect.py extract <iso> <out-dir> [path ...]

Reads XDVDFS (the Xbox 360 disc filesystem) from full XGD2/XGD3 rips or
xiso-style images, and dumps the plaintext headers of default.xex. Standard
library only, read-only on the source image.
"""

import hashlib
import os
import struct
import sys

SECTOR = 2048
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets for the known disc layouts; 0 = trimmed/xiso image.
PARTITION_OFFSETS = {
    0x0: "XDVDFS (trimmed)",
    0xFD90000: "XGD2",
    0x2080000: "XGD3",
    0x18300000: "XGD1",
}
ATTR_DIRECTORY = 0x10


class Entry:
    __slots__ = ("name", "sector", "size", "attr")

    def __init__(self, name, sector, size, attr):
        self.name, self.sector, self.size, self.attr = name, sector, size, attr

    @property
    def is_dir(self):
        return bool(self.attr & ATTR_DIRECTORY)


class XdvdfsImage:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.base = None
        for off, layout in PARTITION_OFFSETS.items():
            self.f.seek(off + 32 * SECTOR)
            vd = self.f.read(SECTOR)
            if vd[:20] == XDVDFS_MAGIC:
                self.base, self.layout = off, layout
                self.root_sector, self.root_size = struct.unpack_from("<II", vd, 20)
                break
        if self.base is None:
            raise ValueError(f"{path}: no XDVDFS volume found (not an Xbox 360 disc image?)")

    def read(self, sector, size, offset=0):
        self.f.seek(self.base + sector * SECTOR + offset)
        return self.f.read(size)

    def listdir(self, sector, size):
        """Walk the directory's binary tree in name order."""
        if size == 0:
            return []
        data = self.read(sector, size)
        out, stack, seen = [], [], set()
        off = 0
        while stack or off is not None:
            while off is not None:
                if off in seen or off + 14 > len(data):
                    off = None
                    break
                left, right = struct.unpack_from("<HH", data, off)
                if left == 0xFFFF and right == 0xFFFF and off == 0:
                    return []  # empty directory
                seen.add(off)
                stack.append(off)
                off = left * 4 if left not in (0, 0xFFFF) else None
            if not stack:
                break
            node = stack.pop()
            _, right, esec, esize, attr, nlen = struct.unpack_from("<HHIIBB", data, node)
            name = data[node + 14 : node + 14 + nlen].decode("latin-1")
            out.append(Entry(name, esec, esize, attr))
            off = right * 4 if right not in (0, 0xFFFF) else None
        return out

    def root(self):
        return self.listdir(self.root_sector, self.root_size)

    def walk(self, entries=None, prefix=""):
        for e in self.root() if entries is None else entries:
            path = f"{prefix}{e.name}"
            yield path, e
            if e.is_dir:
                yield from self.walk(self.listdir(e.sector, e.size), path + "/")

    def find(self, path):
        entries = self.root()
        parts = [p for p in path.replace("\\", "/").split("/") if p]
        for i, part in enumerate(parts):
            match = next((e for e in entries if e.name.lower() == part.lower()), None)
            if match is None:
                return None
            if i == len(parts) - 1:
                return match
            if not match.is_dir:
                return None
            entries = self.listdir(match.sector, match.size)
        return None

    def read_file(self, entry, limit=None):
        return self.read(entry.sector, entry.size if limit is None else min(limit, entry.size))

    def copy_file(self, entry, dst, chunk=8 << 20):
        remaining, pos = entry.size, 0
        with open(dst, "wb") as out:
            while remaining:
                n = min(chunk, remaining)
                out.write(self.read(entry.sector, n, pos))
                pos += n
                remaining -= n


# --- XEX2 ------------------------------------------------------------------

XEX_KEYS = {
    0x000002FF: "resource_info",
    0x000003FF: "file_format_info",
    0x000005FF: "delta_patch_descriptor",
    0x00000405: "base_reference",
    0x000080FF: "bounding_path",
    0x00010001: "original_base_address",
    0x00010100: "entry_point",
    0x00010201: "image_base_address",
    0x000103FF: "import_libraries",
    0x00018002: "checksum_timestamp",
    0x000183FF: "original_pe_name",
    0x000200FF: "static_libraries",
    0x00020104: "tls_info",
    0x00020200: "default_stack_size",
    0x00020301: "default_filesystem_cache_size",
    0x00020401: "default_heap_size",
    0x00030000: "system_flags",
    0x00040006: "execution_info",
    0x00040201: "title_workspace_size",
    0x00040310: "game_ratings",
    0x00040404: "lan_key",
    0x000405FF: "xbox360_logo",
    0x000406FF: "multidisc_media_ids",
    0x000407FF: "alternate_title_ids",
    0x00040801: "additional_title_memory",
}
MODULE_FLAGS = {
    0x01: "title", 0x02: "exports_to_title", 0x04: "system_debugger",
    0x08: "dll", 0x10: "module_patch", 0x20: "patch_full", 0x40: "patch_delta",
    0x80: "user_mode",
}
ENCRYPTION = {0: "none", 1: "normal"}
COMPRESSION = {0: "none", 1: "basic", 2: "normal (LZX)", 3: "delta"}
# Region is a bitmask; any bit inside a group's mask enables (part of) that region.
REGIONS = {0x000000FF: "NTSC/U", 0x0000FF00: "NTSC/J", 0x00FF0000: "PAL"}


def region_name(r):
    if r == 0xFFFFFFFF:
        return "region free"
    names = [n for mask, n in REGIONS.items() if r & mask]
    return f"{' + '.join(names) or 'none'} (0x{r:08X})"


def xex_version(v):
    return f"{v >> 28}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


def parse_xex(data):
    if data[:4] != b"XEX2":
        raise ValueError("default.xex is not an XEX2 image (magic %r)" % data[:4])
    flags, pe_off, _, sec_off, count = struct.unpack_from(">5I", data, 4)
    info = {"module_flags": [n for b, n in MODULE_FLAGS.items() if flags & b],
            "pe_data_offset": pe_off, "headers": {}}
    for i in range(count):
        key, val = struct.unpack_from(">II", data, 0x18 + i * 8)
        name = XEX_KEYS.get(key, f"0x{key:08X}")
        info["headers"][name] = (key, val)

    def blob(name):
        key, val = info["headers"][name]
        return val if key & 0xFF in (0, 1) else data[val:]

    h = info["headers"]
    if "execution_info" in h:
        media, ver, base_ver, title, plat, table, disc, discs, save = struct.unpack_from(
            ">IIIIBBBBI", blob("execution_info"))
        info.update(title_id=f"{title:08X}", media_id=f"{media:08X}",
                    version=xex_version(ver), base_version=xex_version(base_ver),
                    disc=f"{disc}/{discs}", savegame_id=f"{save:08X}")
    if "file_format_info" in h:
        _, enc, comp = struct.unpack_from(">IHH", blob("file_format_info"))
        info["encryption"] = ENCRYPTION.get(enc, enc)
        info["compression"] = COMPRESSION.get(comp, comp)
    for name in ("entry_point", "image_base_address", "original_base_address",
                 "default_stack_size", "default_heap_size", "title_workspace_size"):
        if name in h:
            info[name] = f"0x{h[name][1]:08X}"
    if "original_pe_name" in h:
        b = blob("original_pe_name")
        size = struct.unpack_from(">I", b)[0]
        info["original_pe_name"] = b[4:size].split(b"\0")[0].decode("latin-1")
    if "static_libraries" in h:
        b = blob("static_libraries")
        size = struct.unpack_from(">I", b)[0]
        libs = []
        for off in range(4, size, 16):
            name = b[off:off + 8].split(b"\0")[0].decode("latin-1")
            major, minor, build, qfe = struct.unpack_from(">HHHH", b, off + 8)
            libs.append(f"{name} {major}.{minor}.{build}.{qfe & 0x3FFF}")
        info["static_libraries"] = libs
    if "import_libraries" in h:
        b = blob("import_libraries")
        total, st_size, st_count = struct.unpack_from(">III", b)
        names, j = [], 0
        table = b[12:12 + st_size]
        while j < st_size and len(names) < st_count:
            end = table.index(b"\0", j)
            names.append(table[j:end].decode("latin-1"))
            j = (end + 1 + 3) & ~3
        libs, off = [], 12 + st_size
        while off < total:
            lsize, = struct.unpack_from(">I", b, off)
            ver, ver_min, name_idx, n = struct.unpack_from(">IIHH", b, off + 0x1C)
            libs.append(f"{names[name_idx]} {xex_version(ver)} ({n} imports)")
            off += lsize
        info["import_libraries"] = libs
    if sec_off:
        image_size, = struct.unpack_from(">I", data, sec_off + 4)
        load_addr, = struct.unpack_from(">I", data, sec_off + 0x110)
        region, = struct.unpack_from(">I", data, sec_off + 0x178)
        info.update(image_size=f"0x{image_size:08X}", load_address=f"0x{load_addr:08X}",
                    region=region_name(region))
    return info


# --- commands --------------------------------------------------------------

def open_source(path):
    """Return (reader, xex_bytes) for an ISO or an extracted folder."""
    if os.path.isdir(path):
        xex_path = os.path.join(path, "default.xex")
        with open(xex_path, "rb") as f:
            return None, f.read()
    img = XdvdfsImage(path)
    e = img.find("default.xex")
    if e is None:
        raise ValueError("no default.xex at the root of the image")
    return img, img.read_file(e)


def cmd_info(path):
    img, xex = open_source(path)
    if img:
        files = [e for _, e in img.walk() if not e.is_dir]
        print(f"image          {path}")
        print(f"layout         {img.layout} (partition at 0x{img.base:X})")
        print(f"files          {len(files)}, {sum(e.size for e in files) / 2**30:.2f} GiB")
    info = parse_xex(xex)
    print(f"default.xex    {len(xex):,} bytes  sha1 {hashlib.sha1(xex).hexdigest()}")
    for k in ("title_id", "media_id", "version", "base_version", "disc", "savegame_id",
              "region", "module_flags", "encryption", "compression", "original_pe_name",
              "load_address", "image_size", "entry_point", "default_stack_size",
              "default_heap_size", "title_workspace_size"):
        if k in info:
            v = info[k]
            print(f"{k:<14} {', '.join(v) if isinstance(v, list) else v}")
    for k in ("import_libraries", "static_libraries"):
        if k in info:
            print(f"{k}:")
            for lib in info[k]:
                print(f"  {lib}")
    print("optional headers: " + ", ".join(sorted(info["headers"])))


def cmd_ls(path, sub=""):
    img = XdvdfsImage(path)
    if sub:
        d = img.find(sub)
        if d is None or not d.is_dir:
            sys.exit(f"no directory {sub!r}")
        entries = img.listdir(d.sector, d.size)
    else:
        entries = img.root()
    for e in entries:
        print(f"{'<DIR>' if e.is_dir else f'{e.size:>12,}'}  {e.name}")


def cmd_extract(path, out, *only):
    img = XdvdfsImage(path)
    out = os.path.abspath(out)
    wanted = {p.replace("\\", "/").strip("/").lower() for p in only}
    for rel, e in img.walk():
        if wanted and rel.lower() not in wanted and not any(w.startswith(rel.lower() + "/") for w in wanted):
            continue
        dst = os.path.abspath(os.path.join(out, *rel.split("/")))
        if not dst.startswith(out + os.sep):
            sys.exit(f"refusing to write outside {out}: {rel!r}")
        if e.is_dir:
            os.makedirs(dst, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            print(f"{e.size:>12,}  {rel}")
            img.copy_file(e, dst)


def main(argv):
    cmds = {"info": (cmd_info, 1, 1), "ls": (cmd_ls, 1, 2), "extract": (cmd_extract, 2, 64)}
    if len(argv) < 2 or argv[1] not in cmds:
        sys.exit(__doc__)
    fn, lo, hi = cmds[argv[1]]
    if not lo <= len(argv) - 2 <= hi:
        sys.exit(__doc__)
    fn(*argv[2:])


if __name__ == "__main__":
    main(sys.argv)
