"""Builds the Steam Workshop package: Main.gml with both DLLs embedded, fmConfig.json, the marker and the license.

Usage: python mods/ForagerCoop/package.py <version.dll> <catch_error.dll> <version> <out dir>
Writes <out dir>/ForagerCoop/ and <out dir>/ForagerCoop-Workshop-<version>.zip (the folder inside the zip goes
into %LOCALAPPDATA%\\Forager\\mods\\ before uploading from the game's MODDING menu).
"""
import hashlib
import json
import struct
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MOD_NAME = 'ForagerCoop'
CHUNK_DIGITS = 600  # 60 words per GML string literal


def padded(data: bytes) -> bytes:
    """Zero-padded to whole u32 words (trailing zeros after a PE image are ignored)."""
    return data + b'\0' * (-len(data) % 4)


def chunks(data: bytes) -> str:
    """The bytes as a GML array of strings: each little-endian u32 as 10 decimal digits."""
    digits = ''.join(f'{word:010d}' for (word,) in struct.iter_unpack('<I', data))
    return '[' + ','.join(json.dumps(digits[i:i + CHUNK_DIGITS]) for i in range(0, len(digits), CHUNK_DIGITS)) + ']'


def license_text() -> str:
    minhook = (ROOT / 'native/third_party/minhook/LICENSE.txt').read_text(encoding='utf-8-sig')
    return (ROOT / 'LICENSE').read_text(encoding='utf-8') + '\n\nMinHook (built into the co-op DLL):\n' + minhook


def main(payload_path: str, bridge_path: str, version: str, out: str) -> None:
    payload = padded(Path(payload_path).read_bytes())
    bridge = padded(Path(bridge_path).read_bytes())
    gml = (HERE / 'Main.gml.in').read_text(encoding='utf-8')
    for key, value in (('VERSION', version), ('HASH', hashlib.sha256(payload).hexdigest()[:16]),
                       ('PAYLOAD_SIZE', str(len(payload))), ('BRIDGE_SIZE', str(len(bridge))),
                       ('PAYLOAD', chunks(payload)), ('BRIDGE', chunks(bridge))):
        gml = gml.replace(f'@{key}@', value)
    if '@' in gml:
        sys.exit('workshop/Main.gml.in has an unknown @placeholder@')

    mod = Path(out) / MOD_NAME
    mod.mkdir(parents=True, exist_ok=True)
    files = {
        'Main.gml': gml,
        'fmConfig.json': json.dumps({'name': MOD_NAME, 'author': 'melogo5', 'targetAPIVersion': '1.0.1',
                                     'dependencies': []}, indent=2) + '\n',
        'forager-coop.marker': f'ForagerCoop {version}\n',  # mods/ForagerCoop/bridge.c looks for this file
        'LICENSE.txt': license_text(),
    }
    archive = Path(out) / f'ForagerCoop-Workshop-{version}.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as zf:
        for name, text in files.items():
            (mod / name).write_text(text, encoding='utf-8', newline='\n')
            zf.write(mod / name, f'{MOD_NAME}/{name}')
    print(f'{archive}: payload {len(payload)} bytes, bridge {len(bridge)} bytes')


if __name__ == '__main__':
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
