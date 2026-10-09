# Optional Recovery storage decryption

English | [简体中文](README.zh-CN.md)

This repository contains the optional **backend integration layer** and an opt-in
[Android 17 existing-key backend implementation](android17/README.md). The latter
implements metadata, DE, Synthetic Password and CE recovery for reviewed formats
but is disabled by default. Neither Android compilation nor on-device decryption
has been validated. No tested device profile is included or enabled.
Installing the old callback example does not unlock storage; it reports unsupported.

The implementation provides isolated backend execution, bounded IPC, credential
selection, staged unlock orchestration, independent fscrypt key verification,
and access to the existing ZIP installer. It does not parse stored SP blobs,
restore metadata/DE/CE keys, start vendor HALs, or bypass authentication itself.
Those operations are provided by a separate Android-version-compatible backend,
such as the optional implementation in `android17/`. Keep the framework/backend
and source-implementation/device-validation distinctions when publishing images.

This API is not a compatibility certificate. In particular, the supplied backend
example in `examples/` has no cryptographic operations and must not be enabled
in a released device configuration. The actual `android17/` backend has format
and hardware limits documented separately and requires a reviewed device profile.

## Device-independent packaging

Keep all device-specific code in `device/<vendor>/<device>/recovery-crypto/`:

```text
bootable/recovery/crypto/        framework, ABI and worker (this repository)
device/<vendor>/<device>/       backend, Recovery fstab, init, VINTF, sepolicy
vendor/<vendor>/<device>/       proprietary security services and firmware
system/vold/                   reviewed Android 17 crypto implementation
system/security/               keystore2 implementation
```

The framework has no Xiaomi paths, service names, key blobs, kernel addresses,
hardcoded cipher settings or hardware library dependencies. Devices need no
Recovery source patches or custom compile flags to register a backend.
There is no default backend. Devices without adaptation keep their ADB sideload,
removable-storage installation, settings, wipe and reboot paths without a new
prompt or security-service startup. With a backend installed, ordinary
interactive Recovery entry prepares metadata/DE storage and asks for the saved
lock-screen credential before showing the home menu. Patterns use the graphical
grid; PIN/password use the existing input UI. A no-credential protector unlocks
without an input prompt. Already accessible storage skips redundant preparation.

Cancellation or failure returns to the home menu without blocking other
features. `Apply update` additionally offers `Choose ZIP from internal storage`;
it rechecks actual kernel key presence/storage access, reuses a successful startup
unlock, and offers another unlock if startup was cancelled or failed. No cached
boolean is treated as proof of decryption. Command-driven OTA, ADB sideload,
wipes, rescue, `--just_exit` and headless/quiescent startup skip this interactive
prompt so they can proceed without a password. No credential is tried
automatically, and hardware throttling remains enforced.

The file browser currently exposes primary-user storage (user 0) only. The ABI
accepts a user ID for future multi-user callers. Work profiles and adoptable
storage are not supported by this UI. PIN, Android pattern cells and basic ASCII
passwords are accepted locally using Recovery's touch/key menu; this is not a
full touchscreen keyboard, and non-ASCII passwords need an input extension.
Patterns use a touchable grid. The generic Android 17 backend reads the saved
3x3, 4x4, 5x5 or 6x6 size per user; it never tries several sizes automatically.

## Backend registration

For the supported Android 17 formats, use the implementation, dependency helper
and device-tree packaging templates documented in [android17/README.md](android17/README.md).
That path needs no custom decryption callbacks for each device: the shared
implementation stays here and the device supplies configuration/HAL packaging.
The steps below are for a separate platform/vendor backend using the public ABI.

1. Copy `examples/backend.cpp.example` and `examples/Android.bp.example` into
   your device tree as `backend.cpp` and `Android.bp`. Replace the unsupported
   callbacks with a reviewed implementation for your platform.
2. Give the module a unique name, but keep its installed stem
   `librecovery_crypto_backend`. Install only one such backend per product.
3. In the device product makefile:

   ```make
   PRODUCT_PACKAGES += librecovery_crypto_mydevice
   ```

4. Package the actual Recovery variants of dependencies, compatible vendor
   services, VINTF fragments and init/sepolicy definitions in the device tree.
   The backend is installed in the Recovery ramdisk as
   `/system/lib64/librecovery_crypto_backend.so` (32-bit: `/system/lib/...`).
   The worker is the same bitness as the Recovery executable. Both files must
   be regular, root-owned and not group/world-writable.

No library name is selected from properties, a ZIP or userdata. The framework
does not scan/load every vendor library. Missing files hide the new entry;
bad ABI, missing dependencies, crashes and timeouts produce a failure prompt
and return to the regular menu. A failed unlock never falls back to a wipe.

## Android 17 backend requirements

`include/recovery_crypto/backend.h` is the versioned C ABI. Do not expose C++ STL
objects across it. All callbacks run in one worker session, preserving platform
library state across metadata, DE and CE stages:

The v1 structure and required export retain their original layout. A backend
may additionally export `recovery_crypto_get_pattern_size_v1(user, size)` to
report a saved 3..6 grid size after credential discovery. Without that export,
the worker keeps the original 3x3 contract. Both IPC peers use protocol v2 and
must be rebuilt together; older worker binaries are rejected. Reported size is
bound to the session and cannot be changed by an unlock request. Pattern cells
are zero-based row-major bytes, not ASCII decimal numbers.

