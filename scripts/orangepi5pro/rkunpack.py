#!/usr/bin/env python3
"""Unpack a Rockchip RKFW update.img: the loader and every RKAF part."""
import os
import struct
import sys

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)

with open(src, 'rb') as f:
    hdr = f.read(0x66)
    assert hdr[:4] == b'RKFW', hdr[:4]
    chip = hdr[0x15:0x19]
    loader_off, loader_len, image_off, image_len = struct.unpack_from('<IIII', hdr, 0x19)
    print(f'chip={chip[::-1]} loader@{loader_off:#x}+{loader_len:#x} image@{image_off:#x}+{image_len:#x}')

    f.seek(loader_off)
    with open(os.path.join(out, 'MiniLoaderAll.bin'), 'wb') as o:
        o.write(f.read(loader_len))

    f.seek(image_off)
    af = f.read(0x800)
    assert af[:4] == b'RKAF', af[:4]
    length, = struct.unpack_from('<I', af, 4)
    model = af[8:8 + 0x22].rstrip(b'\0').decode(errors='replace')
    num_parts, = struct.unpack_from('<I', af, 0x88)
    print(f'RKAF length={length:#x} model={model!r} parts={num_parts}')
    for i in range(num_parts):
        base = 0x8c + i * 112
        name = af[base:base + 32].rstrip(b'\0').decode()
        filename = af[base + 32:base + 92].rstrip(b'\0').decode()
        nand_size, pos, nand_addr, padded, size = struct.unpack_from('<IIIII', af, base + 92)
        print(f'  {name:16} {filename:32} pos={pos:#010x} size={size:#010x} nand_addr={nand_addr:#010x}')
        if filename in ('SELF', 'RESERVED') or size == 0:
            continue
        f.seek(image_off + pos)
        data = f.read(size)
        dst = os.path.join(out, os.path.basename(filename))
        with open(dst, 'wb') as o:
            o.write(data)
