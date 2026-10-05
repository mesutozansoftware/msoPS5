#!/usr/bin/env python3
"""Generates the package-installer test fixtures.

Every fixture holds the same small fake game (see GAME_FILES) in a different container:

  plain.ffpfs            unsigned PFS image
  compressed.ffpfsc      unsigned PFS image with PFSC (zlib) compressed files
  signed32.ffpfs         signed PFS image (32-bit pointers), AES-XTS encrypted with the
                         all-zero EKPFS (classic key schedule), scattered blocks and
                         single/double indirect blocks
  signed64.ffpfs         signed PFS image (64-bit pointers), encrypted with the PS5
                         ("newCrypt") key schedule
  game.exfat             exFAT image with the game in a sub folder, FAT chains, a file whose
                         valid data length is shorter than its length, and Unicode names
  exfat-wrapped.ffpfsc   PFS image whose only file is game.exfat, PFSC compressed
  fake.pkg               finalized PS5 package (FIH): encrypted outer PFS holding a PFSC
                         compressed pfs_image.dat with an encrypted inner PFS
  retail.pkg             finalized package marked as retail (must be rejected)

The expected installed trees are written to expected/game (PFS and package fixtures) and
expected/exfat-game (exFAT fixtures).

Usage: generate_fixtures.py <output-dir>. Needs Python 3 and the `cryptography` package; the
output is deterministic.
"""

import hashlib
import sys
import hmac
import struct
import zlib
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PFS_MAGIC = 20130315
BLOCK = 0x1000
SECTOR = 0x1000
CONTENT_ID = "UP9000-PPSA01234_00-MSOPS5TESTGAME00"
PASSCODE = b"0" * 32


def pattern(seed: int, size: int) -> bytes:
    """Deterministic, poorly compressible bytes."""
    out = bytearray()
    counter = 0
    while len(out) < size:
        out += hashlib.sha256(struct.pack("<II", seed, counter)).digest()
        counter += 1
    return bytes(out[:size])


GAME_FILES = {
    "eboot.bin": pattern(1, 9000),
    "sce_sys/param.json": b'{"titleId": "PPSA01234", "contentId": "' + CONTENT_ID.encode() + b'"}',
    "sce_sys/icon0.png": pattern(2, 300),
    "data/empty.bin": b"",
    "data/zeros.bin": b"\0" * 70000,
    # Spans 12 direct blocks, a full single indirect block and part of a double indirect one
    # in the signed fixtures (4 KiB blocks, 113 or 102 records per indirect block).
    "data/large.bin": pattern(3, 4096 * 140 + 123),
    "data/nested/deeper/note.txt": b"hello from msoPS5\n" * 40,
}


# --------------------------------------------------------------------------- crypto helpers


def compute_ekpfs(content_id: str, passcode: bytes, sha3: bool) -> bytes:
    h = (lambda d: hashlib.sha3_256(d).digest()) if sha3 else (lambda d: hashlib.sha256(d).digest())
    material = h(struct.pack(">I", 1)) + h(content_id.encode().ljust(48, b"\0")) + passcode
    return h(material)


def xts_keys(ekpfs: bytes, seed: bytes, new_crypt: bool) -> bytes:
    root = hmac.new(ekpfs, seed, hashlib.sha256).digest() if new_crypt else ekpfs
    key = hmac.new(root, struct.pack("<I", 1) + seed, hashlib.sha256).digest()
    tweak_key, data_key = key[:16], key[16:]
    return data_key + tweak_key


