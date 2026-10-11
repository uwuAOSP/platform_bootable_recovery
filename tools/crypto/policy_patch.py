#!/usr/bin/env python3
# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
"""Plan the explicit reviewed SELinux patch in isolation. Never compiles/writes Android."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

PATCH_DIR = Path(__file__).resolve().parent / 'patches'
FILES = ('private/vold.te', 'private/keystore.te', 'public/te_macros')
REVIEWS = ('android17-recovery-key-access', 'android17-uwu-recovery-key-access')


def plan(source_root):
    patches = []
    for review in REVIEWS:
        manifest = json.loads((PATCH_DIR / (review + '.json')).read_text())
        patch = PATCH_DIR / (review + '.patch')
        if (manifest.get('schema') != 1 or manifest.get('patch') != patch.name or
                set(manifest.get('files', {})) != set(FILES) or
                hashlib.sha256(patch.read_bytes()).hexdigest() != manifest.get('sha256')):
            raise ValueError('Recovery crypto policy patch/review differs; review before deployment: ' + review)
        patches.append(patch)
    root = source_root.resolve() / 'system/sepolicy'
    originals = {}
    for relative in FILES:
        target = root / relative
        if not target.is_file() or any(p.is_symlink() for p in (target, *target.parents)):
            raise ValueError('Missing or symlinked policy source: ' + relative)
        originals[relative] = target.read_bytes()

    # Git sees only copies, outside any checkout. The caller owns backup/write
    # transaction handling; --check never mutates the real Android source tree.
    env = {k: v for k, v in os.environ.items()
           if k not in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE')}
    with tempfile.TemporaryDirectory(prefix='recovery-policy-review-') as directory:
        stage = Path(directory)
        for relative, data in originals.items():
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)

        def git_apply(patch, *options):
            try:
                return subprocess.run(['git', '-c', 'core.autocrlf=false', 'apply',
                                       '--whitespace=error-all', *options, str(patch)],
                                      cwd=stage, env=env, capture_output=True, timeout=30)
            except subprocess.TimeoutExpired as error:
                raise ValueError('Recovery crypto policy patch check timed out; no source changed') from error

        # Only complete reviewed variants may match. Do not combine successful
        # hunks from different patches or invent exceptions for unknown rules.
        matches, errors = [], []
        for patch in patches:
            forward = git_apply(patch, '--check')
            reverse = git_apply(patch, '--reverse', '--check')
            if not forward.returncode:
                matches.append((patch, False))
            if not reverse.returncode:
                matches.append((patch, True))
            errors.append(patch.name + ': ' + forward.stderr.decode(errors='replace')[:500])
        if len(matches) != 1:
            raise ValueError('Recovery crypto policy patch conflicts; review source before deployment: ' +
                             ('ambiguous reviewed variants' if matches else '\n'.join(errors)))
        patch, already_applied = matches[0]
        if already_applied:
            return {}
        applied = git_apply(patch)
        if applied.returncode:
            raise ValueError('Cannot stage Recovery crypto policy patch: ' +
                             applied.stderr.decode(errors='replace')[:1000])
        return {'system/sepolicy/' + relative: (data.decode(), (stage / relative).read_text())
                for relative, data in originals.items() if (stage / relative).read_bytes() != data}
