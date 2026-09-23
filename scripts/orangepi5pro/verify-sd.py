#!/usr/bin/env python3
"""Read back every data region of a sparse image from the drive and compare.

    verify-sd.py <image> <device, e.g. /dev/sda>
"""
import hashlib
import os
import sys

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

image, device = sys.argv[1], sys.argv[2]
bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
block_path = f'/org/freedesktop/UDisks2/block_devices/{os.path.basename(device)}'
result, fds = bus.call_with_unix_fd_list_sync(
    'org.freedesktop.UDisks2', block_path, 'org.freedesktop.UDisks2.Block', 'OpenForBackup',
    GLib.Variant('(a{sv})', ({},)), GLib.VariantType('(h)'),
    Gio.DBusCallFlags.ALLOW_INTERACTIVE_AUTHORIZATION, 600000, None, None)
dev = fds.get(result.unpack()[0])
# Don't let the page cache answer for the card
os.posix_fadvise(dev, 0, 0, os.POSIX_FADV_DONTNEED)

src = os.open(image, os.O_RDONLY)
pos = 0
bad = 0
checked = 0
while True:
    try:
        start = os.lseek(src, pos, os.SEEK_DATA)
    except OSError:
        break
    end = os.lseek(src, start, os.SEEK_HOLE)
    off = start
    while off < end:
        n = min(8 << 20, end - off)
        a = os.pread(src, n, off)
        b = os.pread(dev, n, off)
        if hashlib.sha1(a).digest() != hashlib.sha1(b).digest():
            print(f'MISMATCH at byte {off:#x} (+{n:#x})', flush=True)
            bad += 1
        off += n
        checked += n
    pos = end
print(f'checked {checked / 2**30:.2f} GiB, {"all identical" if not bad else f"{bad} bad chunks"}')
sys.exit(1 if bad else 0)