def xts_encrypt(image: bytearray, plain_bytes: int, key: bytes) -> None:
    for offset in range(plain_bytes, len(image), SECTOR):
        tweak = struct.pack("<QQ", offset // SECTOR, 0)
        enc = Cipher(algorithms.AES(key), modes.XTS(tweak)).encryptor()
        image[offset : offset + SECTOR] = enc.update(bytes(image[offset : offset + SECTOR])) + enc.finalize()


# --------------------------------------------------------------------------- PFSC


def pfsc(data: bytes, logical_block: int = 0x10000) -> bytes:
    count = (len(data) + logical_block - 1) // logical_block
    logical_size = count * logical_block
    padded = data.ljust(logical_size, b"\0")
    table_offset = 0x400
    data_start = (table_offset + (count + 1) * 8 + 0xF) & ~0xF
    blocks = []
    for i in range(count):
        raw = padded[i * logical_block : (i + 1) * logical_block]
        packed = zlib.compress(raw, 9)
        blocks.append(packed if len(packed) < logical_block else raw)
    offsets = [data_start]
    for b in blocks:
        offsets.append(offsets[-1] + len(b))
    out = bytearray(data_start)
    struct.pack_into("<IIIIqqQq", out, 0, 0x43534650, 0, 6, logical_block, logical_block,
                     table_offset, data_start, logical_size)
    struct.pack_into(f"<{count + 1}Q", out, table_offset, *offsets)
    for b in blocks:
        out += b
    return bytes(out)


# --------------------------------------------------------------------------- PFS


class Node:
    def __init__(self, name, data=None):
        self.name = name
        self.data = data  # None for directories
        self.children = []
        self.inode = 0


def build_tree(files: dict) -> Node:
    root = Node("uroot")
    for path, data in files.items():
        node = root
        parts = path.split("/")
        for part in parts[:-1]:
            nxt = next((c for c in node.children if c.name == part and c.data is None), None)
            if nxt is None:
                nxt = Node(part)
                node.children.append(nxt)
            node = nxt
        node.children.append(Node(parts[-1], data))
    return root


def dirent(inode: int, kind: int, name: str) -> bytes:
    raw = name.encode()
    size = (len(raw) + 17 + 7) & ~7
    return struct.pack("<IIII", inode, kind, len(raw), size) + raw + b"\0" * (size - 16 - len(raw))


def build_pfs(files: dict, *, signed=False, wide=False, compress=False, ekpfs=None, new_crypt=False,
              scatter=False) -> bytes:
    """Builds a PFS image. `scatter` stores signed files in non-contiguous blocks."""
    inode_size = (0x310 if wide else 0x2C8) if signed else 0xA8
    entry_size = 32 + (8 if wide else 4)
    per_block = BLOCK // inode_size

    superroot = Node("")
    uroot = build_tree(files)
    nodes = [superroot, uroot]

    def collect(node):
        for child in node.children:
            nodes.append(child)
            if child.data is None:
                collect(child)

    collect(uroot)
    for i, node in enumerate(nodes):
        node.inode = i

    inode_blocks = (len(nodes) + per_block - 1) // per_block
    next_block = 1 + inode_blocks
    blocks = {}  # block number -> bytes
    inodes = {}

    def payload_for(node, parent):
        if node.data is not None:
            return pfsc(node.data) if compress and len(node.data) > 0 else node.data
        out = bytearray()
        if node is superroot:
            out += dirent(superroot.inode, 4, ".") + dirent(superroot.inode, 5, "..")
            out += dirent(uroot.inode, 3, "uroot")
            return bytes(out)
        out += dirent(node.inode, 4, ".") + dirent(parent.inode, 5, "..")
        for child in node.children:
            entry = dirent(child.inode, 2 if child.data is not None else 3, child.name)
            if len(out) % BLOCK + len(entry) > BLOCK:
                out += b"\0" * (BLOCK - len(out) % BLOCK)
            out += entry
        return bytes(out)

    parents = {uroot.inode: superroot}

    def mark_parents(node):
        for child in node.children:
            parents[child.inode] = node
            if child.data is None:
                mark_parents(child)

    mark_parents(uroot)

    for node in nodes:
        payload = payload_for(node, parents.get(node.inode, superroot))
        count = (len(payload) + BLOCK - 1) // BLOCK
        if node.data is None:
            count = max(count, 1)
        if signed and scatter and count > 1:
            # Interleave with a gap block so the file is not contiguous.
            numbers = []
            for _ in range(count):
                numbers.append(next_block)
                next_block += 2
        else:
            numbers = list(range(next_block, next_block + count))
            next_block += count
        for i, number in enumerate(numbers):
            blocks[number] = payload[i * BLOCK : (i + 1) * BLOCK].ljust(BLOCK, b"\0")

        pointers_direct = numbers[:12]
        rest = numbers[12:]
        ib = [0] * 5
        if signed and rest:
            records_per_block = BLOCK // entry_size
            fmt = "<q" if wide else "<i"

            def record_block(ptrs):
                nonlocal next_block
                number = next_block
                next_block += 1
                raw = bytearray(BLOCK)
                for i, p in enumerate(ptrs):
                    struct.pack_into(fmt, raw, i * entry_size + 32, p)
                blocks[number] = bytes(raw)
                return number

            ib[0] = record_block(rest[:records_per_block])
            rest = rest[records_per_block:]
            if rest:
                children = []
                while rest:
                    children.append(record_block(rest[:records_per_block]))
                    rest = rest[records_per_block:]
                ib[1] = record_block(children)

        mode = (0x4000 if node.data is None else 0x8000) | 0x16D
        flags = 1 if (compress and node.data) else 0
        if node.data is not None and flags:
            size, size_compressed = len(payload), len(node.data)
        else:
            size = size_compressed = len(payload) if node.data is not None else count * BLOCK
        raw = bytearray(inode_size)
        struct.pack_into("<HHIqq", raw, 0, mode, 1, flags, size, size_compressed)
        struct.pack_into("<I", raw, 0x60, count)
        if signed:
            base = 0x68 if wide else 0x64
            fmt = "<q" if wide else "<i"
            for i in range(12):
                struct.pack_into(fmt, raw, base + i * entry_size + 32,
                                 pointers_direct[i] if i < len(pointers_direct) else 0)
            for i in range(5):
                struct.pack_into(fmt, raw, base + (12 + i) * entry_size + 32, ib[i])
        else:
            struct.pack_into("<i", raw, 0x64, numbers[0] if numbers else 0)
        inodes[node.inode] = bytes(raw)

    total_blocks = next_block
    image = bytearray(total_blocks * BLOCK)
    seed = hashlib.sha256(b"msoPS5 seed").digest()[:16]

    mode = 0x8  # case-insensitive
    if signed:
        mode |= 0x1
    if wide:
        mode |= 0x2
    if ekpfs is not None:
        mode |= 0x4
    struct.pack_into("<qq", image, 0, 2, PFS_MAGIC)
    struct.pack_into("<H", image, 0x1C, mode)
    struct.pack_into("<I", image, 0x20, BLOCK)
    struct.pack_into("<qqqq", image, 0x28, 1, len(nodes), total_blocks, inode_blocks)
    struct.pack_into("<I", image, 0x50 + 0x60, inode_blocks)
    struct.pack_into("<q", image, 0x50 + 0x68 + 32, 1)
    image[0x370:0x380] = seed

    for index, raw in inodes.items():
        block, slot = divmod(index, per_block)
        offset = (1 + block) * BLOCK + slot * inode_size
        image[offset : offset + inode_size] = raw
    for number, raw in blocks.items():
        image[number * BLOCK : (number + 1) * BLOCK] = raw

    if ekpfs is not None:
        xts_encrypt(image, BLOCK, xts_keys(ekpfs, seed, new_crypt))
    return bytes(image)


# --------------------------------------------------------------------------- exFAT


def build_exfat(files: dict) -> bytes:
    sector_shift, cluster_shift = 9, 3
    sector = 1 << sector_shift
    cluster = sector << cluster_shift

    fat = {0: 0xFFFFFFF8, 1: 0xFFFFFFFF}
    heap = {}
    next_cluster = [2]

    def allocate(data: bytes, contiguous: bool) -> int:
        count = max(1, (len(data) + cluster - 1) // cluster)
        numbers = []
        for _ in range(count):
            numbers.append(next_cluster[0])
            # FAT-chained data skips a cluster each time so it is never accidentally contiguous.
            next_cluster[0] += 1 if contiguous else 2
        for i, number in enumerate(numbers):
            heap[number] = data[i * cluster : (i + 1) * cluster].ljust(cluster, b"\0")
        if not contiguous:
            for a, b in zip(numbers, numbers[1:]):
                fat[a] = b
            fat[numbers[-1]] = 0xFFFFFFFF
        return numbers[0]

    def name_entries(name: str):
        utf16 = name.encode("utf-16-le")
        units = len(utf16) // 2
        parts = []
        for i in range(0, units, 15):
            chunk = utf16[i * 2 : (i + 15) * 2].ljust(30, b"\0")
            parts.append(bytes([0xC1, 0]) + chunk)
        return units, parts

    def entry_set(name, is_dir, first, valid, length, contiguous):
        units, names = name_entries(name)
        file_entry = bytearray(32)
        file_entry[0] = 0x85
        file_entry[1] = 1 + len(names)
        struct.pack_into("<H", file_entry, 4, 0x10 if is_dir else 0x20)
        stream = bytearray(32)
        stream[0] = 0xC0
        stream[1] = 0x01 | (0x02 if contiguous else 0)
        stream[3] = units
        struct.pack_into("<Q", stream, 8, valid)
        struct.pack_into("<I", stream, 20, first)
        struct.pack_into("<Q", stream, 24, length)
        return bytes(file_entry) + bytes(stream) + b"".join(names)

    def write_dir(node) -> bytes:
        body = bytearray()
        for i, child in enumerate(node.children):
            if child.data is None:
                data = write_dir(child)
                data = data.ljust(max(1, (len(data) + cluster - 1) // cluster) * cluster, b"\0")
                first = allocate(data, contiguous=True)
                body += entry_set(child.name, True, first, len(data), len(data), True)
                continue
            data = child.data
            # Alternate between contiguous storage and FAT chains.
            contiguous = i % 2 == 0
            valid = length = len(data)
            if child.name == "note.txt":
                length += 5000  # the tail past the valid data length must read as zero
            first = allocate(data.ljust(length, b"\0"), contiguous) if length else 0
            body += entry_set(child.name, False, first, valid, length, contiguous)
        return bytes(body)

    root_body = write_dir(build_tree(files))
    # The root directory always uses a FAT chain; spread it over two clusters.
    root_first = allocate(root_body.ljust(cluster * 2, b"\0"), contiguous=False)

    cluster_count = next_cluster[0] - 2
    fat_offset_sectors = 24
    fat_length_sectors = ((cluster_count + 2) * 4 + sector - 1) // sector
    heap_offset_sectors = (fat_offset_sectors + fat_length_sectors + 7) & ~7
    total_sectors = heap_offset_sectors + cluster_count * (cluster // sector)

    boot = bytearray(sector)
    boot[0:3] = b"\xEB\x76\x90"
    boot[3:11] = b"EXFAT   "
    struct.pack_into("<QQIIIII", boot, 64, 0, total_sectors, fat_offset_sectors, fat_length_sectors,
                     heap_offset_sectors, cluster_count, root_first)
    boot[108] = sector_shift
    boot[109] = cluster_shift
    boot[110] = 1
    boot[510:512] = b"\x55\xAA"

    image = bytearray(total_sectors * sector)
    image[0:sector] = boot
    fat_base = fat_offset_sectors * sector
    for number, value in fat.items():
        struct.pack_into("<I", image, fat_base + number * 4, value)
    heap_base = heap_offset_sectors * sector
    for number, raw in heap.items():
        offset = heap_base + (number - 2) * cluster
        image[offset : offset + cluster] = raw
    return bytes(image)


# --------------------------------------------------------------------------- FIH package


def build_fih(outer: bytes, retail: bool = False) -> bytes:
    header = bytearray(0x10000)
    header[0:4] = b"\x7FFIH"
    header[5] = 0x80 if retail else 0x00
    cnt = bytearray(0x5A0)
    cnt[0:4] = b"\x7FCNT"
    cnt[0x40 : 0x40 + len(CONTENT_ID)] = CONTENT_ID.encode()
    struct.pack_into("<QQQ", header, 0x10, 0x10000, len(outer), 0x10000)
    struct.pack_into("<Q", header, 0x58, 0x10000 + len(outer))
    struct.pack_into("<Q", header, 0xA0, len(cnt))
    return bytes(header) + outer + bytes(cnt)


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit("usage: generate_fixtures.py <output-dir>")
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    zero = b"\0" * 32
    exfat_files = {f"PPSA05678/{k}": v for k, v in GAME_FILES.items()}
    exfat_files["PPSA05678/data/café \U0001F3AE.txt"] = "unicode name ü".encode()
    exfat_files["readme.txt"] = b"outside the game folder\n"
    exfat = build_exfat(exfat_files)

    sha3_key = compute_ekpfs(CONTENT_ID, PASSCODE, sha3=True)
    sha2_key = compute_ekpfs(CONTENT_ID, PASSCODE, sha3=False)
    inner = build_pfs(GAME_FILES, ekpfs=sha2_key, new_crypt=False)
    outer = build_pfs({"pfs_image.dat": inner}, signed=True, wide=True, compress=True,
                      ekpfs=sha3_key, new_crypt=True)

    fixtures = {
        "plain.ffpfs": build_pfs(GAME_FILES),
        "compressed.ffpfsc": build_pfs(GAME_FILES, compress=True),
        "signed32.ffpfs": build_pfs(GAME_FILES, signed=True, ekpfs=zero, scatter=True),
        "signed64.ffpfs": build_pfs(GAME_FILES, signed=True, wide=True, ekpfs=zero, new_crypt=True,
                                    scatter=True),
        "game.exfat": exfat,
        "exfat-wrapped.ffpfsc": build_pfs({"game.exfat": exfat}, compress=True),
        "fake.pkg": build_fih(outer),
        "retail.pkg": build_fih(outer, retail=True)[:0x10100],
    }
    for name, data in fixtures.items():
        (out / name).write_bytes(data)

    exfat_game = {k[len("PPSA05678/"):]: v for k, v in exfat_files.items() if k.startswith("PPSA05678/")}
    exfat_game["data/nested/deeper/note.txt"] += b"\0" * 5000
    for tree, contents in (("game", GAME_FILES), ("exfat-game", exfat_game)):
        for path, data in contents.items():
            target = out / "expected" / tree / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)


if __name__ == "__main__":
    main()
