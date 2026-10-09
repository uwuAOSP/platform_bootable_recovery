# Recovery 可选存储解密框架

[English](README.md) | 简体中文 | [Android 17 后端与适配指南](android17/README.zh-CN.md)

**开始适配新设备，请先阅读 [逐文件设备适配指南](android17/README.zh-CN.md)。**
该指南提供文件清单、完整配置示例、产品/BoardConfig 接入和检查步骤；本文件说明
通用框架及自定义后端 ABI。

本目录提供可选的**后端集成框架**，并包含默认关闭的
[Android 17 已有密钥恢复后端](android17/README.zh-CN.md)。后端实现已审查格式的
metadata、DE、Synthetic Password 和 CE 恢复；每个设备仍需自己的配置、厂商
安全服务和真机验证。编译或源码检查不能证明设备解密成功。本仓库不默认启用
任何设备的解密配置，旧 `examples/` 回调示例只返回“不支持”，不能用于解锁。

框架负责隔离后端执行、有界 IPC、凭据输入、分阶段解锁、独立 fscrypt 密钥
校验和已有 ZIP 安装入口。框架本身不解析 SP blob、不恢复密钥、不启动厂商
HAL，也不绕过认证；这些由兼容相应 Android 版本的后端实现。

## 通用代码与设备代码的边界

```text
bootable/recovery/crypto/               通用框架、ABI、worker 和可选后端
device/<厂商>/<设备>/recovery-crypto/   设备配置、fstab、init、VINTF、SELinux
device/qcom/recovery-crypto-common/     可选高通公共适配，由设备项目管理
vendor/<厂商>/<设备>/                  厂商安全服务、专有库及固件
system/vold/                           对应平台的加密实现，供源码审查
system/security/                       对应平台的 keystore2 实现
```

框架不包含小米路径、服务名、密钥 blob、内核地址或硬编码的加密算法，也不
直接依赖某个设备的硬件库。设备无需修改 Recovery 源码即可注册后端。

没有设备适配时，不启动安全服务或显示新增解锁提示，普通 ADB sideload、可移动
存储安装、设置、清除数据和重启保持可用。安装后端后，普通交互式进入 Recovery
会准备 metadata/DE 并询问已有锁屏凭据：图案用图形网格，PIN/密码用输入界面；
确认没有锁屏凭据的 protector 无需输入即可解锁。已有可访问存储会跳过重复准备。

取消或失败后进入主界面，不阻塞其他功能。“安装更新”提供内部存储 ZIP 入口，
每次核对内核密钥和真实目录访问状态，复用启动时已成功的解锁；否则可再次
尝试。缓存布尔值不作为成功依据。命令驱动的 OTA、ADB sideload、wipe、rescue、
`--just_exit` 和无界面/静默启动跳过交互提示，不自动试密码，保留硬件限流。

文件浏览器目前只展示主用户（用户 0）存储；ABI 为将来多用户调用保留用户 ID。
不支持工作资料挑战和可采纳存储。支持 PIN、Android 图案单元和基本 ASCII
密码；非 ASCII 密码需要扩展输入。图案支持每个用户保存的 3×3、4×4、5×5、
6×6 网格，不自动尝试多个尺寸。

## 注册后端

使用受支持的 Android 17 格式时，直接采用
[Android 17 后端、依赖 helper 与设备打包模板](android17/README.zh-CN.md)。
共享实现留在 Recovery，设备提供配置和 HAL 打包，无需为每台设备重写解密回调。

为其他平台或厂商实现独立后端时：

1. 将 `examples/backend.cpp.example` 和 `examples/Android.bp.example` 复制到
   设备树，命名为 `backend.cpp`、`Android.bp`；以已审查的平台实现替换返回
   “不支持”的回调。示例不含密码学操作，不能直接启用发布。
2. 模块名称应唯一，但安装文件 stem 保持 `librecovery_crypto_backend`。
   每个产品只安装一个后端。
3. 在设备产品配置中加入模块：

   ```make
   PRODUCT_PACKAGES += librecovery_crypto_mydevice
   ```

4. 在设备树打包依赖的 Recovery 变体、匹配的厂商服务、VINTF 和 init/SELinux。
   后端安装到 `/system/lib64/librecovery_crypto_backend.so`；32 位为
   `/system/lib/librecovery_crypto_backend.so`。worker 与 Recovery 的位数相同。
   后端和 worker 必须为 root 拥有的普通文件，不能允许 group/world 写入。

框架不根据属性、ZIP 或 userdata 选择库，也不扫描加载全部 vendor 库。缺少
文件时隐藏相应新增入口；ABI 错误、缺失依赖、崩溃或超时显示失败并返回菜单。
解锁失败不会自动清除数据。

## 后端 ABI 与操作要求

[`include/recovery_crypto/backend.h`](include/recovery_crypto/backend.h) 是版本化
C ABI，不能跨 ABI 暴露 C++ STL 对象。全部回调在同一 worker 会话执行，保留
metadata、DE、CE 阶段之间所需的平台库状态。

v1 结构与必需导出保持原布局。后端可额外导出
`recovery_crypto_get_pattern_size_v1(user, size)`，在凭据发现后返回保存的 3～6
网格尺寸。没有此导出时保留原 3×3 契约。IPC 两端都使用协议 v2，必须一起
重编译，拒绝旧 worker。网格尺寸绑定会话，不能由解锁请求修改。ABI 的图案
单元是从 0 开始的行优先字节，不是 ASCII 十进制字符串。

