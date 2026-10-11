# Android 17 Recovery 设备解密适配指南

[English](README.md) | 简体中文 | [通用框架](../README.zh-CN.md)

示例统一使用 `device/acme/mydevice`、`vendor/acme/mydevice` 和模块前缀
`mydevice_rec_`。请替换为自己的设备路径和唯一模块名前缀。本文的示例假设有
**真实 AIDL KeyMint、AIDL Gatekeeper，以及 KeyMint 进程提供的 SecureClock /
SharedSecret**；服务组合不同的设备按后面的分支调整。示例中的厂商库、服务路径、
策略域和 fstab 必须来自本设备，不能仅改设备名就直接使用。

## 0. 前提条件

ADB 命令在正常 Android 系统执行；`rg` 在源码根目录执行，`.config` 路径替换为实际路径。

| 条件 | 查询方法 |
| --- | --- |
| 有真实 AIDL KeyMint | `adb -d shell service check android.hardware.security.keymint.IKeyMintDevice/default`；实例名以 VINTF 为准 |
| 能获取实际 Gatekeeper / Weaver 实现 | 见 [Gatekeeper 和 Weaver 查询与配置](#gatekeeper-和-weaver-查询与配置) |
| 内核支持设备实际的 fscrypt、metadata 加密和硬件封装密钥路径 | `rg 'CONFIG_.*(ENCRYPTION\|DEFAULT_KEY\|CRYPTO)' path/to/kernel/.config`<br>`adb -d shell "cat /vendor/etc/fstab.*"`；按 fstab 核对配置，使用 wrapped key 时还需核对[存储驱动支持](https://source.android.com/docs/security/features/encryption/hw-wrapped-keys) |

## 1. 要创建哪些文件

设备适配目录建议如下。真实文件名要与后面的 `src`、include 和安装声明一致：

```text
device/acme/mydevice/
  BoardConfig.mk                  ← 已有文件，增加一个 include
  device.mk                       ← 已有产品文件，增加一个 inherit-product
  recovery-crypto/
    config.mk                     ← 本设备开关，两处接入共用
    crypto.mk                     ← 选择要打包的模块
    BoardConfig.mk                ← 接入设备策略与平台密钥读取开关
    Android.bp                    ← 定义配置文件模块；需要时定义认证桥
    recovery.crypto.conf          ← 后端使用哪个安全服务
    recovery.crypto.fstab         ← metadata / userdata 配置
    manifest.xml                  ← Recovery 安全 HAL 声明
    init.recovery.mydevice-crypto.rc ← 安全服务的启动流程
    sepolicy/
      recovery.te                 ← 设备所需权限
      file_contexts               ← 仅在需要新增文件标签时创建
      service_contexts            ← 仅在需要新增服务实例标签时创建
    ueventd.crypto.rc             ← 仅在现有节点权限不足时创建并接入

vendor/acme/mydevice/
  Android.bp                      ← 通常已有 namespace，不覆盖自动生成内容
  proprietary/
    Android.bp                    ← 声明 Recovery 专用厂商模块，不覆盖已有内容
    vendor/bin/hw/...             ← 已提取的安全服务
    vendor/lib64/...              ← 已提取的依赖库
```

| 文件 | 是否需要 | 作用与内容来源 |
| --- | --- | --- |
| `recovery.crypto.conf` | 必需 | 后端服务选择；来源为实际 HAL 和正常 keystore2 的 SharedSecret 协商参与者 |
| `recovery.crypto.fstab` | 必需 | 分区及加密配置；来源为正常系统的 `/metadata`、`/data` fstab 条目 |
| `Android.bp` | 必需 | 定义模块以及安装位置；由维护者编写 |
| 产品 `.mk` / BoardConfig 接入 | 必需 | 显式选择模块和策略；由维护者编写，示例将其拆成三个 `.mk` 文件 |
| `manifest.xml` | 相应声明必须存在 | 若已有 Recovery 片段已声明全部所需 HAL，可复用；否则新增片段 |
| init RC | 相应启动配置必须存在 | 若已有 Recovery RC 已正确启动全部服务，可复用；否则新增 |
| 厂商 Recovery 模块 | 依设备而定 | 已有模块提供正确 Recovery 变体时复用，否则为专有程序和库声明新模块 |
| `sepolicy/` | 所需权限必须具备 | 补充现有策略未提供的权限；不能凭空复制其他设备的域和设备节点 |
| `auth_service.cpp` 或公共层认证桥 | 可选 | 只有实际服务需要桥接时才添加，不是每台设备都必须有 |
| ueventd、固件、persist 挂载配置 | 依设备而定 | 来自本设备安全服务实际需要的节点、固件路径和启动顺序 |

下面依次编写这些文件。平台 helper 修改的依赖和 SELinux 补丁属于其他平台项目，
不是放在这个设备目录里的文件，见步骤 10。

## 2. 编写 `recovery.crypto.conf`

完整配置示例：

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

| 字段 | 如何填写 |
| --- | --- |
| `profile_version`、`platform_sdk` | 当前解析器只接受 `1`、`37`；不能修改数字来宣称支持其他 Android 版本 |
| `storage_binding` | 当前只接受 `none`，填写前必须确认实际平台未使用 storage-binding seed |
| `fstab` | Recovery 内的独立 crypto fstab 路径，必须在 `/system/etc/` 下 |
| `keymint_service` | `android.hardware.security.keymint.IKeyMintDevice/` 加真实实例名 |
| `security_level` | 该密钥使用的真实安全级别，只接受 `tee` 或 `strongbox` |
| `gatekeeper_transport`、`weaver_transport` | `aidl`、`hidl` 或 `none`，由实际认证实现决定 |
| `gatekeeper_instance`、`weaver_instance` | 使用相应服务时填写实际实例名，例如 `default`；`none` 时必须留空 |
| `secureclock_service` | 实际 SecureClock 完整服务名；仅在确认不需要时留空 |
| `sharedsecret_services` | 逗号分隔的 AIDL SharedSecret 完整服务名，至少一个，不重复、不加空格 |
| `sharedsecret_hidl_instances` | 可选；例如 `4.1/default`，仅支持 `4.0/4.1` 的 `default/strongbox` 实例 |

### Gatekeeper 和 Weaver 查询与配置

在正常 Android 系统执行，分别查看 AIDL 和 HIDL 实现：

```bash
adb -d shell service list
```

```bash
adb -d shell lshal
```

查找 `gatekeeper`、`weaver`，常见输出如下；实例名取最后一个 `/` 后面的部分：

```text
android.hardware.gatekeeper.IGatekeeper/default
android.hardware.weaver.IWeaver/default
android.hardware.gatekeeper@1.0::IGatekeeper/default
android.hardware.weaver@1.0::IWeaver/default
```

在 `device/<厂商>/<设备>/recovery-crypto/recovery.crypto.conf` 中修改：

| 找到的实现 | 配置 |
| --- | --- |
| AIDL Gatekeeper | `gatekeeper_transport=aidl`，`gatekeeper_instance=实际实例名` |
| HIDL Gatekeeper | `gatekeeper_transport=hidl`，`gatekeeper_instance=实际实例名` |
| AIDL / HIDL Weaver | `weaver_transport=aidl` 或 `hidl`，`weaver_instance=实际实例名` |
| 当前凭据没有使用 Weaver | `weaver_transport=none`，`weaver_instance=` |

例如实例名为 `default`，填写 `gatekeeper_instance=default` 或
`weaver_instance=default`。还需核对厂商实现；没查到服务不代表凭据没有使用 Weaver。

需要 HIDL 时，要确认 Recovery 的本地 passthrough 实现实际存在；只有正常
Android 的 HIDL 服务进程或 VINTF 声明还不够。没有可用 passthrough 的设备需要
自己的桥接，见步骤 6。

SharedSecret 要保留正常系统的全部参与者。若设备通过桥接提供 legacy 实例，
例如 diting，可填写：

```ini
sharedsecret_services=android.hardware.security.sharedsecret.ISharedSecret/default,android.hardware.security.sharedsecret.ISharedSecret/legacy
```

如果使用可直接调用的 HIDL Keymaster passthrough，保留实际 AIDL 参与者，并增加：

```ini
sharedsecret_hidl_instances=4.1/default
```

同一硬件参与者不要通过 AIDL 桥和 HIDL 再各列一次；同一 HIDL 实例不要同时列
4.0 和 4.1。AIDL 与 HIDL 参与者总计最多四个。使用 Weaver 不意味着 Gatekeeper
一定可以关闭，SP SID 的验证仍可能需要它。

## 3. 编写 `recovery.crypto.fstab`

`recovery.crypto.fstab` 用来告诉解密后端，设备的数据分区在哪里，以及使用什么加密方式。

下面是 diting 的示例，其他设备必须替换为自己的条目：

```text
/dev/block/bootdevice/by-name/metadata /metadata ext4 noatime,nosuid,nodev wait
/dev/block/bootdevice/by-name/userdata /data f2fs noatime,nosuid,nodev,reserve_root=32768,resgid=1065,fsync_mode=nobarrier,inlinecrypt wait,fileencryption=aes-256-xts:aes-256-cts:v2+inlinecrypt_optimized+wrappedkey_v0,keydirectory=/metadata/vold/metadata_encryption,metadata_encryption=aes-256-xts:wrappedkey_v0
```

每个分区条目必须保持一整行；不要将上述长行拆成多行。fstab 五列依次为：

```text
块设备路径  挂载点  文件系统  Linux挂载选项  Android fs_mgr选项
```

## 4. 编写设备 `Android.bp`

先定义四个配置模块。下面可以作为设备文件的起点；若复用已有 init/VINTF，
删除对应重复模块，同时在产品清单里删除同名条目：

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

| 属性 | 填写规则 |
| --- | --- |
| `name` | 唯一构建模块名，后面的 `PRODUCT_PACKAGES` 引用这个名字 |
| `src` | 相对当前 `Android.bp` 的源文件路径，文件必须存在 |
| `filename` | Recovery 内实际文件名，不一定与模块名相同 |
| `relative_install_path` | `prebuilt_etc` 安装目录下的子目录 |
| `recovery: true` | 安装到 Recovery，不是正常系统分区 |
| `imports` | 未限定模块名的查找范围，指向实际声明 `soong_namespace` 的目录 |

四个文件分别安装到 Recovery 的 `/system/etc/`、`/system/etc/vintf/manifest/`
和 `/system/etc/init/`。`Android.bp` 只定义模块；没有被选中就不会因此打包，
服务也不会因此启动。设备模块的选择见步骤 9。

## 5. 声明厂商安全服务和完整库依赖

先找出正常系统对应的可执行程序、库和依赖。如果现有模块已经提供可用的
Recovery 变体，优先复用，不重复创建。否则在 vendor 项目中为原二进制声明
Recovery 专用模块；不要把不兼容的新版本库替换进原来的系统 HAL。

例如 `vendor/acme/mydevice/proprietary/Android.bp`。下例中二进制路径、库名和
依赖都是**占位示例**，必须用本设备 ELF 的真实内容替换。不要覆盖 vendor
自动生成的主文件；已有同路径文件时保存现有模块并追加/维护专用声明。

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

`srcs` 相对这个模块文件所在目录；模块放在 `proprietary/` 才能使用示例中的
`vendor/...` 路径。`stem` 应保留库的实际文件名，不能把依赖所需的
`libvendorcrypto.so` 安装成模块名对应的另一个文件。HAL passthrough 库还可能
需要 `relative_install_path: "hw"`。上述 64 位配置也要按实际服务位数调整。

按同样方式声明实际 Gatekeeper、需要的 Weaver、独立 SecureClock、TEE listener
等程序，以及它们递归需要的厂商库。检查 ELF 的 `DT_NEEDED`，将每个依赖映射到
真实 Soong 模块，并确认该模块有对应 Recovery/架构变体；还要核对运行时
`dlopen` 的库、配置文件和固件，它们不会全部出现在 `DT_NEEDED` 中。

`shared_libs` 写的是模块名，不是 `.so` 文件路径。平台 Binder/HIDL/AIDL 库和
厂商预编译库的接口版本也要匹配。不要照搬 diting 的二十多个库，也不要用
`check_elf_files: false` 掩盖未解决的依赖。

确认 vendor 祖先目录存在有效 namespace；设备 `imports` 和产品
`PRODUCT_SOONG_NAMESPACES` 都引用它。例如已有 namespace 在
`vendor/acme/mydevice/Android.bp`，无需在 `proprietary/` 再声明一个不同 namespace。

## 6. 是否需要 `auth_service.cpp` / 高通认证桥

如果 Recovery 已能直接访问配置中的原生 AIDL Gatekeeper 和 SharedSecret，
**不需要认证桥模块**。有实际可用的 HIDL passthrough 时，也可按步骤 2 配置 HIDL。

当前 diting 使用设备桥接，将 QTI HIDL Gatekeeper 1.0 和旧 Keymaster 的共享秘密
协商提供成 AIDL 服务。采用相同桥接方案前，必须核对 HAL ABI、factory 原型、
固件与 vendor 属性依赖。公共层源码和规则由设备维护项目提供，不包含在本
Recovery 仓库中。具体参考：

- [高通公共源码和模块定义](https://github.com/Night-stars-1/uwuaosp-diting/tree/main/cloud/diting_recovery_crypto/common)
- [diting 设备 Android.bp](https://github.com/Night-stars-1/uwuaosp-diting/blob/main/cloud/diting_recovery_crypto/Android.bp)

如果已经在 Android 源码中部署了 `device/qcom/recovery-crypto-common`，可以在
设备 namespace 的 `imports` 加入它，并追加以下认证桥声明；库模块要先按步骤 5
定义，不能保留不存在的示例名称：

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

第一段定义可被本设备 `mydevice_recovery_crypto.device_enabled` 控制的 defaults
类型，第二段设置
默认关闭/开启后的依赖，第三段才生成程序。公共 defaults 通过公共目录的
`filegroup` 提供 C++ 源码，避免 Soong 到设备目录错误查找 `auth_service.cpp`。

增加此桥后，还要把模块加入产品清单、在 init 启动它、在 VINTF 声明它注册的
接口，并更新 conf 的 Gatekeeper/SharedSecret 实例。不能只添加 `cc_binary`。
高通公共层不能替代各设备专有库和运行条件的适配。

## 7. 编写 `manifest.xml`

下面对应开头的示例组合：KeyMint、SecureClock、SharedSecret 和原生 AIDL
Gatekeeper。版本和实例必须按本设备实际服务修改；构建链接的 `*-V5-ndk` 库名
不代表厂商进程一定实现 V5 HAL。

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

有 AIDL Weaver 时新增它实际版本的 `android.hardware.weaver` / `IWeaver/<实例>`
声明。桥接提供 `ISharedSecret/legacy` 时，在 SharedSecret 条目加入相应 fqname。
厂商进程若还注册 RKP 等接口，也需保留相应声明。采用 HIDL 时不能把接口名字
随意改成 AIDL 声明，仍需正确的实现与传输配置。

该模块安装成 `/system/etc/vintf/manifest/mydevice-recovery-crypto.xml`，由
`VintfObjectRecovery` 合并。用独立文件，不覆盖 health/fastboot 等已有片段。
**manifest 只声明服务，不会启动服务或提供缺失的实现。**

## 8. 编写 init RC、节点权限和设备策略

### 8.1 `init.recovery.mydevice-crypto.rc`

先参考正常系统对应服务的 RC，再针对 Recovery 调整路径、启动时机、用户、组
和 SELinux 上下文。下例使用 `mydevice_rec_keymint` 和另行声明的
`mydevice_rec_gatekeeper` 两个程序；`<KEYMINT_DOMAIN>`、`<GATEKEEPER_DOMAIN>`
是必须替换的占位符，不是有效的策略域。

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

实际厂商服务可能需要额外组、socket、TEE listener 和固件/persist 的只读挂载。
应在相关挂载和 listener 准备好后再启动安全服务；独立 SecureClock/Weaver
进程也需定义、启动和打包。使用认证桥时，将 Gatekeeper 的启动项替换为真实
认证桥，并保留它需要的厂商实现。不要同时启动两个注册同一实例的服务。

程序路径由模块的 `stem` 和安装位置决定。例如 `stem: "mydevice-recovery-keymint"`
安装为 `/system/bin/mydevice-recovery-keymint`，RC 不能写模块名或原 vendor 路径。
每个 `start` 指向 RC 的 service 名，不是 Soong 模块名。

RC 安装到 `/system/etc/init/` 以便 init 自动发现，不与已有
`/init.recovery.qcom.rc` 争抢相同复制目标。不要在 RC 中对 crypto fstab 调用
`mount_all`，userdata 解密挂载由后端执行。确认实际设备的 init 会加载这个目录，
并从日志验证服务启动，不能只检查 RC 文件存在。

### 8.2 `sepolicy/recovery.te`

这不是一份所有厂商都能直接使用的完整策略。下面给出读取已有密钥与存储的
公共起点；还需要按真实 HAL 域补充 Binder、服务注册、设备节点、固件和入口权限。
平台密钥读取 neverallow 例外由步骤 10 的明确补丁与 BoardConfig 开关处理。

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

根据服务选择正确 HAL 客户端属性和 `service_manager find`、`binder_call` 权限；
服务端域还需要其 HAL 服务端角色与注册权限。不要因某个 AVC 就授予所有 domain
访问所有文件。使用 Gatekeeper passthrough 时，还需考虑 vendor 属性隔离，不能
简单给 coredomain Recovery 加上全部 vendor 权限。

ramdisk 程序可能保持 `rootfs` 标签，init 使用显式 `seclabel` 时仍需要审查正确
的入口执行和域切换权限。不能仅添加一条 `file_contexts` 就假定已完成入口适配。
当前 diting 的实际做法见
[公共 recovery.te](https://github.com/Night-stars-1/uwuaosp-diting/blob/main/cloud/diting_recovery_crypto/common/sepolicy/recovery.te)。
其域和设备节点属于该 QTI 方案，其他平台需替换，不能原样套用。

`recovery_only` 是 m4 宏：块内注释也不能含破坏配对的单引号。所有新权限限制
在 Recovery 中；保持 enforcing，不移除 neverallow，也不为失败添加清除数据回退。

### 8.3 `file_contexts` 和 `service_contexts`

已有正确标签就复用，不重复声明冲突规则。新增可执行路径时，可以按已审查的
设备入口策略添加：

```text
/system/bin/mydevice-recovery-keymint u:object_r:<KEYMINT_EXEC_TYPE>:s0
```

这里的 `<KEYMINT_EXEC_TYPE>` 必须换成实际已定义的文件类型，且最终 ramdisk
标签、init 上下文和 `.te` 入口权限需一致。不要套用不兼容的 vendor 镜像入口标签。
`file_contexts` 不使用 `.te` 的 `recovery_only` 宏。

新增服务实例时，在 `service_contexts` 为真实服务名配置标签。例如桥接增加
legacy SharedSecret，而现有策略尚未覆盖它时：

```text
android.hardware.security.sharedsecret.ISharedSecret/legacy u:object_r:hal_sharedsecret_service:s0
```

这仍不授予服务端注册和客户端访问权限，对应 `.te` 规则要同时具备。

### 8.4 `ueventd.crypto.rc` 与固件

检查正常系统安全服务实际访问哪些节点，以及 Recovery 下的属主、组、权限。
只有需要补充时才创建节点规则；不要将所有 `/dev` 设为可读写。

例如 QTI 设备可能需要 `/dev/qseecom`、DMA heap、ION 等，但具体节点、权限和
用户组由本设备决定。将文件安装到独立 Recovery 路径，并在**实际已加载的
Recovery ueventd 配置**中导入它，或者合并进设备自己管理的规则；不要覆盖另一
模块拥有的同名复制目标。init RC 和 ueventd RC 是两套不同配置，不能互相代替。

固件目录必须在真实 Recovery ramdisk 中存在，挂载当前槽匹配的固件。需要
persist 时核对只读挂载与服务行为。libdl 加载、firmware/config 文件和节点
权限都要验证，即使全部 `shared_libs` 已经链接通过。

## 9. 接入产品和 BoardConfig

### 9.1 创建 `recovery-crypto/config.mk`

```make
# SPDX-License-Identifier: Apache-2.0
MYDEVICE_RECOVERY_CRYPTO ?= false
```

该设备开关默认关闭。完成文件和服务审查后，可在此将默认值改为 `true`；临时
构建也可导出 `MYDEVICE_RECOVERY_CRYPTO=true`。产品与 BoardConfig 都引用同一
文件，避免一边打包而另一边没有策略。这里的变量名由设备自定。

### 9.2 创建 `recovery-crypto/crypto.mk`

下例对应本指南的原生 AIDL Gatekeeper 方案：先完成步骤 5 中 KeyMint、Gatekeeper
和全部依赖的真实模块声明。`mydevice_rec_libvendorcrypto` 是前面占位库模块，
要改成实际依赖；如果用认证桥，替换相应模块清单。

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

示例为厂商模块和认证桥使用**设备自己的 Soong 开关**，与共享后端开关分开。
这样其他机型启用 Android 17 后端时，不会连带启用本设备的专有模块。已有 diting
示例使用共享的 android17 条件；新设备不要把所有专有模块都绑定到那个全局开关。

这些名称不是系统自动产生的：四个配置模块来自步骤 4 的 `Android.bp`，服务
和厂商库来自步骤 5 的 vendor 声明，后端来自本目录已有 `Android.bp`。

`PRODUCT_PACKAGES` 使用模块 `name`，init 使用 service 名与安装后的程序路径，
conf/VINTF 使用 HAL 接口实例；这是四类名称，不能混用。这里没有选择
`recovery_crypto_android17_test`，它是维护者单独运行的普通 Android 测试模块。

需要高通公共层时，在 `PRODUCT_SOONG_NAMESPACES` 追加实际公共 namespace，
并选择真实认证桥模块和全部运行依赖。新增 `PRODUCT_COPY_FILES` 也放在同一
设备开关条件内，且必须检查其 Recovery 目标没有和已有文件冲突。

在已有设备产品文件（例如 `device/acme/mydevice/device.mk`）加入：

```make
$(call inherit-product, device/acme/mydevice/recovery-crypto/crypto.mk)
```

### 9.3 创建 `recovery-crypto/BoardConfig.mk`

```make
# SPDX-License-Identifier: Apache-2.0
include device/acme/mydevice/recovery-crypto/config.mk

ifeq ($(MYDEVICE_RECOVERY_CRYPTO),true)
BOARD_VENDOR_SEPOLICY_DIRS += device/acme/mydevice/recovery-crypto/sepolicy
BOARD_SEPOLICY_M4DEFS += recovery_crypto_android17=true
endif
```

在已有设备 `BoardConfig.mk` 加入：

```make
include device/acme/mydevice/recovery-crypto/BoardConfig.mk
```

需要公共层策略时，在相同条件内加入该实际策略目录。不要将设备开关或这些
include 加进所有设备共用的 Recovery 产品配置。

四处配置的职责是：

| 位置 | 职责 |
| --- | --- |
| 设备开关 `MYDEVICE_RECOVERY_CRYPTO` | 决定本设备是否接入 |
| `SOONG_CONFIG_recovery_crypto_android17` | 开启共享 Android 17 后端 |
| `SOONG_CONFIG_mydevice_recovery_crypto_device_enabled` | 仅开启本设备的厂商模块与认证桥 |
| `BOARD_SEPOLICY_M4DEFS` | 开启平台补丁中的 Recovery 专用密钥读取例外 |

前面的四个普通 `prebuilt_etc` 没有继承认证桥的 `enabled` 开关，它们由这里的
产品条件决定是否安装。保持设备专用模块及其副作用均在该设备的启用条件内。

## 10. 准备平台依赖和密钥读取补丁

先进入 Android 源码根目录；下面的 `/path/to/android` 必须替换为实际路径。
每条命令单独运行，失败后停止，不继续编译。

预览：

```bash
python3 bootable/recovery/tools/crypto/prepare_android17.py /path/to/android
```

应用：

```bash
python3 bootable/recovery/tools/crypto/prepare_android17.py /path/to/android --apply
```

检查：

```bash
python3 bootable/recovery/tools/crypto/prepare_android17.py /path/to/android --check
```

helper 补充 AIDL、analyzer 和 SQLite Recovery 依赖声明，并检查/应用明确的
`system/sepolicy` 补丁。它不生成设备 HAL 配置、不部署厂商库、不启用设备，
也不编译或刷机。脚本不要求创建源码哈希清单或部署记录 JSON。

平台升级后，维护者应确认 SP、vold、keystore2 的格式与后端实现仍匹配。
依赖声明或策略补丁冲突时，需要按实际源码调整，脚本不会覆盖未知改动。

这些改动分别属于 `hardware/interfaces`、`system/tools/aidl`、`external/sqlite`
和 `system/sepolicy`。维护发行版时提交到相应 fork，并由 manifest 跟踪；
或在同步这些项目后重新审查和应用。具体补丁范围见 [密钥访问补丁](../../tools/crypto/patches/android17-recovery-key-access.patch)。

## 11. 编译前和镜像打包后检查什么

先确认设备开关已经启用、所有模块名可解析、厂商依赖有正确 Recovery 变体。
再由维护者使用当前 ROM 的构建入口选择实际产品/版本/变体，编译 Recovery。
本指南不自动启动编译，也不提供一条适合所有设备布局的刷入命令。

**Soong-only 的 `recoveryimage` 目标在当前 uwuAOSP 中可能只生成压缩 ramdisk。**
不能仅因文件名叫 `recovery.img` 就当成完整可刷镜像；需按本设备 header、内核/
ramdisk 所在分区、AVB、rollback 和当前槽布局完成打包。diting 的打包器仅适用
于其已审查的无内核 v4 布局，不能直接用于其他设备。

在实际待刷的 ramdisk/镜像中核对以下内容，不能只看可能残留的旧 product 输出：

| 内容 | 本指南示例的实际位置 |
| --- | --- |
| 后端 | `/system/lib64/librecovery_crypto_backend.so`，32 位使用对应 lib 目录 |
| worker | `/system/bin/recovery_crypto_worker` |
| 后端配置 | `/system/etc/recovery.crypto.conf` |
| crypto fstab | `/system/etc/recovery.crypto.fstab` |
| VINTF | `/system/etc/vintf/manifest/mydevice-recovery-crypto.xml` |
| init RC | `/system/etc/init/init.recovery.mydevice-crypto.rc` |
| KeyMint / Gatekeeper | RC 中配置的真实可执行路径 |
| 全部库、固件目录、配置、节点规则 | 根据依赖闭包及运行时加载路径逐项确认 |
| 策略及属性 | 真实文件/进程标签、匹配的系统和 vendor 安全补丁等属性 |

worker 位数须与 Recovery 一致；后端与配置必须是 root 拥有的普通文件，不允许
组或其他用户写入。当前私有 IPC 为 v2，Recovery 与 worker 要一起重编译。
不要伪造安全补丁等级让旧密钥通过，也不在 Recovery 中自动升级 blob。

## 12. 诊断与真机验证

### 解锁失败怎么定位

进入新 Recovery 后，先核对自己定义的 init service 状态。例如下面命令在电脑
执行，每条分别运行：

```bash
adb -d shell getprop init.svc.mydevice-recovery-keymint
```

```bash
adb -d shell getprop init.svc.mydevice-recovery-gatekeeper
```

如果使用认证桥或 TEE listener，改为它们实际的 init service 名。空值一般表示
没有对应服务状态，`stopped` 表示当前未运行；需要结合 init 日志确认未加载、
启动失败或进程退出的具体原因。`running` 也不能单独证明 HAL 的密钥操作正常。

在手机尝试一次解锁，**重启 Recovery 前**读取：

```bash
adb -d pull /tmp/recovery.log recovery-decrypt.log
```

| 失败阶段 | 先检查什么 |
| --- | --- |
| `services` | conf 实例、服务启动、VINTF、SharedSecret 全部参与者、Binder/SELinux、固件 |
| `metadata` | crypto fstab、分区路径、内核 dm-default-key/硬件密钥能力、KeyMint 操作 |
| `de_keys` | 已有 DE 密钥读取权限、内核 fscrypt 和目录策略匹配 |
| `credential_type` | locksettings/SP 格式与当前源码审查基线 |
| `gatekeeper_verify` / `weaver_read` | 认证 HAL、输入格式、硬件错误或限流 |
| `sp_unlock` | 前面 SP 子阶段、protector 密钥、KeyMint 和令牌处理 |
| `ce_load` | CE 候选密钥、wrapped key 转换、内核安装及 media 目录策略 |

`result=0` 为成功，1 不支持，2 服务不可用，3 已有密钥/状态缺失，4 需升级密钥，
5 凭据拒绝，6 硬件限流，7 其他 I/O/格式/密码学错误。`source=none code=0` 不是
成功标记。完整子阶段含义见 [诊断阶段定义](diagnostic.h)。

按实际设备能收集到的 init、内核、SELinux AVC 和厂商服务日志继续定位；诊断
不依赖 logcat。不要在日志加入 PIN、图案、密钥或认证令牌，也不自动反复尝试
凭据。失败后仍要验证取消、ADB sideload、重启和普通菜单能使用。

真机验收至少包括实际支持的空锁屏、PIN、密码、图案路径，错误凭据与限流，
成功后的文件名/内容读取，以及返回 Android 后原凭据仍可用。当前 UI 只展示
用户 0，图案支持 3×3～6×6，屏幕密码输入限基本 ASCII；这些限制不能靠设备
conf 扩大。

## 13. 未适配、临时关闭和后续 repo sync

没有在产品和 BoardConfig 接入的设备不会因同步 Recovery 自动启用后端。
本示例 `config.mk` 默认 false，临时关闭也可在构建环境使用：

```bash
export MYDEVICE_RECOVERY_CRYPTO=false
```

共享默认值改为 true 的设备同样需保留这个可关闭条件。关闭时不能只从包清单
删除后端，却保留新增安全服务、固件挂载和无条件权限。

将设备目录、vendor 声明和平台 helper 的改动分别提交到所属项目。之后在
Android 源码根目录执行：

```bash
repo sync -c bootable/recovery
```

只更新通用代码，不覆盖 device/vendor 适配，也不自动应用 helper。更新其他
项目后重新检查其适配；不要向 `bootable/recovery` 复制设备专用补丁，也不使用
整仓 reset/clean 丢弃已有适配。

## diting 已有适配怎么使用

这部分只适用于 diting 准备仓库，不是上述新设备的通用安装器。Android 源码
已同步，且准备仓库为当前版本时，从准备仓库目录预览：

```bash
python3 cloud/install_diting_recovery_crypto.py /home/android/uwu-diting
```

应用：

```bash
python3 cloud/install_diting_recovery_crypto.py /home/android/uwu-diting --apply
```

检查：

```bash
python3 cloud/install_diting_recovery_crypto.py /home/android/uwu-diting --check
```

Diting 的 `--check` 检查已接入的实际文件与依赖，不比较模板内容，也不需要
部署 JSON。直接修改设备树适配文件后可编译；修改准备仓库中的模板时，只有主动
执行 `--apply` 才会部署到 Android 源码，覆盖前会备份。`--remove` 取消产品/
BoardConfig 接入并保留适配文件。

之后由维护者显式编译：

```bash
SOONG_ONLY=true BUILD_JOBS=16 bash cloud/build.sh /home/android/uwu-diting recovery
```

`cloud/build.sh` 位于 diting **准备仓库**，不在 `bootable/recovery` 中。已部署后
构建入口只检查，不自动同步或部署解密适配。完整说明和受管文件列表见
[diting 适配文档](https://github.com/Night-stars-1/uwuaosp-diting/blob/main/cloud/DITING_RECOVERY_CRYPTO.md)。
CNB 中准备仓库通常为 `/workspace`，旧服务器为 `/home/uwuaosp-diting-prep`；
实际源码路径以自己的环境为准。

## 后续参考

- [通用框架与自定义后端 ABI](../README.zh-CN.md)
- [最小 Android.bp 模板](device.example.bp)和[最小产品选择模板](device.example.mk)
  仅提供起点，不包含完整 HAL/策略；以本文步骤补齐。
- [配置字段示例](profile.example.conf)
