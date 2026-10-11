# Android 17 Recovery device decryption adaptation guide

English | [简体中文](README.zh-CN.md) | [General framework](../README.md)

Examples use `device/acme/mydevice`, `vendor/acme/mydevice`, and the module prefix
`mydevice_rec_`. Replace them with your device paths and a unique module prefix.
The examples assume **a real AIDL KeyMint, AIDL Gatekeeper, and SecureClock /
SharedSecret provided by the KeyMint process**. Follow the alternatives below if
your device uses a different service arrangement. Vendor libraries, service paths,
policy domains, and fstab entries must come from your device; changing the device
name alone is not enough.

## 0. Prerequisites

Run ADB commands in normal Android, and `rg` from the source root. Replace the
`.config` path with your actual kernel configuration path.

| Requirement | How to check |
| --- | --- |
| A real AIDL KeyMint | `adb -d shell service check android.hardware.security.keymint.IKeyMintDevice/default`; use the instance declared in VINTF |
| Access to the actual Gatekeeper / Weaver implementation | See [Gatekeeper and Weaver queries and configuration](#gatekeeper-and-weaver-queries-and-configuration) |
| Kernel support for the device's actual fscrypt, metadata encryption, and hardware-wrapped key paths | `rg 'CONFIG_.*(ENCRYPTION\|DEFAULT_KEY\|CRYPTO)' path/to/kernel/.config`<br>`adb -d shell "cat /vendor/etc/fstab.*"`; check the configuration against fstab. For wrapped keys, also verify [storage driver support](https://source.android.com/docs/security/features/encryption/hw-wrapped-keys) |

## 1. Files to create

The following layout is recommended. File names must match the `src`, include,
and installation declarations used below:

```text
device/acme/mydevice/
  BoardConfig.mk                  ← Existing file; add an include
  device.mk                       ← Existing product file; add inherit-product
  recovery-crypto/
    config.mk                     ← Device flag shared by both integration points
    crypto.mk                     ← Select modules for packaging
    BoardConfig.mk                ← Device policy and platform key-read flag
    Android.bp                    ← Configuration modules; optional authentication bridge
    recovery.crypto.conf          ← Security services used by the backend
    recovery.crypto.fstab         ← Metadata / userdata configuration
    manifest.xml                  ← Recovery security HAL declarations
    init.recovery.mydevice-crypto.rc ← Security service startup
    sepolicy/
      recovery.te                 ← Required device permissions
      file_contexts               ← Create only if new file labels are needed
      service_contexts            ← Create only if new service instance labels are needed
    ueventd.crypto.rc             ← Create and integrate only if node permissions are missing

vendor/acme/mydevice/
  Android.bp                      ← Usually has a namespace; preserve generated content
  proprietary/
    Android.bp                    ← Recovery vendor modules; preserve existing content
    vendor/bin/hw/...             ← Extracted security services
    vendor/lib64/...              ← Extracted dependency libraries
```

| File | Required? | Purpose and source |
| --- | --- | --- |
| `recovery.crypto.conf` | Required | Selects backend services; derive it from the actual HALs and normal keystore2's SharedSecret negotiation participants |
| `recovery.crypto.fstab` | Required | Partition and encryption configuration; use the normal system's `/metadata` and `/data` fstab entries |
| `Android.bp` | Required | Defines modules and installation locations; written by the maintainer |
| Product `.mk` / BoardConfig integration | Required | Explicitly selects modules and policy; written by the maintainer. This example uses three `.mk` files |
| `manifest.xml` | The declarations must exist | Reuse an existing Recovery fragment if it declares every required HAL; otherwise add a fragment |
| Init RC | The startup configuration must exist | Reuse an existing Recovery RC if it correctly starts every required service; otherwise add one |
| Vendor Recovery modules | Device-dependent | Reuse modules with suitable Recovery variants; otherwise declare new modules for proprietary binaries and libraries |
| `sepolicy/` | The permissions must exist | Add permissions missing from existing policy; do not blindly copy another device's domains or device nodes |
| `auth_service.cpp` or a common authentication bridge | Optional | Add only when the actual services require bridging; not every device needs it |
| Ueventd, firmware, and persist mount configuration | Device-dependent | Derive from the nodes, firmware paths, and startup order required by this device's security services |

Write these files in the order below. Dependencies and SELinux patches modified
by the platform helper belong to other platform projects, not this device
directory; see step 10.

## 2. Write `recovery.crypto.conf`

Complete configuration example:

```ini
profile_version=1
platform_sdk=37
storage_binding=none
fstab=/system/etc/recovery.crypto.fstab
keymint_service=android.hardware.security.keymint.IKeyMintDevice/default
security_level=tee
gatekeeper_transport=aidl
gatekeeper_instance=default
weaver_transport=none
weaver_instance=
secureclock_service=android.hardware.security.secureclock.ISecureClock/default
sharedsecret_services=android.hardware.security.sharedsecret.ISharedSecret/default
```

| Field | What to enter |
| --- | --- |
| `profile_version`, `platform_sdk` | The current parser accepts only `1` and `37`; changing these numbers does not add support for other Android versions |
| `storage_binding` | Currently accepts only `none`; first confirm that the actual platform does not use a storage-binding seed |
| `fstab` | Path to the separate crypto fstab inside Recovery; must be under `/system/etc/` |
| `keymint_service` | `android.hardware.security.keymint.IKeyMintDevice/` followed by the actual instance name |
| `security_level` | The actual security level used by the key; only `tee` or `strongbox` |
| `gatekeeper_transport`, `weaver_transport` | `aidl`, `hidl`, or `none`, according to the actual authentication implementation |
| `gatekeeper_instance`, `weaver_instance` | The actual instance name when using the service, such as `default`; must be empty for `none` |
| `secureclock_service` | The full actual SecureClock service name; leave empty only after confirming it is unnecessary |
| `sharedsecret_services` | Comma-separated full AIDL SharedSecret service names; at least one, without duplicates or spaces |
| `sharedsecret_hidl_instances` | Optional, such as `4.1/default`; supports only `4.0/4.1` with `default/strongbox` instances |

### Gatekeeper and Weaver queries and configuration

In normal Android, list the AIDL and HIDL implementations separately:

```bash
adb -d shell service list
```

```bash
adb -d shell lshal
```

Look for `gatekeeper` and `weaver`. Common results are shown below; the instance
name is the part after the last `/`:

```text
android.hardware.gatekeeper.IGatekeeper/default
android.hardware.weaver.IWeaver/default
android.hardware.gatekeeper@1.0::IGatekeeper/default
android.hardware.weaver@1.0::IWeaver/default
```

Edit `device/<vendor>/<device>/recovery-crypto/recovery.crypto.conf`:

| Implementation found | Configuration |
| --- | --- |
| AIDL Gatekeeper | `gatekeeper_transport=aidl`, `gatekeeper_instance=actual-instance-name` |
| HIDL Gatekeeper | `gatekeeper_transport=hidl`, `gatekeeper_instance=actual-instance-name` |
| AIDL / HIDL Weaver | `weaver_transport=aidl` or `hidl`, `weaver_instance=actual-instance-name` |
| The current credential does not use Weaver | `weaver_transport=none`, `weaver_instance=` |

For the `default` instance, enter `gatekeeper_instance=default` or
`weaver_instance=default`. Also verify the vendor implementation; failing to find
a service does not prove that the credential does not use Weaver.

When HIDL is needed, confirm that a working local passthrough implementation
exists in Recovery. A HIDL service process or VINTF declaration in normal Android
alone is insufficient. Devices without a usable passthrough implementation need
their own bridge; see step 6.

Keep every SharedSecret participant used by normal Android. For a device with a
bridge providing a legacy instance, such as diting, use:

```ini
sharedsecret_services=android.hardware.security.sharedsecret.ISharedSecret/default,android.hardware.security.sharedsecret.ISharedSecret/legacy
```

If using a directly callable HIDL Keymaster passthrough implementation, retain
the actual AIDL participants and add:

```ini
sharedsecret_hidl_instances=4.1/default
```

Do not list the same hardware participant through both an AIDL bridge and HIDL,
or list the same HIDL instance as both 4.0 and 4.1. The combined AIDL and HIDL
participant limit is four. Using Weaver does not necessarily let you disable
Gatekeeper: SP SID verification may still require it.

## 3. Write `recovery.crypto.fstab`

`recovery.crypto.fstab` tells the decryption backend where the device's data
partitions are and which encryption methods they use.

The following example is for diting. Other devices must use their own entries:

```text
/dev/block/bootdevice/by-name/metadata /metadata ext4 noatime,nosuid,nodev wait
/dev/block/bootdevice/by-name/userdata /data f2fs noatime,nosuid,nodev,reserve_root=32768,resgid=1065,fsync_mode=nobarrier,inlinecrypt wait,fileencryption=aes-256-xts:aes-256-cts:v2+inlinecrypt_optimized+wrappedkey_v0,keydirectory=/metadata/vold/metadata_encryption,metadata_encryption=aes-256-xts:wrappedkey_v0
```

Keep each partition entry on a single line; do not split the long line above.
The five fstab columns are:

```text
Block device path  Mount point  Filesystem  Linux mount options  Android fs_mgr options
```

## 4. Write the device `Android.bp`

First define the four configuration modules. Use the following as a starting
point. If reusing existing init/VINTF files, remove the corresponding duplicate
modules and their entries in the product package list:

```bp
// SPDX-License-Identifier: Apache-2.0
soong_namespace {
    imports: ["vendor/acme/mydevice"],
}

prebuilt_etc {
    name: "mydevice_rec_crypto_config",
    recovery: true,
    src: "recovery.crypto.conf",
    filename: "recovery.crypto.conf",
}
prebuilt_etc {
    name: "mydevice_rec_crypto_fstab",
    recovery: true,
    src: "recovery.crypto.fstab",
    filename: "recovery.crypto.fstab",
}
prebuilt_etc {
    name: "mydevice_rec_crypto_manifest",
    recovery: true,
    src: "manifest.xml",
    relative_install_path: "vintf/manifest",
    filename: "mydevice-recovery-crypto.xml",
}
prebuilt_etc {
    name: "mydevice_rec_crypto_init",
    recovery: true,
    src: "init.recovery.mydevice-crypto.rc",
    relative_install_path: "init",
    filename: "init.recovery.mydevice-crypto.rc",
}
```

| Property | What to enter |
| --- | --- |
| `name` | A unique build module name, referenced later by `PRODUCT_PACKAGES` |
| `src` | Source path relative to this `Android.bp`; the file must exist |
| `filename` | Actual file name inside Recovery; it may differ from the module name |
| `relative_install_path` | Subdirectory within the `prebuilt_etc` installation directory |
| `recovery: true` | Installs into Recovery, not the normal system partition |
| `imports` | Search scope for unqualified module names; use directories that actually declare `soong_namespace` |

The files install under Recovery's `/system/etc/`,
`/system/etc/vintf/manifest/`, and `/system/etc/init/`. `Android.bp` only defines
modules. It neither selects them for packaging nor starts services. See step 9
for device module selection.

## 5. Declare vendor security services and all library dependencies

Find the corresponding executables, libraries, and dependencies in normal Android.
Reuse existing modules if they provide suitable Recovery variants. Otherwise,
declare Recovery-specific modules for the original binaries in the vendor
project. Do not replace the normal system HAL's libraries with incompatible
newer versions.

For example, use `vendor/acme/mydevice/proprietary/Android.bp`. Binary paths,
library names, and dependencies below are **placeholders**: replace them with
the actual contents of your device's ELF files. Do not overwrite the generated
main vendor file. If the target file already exists, retain its modules and add
or maintain the dedicated declarations.

```bp
soong_config_module_type {
    name: "mydevice_rec_prebuilt_binary_type",
    module_type: "cc_prebuilt_binary",
    config_namespace: "mydevice_recovery_crypto",
    bool_variables: ["device_enabled"],
    properties: ["enabled", "shared_libs"],
}
soong_config_module_type {
    name: "mydevice_rec_prebuilt_library_type",
    module_type: "cc_prebuilt_library_shared",
    config_namespace: "mydevice_recovery_crypto",
    bool_variables: ["device_enabled"],
    properties: ["enabled", "shared_libs"],
}

mydevice_rec_prebuilt_binary_type {
    name: "mydevice_rec_keymint",
    stem: "mydevice-recovery-keymint",
    srcs: ["vendor/bin/hw/android.hardware.security.keymint-service-example"],
    recovery: true,
    compile_multilib: "64",
    enabled: false,
    strip: { none: true },
    visibility: ["//visibility:public"],
    soong_config_variables: {
        device_enabled: {
            enabled: true,
            shared_libs: ["libbase", "libbinder_ndk", "liblog", "mydevice_rec_libvendorcrypto"],
        },
    },
}
mydevice_rec_prebuilt_library_type {
    name: "mydevice_rec_libvendorcrypto",
    stem: "libvendorcrypto",
    srcs: ["vendor/lib64/libvendorcrypto.so"],
    recovery: true,
    compile_multilib: "64",
    enabled: false,
    strip: { none: true },
    visibility: ["//visibility:public"],
    soong_config_variables: {
        device_enabled: {
            enabled: true,
            shared_libs: ["liblog"],
        },
    },
}
```

`srcs` is relative to the module file's directory; the example's `vendor/...`
paths require the modules to be under `proprietary/`. Preserve the library's
actual file name in `stem`: a dependency on `libvendorcrypto.so` cannot be
satisfied by installing it under a different module-derived file name.
HAL passthrough libraries may also need `relative_install_path: "hw"`. Adjust
the 64-bit configuration to the actual service architecture.

Declare the actual Gatekeeper, required Weaver, separate SecureClock, TEE
listeners, and their recursive vendor library dependencies in the same way.
Inspect ELF `DT_NEEDED`, map every dependency to a real Soong module, and confirm
its matching Recovery/architecture variant. Also check runtime `dlopen`
libraries, configuration files, and firmware; `DT_NEEDED` does not list them all.

`shared_libs` contains module names, not `.so` paths. Platform Binder/HIDL/AIDL
libraries and vendor prebuilts must use compatible interface versions. Do not
copy diting's twenty-plus libraries without checking, or hide unresolved
dependencies with `check_elf_files: false`.

Confirm that a vendor ancestor directory has a valid namespace, referenced by
both the device's `imports` and the product's `PRODUCT_SOONG_NAMESPACES`. If
`vendor/acme/mydevice/Android.bp` already declares the namespace, there is no
need to declare a separate one under `proprietary/`.

## 6. Decide whether `auth_service.cpp` / a Qualcomm authentication bridge is needed

If Recovery can directly access the configured native AIDL Gatekeeper and
SharedSecret, **no authentication bridge module is needed**. A working HIDL
passthrough implementation can also be configured as described in step 2.

Diting currently uses a device bridge to expose QTI HIDL Gatekeeper 1.0 and
legacy Keymaster shared-secret negotiation as AIDL services. Before using the
same bridge, review the HAL ABI, factory prototypes, firmware, and vendor
property dependencies. The device maintenance project provides the common
source and rules; they are not included in this Recovery repository. See:

- [Qualcomm common source and module definitions](https://github.com/Night-stars-1/uwuaosp-diting/tree/main/cloud/diting_recovery_crypto/common)
- [Diting device Android.bp](https://github.com/Night-stars-1/uwuaosp-diting/blob/main/cloud/diting_recovery_crypto/Android.bp)

If `device/qcom/recovery-crypto-common` is already deployed in your Android
source tree, add it to the device namespace's `imports` and append the bridge
declarations below. Define the library modules from step 5 first; do not leave
nonexistent example names in the configuration:

```bp
soong_config_module_type {
    name: "mydevice_crypto_cc_defaults_type",
    module_type: "cc_defaults",
    config_namespace: "mydevice_recovery_crypto",
    bool_variables: ["device_enabled"],
    properties: ["enabled", "shared_libs"],
}
mydevice_crypto_cc_defaults_type {
    name: "mydevice_crypto_bridge_defaults",
    enabled: false,
    soong_config_variables: {
        device_enabled: {
            enabled: true,
            shared_libs: [
                "mydevice_rec_libqtikeymaster4", "libhidlbase", "libutils", "liblog",
                "libbinder_ndk", "libbase", "android.hardware.gatekeeper-V1-ndk",
                "android.hardware.gatekeeper@1.0",
                "android.hardware.security.sharedsecret-V1-ndk",
                "android.hardware.security.keymint-V5-ndk",
                "android.hardware.keymaster@4.0", "android.hardware.keymaster@4.1",
            ],
        },
    },
}
cc_binary {
    name: "mydevice_rec_auth_service",
    defaults: ["mydevice_crypto_bridge_defaults", "qcom_recovery_crypto_auth_sources"],
    stem: "mydevice-recovery-auth",
    recovery: true,
    compile_multilib: "64",
}
```

The first block defines a defaults type controlled by this device's
`mydevice_recovery_crypto.device_enabled`. The second disables it by default
and sets dependencies when enabled; the third creates the executable. The
common defaults provide C++ sources through a `filegroup` in the common
directory, so Soong does not incorrectly look for `auth_service.cpp` in the
device directory.

After adding the bridge, select its module in the product list, start it through
init, declare its registered interfaces in VINTF, and update the
Gatekeeper/SharedSecret instances in the conf. Adding `cc_binary` alone is not
enough. The Qualcomm common layer does not replace adaptation of each device's
proprietary libraries and runtime requirements.

## 7. Write `manifest.xml`

This example matches the initial service arrangement: KeyMint, SecureClock,
SharedSecret, and native AIDL Gatekeeper. Set versions and instances to match
the actual device services. Linking against a `*-V5-ndk` library does not prove
that the vendor process implements a V5 HAL.

```xml
<manifest version="1.0" type="device">
    <hal format="aidl">
        <name>android.hardware.security.keymint</name>
        <version>1</version>
        <fqname>IKeyMintDevice/default</fqname>
    </hal>
    <hal format="aidl">
        <name>android.hardware.security.secureclock</name>
        <version>1</version>
        <fqname>ISecureClock/default</fqname>
    </hal>
    <hal format="aidl">
        <name>android.hardware.security.sharedsecret</name>
        <version>1</version>
        <fqname>ISharedSecret/default</fqname>
    </hal>
    <hal format="aidl">
        <name>android.hardware.gatekeeper</name>
        <version>1</version>
        <fqname>IGatekeeper/default</fqname>
    </hal>
</manifest>
```

For AIDL Weaver, add `android.hardware.weaver` / `IWeaver/<instance>` with its
actual version. If a bridge provides `ISharedSecret/legacy`, add that fqname to
the SharedSecret entry. Preserve declarations for other interfaces registered
by the vendor process, such as RKP. HIDL interfaces cannot become AIDL simply
by changing their manifest names; they still need the correct implementation
and transport configuration.

The module installs as
`/system/etc/vintf/manifest/mydevice-recovery-crypto.xml`, merged by
`VintfObjectRecovery`. Use a separate fragment and preserve existing
health/fastboot fragments. **The manifest only declares services; it does not
start them or supply missing implementations.**

## 8. Write init RC, device node permissions, and device policy

### 8.1 `init.recovery.mydevice-crypto.rc`

Start with the corresponding service RC from normal Android, then adjust paths,
startup timing, users, groups, and SELinux contexts for Recovery. This example
uses `mydevice_rec_keymint` and a separately declared `mydevice_rec_gatekeeper`
executable. `<KEYMINT_DOMAIN>` and `<GATEKEEPER_DOMAIN>` are placeholders that
must be replaced, not valid policy domains.

```rc
on post-fs
    start mydevice-recovery-keymint
    start mydevice-recovery-gatekeeper

service mydevice-recovery-keymint /system/bin/mydevice-recovery-keymint
    class hal
    user system
    group system
    seclabel u:r:<KEYMINT_DOMAIN>:s0
    disabled
    oneshot

service mydevice-recovery-gatekeeper /system/bin/mydevice-recovery-gatekeeper
    class hal
    user system
    group system
    seclabel u:r:<GATEKEEPER_DOMAIN>:s0
    disabled
    oneshot

on property:init.svc.fastbootd=running
    stop mydevice-recovery-gatekeeper
    stop mydevice-recovery-keymint
```

Vendor services may require additional groups, sockets, TEE listeners, and
read-only firmware/persist mounts. Start security services after these mounts
and listeners are ready. Separate SecureClock/Weaver processes must also be
defined, started, and packaged. When using an authentication bridge, replace
the Gatekeeper startup entry with the actual bridge and retain its required
vendor implementation. Do not start two services registering the same instance.

The executable path comes from the module's `stem` and installation location.
For example, `stem: "mydevice-recovery-keymint"` installs as
`/system/bin/mydevice-recovery-keymint`; the RC must not use the module name or
original vendor path instead. Each `start` references an RC service name, not
a Soong module name.

Install the RC under `/system/etc/init/` for automatic discovery by init,
without competing with the existing copy target `/init.recovery.qcom.rc`.
Do not call `mount_all` on the crypto fstab in RC: the backend handles userdata
decryption and mounting. Confirm that init on the actual device loads this
directory, and verify startup through logs; the RC file's presence alone is
insufficient.

### 8.2 `sepolicy/recovery.te`

This is not a complete policy suitable for every vendor. The following provides
a common starting point for reading existing keys and storage. Add Binder,
service registration, device node, firmware, and entry-point permissions for
the actual HAL domains. Step 10's explicit patches and the BoardConfig flag
provide the platform neverallow exceptions for reading keys.

```te
recovery_only(`
  r_dir_file(recovery, metadata_file)
  r_dir_file(recovery, system_data_file)
  r_dir_file(recovery, system_data_root_file)
  r_dir_file(recovery, system_userdir_file)
  r_dir_file(recovery, unencrypted_data_file)
  r_dir_file(recovery, media_rw_data_file)
  allow recovery { vold_metadata_file vold_data_file keystore_data_file }:dir { open read getattr search };
  allow recovery { vold_metadata_file vold_data_file keystore_data_file }:file { open read getattr map };
  allow recovery { system_data_file system_data_root_file system_userdir_file media_rw_data_file }:dir ioctl;
  allowxperm recovery { system_data_file system_data_root_file system_userdir_file media_rw_data_file }:dir ioctl { 0x6616 0x6617 0x661a };
')
```

Select appropriate HAL client attributes, `service_manager find`, and
`binder_call` permissions for the services. Server domains also need their HAL
server roles and registration permissions. Do not grant every domain access to
every file to fix an AVC. Gatekeeper passthrough also requires consideration of
vendor property isolation; granting all vendor permissions to coredomain
Recovery is not a suitable shortcut.

Ramdisk executables may retain a `rootfs` label. Even with an explicit init
`seclabel`, review entry-point execution and domain transition permissions.
Adding a `file_contexts` entry alone does not complete entry-point adaptation.
For diting's current implementation, see the
[common recovery.te](https://github.com/Night-stars-1/uwuaosp-diting/blob/main/cloud/diting_recovery_crypto/common/sepolicy/recovery.te).
Its domains and device nodes belong to that QTI arrangement and must be
replaced for other platforms.

`recovery_only` is an m4 macro: even comments inside the block must not contain
single quotes that break its quoting pairs. Restrict all new permissions to
Recovery. Keep enforcing, retain neverallow rules, and do not add a data-wipe
fallback for decryption failures.

### 8.3 `file_contexts` and `service_contexts`

Reuse existing correct labels; do not add conflicting duplicates. For a new
executable path, add an entry based on reviewed device entry-point policy:

```text
/system/bin/mydevice-recovery-keymint u:object_r:<KEYMINT_EXEC_TYPE>:s0
```

Replace `<KEYMINT_EXEC_TYPE>` with an actual defined file type. The final
ramdisk label, init context, and `.te` entry-point permissions must agree.
Do not reuse incompatible vendor-image entry-point labels. `file_contexts`
does not use the `.te` macro `recovery_only`.

For new service instances, label the actual service name in `service_contexts`.
For example, if a bridge adds a legacy SharedSecret instance not covered by
existing policy:

```text
android.hardware.security.sharedsecret.ISharedSecret/legacy u:object_r:hal_sharedsecret_service:s0
```

This does not itself grant server registration or client access; the
corresponding `.te` rules must also exist.

### 8.4 `ueventd.crypto.rc` and firmware

Check which nodes the normal system's security services actually access, and
their owners, groups, and permissions in Recovery. Create node rules only where
needed; do not make all of `/dev` readable and writable.

QTI devices may need `/dev/qseecom`, DMA heap, or ION, for example, but the actual
nodes, permissions, and groups depend on the device. Install the file at a
separate Recovery path and import it from the **Recovery ueventd configuration
that is actually loaded**, or merge the rules into device-managed configuration.
Do not overwrite a copy target owned by another module. Init RC and ueventd RC
are separate configurations and cannot substitute for each other.

Firmware directories must exist in the actual Recovery ramdisk, with firmware
mounts matching the current slot. If persist is required, verify its read-only
mount and service behavior. Validate libdl loading, firmware/configuration files,
and node permissions even after all `shared_libs` link successfully.

## 9. Integrate with the product and BoardConfig

### 9.1 Create `recovery-crypto/config.mk`

```make
# SPDX-License-Identifier: Apache-2.0
MYDEVICE_RECOVERY_CRYPTO ?= false
```

This device flag defaults to disabled. After reviewing files and services, you
may change the default to `true`, or export `MYDEVICE_RECOVERY_CRYPTO=true` for
a temporary build. Both the product and BoardConfig reference this file so
packaging and policy remain aligned. The device chooses this variable name.

### 9.2 Create `recovery-crypto/crypto.mk`

This example uses the guide's native AIDL Gatekeeper arrangement. First complete
step 5's actual KeyMint, Gatekeeper, and dependency module declarations.
`mydevice_rec_libvendorcrypto` is the placeholder library from above; replace it
with the actual dependencies. Adjust the module list if using an authentication
bridge.

```make
# SPDX-License-Identifier: Apache-2.0
include device/acme/mydevice/recovery-crypto/config.mk

ifeq ($(MYDEVICE_RECOVERY_CRYPTO),true)
SOONG_CONFIG_NAMESPACES += recovery_crypto mydevice_recovery_crypto
SOONG_CONFIG_recovery_crypto += android17
SOONG_CONFIG_recovery_crypto_android17 := true
SOONG_CONFIG_mydevice_recovery_crypto += device_enabled
SOONG_CONFIG_mydevice_recovery_crypto_device_enabled := true

PRODUCT_SOONG_NAMESPACES += \
    device/acme/mydevice/recovery-crypto \
    vendor/acme/mydevice

PRODUCT_PACKAGES += \
    librecovery_crypto_android17 \
    mydevice_rec_crypto_config \
    mydevice_rec_crypto_fstab \
    mydevice_rec_crypto_manifest \
    mydevice_rec_crypto_init \
    mydevice_rec_keymint \
    mydevice_rec_gatekeeper \
    mydevice_rec_libvendorcrypto
endif
```

The vendor modules and authentication bridge use a **device-specific Soong
flag**, separate from the shared backend flag. Enabling the Android 17 backend
on another device therefore does not enable this device's proprietary modules.
The existing diting example uses the shared android17 condition; new devices
should not bind all their proprietary modules to that global flag.

These names are not generated automatically: step 4's `Android.bp` defines the
four configuration modules, step 5's vendor declarations define services and
vendor libraries, and the existing `Android.bp` in this directory defines the
backend.

`PRODUCT_PACKAGES` uses module `name`; init uses service names and installed
executable paths; conf/VINTF uses HAL interface instances. These four kinds of
names are not interchangeable. This list does not select
`recovery_crypto_android17_test`, a normal Android test module run separately
by maintainers.

If using the Qualcomm common layer, append its actual namespace to
`PRODUCT_SOONG_NAMESPACES` and select the actual bridge module and all runtime
dependencies. Put new `PRODUCT_COPY_FILES` entries under the same device flag,
and verify that their Recovery destinations do not conflict with existing files.

Add this to the existing device product file, such as
`device/acme/mydevice/device.mk`:

```make
$(call inherit-product, device/acme/mydevice/recovery-crypto/crypto.mk)
```

### 9.3 Create `recovery-crypto/BoardConfig.mk`

```make
# SPDX-License-Identifier: Apache-2.0
include device/acme/mydevice/recovery-crypto/config.mk

ifeq ($(MYDEVICE_RECOVERY_CRYPTO),true)
BOARD_VENDOR_SEPOLICY_DIRS += device/acme/mydevice/recovery-crypto/sepolicy
BOARD_SEPOLICY_M4DEFS += recovery_crypto_android17=true
endif
```

Add this to the existing device `BoardConfig.mk`:

```make
include device/acme/mydevice/recovery-crypto/BoardConfig.mk
```

If common-layer policy is needed, add its actual policy directory under the
same condition. Do not add the device flag or these includes to Recovery
product configuration shared by every device.

The four configuration points have separate responsibilities:

| Location | Responsibility |
| --- | --- |
| Device flag `MYDEVICE_RECOVERY_CRYPTO` | Controls this device's integration |
| `SOONG_CONFIG_recovery_crypto_android17` | Enables the shared Android 17 backend |
| `SOONG_CONFIG_mydevice_recovery_crypto_device_enabled` | Enables only this device's vendor modules and authentication bridge |
| `BOARD_SEPOLICY_M4DEFS` | Enables the Recovery-only key-read exceptions in the platform patches |

The four ordinary `prebuilt_etc` modules above do not inherit the authentication
bridge's `enabled` flag. Their installation is selected by the product condition
here. Keep device-specific modules and their side effects within that device's
enable condition.

## 10. Prepare platform dependencies and key-read patches

Enter the Android source root first. Replace `/path/to/android` below with the
actual path. Run each command separately and stop on failure before compiling.

Preview:

```bash
python3 bootable/recovery/tools/crypto/prepare_android17.py /path/to/android
```

Apply:

```bash
python3 bootable/recovery/tools/crypto/prepare_android17.py /path/to/android --apply
```

Check:

```bash
python3 bootable/recovery/tools/crypto/prepare_android17.py /path/to/android --check
```

The helper adds AIDL, analyzer, and SQLite Recovery dependency declarations
and checks/applies explicit `system/sepolicy` patches. It does not generate
device HAL configuration, deploy vendor libraries, enable a device, compile,
or flash. It does not require a source hash manifest or deployment record JSON.

After a platform upgrade, maintainers must confirm that SP, vold, and keystore2
formats still match the backend implementation. Resolve dependency declaration
or policy patch conflicts against the actual source; the script does not
overwrite unknown changes.

These changes belong to `hardware/interfaces`, `system/tools/aidl`,
`external/sqlite`, and `system/sepolicy`. When maintaining a distribution,
commit them to the corresponding forks and track them in the manifest, or
review and reapply them after syncing those projects. See the
[key-access patch](../../tools/crypto/patches/android17-recovery-key-access.patch)
for its scope.

## 11. Checks before building and after image packaging

Confirm that the device flag is enabled, all module names resolve, and vendor
dependencies provide the correct Recovery variants. The maintainer then selects
the actual product/release/variant through the ROM's build entry point and
builds Recovery. This guide does not start builds automatically or provide a
flashing command suitable for every partition layout.

**In current uwuAOSP, the Soong-only `recoveryimage` target may produce only a
compressed ramdisk.** A file named `recovery.img` is not necessarily a complete
flashable image. Package it according to the device's header, kernel/ramdisk
partitions, AVB, rollback settings, and current slot layout. Diting's packager
applies only to its reviewed kernel-less v4 layout, not other devices.

Check the actual ramdisk/image to be flashed, rather than potentially stale
product outputs:

| Content | Actual location in this example |
| --- | --- |
| Backend | `/system/lib64/librecovery_crypto_backend.so`; use the corresponding lib directory for 32-bit |
| Worker | `/system/bin/recovery_crypto_worker` |
| Backend configuration | `/system/etc/recovery.crypto.conf` |
| Crypto fstab | `/system/etc/recovery.crypto.fstab` |
| VINTF | `/system/etc/vintf/manifest/mydevice-recovery-crypto.xml` |
| Init RC | `/system/etc/init/init.recovery.mydevice-crypto.rc` |
| KeyMint / Gatekeeper | The actual executable paths configured in RC |
| All libraries, firmware directories, configuration, and node rules | Verify each against the dependency closure and runtime loading paths |
| Policy and properties | Actual file/process labels and matching system/vendor security patch properties |

The worker architecture must match Recovery. The backend and configuration must
be regular files owned by root, without group or other-user write access. The
current private IPC is v2; rebuild Recovery and the worker together. Do not fake
security patch levels to accept old keys, or automatically upgrade blobs in
Recovery.

## 12. Diagnostics and device validation

### Locating an unlock failure

After entering the new Recovery, first check the init services you defined.
Run each command separately on the computer, for example:

```bash
adb -d shell getprop init.svc.mydevice-recovery-keymint
```

```bash
adb -d shell getprop init.svc.mydevice-recovery-gatekeeper
```

For an authentication bridge or TEE listener, use its actual init service name.
An empty value generally means no corresponding service state; `stopped` means
the service is not currently running. Use init logs to distinguish a configuration
that was not loaded, startup failure, or process exit. `running` alone does not
prove that the HAL's key operations work.

Try unlocking once on the device, then retrieve the log **before rebooting
Recovery**:

```bash
adb -d pull /tmp/recovery.log recovery-decrypt.log
```

| Failure stage | What to check first |
| --- | --- |
| `services` | Conf instances, service startup, VINTF, all SharedSecret participants, Binder/SELinux, and firmware |
| `metadata` | Crypto fstab, partition paths, kernel dm-default-key/hardware-key support, and KeyMint operations |
| `de_keys` | Read access to existing DE keys, kernel fscrypt, and matching directory policies |
| `credential_type` | Locksettings/SP formats against the currently reviewed source baseline |
| `gatekeeper_verify` / `weaver_read` | Authentication HALs, input formats, hardware errors, or throttling |
| `sp_unlock` | Earlier SP substages, protector keys, KeyMint, and token processing |
| `ce_load` | Candidate CE keys, wrapped-key conversion, kernel key installation, and media directory policy |

`result=0` means success; 1 unsupported, 2 service unavailable, 3 existing
key/state missing, 4 key upgrade required, 5 credential rejected, 6 hardware
throttling, and 7 other I/O, format, or cryptographic errors.
`source=none code=0` is not a success marker. See the
[diagnostic stage definitions](diagnostic.h) for the complete substages.

Continue diagnosis using the available init, kernel, SELinux AVC, and vendor
service logs. Diagnostics do not depend on logcat. Do not log PINs, patterns,
keys, or authentication tokens, or retry credentials automatically. After a
failure, also verify that cancel, ADB sideload, reboot, and ordinary menus work.

Device validation must cover the supported no-lock, PIN, password, and pattern
paths; incorrect credentials and throttling; reading file names and contents
after success; and continued use of the original credential after returning to
Android. The current UI shows only user 0, supports 3×3 through 6×6 patterns,
and limits on-screen password input to basic ASCII. Device conf cannot expand
these limits.

## 13. Unadapted devices, temporary disabling, and later repo sync

Devices without product and BoardConfig integration do not enable the backend
automatically by syncing Recovery. This example's `config.mk` defaults to false.
To disable it temporarily in the build environment:

```bash
export MYDEVICE_RECOVERY_CRYPTO=false
```

Devices that change the shared default to true must still keep this disable
condition. Disabling must cover added security services, firmware mounts, and
permissions as well as the backend package.

Commit device files, vendor declarations, and platform helper changes to their
respective projects. Later, from the Android source root, run:

```bash
repo sync -c bootable/recovery
```

This updates only the generic code. It neither overwrites device/vendor
adaptations nor automatically applies the helper. Recheck adaptations after
updating other projects. Do not copy device-specific patches into
`bootable/recovery` or discard existing adaptations with repository-wide
reset/clean operations.

## Using the existing diting adaptation

This section applies only to the diting preparation repository; it is not a
generic installer for the new-device example above. With Android sources
synced and the preparation repository up to date, preview from the preparation
repository directory:

```bash
python3 cloud/install_diting_recovery_crypto.py /home/android/uwu-diting
```

Apply:

```bash
python3 cloud/install_diting_recovery_crypto.py /home/android/uwu-diting --apply
```

Check:

```bash
python3 cloud/install_diting_recovery_crypto.py /home/android/uwu-diting --check
```

Diting's `--check` checks actual installed files and dependencies, without
comparing template contents or requiring deployment JSON. You can build after
editing adaptation files directly in the device tree. Changes to templates in
the preparation repository are deployed into Android sources only when you
explicitly run `--apply`, which backs up files before overwriting them.
`--remove` detaches product/BoardConfig integration and preserves adaptation
files.

The maintainer then explicitly builds:

```bash
SOONG_ONLY=true BUILD_JOBS=16 bash cloud/build.sh /home/android/uwu-diting recovery
```

`cloud/build.sh` belongs to the diting **preparation repository**, not
`bootable/recovery`. After deployment, the build entry point only checks the
adaptation; it does not automatically sync or deploy it. See the
[diting adaptation documentation](https://github.com/Night-stars-1/uwuaosp-diting/blob/main/cloud/DITING_RECOVERY_CRYPTO.md)
for full instructions and the managed file list.
The preparation repository is usually `/workspace` in CNB and
`/home/uwuaosp-diting-prep` on the old server. Use the actual source path for
your environment.

## Further references

- [General framework and custom backend ABI](../README.md)
- [Minimal Android.bp template](device.example.bp) and [minimal product selection template](device.example.mk)
  are starting points, without complete HAL/policy integration; complete them
  using the steps in this guide.
- [Configuration field example](profile.example.conf)
