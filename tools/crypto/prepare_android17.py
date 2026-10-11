#!/usr/bin/env python3
# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
"""Prepare reviewed Android 17 Recovery dependencies; never builds or touches a phone."""
import argparse
import difflib
import importlib.util
from pathlib import Path
import re
import time

INTERFACES = {
    'security/keymint': 'android.hardware.security.keymint',
    'security/secureclock': 'android.hardware.security.secureclock',
    'security/sharedsecret': 'android.hardware.security.sharedsecret',
    'gatekeeper': 'android.hardware.gatekeeper',
    'weaver': 'android.hardware.weaver',
}
# AIDL propagates recovery_available to its generated C++ analyzers too.
# Review the complete runtime declaration, not the unrelated compiler modules.
ANALYZER_BLOCK = '''cc_library_static {
    name: "aidl-analyzer-main",
    host_supported: true,
    vendor_available: true,
    shared_libs: [
        "libbase",
        "libbinder",
    ],
    srcs: [
        "analyzer/analyzerMain.cpp",
        "analyzer/Analyzer.cpp",
    ],
    export_include_dirs: ["analyzer/include"],
}'''
SQLITE_BLOCK = '''\n// BEGIN recovery-crypto Android 17 SQLite (unlock-only backend)
cc_library_static {
    name: "librecovery_crypto_sqlite",
    defaults: ["sqlite-minimal-defaults", "release_package_libsqlite3_library_defaults"],
    recovery_available: true,
    host_supported: true,
    cflags: ["-DSQLITE_OMIT_LOAD_EXTENSION", "-DSQLITE_TEMP_STORE=3"],
    visibility: ["//visibility:public"],
}
// END recovery-crypto Android 17 SQLite
'''


def module_span(source, name, module_type):
    pattern = r'(' + re.escape(module_type) + r'\s*\{\s*name:\s*"' + re.escape(name) + r'",)'
    matches = list(re.finditer(pattern, source))
    if len(matches) != 1:
        raise ValueError(f'Unknown module layout: {name}')
    start = matches[0].end()
    # Interface may contain nested backend/version blocks; find the entire balanced block.
    begin = source.rfind('{', 0, start)
    depth, end, quoted, escaped, comment = 1, begin + 1, False, False, False
    while end < len(source) and depth:
        ch = source[end]
        if comment:
            if ch == '\n':
                comment = False
        elif quoted:
            if escaped:
                escaped = False
            elif ch == '\\':
                escaped = True
            elif ch == '"':
                quoted = False
        elif source[end:end+2] == '//':
            comment = True
        elif ch == '"':
            quoted = True
        elif ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        end += 1
    if depth:
        raise ValueError('Unbalanced module')
    return begin, start, end


def add_recovery_variant(source, name, module_type='aidl_interface'):
    begin, start, end = module_span(source, name, module_type)
    body = source[begin:end]
    existing = re.findall(r'\brecovery_available:\s*(true|false)', body)
    if existing:
        if existing != ['true']:
            raise ValueError(f'Refusing to override recovery_available in {name}')
        return source
    return source[:start] + '\n    recovery_available: true,' + source[start:]


def prepare_analyzer(source):
    name = 'aidl-analyzer-main'
    begin, _, end = module_span(source, name, 'cc_library_static')
    body = source[begin:end]
    # The span starts at the opening brace; accept only the reviewed module,
    # with whitespace differences and our additive Recovery declaration.
    original = re.sub(r'\brecovery_available:\s*true\s*,', '', body)
    expected = ANALYZER_BLOCK[ANALYZER_BLOCK.index('{'):]
    if re.sub(r'\s+', '', original) != re.sub(r'\s+', '', expected):
        raise ValueError('AIDL analyzer runtime needs review before adding a Recovery variant')
    return add_recovery_variant(source, name, 'cc_library_static')


def policy_plan(source_root):
    path = Path(__file__).with_name('policy_patch.py')
    spec = importlib.util.spec_from_file_location('recovery_crypto_policy_patch', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.plan(source_root)


def plan(source_root):
    changes = {}
    for path, name in INTERFACES.items():
        relative = f'hardware/interfaces/{path}/aidl/Android.bp'
        before = (source_root / relative).read_text(encoding='utf-8')
        after = add_recovery_variant(before, name)
        if after != before:
            changes[relative] = (before, after)
    relative = 'system/tools/aidl/Android.bp'
    before = (source_root / relative).read_text(encoding='utf-8')
    after = prepare_analyzer(before)
    if after != before:
        changes[relative] = (before, after)
    relative = 'external/sqlite/dist/Android.bp'
    before = (source_root / relative).read_text(encoding='utf-8')
    if 'name: "librecovery_crypto_sqlite"' in before:
        if SQLITE_BLOCK not in before:
            raise ValueError('Existing SQLite adapter differs from the reviewed block')
    else:
        if not all(token in before for token in ('name: "sqlite-minimal-defaults"', 'name: "release_package_libsqlite3_library_defaults"')):
            raise ValueError('Unknown SQLite defaults')
        changes[relative] = (before, before + SQLITE_BLOCK)
    changes.update(policy_plan(source_root))
    return changes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source_root', type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--apply', action='store_true')
    mode.add_argument('--check', action='store_true')
    args = parser.parse_args()
    source = args.source_root.resolve()
    changes = plan(source)
    if args.check:
        if changes:
            raise ValueError('Recovery variants not prepared: ' + ', '.join(changes))
        print('Recovery dependencies and explicit policy patch verified. No compilation or device validation.')
        return
    if not args.apply:
        for path, (before, after) in changes.items():
            print(''.join(difflib.unified_diff(before.splitlines(True), after.splitlines(True), fromfile=path, tofile=path)), end='')
        print('Preview only. No files changed, no backend enabled, no build ran.')
        return
    if changes:
        for path, (before, _) in changes.items():
            if (source / path).read_text(encoding='utf-8') != before:
                raise ValueError('Source changed during preflight: ' + path)
        backup = source.parent / (source.name + '-backups') / f'recovery-crypto-deps-{time.time_ns()}'
        for path in changes:
            target = backup / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((source / path).read_bytes())
        # All preflight checks and backups finish before the first source mutation.
        for path, (_, after) in changes.items():
            target = source / path
            temp = target.with_name(target.name + '.recovery-crypto.tmp')
            temp.write_text(after, encoding='utf-8', newline='\n')
            temp.replace(target)
        print('Backup: ' + str(backup))
    print('Dependencies prepared. Backend remains opt-in; no compilation or phone operation ran.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError) as error:
        raise SystemExit('ERROR: ' + str(error))
