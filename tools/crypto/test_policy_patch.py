#!/usr/bin/env python3
# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
"""Git patch and m4 expansion tests only. No Android/SELinux/native compilation."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import policy_patch

FIXTURES = Path(__file__).parent / 'tests/fixtures/sepolicy'


class PolicyPatchTests(unittest.TestCase):
    fixtures = FIXTURES
    review = 'android17-recovery-key-access'
    metadata_exclusions = 'domain-init-vold-recovery'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        shutil.copytree(self.fixtures, self.root / 'system/sepolicy')

    def test_review_hashes_match_original_and_patched_fixtures(self):
        import hashlib
        import json
        review = json.loads((policy_patch.PATCH_DIR / (self.review + '.json')).read_text())
        changes = policy_patch.plan(self.root)
        for relative in policy_patch.FILES:
            self.assertEqual(hashlib.sha256((self.fixtures / relative).read_bytes()).hexdigest(),
                             review['files'][relative]['base_sha256'])
            self.assertEqual(hashlib.sha256(changes['system/sepolicy/' + relative][1].encode()).hexdigest(),
                             review['files'][relative]['patched_sha256'])
        self.assertEqual(hashlib.sha256((self.fixtures / 'public/global_macros').read_bytes()).hexdigest(),
                         review['fixture_global_macros_sha256'])

    def test_preview_is_read_only_and_apply_is_idempotent(self):
        original = {p: p.read_bytes() for p in (self.root / 'system/sepolicy').rglob('*') if p.is_file()}
        changes = policy_patch.plan(self.root)
        self.assertEqual(set(changes), {'system/sepolicy/' + p for p in policy_patch.FILES})
        self.assertEqual(original, {p: p.read_bytes() for p in original})
        for p, (_, after) in changes.items():
            (self.root / p).write_text(after, newline='\n')
        self.assertEqual(policy_patch.plan(self.root), {})

    def test_unrelated_changes_are_preserved(self):
        p = self.root / 'system/sepolicy/private/vold.te'
        p.write_text('# unrelated maintainer comment\n' + p.read_text(), newline='\n')
        changes = policy_patch.plan(self.root)
        self.assertTrue(changes['system/sepolicy/private/vold.te'][1].startswith('# unrelated maintainer comment\n'))

    def test_conflicting_rule_refused_without_writes(self):
        p = self.root / 'system/sepolicy/private/vold.te'
        p.write_text(p.read_text().replace('-vold\n} data_file_type:dir ioctl', '-vold\n  -custom_reader\n} data_file_type:dir ioctl'), newline='\n')
        original = p.read_bytes()
        with self.assertRaisesRegex(ValueError, 'patch conflicts'):
            policy_patch.plan(self.root)
        self.assertEqual(p.read_bytes(), original)

    def test_partial_application_and_tampered_patch_refused(self):
        changes = policy_patch.plan(self.root)
        p = 'system/sepolicy/private/vold.te'
        (self.root / p).write_text(changes[p][1], newline='\n')
        with self.assertRaisesRegex(ValueError, 'patch conflicts'):
            policy_patch.plan(self.root)
        from unittest.mock import patch
        damaged = self.root / 'damaged-patch'
        shutil.copytree(policy_patch.PATCH_DIR, damaged)
        with (damaged / (self.review + '.patch')).open('a') as f:
            f.write('\nchanged\n')
        with patch.object(policy_patch, 'PATCH_DIR', damaged):
            with self.assertRaisesRegex(ValueError, 'patch/review differs'):
                policy_patch.plan(self.root)

    def m4(self, texts, recovery, enabled):
        executable = shutil.which('m4')
        if executable:
            command = [executable]
        elif os.name == 'nt' and Path('C:/Windows/System32/wsl.exe').is_file():
            command = ['C:/Windows/System32/wsl.exe', '--exec', 'm4']
        else:
            self.skipTest('m4 not installed; run these expansion checks on the Linux build host')
        command += ['-Dtarget_recovery=' + str(recovery).lower(),
                    '-Drecovery_crypto_android17=' + str(enabled).lower()]
        result = subprocess.run(command, input='\n'.join(texts).encode(), capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
        return re.sub(r'^#.*$', '', result.stdout.decode(), flags=re.M)

    def test_four_build_conditions_preserve_isolation_and_delete_ban(self):
        changes = policy_patch.plan(self.root)
        order = ('public/te_macros', 'private/vold.te', 'private/keystore.te')
        permissions = (self.fixtures / 'public/global_macros').read_text()
        before = [permissions] + [(self.fixtures / p).read_text() for p in order]
        after = [permissions] + [changes['system/sepolicy/' + p][1] for p in order]
        xperm = re.compile(r'neverallowxperm\s*(\{[^{}]*\}|\w+)\s+(\w+):(\w+)\s+(\w+)\s+\{([^{}]*)\};')

        def canonical(text):
            groups = {}
            for match in xperm.finditer(text):
                key = tuple(re.sub(r'\s+', '', match[i]) for i in range(1, 5))
                groups.setdefault(key, set()).update(match[5].split())
            return re.sub(r'\s+', '', xperm.sub('', text)), groups

        for recovery, enabled in ((False, False), (False, True), (True, False), (True, True)):
            with self.subTest(recovery=recovery, enabled=enabled):
                baseline = self.m4(before, recovery, enabled)
                result = self.m4(after, recovery, enabled)
                if not (recovery and enabled):
                    self.assertEqual(canonical(result), canonical(baseline))
                    continue
                compact = re.sub(r'\s+', '', result)
                self.assertIn('neverallow{' + self.metadata_exclusions + '}vold_metadata_file:dir*;', compact)
                self.assertIn('neverallow{domain-keystore-init-recovery}keystore_data_file:dir*;', compact)
                self.assertIn('neverallowrecoverykeystore_data_file:file~{getattropenreadioctllockmapwatchwatch_reads};', compact)
                self.assertNotIn('~{{', compact)
                self.assertIn('neverallowrecoverykeystore_data_file:{lnk_filesock_filefifo_file}*;', compact)
                for match in xperm.finditer(result):
                    if 'FS_IOC_REMOVE_ENCRYPTION_KEY' in match[5] or 'FS_IOC_SET_ENCRYPTION_POLICY' in match[5]:
                        self.assertNotIn('-recovery', match[1])
                self.assertIn('FS_IOC_ADD_ENCRYPTION_KEY', result)
                self.assertIn('FS_IOC_GET_ENCRYPTION_KEY_STATUS', result)


class UwuPolicyPatchTests(PolicyPatchTests):
    fixtures = FIXTURES.with_name('sepolicy-uwu')
    review = 'android17-uwu-recovery-key-access'
    metadata_exclusions = 'domain-apexd-init-vold-recovery'

    def test_unknown_metadata_exclusion_refused_without_writes(self):
        p = self.root / 'system/sepolicy/private/vold.te'
        p.write_text(p.read_text().replace('-apexd', '-custom_metadata_reader'), newline='\n')
        original = p.read_bytes()
        with self.assertRaisesRegex(ValueError, 'patch conflicts'):
            policy_patch.plan(self.root)
        self.assertEqual(p.read_bytes(), original)


if __name__ == '__main__':
    unittest.main()
