#!/usr/bin/env python3
# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
"""Generate synthetic Android-format native test vectors using Python/OpenSSL and SQLite.

Does not compile, connect to a device, or inspect any real key/credential file.
"""
import hashlib
import hmac
from pathlib import Path
import sqlite3
import struct
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def vectors():
    sp = b'0123456789abcdef' * 8  # Deliberately synthetic ASCII-hex SP, not a user's secret.
    context = b'android-synthetic-password-personalization-context'
    v2 = hashlib.sha512(b'fbe-key'.ljust(128, b'\0') + sp).digest()
    v3 = hmac.digest(sp, struct.pack('>I', 1) + b'fbe-key\0' + context +
                     struct.pack('>II', len(context)*8, 256), 'sha256')
    salt = bytes(range(16))
    pin = hashlib.scrypt(b'123456', salt=salt, n=512, r=8, p=2, dklen=32)
    pwd = struct.pack('>iBBB', 3, 9, 3, 1) + struct.pack('>I', 16) + salt + struct.pack('>I', 9) + b'\1' + bytes(range(8)) + struct.pack('>i', -1)
    with tempfile.TemporaryDirectory(prefix='recovery-crypto-vectors-') as temp:
        path = Path(temp) / 'fixture.db'
        db = sqlite3.connect(path)
        db.execute('PRAGMA page_size=512')
        db.execute('CREATE TABLE fixture(value TEXT)')
        db.commit()
        db.execute('PRAGMA journal_mode=WAL')
        db.execute('PRAGMA wal_autocheckpoint=0')
        original = path.read_bytes()
        db.execute("INSERT INTO fixture VALUES ('committed synthetic fixture')")
        db.commit()
        wal = Path(str(path) + '-wal').read_bytes()
        db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
        committed = path.read_bytes()
        db.close()
    # Compare page content; checkpoint alone changes page-1 bookkeeping which isn't
    # always written to WAL. Use the WAL frames to construct its committed image.
    page = struct.unpack('>I', wal[8:12])[0]
    image = bytearray(original)
    commits = [(p, struct.unpack('>I', wal[p+4:p+8])[0])
               for p in range(32, len(wal), page+24)]
    end, count = next((p, count) for p, count in reversed(commits) if count)
    image.extend(b'\0' * max(0, count*page-len(image)))
    del image[count*page:]
    for p in range(32, end+1, page+24):
        number = struct.unpack('>I', wal[p:p+4])[0]
        if number <= count:
            image[(number-1)*page:number*page] = wal[p+24:p+24+page]
    # SQLite itself verifies the reconstructed database's committed contents.
    with sqlite3.connect(':memory:') as verify:
        normalized = bytearray(image)
        normalized[18:20] = b'\1\1'
        verify.deserialize(bytes(normalized))
        assert verify.execute('SELECT value FROM fixture').fetchall() == [('committed synthetic fixture',)]
    return {'SyntheticPassword': sp.decode(), 'FbeV2': v2.hex(), 'FbeV3': v3.hex(),
            'StretchedPin': pin.hex(), 'PasswordData': pwd.hex(), 'Database': original.hex(),
            'Wal': wal.hex(), 'CommittedDatabase': bytes(image).hex()}


def generate():
    destination = ROOT / 'crypto/android17/tests/vectors.h'
    lines = ['/*', ' * SPDX-FileCopyrightText: The uwuAOSP Project',
             ' * SPDX-License-Identifier: Apache-2.0', ' */',
             '// Synthetic fixtures; regenerate with tools/crypto/generate_vectors.py.']
    for name, value in vectors().items():
        lines.append(f'inline constexpr const char* k{name}=')
        lines.extend('    "' + value[i:i+96] + '"' for i in range(0, len(value), 96))
        lines[-1] += ';'
    destination.write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
    print('Synthetic reference vectors generated; SQLite reconstruction checked. No native test or build ran.')


if __name__ == '__main__':
    generate()