| 回调 | 设备后端需要完成的工作 |
| --- | --- |
| `prepare_services` | 有界查找实际 KeyMint/Keymaster、Gatekeeper/Weaver、SharedSecret 和适用的 keystore2；检查系统版本与补丁兼容性，仅启动已审查服务 |
| `mount_metadata` | 按实际 fstab 挂载 metadata，恢复已有 metadata 密钥，为 userdata 建立正确的 dm-default-key 映射，确认 `/data` 挂载成功 |
| `load_de_keys` | 恢复已有主用户 DE 和系统密钥，安装到 fscrypt，不走创建新密钥的初始化路径 |
| `get_credential_type` | 从已有状态确定活动 SP protector 和凭据类型；`NONE` 必须是真实无 LSKF，不能代表文件缺失、解析失败或服务不可用 |
| `unlock_ce` | 验证 PIN/密码/图案，处理 SP 格式、scrypt、Gatekeeper/Weaver 和 KeyMint 认证令牌，解封 SP、派生 fbe-key、恢复并安装已有 CE 密钥，保留硬件限流 |
| `finish` | 清除临时秘密并释放句柄，不撤销密钥或卸载 userdata，供后续浏览和安装使用 |

锁屏 PIN/密码不是 vold 的 CE secret；无锁屏用户也需要正确的 SP 恢复路径。
返回稳定的 ABI 错误码，不返回可能含密钥信息的厂商字符串。`unlock_ce` 返回
`RC_RETRY` 时必须包含硬件重试等待时间，不自动重试；再次验证需用户操作。

已审查的 Android 17 vold 版本为 `11d8982f824d52e8122c7aa732207ba66b8f23c8`：

- 仅在 Soong 依赖 `libvold`/`vold` 不代表能用于 Recovery；依赖项目也需变体。
- `fscrypt_mount_metadata_encrypted` 有九个参数。只设置 `needs_encrypt=false`、
  `should_format=false` 不足以保证不修改状态；`read_key` 会准备目录，
  `retrieveKey` 可能升级 KeyMint blob，必须审查这些路径。
- `fscrypt_unlock_ce_storage` 使用 `read_and_fixate_user_ce_key`，可能删除候选
  密钥、重命名目录；Recovery 需要不执行此维护操作的读取与安装路径。
- Framework 的 `SyntheticPasswordManager.unlockLskfBasedProtector` 可能重新
  注册 Gatekeeper 并写状态/指标，不能把这些副作用照搬进解锁后端。
- SP v3 使用 SP800 派生子密钥；旧 TWRP 接口或直接把 PIN 交给 vold 不等价。

不得将创建/替换密钥、删除 locksettings、格式化、重新注册凭据、关闭加密或
修改正常系统服务作为失败回退。尚未支持并审查的密钥升级返回
`RC_KEY_UPGRADE_REQUIRED`，不按未知 ABI 重写 blob。

## 独立验证成功与失败处理

worker 返回成功不会直接打开文件浏览器。Recovery 要求真实的 ext4/F2FS
`/data` 挂载，不跟随路径组件符号链接打开 `/data/media/0`，读取其 fscrypt
v1/v2 策略，确认内核报告相应密钥存在，并验证目录枚举成功。空目录可以有效；
挂载的明文目录或 ramdisk 目录不视为已解密的加密存储。

ZIP 浏览器沿用已有签名和安装检查，不保证所有第三方安装器兼容。内部存储
安装拒绝 `.map` 文件和解析后逃出当前用户 media 根目录的路径；普通公共卷
行为不变。

凭据不进入 argv、环境变量、文件或日志。输入与 IPC 缓冲区锁定内存、排除转储，
退出时清除。worker 禁用 core dump，标准输出和错误重定向到 `/dev/null`；
设备适配还需抑制 Binder、厂商及平台日志中的秘密。Android 17 后端仅向已有
`/tmp/recovery.log` 写固定诊断检查点和数字错误码，详见
[中文诊断说明](android17/README.zh-CN.md#解锁失败怎么定位)。

每个操作有 45 秒截止时间，进度输出不会延长它。进程隔离限制崩溃或卡死的
影响，但无法撤销 root 代码错误造成的内核/TEE 副作用。发布前必须审查和测试，
保持 SELinux enforcing，并使用范围明确的设备权限。

## 源码同步

设备配置、自定义后端及打包声明提交到 **device/vendor 仓库**；本仓库保存通用
框架、ABI、UI 和共享后端。进入自己的 Android 源码目录：

```bash
cd /path/to/android
```

再单独执行：

```bash
repo sync -c bootable/recovery
```

此操作更新通用实现，不编辑或删除设备适配，也不更新其他项目的本地状态。
同步设备树或平台项目前仍需提交并由 manifest 跟踪适配。ABI 不匹配会使可选
解锁失败，不应破坏其他 Recovery 功能；安装器不应在同步后向 Recovery 复制
设备专用补丁。

## 验证范围

协议回归测试源码位于 `crypto/tests/protocol_test.cpp`。源码验证不证明 Android
链接、HAL 兼容或设备解密；维护者需自行构建并运行测试和实际 Recovery。

启用设备应覆盖后端/依赖缺失、worker 崩溃/超时、密钥缺失、错误凭据、限流、
空锁屏、PIN、密码、图案、内核密钥状态、文件浏览，以及失败后的普通 sideload。
Android 17 后端的更具体支持范围与检查步骤见
[设备适配指南](android17/README.zh-CN.md)。

参考：

- [AOSP 文件级加密](https://source.android.com/docs/security/features/encryption/file-based)
- [AOSP metadata 加密](https://source.android.com/docs/security/features/encryption/metadata)
- [AOSP 硬件封装密钥](https://source.android.com/docs/security/features/encryption/hw-wrapped-keys)
