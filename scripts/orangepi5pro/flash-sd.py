#!/usr/bin/env python3
"""Write a (sparse) disk image to a removable drive through UDisks, so no sudo is needed:
the desktop asks for the password instead. Only the image's data regions are written.

    flash-sd.py <image> <device, e.g. /dev/sda>
"""
import os
import sys
import time

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

image, device = sys.argv[1], sys.argv[2]
name = os.path.basename(device)
bus = Gio.bus_get_sync(Gio.BusType.SYSTEM)
UD = 'org.freedesktop.UDisks2'
block_path = f'/org/freedesktop/UDisks2/block_devices/{name}'


def prop(path, iface, key):
    return bus.call_sync(UD, path, 'org.freedesktop.DBus.Properties', 'Get',
                         GLib.Variant('(ss)', (iface, key)), GLib.VariantType('(v)'),
                         Gio.DBusCallFlags.NONE, -1, None).unpack()[0]


# Refuse anything but a removable drive exactly the size of the image
size = prop(block_path, UD + '.Block', 'Size')
drive = prop(block_path, UD + '.Block', 'Drive')
removable = prop(drive, UD + '.Drive', 'Removable')
model = prop(drive, UD + '.Drive', 'Model')
image_size = os.stat(image).st_size
print(f'{device}: {model!r}, {size} bytes, removable={removable}')
if not removable or prop(block_path, UD + '.Block', 'HintSystem'):
    sys.exit('not a removable drive, refusing')
if size != image_size:
    sys.exit(f'drive size {size} != image size {image_size}, refusing')

# Unmount anything the desktop mounted from the card
for i in range(1, 64):
    part = f'{block_path}{i}'
    try:
        mounts = prop(part, UD + '.Filesystem', 'MountPoints')
    except GLib.Error:
        continue
    if mounts:
        bus.call_sync(UD, part, UD + '.Filesystem', 'Unmount', GLib.Variant('(a{sv})', ({},)),
                      None, Gio.DBusCallFlags.NONE, -1, None)
        print(f'unmounted {part}')

print('Opening the drive for writing (approve the password prompt on the desktop)...')
result, fds = bus.call_with_unix_fd_list_sync(
    UD, block_path, UD + '.Block', 'OpenForRestore', GLib.Variant('(a{sv})', ({},)),
    GLib.VariantType('(h)'), Gio.DBusCallFlags.ALLOW_INTERACTIVE_AUTHORIZATION,
    600000, None, None)
out = fds.get(result.unpack()[0])

src = os.open(image, os.O_RDONLY)
extents = []
pos = 0
while True:
    try:
        start = os.lseek(src, pos, os.SEEK_DATA)
    except OSError:
        break
    end = os.lseek(src, start, os.SEEK_HOLE)
    extents.append((start, end))
    pos = end
total = sum(e - s for s, e in extents)
print(f'writing {total / 2**30:.2f} GiB in {len(extents)} regions')

done = 0
t0 = last = time.time()
chunk = 8 << 20
for start, end in extents:
    off = start
    while off < end:
        n = min(chunk, end - off)
        buf = os.pread(src, n, off)
        view = memoryview(buf)
        while view:
            w = os.pwrite(out, view, off)
            view = view[w:]
            off += w
        done += n
        if time.time() - last > 5:
            last = time.time()
            rate = done / (last - t0) / 2**20
            print(f'  {done / total * 100:5.1f}%  {rate:.1f} MiB/s', flush=True)
print('flushing to the card...', flush=True)
os.fsync(out)
os.close(out)
print(f'done in {time.time() - t0:.0f}s')
