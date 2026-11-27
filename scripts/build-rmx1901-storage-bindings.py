#!/usr/bin/env python3
"""Bind Rmx1901WindowPublisher to the original IOStorageFamily symbols of the pinned kernel.

IOBlockStorageDevice lives in the prelinked IOStorageFamily kext, so the native
kernel link cannot resolve its constructor, destructor, metaclass, vtable or
inherited methods. The recorded absolute symbols in
scripts/data/native-storage-original-bindings.S were derived from the same
iPadOS 17.7.11 21H461 kernelcache this board pins and already apply the
profile's -0x1000000 collection slide. This recipe re-archives the driver
object with that bindings object, the same way the proven gtaxllte driver does.
"""
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path.cwd()  # the UniXNU tree: archives run with cwd=tree
DEVICE = Path(os.environ.get('UNIXNU_DEVICE', ROOT / 'kits/rmx1901'))
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np  # noqa: E402

out = (ROOT / 'build/rmx1901-windows').resolve()
archive = out / 'libRmx1901Windows.a'
obj_path = out / 'Rmx1901WindowPublisher.o'
recorded = DEVICE / 'scripts/data/native-storage-original-bindings.S'
if not recorded.is_file():
    recorded = ROOT / 'scripts/data/native-storage-original-bindings.S'
if not obj_path.is_file():
    raise SystemExit(f'{obj_path} not found; build the driver first')
if not recorded.is_file():
    raise SystemExit(f'{recorded} is missing')

data = obj_path.read_bytes()
image = np.macho(data)
offset = next(at for command, at, size in image['commands'] if command == 2)
symoff, count, stroff, _ = struct.unpack_from('<4I', data, offset + 8)
symbols = []
for index in range(count):
    strx, kind, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
    symbols.append((data[stroff + strx:data.index(b'\0', stroff + strx)].decode(), value, kind))
imports = sorted({name for name, value, kind in symbols if kind == 1 and
                  ('IOBlockStorageDevice' in name or 'IOMedia' in name or 'IOStorage' in name)})

values = {}
for line in recorded.read_text().splitlines():
    match = re.match(r'\.set (\S+), (0x[0-9a-f]+)$', line.strip())
    if match:
        values[match.group(1)] = int(match.group(2), 16)
missing = set(imports) - set(values)
if missing:
    raise SystemExit('The recorded bindings do not cover: ' + ', '.join(sorted(missing)))
bindings = {name: values[name] for name in imports}
source = out / 'original-bindings.S'
source.write_text(''.join(f'.globl {name}\n.set {name}, {value:#x}\n'
                          for name, value in sorted(bindings.items())))
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
                '-Wno-incompatible-sysroot', '-c', source, '-o', out / 'original-bindings.o'],
               check=True)
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj_path, out / 'original-bindings.o'], check=True)
nm = subprocess.run(['xcrun', 'llvm-nm', '-g', str(archive)], capture_output=True, text=True, check=True)
missing_after = [name for name in bindings if name not in nm.stdout]
if missing_after:
    raise SystemExit('binding symbols missing from the archive: ' + ', '.join(sorted(missing_after)))
print('bound absolute symbols:', ', '.join(sorted(bindings)))
(out / 'bindings.json').write_text(json.dumps(dict(
    component='Rmx1901WindowPublisher', archive=archive.name,
    provenance='scripts/data/native-storage-original-bindings.S',
    compiled_object_sha256=hashlib.sha256(data).hexdigest(),
    bindings={name: hex(value) for name, value in sorted(bindings.items())},
    original_code_modified=False, actual_io_qualified=False), indent=2) + '\n')
print(f'PASS: Rmx1901WindowPublisher bound to {len(bindings)} original IOStorageFamily symbols')
