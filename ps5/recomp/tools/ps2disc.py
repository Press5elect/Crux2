#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - a PS2 disc image, read: the game's executable out of it (the file BOOT2 names in its
# SYSTEM.CNF), or the whole disc as an .iso. Python's own library only, on Windows and on Linux.
#
#   ps2disc.py info  GAME.chd                 what the disc is and which executable it boots
#   ps2disc.py exe   GAME.chd [--out DIR]     the executable (SLUS_213.76, say) into DIR
#   ps2disc.py iso   GAME.chd OUT.iso         the disc's 2048-byte sectors as an .iso
#
# Reads:
#   .chd   MAME's compressed disc images, version 5 (chdman createdvd and createcd): the codecs
#          lzma, zlib, huff, flac, zstd and, for CDs, cdlz, cdzl, cdfl, cdzs. zstd needs Python
#          3.14, or `pip install zstandard`; chdman uses it only when asked (-c zstd).
#   .iso   2048-byte sectors
#   .bin   a CD's raw 2352-byte sectors (or its .cue, which names the .bin)
#   .cso   CISO (deflate)
#   a folder holding the disc's files (SYSTEM.CNF at its top)
#
# CHD reading follows MAME's format as libchdr reads it (libchdr: BSD-3-Clause, Romain Tisserand and
# Aaron Giles); none of its code is here.
import argparse
import array
import binascii
import ctypes
import lzma
import os
import re
import struct
import sys
import zlib
from pathlib import Path

SECTOR = 2048


class DiscError(Exception):
    pass


# The native helper for the "huff" codec (accel/crux2_disc_accel.c), when it is next to this file and
# loads: Python alone decodes such a hunk ~50 times slower (it matters for a whole .iso, not an
# executable). CRUX2_DISC_NO_ACCEL=1 leaves it out.
_accel_huff = None


def _load_accel():
    global _accel_huff
    if os.environ.get("CRUX2_DISC_NO_ACCEL"):
        return
    folder = Path(__file__).resolve().parent / "accel"
    name = "crux2_disc_accel.dll" if os.name == "nt" else "libcrux2_disc_accel.so"
    try:
        lib = ctypes.CDLL(str(folder / name))
        fn = lib.crux2_huff
        fn.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_char_p, ctypes.c_uint]
        fn.restype = ctypes.c_int
        _accel_huff = fn
    except (OSError, AttributeError):
        _accel_huff = None


_load_accel()


# --------------------------------------------------------------------------------------------------
# Bits, read from the top bit of each byte down (as the CHD map, its huffman codec and FLAC write them)


class Bits:
    __slots__ = ("s", "p", "n")

    def __init__(self, data: bytes):
        self.n = len(data) * 8
        self.s = format(int.from_bytes(data, "big"), f"0{self.n}b") if data else ""
        self.p = 0

    def read(self, k: int) -> int:
        if k == 0:
            return 0
        p = self.p
        self.p = p + k
        chunk = self.s[p:p + k]
        if len(chunk) < k:  # past the end reads as zeros (and overflow() says so)
            chunk = chunk.ljust(k, "0")
        return int(chunk, 2)

    def peek(self, k: int) -> int:
        chunk = self.s[self.p:self.p + k]
        if len(chunk) < k:
            chunk = chunk.ljust(k, "0")
        return int(chunk, 2)

    def signed(self, k: int) -> int:
        v = self.read(k)
        return v - (1 << k) if k and v >> (k - 1) else v

    def unary(self) -> int:
        """Zeros up to the next one, which is taken too."""
        i = self.s.find("1", self.p)
        if i < 0:
            raise DiscError("compressed data ends early")
        z = i - self.p
        self.p = i + 1
        return z

    def align(self):
        self.p = (self.p + 7) & ~7

    def overflow(self) -> bool:
        return self.p > self.n


# --------------------------------------------------------------------------------------------------
# Huffman codes (canonical, MAME's two ways of storing a tree)


