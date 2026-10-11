# Policy review fixtures

These public AOSP SELinux sources were retrieved from
https://android.googlesource.com/platform/system/sepolicy/+/refs/heads/main/
on 2026-10-01. They are review/test inputs, not device deployment payloads.
They retain AOSP's Apache-2.0 licensing; see the platform system/sepolicy license.
Their hashes are recorded in `../../../patches/android17-recovery-key-access.json`.

The tests apply the explicit patch to temporary copies, and optionally expand
macros using GNU m4 (on Windows, an existing WSL m4 can be used). They never
compile Android or a SELinux binary policy, start a HAL, or access device keys.
