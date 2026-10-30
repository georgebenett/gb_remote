"""Pack a gb_receiver app image for the rx_fw partition (layout in main/rx_ota.c).

usage: pack_rx_fw.py gb_receiver_vX.Y.Z.bin rx_fw.bin
"""
import struct
import sys
import zlib

src, dst = sys.argv[1:]
raw = open(src, 'rb').read()
# Image header (24) + first segment header (8), then the app descriptor.
assert raw[0] == 0xE9 and raw[32:36] == struct.pack('<I', 0xABCD5432), 'not an app image'
version = raw[48:80]
# bundle_to_remotes.sh names the file from version.h; a stale build would
# carry an older version inside, and the remote compares that one.
name_ver = src.rsplit('_v', 1)[-1].rsplit('.bin', 1)[0]
inner_ver = version.split(b'\0')[0].decode()
assert name_ver == inner_ver, f'{src} carries version {inner_ver}: rebuild gb_receiver'
# 8 KB window: the receiver inflates into an 8 KB ring (OTA_WINDOW).
z = zlib.compressobj(9, zlib.DEFLATED, 13)
data = z.compress(raw) + z.flush()
open(dst, 'wb').write(struct.pack('<4sII32s', b'GBZ1', len(raw), len(data), version) + data)
print(f'rx_fw: {len(raw)} -> {len(data)} bytes ({100 * len(data) // len(raw)}%)')