class Huffman:
    def __init__(self, numcodes: int, maxbits: int):
        self.numcodes = numcodes
        self.maxbits = maxbits
        self.lengths = [0] * numcodes
        self.symbol = None
        self.length = None

    def import_rle(self, bits: Bits):
        numbits = 5 if self.maxbits >= 16 else 4 if self.maxbits >= 8 else 3
        lengths, cur = self.lengths, 0
        while cur < self.numcodes:
            nb = bits.read(numbits)
            if nb != 1:
                lengths[cur] = nb
                cur += 1
                continue
            nb = bits.read(numbits)
            if nb == 1:
                lengths[cur] = 1
                cur += 1
                continue
            rep = bits.read(numbits) + 3
            if rep + cur > self.numcodes:
                raise DiscError("CHD: a damaged huffman tree")
            lengths[cur:cur + rep] = [nb] * rep
            cur += rep
        self.build()

    def import_huffman(self, bits: Bits):
        small = Huffman(24, 6)
        small.lengths[0] = bits.read(3)
        start = bits.read(3) + 1
        count = 0
        for index in range(1, 24):
            if index < start or count == 7:
                small.lengths[index] = 0
            else:
                count = bits.read(3)
                small.lengths[index] = 0 if count == 7 else count
        small.build()
        rlefullbits, temp = 0, self.numcodes - 9
        while temp:
            temp >>= 1
            rlefullbits += 1
        lengths, cur, last = self.lengths, 0, 0
        while cur < self.numcodes:
            value = small.decode(bits)
            if value != 0:
                last = value - 1
                lengths[cur] = last
                cur += 1
            else:
                count = bits.read(3) + 2
                if count == 9:
                    count += bits.read(rlefullbits)
                take = min(count, self.numcodes - cur)
                lengths[cur:cur + take] = [last] * take
                cur += take
        self.build()

    def build(self):
        histo = [0] * 33
        for length in self.lengths:
            if length > self.maxbits:
                raise DiscError("CHD: a damaged huffman tree")
            histo[length] += 1
        start = 0
        for codelen in range(32, 0, -1):
            nextstart = (start + histo[codelen]) >> 1
            if codelen != 1 and nextstart * 2 != start + histo[codelen]:
                raise DiscError("CHD: a damaged huffman tree")
            histo[codelen] = start
            start = nextstart
        size = 1 << self.maxbits
        symbol = [0] * size
        length = [1] * size
        for code, bitcount in enumerate(self.lengths):
            if bitcount > 0:
                value = histo[bitcount]
                histo[bitcount] += 1
                shift = self.maxbits - bitcount
                lo, hi = value << shift, (value + 1) << shift
                symbol[lo:hi] = [code] * (hi - lo)
                length[lo:hi] = [bitcount] * (hi - lo)
        self.symbol, self.length = symbol, length

    def decode(self, bits: Bits) -> int:
        w = bits.peek(self.maxbits)
        bits.p += self.length[w]
        return self.symbol[w]


# --------------------------------------------------------------------------------------------------
# FLAC frames (as chdman writes them: no stream header, 16-bit samples, two channels)


def _flac_residual(b: Bits, blocksize: int, order: int, out: list):
    method = b.read(2)
    if method > 1:
        raise DiscError("FLAC: an unknown residual coding")
    pbits = 4 if method == 0 else 5
    escape = (1 << pbits) - 1
    porder = b.read(4)
    per = blocksize >> porder
    for part in range(1 << porder):
        count = per - order if part == 0 else per
        param = b.read(pbits)
        if param == escape:
            raw = b.read(5)
            out.extend(b.signed(raw) for _ in range(count))
            continue
        s, find = b.s, b.s.find
        p = b.p
        for _ in range(count):
            i = find("1", p)
            if i < 0:
                raise DiscError("FLAC: data ends early")
            q = i - p
            p = i + 1
            if param:
                r = int(s[p:p + param], 2)
                p += param
                v = (q << param) | r
            else:
                v = q
            out.append((v >> 1) ^ -(v & 1))
        b.p = p


def _flac_subframe(b: Bits, blocksize: int, bps: int) -> list:
    b.read(1)
    kind = b.read(6)
    wasted = b.unary() + 1 if b.read(1) else 0
    bps -= wasted
    if kind == 0:
        samples = [b.signed(bps)] * blocksize
    elif kind == 1:
        samples = [b.signed(bps) for _ in range(blocksize)]
    elif 8 <= kind <= 12:
        order = kind - 8
        samples = [b.signed(bps) for _ in range(order)]
        res = []
        _flac_residual(b, blocksize, order, res)
        if order == 0:
            samples = res
        elif order == 1:
            for r in res:
                samples.append(r + samples[-1])
        elif order == 2:
            for r in res:
                samples.append(r + 2 * samples[-1] - samples[-2])
        elif order == 3:
            for r in res:
                samples.append(r + 3 * samples[-1] - 3 * samples[-2] + samples[-3])
        else:
            for r in res:
                samples.append(r + 4 * samples[-1] - 6 * samples[-2] + 4 * samples[-3] - samples[-4])
    elif kind >= 32:
        order = (kind & 31) + 1
        samples = [b.signed(bps) for _ in range(order)]
        precision = b.read(4) + 1
        if precision == 16:
            raise DiscError("FLAC: a bad LPC precision")
        shift = b.signed(5)
        if shift < 0:
            raise DiscError("FLAC: a negative LPC shift")
        coefs = [b.signed(precision) for _ in range(order)]
        res = []
        _flac_residual(b, blocksize, order, res)
        rc = coefs[::-1]  # oldest sample first
        for n, r in enumerate(res):
            acc = 0
            window = samples[n:n + order]
            for c, s in zip(rc, window):
                acc += c * s
            samples.append(r + (acc >> shift))
    else:
        raise DiscError("FLAC: an unknown subframe")
    if wasted:
        samples = [s << wasted for s in samples]
    return samples