1. `prepare_services`: perform bounded lookups for the device's actual
   KeyMint/Keymaster, Gatekeeper or Weaver, SharedSecret and keystore2 services.
   Validate OS/version/security patch compatibility. Service presence is not
   proof that key operations work. Start only device-tree-reviewed services.
2. `mount_metadata`: mount the metadata partition from the actual fstab, restore
   the **existing** metadata key and create the correct `dm-default-key` mapping
   for userdata. Mount mapped userdata at `/data`; confirm the mount succeeded.
3. `load_de_keys`: restore the existing primary-user DE and systemwide keys and
   install them in fscrypt. Do not call initialization paths which create keys.
4. `get_credential_type`: obtain the user's active SP protector/credential type
   from existing platform state. `NONE` must mean an actual no-LSKF protector,
   not a missing file, failed parse or unavailable credential service.
5. `unlock_ce`: PIN/password bytes are the lock-screen credential, **not** vold's
   CE secret. Use the matching platform's SP protector format, scrypt parameters,
   Gatekeeper/Weaver verification and KeyMint auth-token handling; unwrap SP,
   derive the `fbe-key` subkey and restore/install the existing CE key. Even
   no-lock-screen users need the proper SP path. Enforce hardware throttling.
6. `finish`: clear transient secrets and release handles without revoking the
   keys or unmounting userdata; file browsing/installation follows this call.

Return stable ABI error codes, not vendor error strings containing key data.
`RC_RETRY` from `unlock_ce` must include the hardware retry delay. Nothing is
automatically retried; retrying after rejection requires another user action.

The currently reviewed Android 17 vold revision is
`11d8982f824d52e8122c7aa732207ba66b8f23c8`. In that revision:

- `libvold`/`vold` are not Recovery-ready simply by depending on them in Soong.
  Recovery variants of dependencies must be supplied in their owning projects.
- `fscrypt_mount_metadata_encrypted` has nine arguments. Passing
  `needs_encrypt=false, should_format=false` is insufficient to establish an
  entirely non-mutating Recovery path: `read_key` also prepares directories and
  `retrieveKey` can upgrade stored KeyMint blobs. Inspect those paths.
- `fscrypt_unlock_ce_storage` uses `read_and_fixate_user_ce_key`; fixating can
  delete alternate keys and rename key directories. Recovery needs a
  retrieve-and-install path which does not perform this key maintenance.
- Framework `SyntheticPasswordManager.unlockLskfBasedProtector` can reenroll
  Gatekeeper credentials and write state/metrics. Do not blindly translate all
  those side effects into an unlock-only Recovery backend.
- SP v3 uses SP800-derived subkeys. Old TWRP interfaces or a direct PIN-to-vold
  call are not equivalent to this Android 17 flow.

Never create/replace keys, delete locksettings, format data, reenroll credentials,
disable encryption or change normal-system services as an error fallback. Key
upgrade handling requires explicit platform review; until supported, return
`RC_KEY_UPGRADE_REQUIRED` rather than rewriting a blob with an uncertain ABI.

## Independent success checks and failure limits

The worker's return value does not open the file picker by itself. Recovery
requires a real `/data` ext4/F2FS mount, opens `/data/media/0` without following
path-component symlinks, retrieves its fscrypt v1/v2 policy, checks that the kernel
reports the policy key present, and verifies directory enumeration succeeds.
Empty directories are valid; mounted plaintext/ramdisk directories are not
treated as successful encrypted storage. No filenames/key data are logged.

The ZIP picker passes through the existing signature and installation checks.
Choosing a ZIP does not imply every third-party installer is compatible.
Internal-storage installation rejects `.map` files and resolved paths escaping
the current user's media root. Ordinary public-volume behavior remains unchanged.

Credentials never enter argv, environment variables, files or log output. Input
and IPC request buffers use locked, dump-excluded memory and are wiped on exit.
The worker has core dumps disabled and stdout/stderr redirected to `/dev/null`.
Adapters must also suppress Binder/vendor/platform logs containing secrets.
The optional Android 17 backend separately appends fixed, typed diagnostic
checkpoints and numeric error codes to the existing `/tmp/recovery.log`; this
does not forward worker or vendor output. See its [diagnostic guide](android17/README.md#locating-an-unlock-failure).
Each operation has a 45-second deadline, even if the backend emits progress.
Worker isolation contains a process crash or hang; it cannot undo kernel/TEE
effects caused by buggy code running as root. Review and test the backend before
enabling it. Keep SELinux enforcing with device-specific, narrowly scoped rules.

## Source synchronization

Commit the backend and its packaging to the **device/vendor** repositories.
This repository holds only the generic ABI and UI. Then:

```bash
cd /path/to/android
repo sync -c bootable/recovery
```

updates the generic implementation without editing/deleting the backend. It
does not update other projects' local state; a full device-tree sync still needs
committed/manifest-tracked adaptation. ABI mismatches fail the optional unlock
operation instead of breaking other Recovery features. No installation script
should copy a device patch into `bootable/recovery` after sync.

## Validation status

Protocol regression test sources: `crypto/tests/protocol_test.cpp`. Source
validation is not proof of Android linking, HAL compatibility or decryption.
Build/run the supplied test and Recovery yourself before using any image.
For an enabled device, verify missing backend/dependencies, worker crash/timeout,
missing keys, wrong credentials, throttle, no-lock-screen, PIN, password/pattern,
kernel key presence, file browsing and continued ADB sideload after failure.

Primary references:

- https://source.android.com/docs/security/features/encryption/file-based
- https://source.android.com/docs/security/features/encryption/metadata
- https://source.android.com/docs/security/features/encryption/hw-wrapped-keys
