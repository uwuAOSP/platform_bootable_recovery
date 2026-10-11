#!/usr/bin/env python3
# SPDX-FileCopyrightText: The uwuAOSP Project
# SPDX-License-Identifier: Apache-2.0
"""Host deployment tests only; these do not compile or validate Android decryption."""
from pathlib import Path
import tempfile
import shutil
import unittest
import prepare_android17 as prepare


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        for path, name in prepare.INTERFACES.items():
            file = self.root / f'hardware/interfaces/{path}/aidl/Android.bp'
            file.parent.mkdir(parents=True)
            file.write_text(f'aidl_interface {{\n    name: "{name}",\n    backend: {{ ndk: {{ enabled: true, }}, }},\n}}\n')
        self.sqlite = self.root / 'external/sqlite/dist/Android.bp'
        self.sqlite.parent.mkdir(parents=True)
        self.sqlite.write_text('cc_defaults { name: "sqlite-minimal-defaults", }\ncc_defaults { name: "release_package_libsqlite3_library_defaults", }\n')
        self.analyzer = self.root / 'system/tools/aidl/Android.bp'
        self.analyzer.parent.mkdir(parents=True)
        self.analyzer.write_text('// Unrelated compiler declarations stay intact\n' + prepare.ANALYZER_BLOCK + '\n')
        fixtures = Path(__file__).parent / 'tests/fixtures/sepolicy'
        shutil.copytree(fixtures, self.root / 'system/sepolicy')

    def tearDown(self):
        self.temp.cleanup()

    def test_preview_is_read_only_and_only_dependency_projects_change(self):
        originals = {p: p.read_bytes() for p in self.root.rglob('*') if p.is_file()}
        changes = prepare.plan(self.root)
        self.assertEqual(len(changes), 10)
        for path in changes:
            self.assertTrue(path.startswith(('hardware/interfaces/', 'external/sqlite/', 'system/tools/aidl/', 'system/sepolicy/')))
        self.assertEqual(originals, {p: p.read_bytes() for p in originals})

    def test_applied_plan_is_idempotent(self):
        for path, (_, after) in prepare.plan(self.root).items():
            (self.root / path).write_text(after, newline='\n')
        self.assertEqual(prepare.plan(self.root), {})

    def test_unrelated_platform_sources_do_not_require_a_hash_manifest(self):
        checked = self.root / 'system/vold/FsCrypt.cpp'
        checked.parent.mkdir(parents=True)
        checked.write_text('// platform source updated independently\n')
        before = checked.read_bytes()
        self.assertTrue(prepare.plan(self.root))
        self.assertEqual(checked.read_bytes(), before)
        self.assertFalse((self.root / 'review.json').exists())

    def test_existing_disabled_variant_is_not_overridden(self):
        with self.assertRaises(ValueError):
            prepare.add_recovery_variant('aidl_interface { name: "example", recovery_available: false, }', 'example')

    def test_duplicate_interface_is_refused(self):
        block = 'aidl_interface { name: "example", }\n'
        with self.assertRaises(ValueError):
            prepare.add_recovery_variant(block * 2, 'example')

    def test_changed_sqlite_adapter_is_refused(self):
        self.sqlite.write_text('cc_library_static { name: "librecovery_crypto_sqlite", }')
        with self.assertRaisesRegex(ValueError, 'differs'):
            prepare.plan(self.root)

    def test_analyzer_variant_fills_generated_aidl_dependency(self):
        before, after = prepare.plan(self.root)['system/tools/aidl/Android.bp']
        self.assertEqual(after.count('recovery_available: true'), 1)
        self.assertEqual(after.replace('\n    recovery_available: true,', ''), before)
        self.assertEqual(prepare.prepare_analyzer(after), after)

    def test_changed_analyzer_dependencies_are_refused_before_writes(self):
        self.analyzer.write_text(prepare.ANALYZER_BLOCK.replace('"libbinder",', '"libbinder", "unknown-runtime",'))
        before = self.sqlite.read_bytes()
        with self.assertRaisesRegex(ValueError, 'runtime needs review'):
            prepare.plan(self.root)
        self.assertEqual(self.sqlite.read_bytes(), before)

    def test_analyzer_explicit_disable_and_duplicate_are_refused(self):
        with self.assertRaises(ValueError):
            prepare.prepare_analyzer(prepare.ANALYZER_BLOCK.replace('host_supported:', 'recovery_available: false, host_supported:'))
        with self.assertRaisesRegex(ValueError, 'Unknown module layout'):
            prepare.prepare_analyzer(prepare.ANALYZER_BLOCK * 2)


if __name__ == '__main__':
    unittest.main()