def flac_decode(data: bytes, total: int, big_endian: bool) -> bytes:
    """total stereo 16-bit samples from chdman's FLAC frames, as bytes in the order asked."""
    b = Bits(data)
    out = array.array("h")
    got = 0
    while got < total:
        if b.read(14) != 0x3FFE:
            raise DiscError("FLAC: no frame where one should start")
        b.read(2)
        bs_code, sr_code = b.read(4), b.read(4)
        assign, ss_code = b.read(4), b.read(3)
        b.read(1)
        first = b.read(8)
        mask = 0x40
        if first & 0x80:
            while first & mask:
                b.read(8)
                mask >>= 1
        if bs_code == 6:
            blocksize = b.read(8) + 1
        elif bs_code == 7:
            blocksize = b.read(16) + 1
        elif bs_code == 1:
            blocksize = 192
        elif 2 <= bs_code <= 5:
            blocksize = 576 << (bs_code - 2)
        elif bs_code >= 8:
            blocksize = 256 << (bs_code - 8)
        else:
            raise DiscError("FLAC: a reserved block size")
        if sr_code == 12:
            b.read(8)
        elif sr_code in (13, 14):
            b.read(16)
        b.read(8)  # the header's CRC-8
        bps = {0: 16, 1: 8, 2: 12, 4: 16, 5: 20, 6: 24, 7: 32}.get(ss_code)
        if bps is None or assign > 10:
            raise DiscError("FLAC: a reserved sample size or channel layout")
        channels = assign + 1 if assign < 8 else 2
        if channels != 2:
            raise DiscError("FLAC: not two channels")
        ch0 = _flac_subframe(b, blocksize, bps + (1 if assign == 9 else 0))
        ch1 = _flac_subframe(b, blocksize, bps + (1 if assign in (8, 10) else 0))
        b.align()
        b.read(16)  # the frame's CRC-16
        if assign == 8:
            left, right = ch0, [l - s for l, s in zip(ch0, ch1)]
        elif assign == 9:
            left, right = [s + r for s, r in zip(ch0, ch1)], ch1
        elif assign == 10:
            left, right = [], []
            for m, s in zip(ch0, ch1):
                m = (m << 1) | (s & 1)
                left.append((m + s) >> 1)
                right.append((m - s) >> 1)
        else:
            left, right = ch0, ch1
        take = min(blocksize, total - got)
        inter = [0] * (2 * take)
        inter[0::2] = left[:take]
        inter[1::2] = right[:take]
        out.extend(inter)
        got += take
    if big_endian != (sys.byteorder == "big"):
        out.byteswap()
    return out.tobytes()


# --------------------------------------------------------------------------------------------------
# CHD version 5


def _lzma_dict_size(reduce: int) -> int:
    # LzmaEncProps_Normalize at chdman's level 8 (a 64 MiB dictionary, made no larger than a hunk).
    size = 1 << 26
    if size > reduce:
        for i in range(11, 31):
            if reduce <= (2 << i):
                return 2 << i
            if reduce <= (3 << i):
                return 3 << i
    return size


def _lzma(data: bytes, destlen: int, reduce: int) -> bytes:
    filters = [{"id": lzma.FILTER_LZMA1, "dict_size": _lzma_dict_size(reduce), "lc": 3, "lp": 0, "pb": 2}]
    out = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=filters).decompress(data, max_length=destlen)
    if len(out) != destlen:
        raise DiscError("CHD: an lzma hunk came out short")
    return out


def _zlib(data: bytes, destlen: int) -> bytes:
    d = zlib.decompressobj(-15)
    out = d.decompress(data, destlen)
    if len(out) != destlen:
        raise DiscError("CHD: a zlib hunk came out short")
    return out


def _zstd(data: bytes, destlen: int) -> bytes:
    try:
        from compression import zstd  # Python 3.14
        out = zstd.decompress(data)
    except ImportError:
        try:
            import zstandard
        except ImportError:
            raise DiscError("this CHD uses zstd: it needs Python 3.14 or newer, or `pip install zstandard`") from None
        out = zstandard.ZstdDecompressor().decompress(data, max_output_size=destlen)
    if len(out) < destlen:
        raise DiscError("CHD: a zstd hunk came out short")
    return out[:destlen]


CD_FRAME = 2448      # a CD frame in a CHD: 2352 bytes of sector and 96 of subcode
CD_SECTOR = 2352
CD_SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


