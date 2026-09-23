#!/usr/bin/env python3
"""Build the SD card IDB (DDR init + SPL at sector 64) from a Rockchip RK3588
loader (MiniLoaderAll.bin, boot_merger format) and check it against its hashes.

    rkloader.py <MiniLoaderAll.bin> <idblock.bin>
"""
import hashlib
import struct
import sys

RC4_KEY = bytes([124, 78, 3, 4, 85, 5, 9, 7, 45, 44, 123, 56, 23, 13, 23, 17])


def rc4(data):
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + RC4_KEY[i % len(RC4_KEY)]) & 0xff
        s[i], s[j] = s[j], s[i]
    out = bytearray(len(data))
    i = j = 0
    for n, b in enumerate(data):
        i = (i + 1) & 0xff
        j = (j + s[i]) & 0xff
        s[i], s[j] = s[j], s[i]
        out[n] = b ^ s[(s[i] + s[j]) & 0xff]
    return bytes(out)


def descramble(data):
    # Each 512 byte sector is scrambled on its own
    return b''.join(rc4(data[i:i + 512]) for i in range(0, len(data), 512))


src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
assert data[:4] in (b'LDR ', b'BOOT'), data[:4]
num, eoff, esize = struct.unpack_from('<BIB', data, 0x25)
entries = {}
for i in range(num):
    e = data[eoff + i * esize: eoff + (i + 1) * esize]
    name = e[5:45].decode('utf-16-le').rstrip('\0')
    doff, dsize = struct.unpack_from('<II', e, 45)
    entries[name] = descramble(data[doff:doff + dsize])

head, ddr, spl = entries['FlashHead'], entries['FlashData'], entries['FlashBoot']
assert head[:4] == b'RKNS', 'FlashHead is not an RK3588 (RKNS) IDB header'
for i, blob in enumerate((ddr, spl)):
    base = 120 + i * 88
    size_and_off, = struct.unpack_from('<I', head, base)
    sectors = size_and_off >> 16
    assert hashlib.sha256(blob[:sectors * 512]).digest() == head[base + 24:base + 56], f'image {i} hash mismatch'
assert hashlib.sha256(head[:0x600]).digest() == head[0x600:0x620], 'header hash mismatch'
open(dst, 'wb').write(head + ddr + spl)
print(f'{dst}: {len(head + ddr + spl) // 512} sectors, hashes verified')
