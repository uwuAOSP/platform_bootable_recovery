#!/usr/bin/env python3
# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
"""Deployment regression tests; uses Git, never a compiler or USB device."""
import contextlib
import importlib.util
import io
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import shutil

SPEC = importlib.util.spec_from_file_location('prepare_mtp', Path(__file__).with_name('prepare_platform.py'))
HELPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPER)


def fixture(patch):
    """Materialize old hunk context only; unrelated lines are inert placeholders.

    This checks the real Git patch application, not native/AOSP compilation.
    """
    files = {}
    path = None
    index = None
    for line in patch.splitlines(True):
        if line.startswith('--- a/'):
            path = line[6:].strip()
            files[path] = {}
        elif line.startswith('@@'):
            index = int(re.match(r'@@ -(\d+)', line)[1]) - 1
        elif index is not None and line[:1] in (' ', '-'):
            previous = files[path].get(index)
            if previous is not None and previous != line[1:]:
                raise AssertionError('Overlapping source context differs')
            files[path][index] = line[1:]
            index += 1
        elif line.startswith('diff --git'):
            index = None
    return {p: ''.join(lines.get(i, f'// unrelated fixture line {i}\n')
                       for i in range(max(lines) + 1)) for p, lines in files.items()}


class Deployment(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / 'android'
        self.root.mkdir()
        for repository, patch in HELPER.PATCHES.items():
            target = self.root / repository
            target.mkdir(parents=True)
            subprocess.run(['git', 'init', '-q', str(target)], check=True)
            subprocess.run(['git', '-C', str(target), 'config', 'core.autocrlf', 'false'], check=True)
            combined = fixture((HELPER.PAYLOAD / patch).read_text())
            # Both patch generations use original source line offsets. Merge
            # their real context; placeholder lines are not reviewed input.
            for path, data in fixture((HELPER.PAYLOAD / patch.replace('.patch', '-readonly-v1.patch')).read_text()).items():
                old_lines = data.splitlines(True)
                lines = combined.get(path, '').splitlines(True)
                while len(lines) < len(old_lines): lines.append(f'// unrelated fixture line {len(lines)}\n')
                for i, line in enumerate(old_lines):
                    if not line.startswith('// unrelated fixture line '):
                        if not lines[i].startswith('// unrelated fixture line ') and lines[i] != line:
                            raise AssertionError('Old/new reviewed context differs')
                        lines[i] = line
                combined[path] = ''.join(lines)
            for path, data in combined.items():
                file = target / path
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_text(data, encoding='utf-8', newline='\n')

    def deploy(self):
        with contextlib.redirect_stdout(io.StringIO()):
            HELPER.apply(self.root, HELPER.plan(self.root))

    def test_preview_apply_and_idempotence(self):
        before = {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file() and '.git' not in p.parts}
        self.assertTrue(all(item[2] for item in HELPER.plan(self.root)))
        self.assertEqual(before, {p: p.read_bytes() for p in before})
        self.deploy()
        self.assertFalse(any(item[2] for item in HELPER.plan(self.root)))
        self.deploy()
        self.assertEqual(1, len(list((self.root.parent / 'android-backups').iterdir())))

    def test_conflict_preflights_every_project_before_mutation(self):
        policy = self.root / 'system/sepolicy/private/recovery.te'
        policy.write_text('unknown policy\n')
        original = (self.root / 'frameworks/av/media/mtp/Android.bp').read_bytes()
        with self.assertRaisesRegex(ValueError, 'conflicts'):
            self.deploy()
        self.assertEqual(original, (self.root / 'frameworks/av/media/mtp/Android.bp').read_bytes())
        self.assertFalse((self.root.parent / 'android-backups').exists())

    def test_modified_owned_block_cannot_be_duplicated(self):
        self.deploy()
        bp = self.root / 'frameworks/av/media/mtp/Android.bp'
        bp.write_text(bp.read_text().replace('"-DRECOVERY_MTP"', '"-DRECOVERY_MTP_UNSAFE"'), encoding='utf-8', newline='\n')
        with self.assertRaisesRegex(ValueError, 'Partial/modified'):
            HELPER.plan(self.root)

    def test_unrelated_source_changes_survive(self):
        bp = self.root / 'frameworks/av/media/mtp/Android.bp'
        bp.write_text('// downstream customization\n' + bp.read_text(), encoding='utf-8', newline='\n')
        self.deploy()
        self.assertTrue(bp.read_text().startswith('// downstream customization\n'))
        self.assertFalse(any(item[2] for item in HELPER.plan(self.root)))

    def test_paths_cannot_escape_tree(self):
        with self.assertRaises(ValueError):
            HELPER.trusted_path(self.root, '../outside')

    def test_upgrade_from_complete_readonly_deployment(self):
        for repository, patch in HELPER.PATCHES.items():
            operation = HELPER.git(self.root / repository, 'apply', str(HELPER.PAYLOAD / patch.replace('.patch', '-readonly-v1.patch')))
            self.assertEqual(0, operation.returncode, operation.stderr)
        before = (self.root / 'frameworks/av/media/mtp/MtpServer.cpp').read_bytes()
        upgrade = HELPER.plan(self.root)
        self.assertTrue(all(item[2] for item in upgrade))
        self.assertEqual(before, (self.root / 'frameworks/av/media/mtp/MtpServer.cpp').read_bytes())
        self.deploy()
        self.assertFalse(any(item[2] for item in HELPER.plan(self.root)))
        self.assertIn('recoveryBeginUpload', (self.root / 'frameworks/av/media/mtp/MtpServer.cpp').read_text())

    def test_source_race_is_refused_before_first_write(self):
        plan = HELPER.plan(self.root)
        file = self.root / 'system/sepolicy/private/recovery.te'
        file.write_text('// changed concurrently\n' + file.read_text())
        original = (self.root / 'frameworks/av/media/mtp/Android.bp').read_bytes()
        with self.assertRaisesRegex(ValueError, 'Source changed'):
            HELPER.apply(self.root, plan)
        self.assertEqual(original, (self.root / 'frameworks/av/media/mtp/Android.bp').read_bytes())

    def test_upgrade_writable_transport_only_changes_host_helper(self):
        av = self.root / 'frameworks/av'
        old = HELPER.PAYLOAD / 'frameworks-av-writable-v2.patch'
        self.assertEqual(0, HELPER.git(av, 'apply', str(old)).returncode)
        policy = self.root / 'system/sepolicy'
        self.assertEqual(0, HELPER.git(policy, 'apply', str(HELPER.PAYLOAD / 'sepolicy.patch')).returncode)
        plan = HELPER.plan(self.root)
        self.assertEqual({'media/mtp/MtpDataPacket.cpp'}, set(plan[0][1]))
        self.assertFalse(plan[1][2])
        self.deploy()
        self.assertFalse(any(item[2] for item in HELPER.plan(self.root)))

    def test_upgrade_complete_writable_deployment_preserves_other_policy(self):
        for repository, patch in (('frameworks/av', 'frameworks-av.patch'),
                                  ('system/sepolicy', 'sepolicy-writable-v2.patch')):
            result = HELPER.git(self.root / repository, 'apply', str(HELPER.PAYLOAD / patch))
            self.assertEqual(0, result.returncode, result.stderr)
        policy = self.root / 'system/sepolicy/private/recovery.te'
        before = policy.read_text().replace('// unrelated fixture line 100\n',
            '// downstream customization\n')
        policy.write_text(before, encoding='utf-8', newline='\n')
        upgrade = HELPER.plan(self.root)
        self.assertFalse(upgrade[0][2])
        self.assertEqual({'private/recovery.te'}, set(upgrade[1][1]))
        self.assertEqual(before, policy.read_text())
        self.deploy()
        self.assertFalse(any(item[2] for item in HELPER.plan(self.root)))
        expected = before.replace('  allow recovery media_rw_data_file:file create_file_perms;',
            '  # O_TMPFILE uploads are published with linkat after their contents are flushed.\n'
            '  # create_file_perms does not include link.\n'
            '  allow recovery media_rw_data_file:file { create_file_perms link };')
        self.assertEqual(expected, policy.read_text())
        backups = list((self.root.parent / 'android-backups').glob('*/system/sepolicy/private/recovery.te'))
        self.assertEqual(1, len(backups))
        self.assertEqual(before, backups[0].read_text())

    def test_m4_write_exception_only_in_adapted_recovery(self):
        executable = shutil.which('m4')
        if not executable:
            self.skipTest('m4 absent: run source expansion test on Linux; no compiler needed')
        self.deploy()
        source = (self.root / 'system/sepolicy/private/recovery.te').read_text()
        # Expand the real staged policy; this checks quoting and conditional
        # scope only. It does not invoke checkpolicy/secilc or validate policydb.
        macros = "define(`recovery_only', `ifelse(target_recovery, `true', `$1')')\n"
        macros += "define(`with_native_coverage', `')\n"
        for recovery in ('true', 'false'):
            for enabled in ('true', 'false'):
                with self.subTest(recovery=recovery, enabled=enabled):
                    result = subprocess.run([executable, '-Dtarget_recovery=' + recovery,
                                             '-Drecovery_mtp=' + enabled],
                                            input=(macros + source).encode(), capture_output=True, timeout=30)
                    self.assertEqual(0, result.returncode, result.stderr.decode())
                    text = re.sub(r'^#.*$', '', result.stdout.decode(), flags=re.M)
                    permitted = recovery == enabled == 'true'
                    self.assertEqual(permitted, '-media_rw_data_file' in text)
                    self.assertEqual(permitted, 'allow recovery media_rw_data_file:file { create_file_perms link };' in text)
                    self.assertNotIn('-keystore_data_file', text)
                    self.assertNotIn('-vold_data_file', text)
                    self.assertIn('no_x_file_perms', text)


if __name__ == '__main__':
    unittest.main()
