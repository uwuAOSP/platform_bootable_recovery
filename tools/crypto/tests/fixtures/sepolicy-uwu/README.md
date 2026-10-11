# uwuAOSP policy review fixtures

These platform policy files were read over verified SSH from the user's
uwuAOSP source tree on 2026-10-01, at system/sepolicy revision
`b41cbf3b46882139654574fe46ca5cf8175bf8a5`. They are public platform policy
review inputs, not device data or deployment payloads. They retain the
platform's Apache-2.0 licensing.

The checkout's verified source remote is
https://github.com/uwuAOSP/platform_system_sepolicy.

The companion review manifest is
`../../../patches/android17-uwu-recovery-key-access.json`. It records the
original and reviewed patched hashes. Compared with the AOSP fixture,
uwuAOSP's key-isolation rules additionally exclude `apexd` from three metadata
rules; the reviewed patch preserves those exclusions.

Tests check complete patch application, idempotence, unknown-rule refusal,
unchanged non-opt-in semantics and read-only guards using GNU m4 expansion.
They do not compile a binary policy, build Android, or access device keys.
