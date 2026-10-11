#!/usr/bin/env python3
# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
"""Preview/apply reviewed Recovery MTP transport patches. Never builds/flashes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import tempfile

PAYLOAD = Path(__file__).resolve().parents[2] / 'mtp/platform'
PATCHES = {'frameworks/av': 'frameworks-av.patch', 'system/sepolicy': 'sepolicy.patch'}
LEGACY_PATCHES = {
    'frameworks/av': ('frameworks-av-writable-v2.patch', 'frameworks-av-readonly-v1.patch'),
    'system/sepolicy': ('sepolicy-writable-v2.patch', 'sepolicy-readonly-v1.patch'),
}


def trusted_path(root, relative):
    if Path(relative).is_absolute() or '..' in Path(relative).parts:
        raise ValueError('Invalid managed path: ' + relative)
    target = root / relative
    for part in (target, *target.parents):
        if part == root:
            break
        if part.is_symlink():
            raise ValueError('Symlink in managed path: ' + relative)
    if not target.resolve().is_relative_to(root.resolve()):
        raise ValueError('Path outside source tree: ' + relative)
    return target


def git(repository, *args):
    return subprocess.run(['git', '-c', 'core.autocrlf=false', '-C', str(repository), *args],
                          capture_output=True, text=True, timeout=30)


def plan(root):
    review = json.loads((PAYLOAD / 'source-review.json').read_text(encoding='utf-8'))
    result = []
    for relative, filename in PATCHES.items():
        repository = trusted_path(root, relative)
        if git(repository, 'rev-parse', '--show-toplevel').returncode:
            raise ValueError('Missing Git checkout: ' + relative)
        patch = PAYLOAD / filename
        # Validate every managed file, including already-applied deployments.
        for file in review[relative]['sha256']:
            path = trusted_path(root, relative + '/' + file)
            if not path.is_file():
                raise ValueError('Missing reviewed source: ' + str(path))
        originals = {file: trusted_path(root, relative + '/' + file).read_bytes()
                     for file in review[relative]['sha256']}
        if git(repository, 'apply', '--reverse', '--check', str(patch)).returncode == 0:
            result.append((relative, {}, False))
            continue
        matches = [PAYLOAD / name for name in LEGACY_PATCHES[relative]
                   if git(repository, 'apply', '--reverse', '--check', str(PAYLOAD / name)).returncode == 0]
        if len(matches) > 1:
            raise ValueError('Ambiguous Recovery MTP deployment: ' + relative)
        legacy = matches[0] if matches else None
        upgrade = legacy is not None
        if not upgrade and any(b'RECOVERY_MTP' in data for data in originals.values()):
            raise ValueError('Partial/modified Recovery MTP deployment: ' + relative)
        # Stage the exact reviewed old -> new transition. Never unpatch the live
        # checkout during preview/preflight; unrelated downstream edits survive.
        with tempfile.TemporaryDirectory(prefix='recovery-mtp-review-') as directory:
            stage = Path(directory)
            for file, data in originals.items():
                target = stage / file
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            if upgrade:
                operation = git(stage, 'apply', '--reverse', str(legacy))
                if operation.returncode:
                    raise ValueError('Cannot stage previous MTP upgrade: ' + operation.stderr)
            check = git(stage, 'apply', '--check', str(patch))
            if check.returncode:
                raise ValueError('Recovery MTP patch conflicts in ' + relative + ': ' + check.stderr.strip())
            operation = git(stage, 'apply', str(patch))
            if operation.returncode:
                raise ValueError('Cannot stage Recovery MTP patch: ' + operation.stderr)
            changes = {file: (before, (stage / file).read_bytes())
                       for file, before in originals.items() if before != (stage / file).read_bytes()}
        result.append((relative, changes, bool(changes)))
    return result


def apply(root, changes):
    pending = [item for item in changes if item[2]]
    if not pending:
        return
    backup = root.parent / (root.name + '-backups') / ('recovery-mtp-' + str(time.time_ns()))
    originals = {}
    for relative, changes, _ in pending:
        for file, (before, _) in changes.items():
            key = relative + '/' + file
            originals[key] = before
            target = backup / key
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(originals[key])
    (backup / 'source-hashes.json').write_text(json.dumps({
        p: hashlib.sha256(data).hexdigest() for p, data in originals.items()
    }, indent=2) + '\n', encoding='utf-8')
    # Recheck every repository before the first mutation; preserve all unrelated edits.
    for relative, changes, _ in pending:
        for file, (before, _) in changes.items():
            if trusted_path(root, relative + '/' + file).read_bytes() != before:
                raise ValueError('Source changed during preflight: ' + relative)
    for relative, changes, _ in pending:
        for file, (_, after) in changes.items():
            trusted_path(root, relative + '/' + file).write_bytes(after)
    print('Backup: ' + str(backup))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source_root', type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--apply', action='store_true')
    mode.add_argument('--check', action='store_true')
    args = parser.parse_args()
    root = args.source_root.resolve()
    changes = plan(root)
    if args.check:
        pending = [relative for relative, _, needed in changes if needed]
        if pending:
            raise ValueError('Recovery MTP platform changes not installed: ' + ', '.join(pending))
        print('Verified Recovery upload/delete/rename transport and USB policy inputs. No build/device validation.')
    elif args.apply:
        apply(root, changes)
        plan(root)
        print('Recovery MTP platform inputs prepared. Device opt-in still required. No build or phone operation ran.')
    else:
        import difflib
        for relative, files, needed in changes:
            if needed:
                print('Repository: ' + relative)
                for file, (before, after) in files.items():
                    print(''.join(difflib.unified_diff(before.decode().splitlines(True), after.decode().splitlines(True),
                                                     fromfile=file, tofile=file)), end='')
        print('Preview only. No files changed; MTP remains opt-in.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        raise SystemExit('ERROR: ' + str(error))