class Chd:
    def __init__(self, path: Path):
        self.path = Path(path)
        self.f = open(self.path, "rb")
        head = self.f.read(124)
        if len(head) < 16 or head[:8] != b"MComprHD":
            raise DiscError(f"{self.path.name}: not a CHD file")
        version = struct.unpack_from(">I", head, 12)[0]
        if version != 5:
            raise DiscError(f"{self.path.name}: a version {version} CHD; this reads version 5 "
                            "(`chdman copy -i old.chd -o new.chd` makes one)")
        self.codec_tags = [head[16 + 4 * i:20 + 4 * i] for i in range(4)]
        self.logicalbytes, self.mapoffset, self.metaoffset = struct.unpack_from(">QQQ", head, 32)
        self.hunkbytes, self.unitbytes = struct.unpack_from(">II", head, 56)
        if self.hunkbytes == 0 or self.unitbytes == 0:
            raise DiscError(f"{self.path.name}: a damaged CHD header")
        if head[104:124] != bytes(20):
            raise DiscError(f"{self.path.name}: a CHD that needs a parent CHD (a delta); make it whole with chdman copy")
        self.hunkcount = (self.logicalbytes + self.hunkbytes - 1) // self.hunkbytes
        self.meta = self._read_metadata()
        self.is_cd = any(tag in self.meta for tag in (b"CHT2", b"CHTR", b"CHGD", b"CHGT")) or self.unitbytes == CD_FRAME
        self.compressed = self.codec_tags[0] != bytes(4)
        self._codecs = [self._codec(tag) for tag in self.codec_tags]
        self._read_map()
        self._cache = {}       # the hunks read last (another hunk may be a copy of one)

    # ---- metadata
    def _read_metadata(self) -> dict:
        meta = {}
        offset, seen = self.metaoffset, 0
        while offset and seen < 65536:
            self.f.seek(offset)
            raw = self.f.read(16)
            if len(raw) < 16:
                break
            tag, length, nxt = struct.unpack(">IIQ", raw)
            data = self.f.read(length & 0xFFFFFF)
            meta.setdefault(struct.pack(">I", tag), []).append(data)
            offset, seen = nxt, seen + 1
        return meta

    # ---- codecs
    def _codec(self, tag: bytes):
        hb = self.hunkbytes
        if tag == bytes(4):
            return None
        if tag == b"zlib":
            return lambda d, n: _zlib(d, n)
        if tag == b"lzma":
            return lambda d, n: _lzma(d, n, hb)
        if tag == b"zstd":
            return lambda d, n: _zstd(d, n)
        if tag == b"huff":
            return self._huff
        if tag == b"flac":
            return self._flac
        if tag in (b"cdzl", b"cdlz", b"cdfl", b"cdzs"):
            frames = hb // CD_FRAME
            base = {b"cdzl": lambda d, n: _zlib(d, n),
                    b"cdlz": lambda d, n: _lzma(d, n, frames * CD_SECTOR),
                    b"cdzs": lambda d, n: _zstd(d, n)}.get(tag)
            if tag == b"cdfl":
                return self._cdfl
            return lambda d, n, base=base: self._cd(base, d, n)
        name = tag.decode("latin-1", "replace")
        return lambda d, n: (_ for _ in ()).throw(DiscError(f"CHD: the codec '{name}' is not one a disc uses"))

    @staticmethod
    def _huff(data: bytes, destlen: int) -> bytes:
        if _accel_huff is not None:
            out = ctypes.create_string_buffer(destlen)
            if _accel_huff(data, len(data), out, destlen) != 0:
                raise DiscError("CHD: a damaged huffman hunk")
            return out.raw
        bits = Bits(data)
        tree = Huffman(256, 16)
        tree.import_huffman(bits)
        symbol, length = tree.symbol, tree.length
        s = bits.s + "0" * 16
        p, n = bits.p, bits.n
        out = bytearray(destlen)
        for i in range(destlen):
            w = int(s[p:p + 16], 2)
            out[i] = symbol[w]
            p += length[w]
        if p > n:
            raise DiscError("CHD: a huffman hunk ends early")
        return bytes(out)

    @staticmethod
    def _flac(data: bytes, destlen: int) -> bytes:
        if not data or data[0:1] not in (b"L", b"B"):
            raise DiscError("CHD: a damaged flac hunk")
        return flac_decode(data[1:], destlen // 4, data[0:1] == b"B")

    @staticmethod
    def _cd_split(data: bytes, destlen: int):
        frames = destlen // CD_FRAME
        cbytes = 2 if destlen < 65536 else 3
        ecc = (frames + 7) // 8
        header = ecc + cbytes
        base_len = int.from_bytes(data[ecc:ecc + cbytes], "big")
        return frames, data[:ecc], data[header:header + base_len]

    @staticmethod
    def _cd_frames(frames: int, eccmap: bytes, sectors: bytes) -> bytes:
        # The sectors at a frame's stride; the subcode is left as zeros (a disc's files do not use
        # it). A sector chdman took the sync and ECC off has its sync back; its ECC is not made
        # again (only the 2048 bytes of a sector's data are read from here).
        out = bytearray(frames * CD_FRAME)
        for f in range(frames):
            out[f * CD_FRAME:f * CD_FRAME + CD_SECTOR] = sectors[f * CD_SECTOR:(f + 1) * CD_SECTOR]
            if eccmap and eccmap[f >> 3] & (1 << (f & 7)):
                out[f * CD_FRAME:f * CD_FRAME + 12] = CD_SYNC
        return bytes(out)

    def _cd(self, base, data: bytes, destlen: int) -> bytes:
        frames, eccmap, payload = self._cd_split(data, destlen)
        return self._cd_frames(frames, eccmap, base(payload, frames * CD_SECTOR))

    def _cdfl(self, data: bytes, destlen: int) -> bytes:
        # (cdfl has no ECC map or length header: FLAC frames, then the subcode, deflated.)
        frames = destlen // CD_FRAME
        return self._cd_frames(frames, b"", flac_decode(data, frames * CD_SECTOR // 4, True))

    # ---- the map
    def _read_map(self):
        n = self.hunkcount
        self.kind = array.array("B", bytes(n))
        self.length = array.array("I", [0]) * n
        self.offset = array.array("Q", [0]) * n
        self.crc = array.array("H", [0]) * n
        f = self.f
        if not self.compressed:
            f.seek(self.mapoffset)
            raw = f.read(4 * n)
            for i, (off,) in enumerate(struct.iter_unpack(">I", raw)):
                self.kind[i] = 4 if off else 7  # 7: all zeros
                self.offset[i] = off * self.hunkbytes
            return
        f.seek(self.mapoffset)
        head = f.read(16)
        mapbytes = struct.unpack_from(">I", head, 0)[0]
        firstoffs = int.from_bytes(head[4:10], "big")
        mapcrc = struct.unpack_from(">H", head, 10)[0]
        lengthbits, selfbits, parentbits = head[12], head[13], head[14]
        bits = Bits(f.read(mapbytes))
        tree = Huffman(16, 8)
        tree.import_rle(bits)
        kind = self.kind
        # The hunks' kinds: a repeat code gives this hunk the last kind, and that many more after it.
        rep, last = 0, 0
        for i in range(n):
            if rep > 0:
                kind[i] = last
                rep -= 1
                continue
            val = tree.decode(bits)
            if val == 7:      # RLE small
                kind[i] = last
                rep = 2 + tree.decode(bits)
            elif val == 8:    # RLE large
                kind[i] = last
                rep = 2 + 16 + (tree.decode(bits) << 4)
                rep += tree.decode(bits)
            else:
                kind[i] = last = val
        cur = firstoffs
        last_self, last_parent = 0, 0
        raw = bytearray(12 * n)
        for i in range(n):
            k = kind[i]
            offset, length, crc = cur, 0, 0
            if k <= 3:
                length = bits.read(lengthbits)
                cur += length
                crc = bits.read(16)
            elif k == 4:
                length = self.hunkbytes
                cur += length
                crc = bits.read(16)
            elif k == 5:
                last_self = offset = bits.read(selfbits)
            elif k == 6:
                last_parent = offset = bits.read(parentbits)
            elif k in (9, 10):           # SELF_0, SELF_1
                if k == 10:
                    last_self += 1
                kind[i] = k = 5
                offset = last_self
            elif k == 11:                # PARENT_SELF
                kind[i] = k = 6
                last_parent = offset = i * self.hunkbytes // self.unitbytes
            elif k in (12, 13):          # PARENT_0, PARENT_1
                if k == 13:
                    last_parent += self.hunkbytes // self.unitbytes
                kind[i] = k = 6
                offset = last_parent
            self.length[i], self.offset[i], self.crc[i] = length, offset, crc
            raw[12 * i] = k
            raw[12 * i + 1:12 * i + 4] = length.to_bytes(3, "big")
            raw[12 * i + 4:12 * i + 10] = offset.to_bytes(6, "big")
            raw[12 * i + 10:12 * i + 12] = crc.to_bytes(2, "big")
        if bits.overflow() or binascii.crc_hqx(bytes(raw), 0xFFFF) != mapcrc:
            raise DiscError(f"{self.path.name}: its hunk map does not check out (a damaged CHD?)")

    # ---- hunks
    def hunk(self, n: int) -> bytes:
        cached = self._cache.get(n)
        if cached is not None:
            return cached
        if not 0 <= n < self.hunkcount:
            raise DiscError(f"CHD: hunk {n} is past the end")
        k = self.kind[n]
        hb = self.hunkbytes
        if k <= 3:
            self.f.seek(self.offset[n])
            codec = self._codecs[k]
            if codec is None:
                raise DiscError("CHD: a hunk names a codec the file does not have")
            data = codec(self.f.read(self.length[n]), hb)
            check = not self.is_cd   # (CD hunks come back without their subcode and ECC)
        elif k == 4:
            self.f.seek(self.offset[n])
            data = self.f.read(hb)
            check = self.compressed and not self.is_cd
        elif k == 5:
            data = self.hunk(self.offset[n])
            check = False
        elif k == 7:
            data = bytes(hb)
            check = False
        else:
            raise DiscError("CHD: a hunk that lives in a parent CHD")
        if len(data) != hb:
            raise DiscError(f"CHD: hunk {n} is short (a cut-off file?)")
        if check and binascii.crc_hqx(data, 0xFFFF) != self.crc[n]:
            raise DiscError(f"CHD: hunk {n} does not check out (a damaged file?)")
        if len(self._cache) >= 512:
            del self._cache[next(iter(self._cache))]
        self._cache[n] = data
        return data

    def read(self, offset: int, size: int) -> bytes:
        out = bytearray()
        while size > 0:
            n, inside = divmod(offset, self.hunkbytes)
            take = min(size, self.hunkbytes - inside)
            out += self.hunk(n)[inside:inside + take]
            offset += take
            size -= take
        return bytes(out)

    def tracks(self) -> list:
        """A CD's tracks: dicts of TRACK, TYPE, FRAMES, PREGAP, PGTYPE."""
        out = []
        for tag in (b"CHT2", b"CHTR"):
            for raw in self.meta.get(tag, []):
                text = raw.rstrip(b"\x00").decode("ascii", "replace")
                fields = dict(re.findall(r"(\w+):(\S+)", text))
                out.append({"track": int(fields.get("TRACK", "0")), "type": fields.get("TYPE", ""),
                            "frames": int(fields.get("FRAMES", "0")), "pregap": int(fields.get("PREGAP", "0")),
                            "pgtype": fields.get("PGTYPE", "")})
            if out:
                break
        return sorted(out, key=lambda t: t["track"])


# --------------------------------------------------------------------------------------------------
# Sources of 2048-byte sectors


class Source:
    name = ""
    kind = ""
    sectors = 0

    def read_sectors(self, lba: int, count: int) -> bytes:
        raise NotImplementedError

    def close(self):
        pass


class IsoSource(Source):
    def __init__(self, path: Path):
        self.f = open(path, "rb")
        self.name, self.kind = path.name, "ISO"
        self.sectors = os.path.getsize(path) // SECTOR

    def read_sectors(self, lba, count):
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    def close(self):
        self.f.close()


def _raw_user_offset(sector: bytes) -> int:
    if sector[:12] == CD_SYNC or sector[:12] == bytes(12):
        mode = sector[15]
        if mode == 1:
            return 16
        if mode == 2:
            return 24
    raise DiscError("not a data sector (mode 1 or mode 2) where the disc's files start")


class RawBinSource(Source):
    """A CD's raw sectors (2352 bytes each), mode 1 or mode 2 form 1."""

    def __init__(self, path: Path, label: str):
        self.f = open(path, "rb")
        self.name, self.kind = label, "CD (raw .bin)"
        self.sectors = os.path.getsize(path) // CD_SECTOR
        self.f.seek(16 * CD_SECTOR)
        self.user = _raw_user_offset(self.f.read(CD_SECTOR))

    def read_sectors(self, lba, count):
        self.f.seek(lba * CD_SECTOR)
        raw = self.f.read(count * CD_SECTOR)
        return b"".join(raw[i * CD_SECTOR + self.user:i * CD_SECTOR + self.user + SECTOR] for i in range(len(raw) // CD_SECTOR))

    def close(self):
        self.f.close()


class CsoSource(Source):
    def __init__(self, path: Path):
        self.f = open(path, "rb")
        head = self.f.read(24)
        magic, header_size, total, block, _version, align = struct.unpack("<4sIQIBB", head[:22])
        if magic == b"ZISO":
            raise DiscError(f"{path.name}: a ZSO (LZ4) image; convert it to .iso or .chd first")
        if magic != b"CISO" or block == 0:
            raise DiscError(f"{path.name}: not a CSO image")
        self.name, self.kind = path.name, "CSO"
        self.block, self.align, self.total = block, align, total
        blocks = (total + block - 1) // block
        self.f.seek(max(header_size, 24))
        self.index = array.array("I")
        self.index.frombytes(self.f.read(4 * (blocks + 1)))
        if sys.byteorder == "big":
            self.index.byteswap()
        self.sectors = total // SECTOR
        self._n, self._data = -1, b""

    def _blockdata(self, n):
        if n != self._n:
            a, b = self.index[n], self.index[n + 1]
            start = (a & 0x7FFFFFFF) << self.align
            end = (b & 0x7FFFFFFF) << self.align
            self.f.seek(start)
            raw = self.f.read(end - start)
            if a & 0x80000000:
                data = raw[:self.block]
            else:
                data = zlib.decompressobj(-15).decompress(raw, self.block)
            self._n, self._data = n, data
        return self._data

    def read_sectors(self, lba, count):
        out = bytearray()
        offset, size = lba * SECTOR, count * SECTOR
        while size > 0:
            n, inside = divmod(offset, self.block)
            take = min(size, self.block - inside)
            out += self._blockdata(n)[inside:inside + take]
            offset += take
            size -= take
        return bytes(out)

    def close(self):
        self.f.close()


class ChdSource(Source):
    def __init__(self, path: Path):
        self.chd = Chd(path)
        self.name = path.name
        c = self.chd
        if not c.is_cd:
            if c.unitbytes != SECTOR:
                raise DiscError(f"{path.name}: a CHD of {c.unitbytes}-byte units: not a disc image")
            self.kind = "DVD (CHD)"
            self.sectors = c.logicalbytes // SECTOR
            self.start, self.user = 0, 0
            return
        tracks = c.tracks()
        if not tracks:
            raise DiscError(f"{path.name}: a CD CHD without its track list")
        first = tracks[0]
        kind = first["type"]
        if kind == "AUDIO":
            raise DiscError(f"{path.name}: its first track is audio: not a PS2 disc")
        self.kind = f"CD (CHD, {kind})"
        self.start = first["pregap"] if first["pgtype"].startswith("V") else 0
        self.sectors = first["frames"] - self.start
        self.user = {"MODE1": 0, "MODE2_FORM1": 0, "MODE1_RAW": 16, "MODE2_RAW": 24, "MODE2": 8,
                     "MODE2_FORM_MIX": 8}.get(kind)
        if self.user is None:
            raise DiscError(f"{path.name}: a first track of type {kind}")
        if kind in ("MODE1_RAW", "MODE2_RAW"):
            frame = c.read((self.start + 16) * CD_FRAME, CD_SECTOR)
            self.user = _raw_user_offset(frame)

    def read_sectors(self, lba, count):
        c = self.chd
        if not c.is_cd:
            return c.read(lba * SECTOR, count * SECTOR)
        out = bytearray()
        for i in range(count):
            out += c.read((self.start + lba + i) * CD_FRAME + self.user, SECTOR)
        return bytes(out)

    def close(self):
        self.chd.f.close()


def _cue_bin(cue: Path) -> Path:
    text = cue.read_text(errors="replace")
    m = re.search(r'^\s*FILE\s+"?([^"\r\n]+?)"?\s+BINARY', text, re.M | re.I)
    if not m:
        raise DiscError(f"{cue.name}: names no BINARY file")
    if re.search(r"TRACK\s+01\s+AUDIO", text, re.I):
        raise DiscError(f"{cue.name}: its first track is audio: not a PS2 disc")
    path = cue.parent / m.group(1)
    if not path.is_file():
        raise DiscError(f"{cue.name}: names {m.group(1)}, which is not next to it")
    return path


def open_source(path: Path) -> Source:
    path = Path(path)
    with open(path, "rb") as f:
        head = f.read(16 * CD_SECTOR + 32)
    suffix = path.suffix.lower()
    if head[:8] == b"MComprHD":
        return ChdSource(path)
    if head[:4] in (b"CISO", b"ZISO"):
        return CsoSource(path)
    if suffix == ".cue":
        return RawBinSource(_cue_bin(path), path.name)
    if head[:12] == CD_SYNC and os.path.getsize(path) % CD_SECTOR == 0:
        return RawBinSource(path, path.name)
    if len(head) >= 16 * SECTOR + 6 and head[16 * SECTOR + 1:16 * SECTOR + 6] == b"CD001":
        return IsoSource(path)
    if suffix in (".gz", ".7z", ".zip", ".rar"):
        raise DiscError(f"{path.name}: an archive; unpack the disc image from it first")
    raise DiscError(f"{path.name}: not a disc image this knows (.chd, .iso, .bin/.cue, .cso)")


# --------------------------------------------------------------------------------------------------
# ISO 9660


class Iso9660:
    def __init__(self, source: Source):
        self.src = source
        pvd = source.read_sectors(16, 1)
        if pvd[1:6] != b"CD001" or pvd[0] != 1:
            raise DiscError(f"{source.name}: no ISO 9660 file system where a PS2 disc has one")
        root = pvd[156:156 + 34]
        self.root = (struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0])
        self.volume = pvd[40:72].decode("ascii", "replace").strip()

    def listdir(self, extent):
        lba, size = extent
        data = self.src.read_sectors(lba, (size + SECTOR - 1) // SECTOR)[:size]
        out, pos = [], 0
        while pos < len(data):
            length = data[pos]
            if length == 0:
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            rec = data[pos:pos + length]
            name_len = rec[32]
            raw = rec[33:33 + name_len]
            if raw not in (b"\x00", b"\x01"):
                name = raw.decode("latin-1").split(";")[0]
                if name.endswith(".") and "." in name[:-1]:
                    name = name[:-1]
                out.append((name, struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0], bool(rec[25] & 2)))
            pos += length
        return out

    def find(self, path: str):
        parts = [p for p in re.split(r"[\\/]+", path) if p]
        extent, is_dir = self.root, True
        for i, part in enumerate(parts):
            if not is_dir:
                return None
            want = part.split(";")[0].upper()
            for name, lba, size, d in self.listdir(extent):
                if name.upper() == want:
                    extent, is_dir = (lba, size), d
                    break
            else:
                return None
        return extent

    def read_file(self, path: str) -> bytes:
        found = self.find(path)
        if found is None:
            raise DiscError(f"{self.src.name}: no {path} on the disc")
        lba, size = found
        return self.src.read_sectors(lba, (size + SECTOR - 1) // SECTOR)[:size]


# --------------------------------------------------------------------------------------------------
# The game's executable


def parse_system_cnf(text: str) -> str:
    """The path BOOT2 names (SLUS_213.76, or a path inside the disc)."""
    m = re.search(r"^\s*BOOT2\s*=\s*cdrom0?:\\*([^;\r\n]+)", text, re.M | re.I)
    if not m:
        if re.search(r"^\s*BOOT\s*=", text, re.M | re.I):
            raise DiscError("a PlayStation 1 disc (its SYSTEM.CNF has BOOT, not BOOT2)")
        raise DiscError("its SYSTEM.CNF names no BOOT2 executable")
    return m.group(1).strip().replace("\\", "/")


class Disc:
    """A disc image, or a folder holding a disc's files."""

    def __init__(self, path):
        self.path = Path(path)
        self.folder = self.path if self.path.is_dir() else None
        self.source = None
        self.fs = None
        if self.folder is None:
            self.source = open_source(self.path)
            self.fs = Iso9660(self.source)

    def close(self):
        if self.source:
            self.source.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _folder_file(self, rel: str) -> Path:
        cur = self.folder
        for part in [p for p in re.split(r"[\\/]+", rel) if p]:
            want = part.split(";")[0].upper()
            match = next((c for c in cur.iterdir() if c.name.upper() == want), None)
            if match is None:
                raise DiscError(f"{self.folder}: no {rel} in it")
            cur = match
        return cur

    def read_file(self, rel: str) -> bytes:
        if self.folder is not None:
            return self._folder_file(rel).read_bytes()
        return self.fs.read_file(rel)

    def system_cnf(self) -> str:
        return self.read_file("SYSTEM.CNF").decode("latin-1")

    def boot(self):
        """(the executable's file name, its bytes)."""
        rel = parse_system_cnf(self.system_cnf())
        data = self.read_file(rel)
        if data[:4] != b"\x7fELF":
            raise DiscError(f"{rel} on the disc is not an ELF executable")
        return rel.split("/")[-1].split(";")[0], data

    def describe(self) -> str:
        if self.folder is not None:
            return f"a folder of the disc's files ({self.folder})"
        s = self.source
        return f"{s.kind}, {s.sectors} sectors ({s.sectors * SECTOR / 2**20:.0f} MiB), volume '{self.fs.volume}'"

    def write_iso(self, out: Path, progress=None):
        if self.source is None:
            raise DiscError("a folder of files is not a disc image: no .iso is made from it")
        total = self.source.sectors
        step = 256
        tmp = out.with_name(out.name + ".part")
        with open(tmp, "wb") as f:
            for lba in range(0, total, step):
                f.write(self.source.read_sectors(lba, min(step, total - lba)))
                if progress and (lba // step) % 64 == 0:
                    progress(lba, total)
        os.replace(tmp, out)
        if progress:
            progress(total, total)


def _progress(done, total):
    sys.stdout.write(f"\r  {done * SECTOR / 2**20:.0f} of {total * SECTOR / 2**20:.0f} MiB")
    sys.stdout.flush()
    if done >= total:
        sys.stdout.write("\n")


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="A PS2 disc image: its executable, or the disc as an .iso.")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info", help="what the disc is and which executable it boots")
    p.add_argument("disc", type=Path)
    p = sub.add_parser("exe", help="the executable SYSTEM.CNF's BOOT2 names, into a folder")
    p.add_argument("disc", type=Path)
    p.add_argument("--out", type=Path, default=Path("."))
    p = sub.add_parser("iso", help="the disc's 2048-byte sectors as an .iso")
    p.add_argument("disc", type=Path)
    p.add_argument("iso", type=Path)
    args = parser.parse_args(argv)
    try:
        with Disc(args.disc) as disc:
            if args.cmd == "info":
                name, data = disc.boot()
                print(f"{args.disc.name}: {disc.describe()}")
                print(f"  boots {name} ({len(data)} bytes)")
            elif args.cmd == "exe":
                name, data = disc.boot()
                args.out.mkdir(parents=True, exist_ok=True)
                (args.out / name).write_bytes(data)
                print(args.out / name)
            else:
                disc.write_iso(args.iso, _progress)
                print(args.iso)
    except (DiscError, OSError) as e:
        print(f"ps2disc: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
