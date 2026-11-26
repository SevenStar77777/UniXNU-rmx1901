#!/usr/bin/env python3
"""Compile the RMX1901 SHNGROOT window publisher against the actual XNU ABI.

The compile recipe is copied from the source kernel's IOPlatformExpert.cpo.json,
the same way scripts/build-rmx1901-probes.py builds the other board drivers.
The archive is force-loaded by rmx1901_defconfig so the IOKit personality and
the root authorization hooks survive the kernel link.
"""
import json
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'package/Rmx1901WindowPublisher.cpp'
OUT = ROOT / 'build/rmx1901-windows'
RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

OUT.mkdir(parents=True, exist_ok=True)
record = json.loads(RECORD.read_text().strip().rstrip(','))
destination = OUT / 'Rmx1901WindowPublisher.o'
command, index = [], 0
arguments = record['arguments']
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(SOURCE))
    elif value == '-o':
        command.extend(['-o', str(destination)])
        index += 1
    elif value in ('-MF', '-MT', '-MJ'):
        index += 1
    elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend([
    '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
    '-Wno-c99-designator', '-Wno-reorder-init-list',
    '-Wno-deprecated-enum-enum-conversion',
    '-Wno-unnecessary-virtual-specifier',
    '-Wno-unknown-warning-option',
    '-Wno-missing-designated-field-initializers',
    '-I', str(ROOT / '.cache/xnu-work/osfmk'),
])
storage_abi = ROOT / 'build/storage-abi/include'
if storage_abi.is_dir():
    command.extend(['-I', str(storage_abi)])
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True)
(OUT / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-3500:])

data = destination.read_bytes()
magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
    raise SystemExit('Unexpected window publisher object architecture')
offset = 32
deployment = None
for _ in range(count):
    cmd, length = struct.unpack_from('<II', data, offset)
    if cmd == 0x32:
        deployment = struct.unpack_from('<II', data, offset + 8)
    offset += length
if deployment != (2, 0x110000):
    raise SystemExit(f'Unexpected window publisher deployment {deployment}')

archive = OUT / 'libRmx1901Windows.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(destination)], check=True)
print('rmx1901 windows: -Werror build and actual ARM64/iOS 17 object audit passed')
