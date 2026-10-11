// SPDX-License-Identifier: Apache-2.0
// Existing-key-only adaptation of AOSP vold MetadataCrypt/FsCrypt/KeyUtil.
// Copyright (C) 2016 The Android Open Source Project
// Copyright (C) 2026 The uwuAOSP Project
#include "storage.h"
#include <recovery_crypto/media_access.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <fcntl.h>
#include <libdm/dm.h>
#include <linux/blk-crypto.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <linux/fscrypt.h>
#include <mntent.h>
#include <openssl/sha.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include "../secret_memory.h"
#include "dm_ioctl_compat.h"
#include "diagnostic.h"
#include "files.h"
#include "key_storage.h"
#include "primitives.h"
namespace recovery_crypto::android17 {
using android::base::unique_fd;
static bool Mode(const std::string& value, KeyMode* mode) {
  if (value.empty())
    *mode = KeyMode::Raw;
  else if (value == "wrappedkey_v0")
    *mode = KeyMode::WrappedV0;
  else if (value == "wrappedkey")
    *mode = KeyMode::Wrapped;
  else
    return false;
  return true;
}
static bool Mounted(const std::string& path, std::string* source, bool* readonly) {
  FILE* f = setmntent("/proc/mounts", "r");
  if (!f) return false;
  bool found = false;
  char buffer[8192];
  struct mntent entry;
  while (getmntent_r(f, &entry, buffer, sizeof(buffer)))
    if (path == entry.mnt_dir) {
      *source = entry.mnt_fsname;
      *readonly = hasmntopt(&entry, "ro") != nullptr;
      found = true;
    }
  endmntent(f);
  return found;
}
static bool SameDevice(const std::string& a, const std::string& b) {
  struct stat x{}, y{};
  return stat(a.c_str(), &x) == 0 && stat(b.c_str(), &y) == 0 && S_ISBLK(x.st_mode) &&
         S_ISBLK(y.st_mode) && x.st_rdev == y.st_rdev;
}
static bool ValidMountedFilesystem(const std::string& path, const std::string& type) {
  struct statfs s{};
  return statfs(path.c_str(), &s) == 0 &&
         ((type == "ext4" && s.f_type == 0xef53) ||
          (type == "f2fs" && s.f_type == static_cast<decltype(s.f_type)>(0xf2f52010)));
}
static int ReadOnlyMount(const android::fs_mgr::FstabEntry& entry, const std::string& device, bool media_writes = false) {
  auto fd = OpenDirectory(entry.mount_point);
  if (fd.get() < 0) return RC_IO_ERROR;
  std::string source;
  bool ro = false;
  if (Mounted(entry.mount_point, &source, &ro))
    return ro && SameDevice(source, device) &&
                   ValidMountedFilesystem(entry.mount_point, entry.fs_type)
               ? RC_OK
               : RC_IO_ERROR;
  auto flags = entry.flags | MS_RDONLY | MS_NOATIME | MS_NOSUID | MS_NODEV;
  std::string options;
  for (const auto& option : android::base::Split(entry.fs_options, ",")) {
    if (option.empty() || option == "discard" || option.starts_with("discard=")) continue;
    if (!options.empty()) options += ",";
    options += option;
  }
  // Opt-in writable media needs normal journal/roll-forward recovery before
  // later remounting rw. The filesystem may replay its journal at this ro mount;
  // no userspace key/credential writes or fsck are performed.
  if (!media_writes) options += entry.fs_type == "ext4" ? ",noload" : ",norecovery";
  if (!options.empty() && options.front() == ',') options.erase(0, 1);
  if (mount(device.c_str(), entry.mount_point.c_str(), entry.fs_type.c_str(), flags,
            options.c_str()) != 0)
    return RC_IO_ERROR;
  return ValidMountedFilesystem(entry.mount_point, entry.fs_type) ? RC_OK : RC_IO_ERROR;
}
int Storage::Configure(const Config& c) {
  media_writes_ = recovery_crypto::MediaWritesRequested();
  if (!android::fs_mgr::ReadFstabFromFile(c.fstab, &fstab_)) return RC_UNSUPPORTED;
  const auto* data = android::fs_mgr::GetEntryForMountPoint(&fstab_, "/data");
  const auto* metadata = android::fs_mgr::GetEntryForMountPoint(&fstab_, "/metadata");
  if (!data || (data->fs_type != "ext4" && data->fs_type != "f2fs") ||
      !data->blk_device.starts_with("/dev/block/") || data->fs_mgr_flags.logical ||
      !data->user_devices.empty())
    return RC_UNSUPPORTED;
  data_ = *data;
  data_device_ = data_.blk_device;
  auto options = android::base::Split(data_.encryption_options, ":");
  if (options.size() < 1 || options.size() > 3 ||
      (options[0] != "aes-256-xts" && options[0] != "adiantum"))
    return RC_UNSUPPORTED;
  if (options.size() > 1 && options[1] != "aes-256-cts" && options[1] != "aes-256-hctr2" &&
      options[1] != "adiantum")
    return RC_UNSUPPORTED;
  policy_version_ = android::base::GetIntProperty("ro.product.first_api_level", 0) >= 30 ? 2 : 1;
  if (options.size() == 3)
    for (const auto& flag : android::base::Split(options[2], "+")) {
      if (flag == "v1")
        policy_version_ = 1;
      else if (flag == "v2")
        policy_version_ = 2;
      else if (flag == "wrappedkey_v0")
        mode_ = KeyMode::WrappedV0;
      else if (flag == "wrappedkey")
        mode_ = KeyMode::Wrapped;
      else if (flag != "inlinecrypt_optimized" && flag != "emmc_optimized")
        return RC_UNSUPPORTED;
    }
  metadata_encrypted_ = !data_.metadata_key_dir.empty();
  if (metadata_encrypted_) {
    if (!metadata || metadata->fs_type != "ext4" ||
        !metadata->blk_device.starts_with("/dev/block/") ||
        !data_.metadata_key_dir.starts_with("/metadata/"))
      return RC_UNSUPPORTED;
    metadata_ = *metadata;
    auto parts = android::base::Split(data_.metadata_encryption_options, ":");
    if (parts.size() > 2 || parts.empty()) return RC_UNSUPPORTED;
    if (parts[0].empty() || parts[0] == "aes-256-xts")
      metadata_cipher_ = "aes-xts-plain64";
    else if (parts[0] == "adiantum")
      metadata_cipher_ = "xchacha12,aes-adiantum-plain64";
    else
      return RC_UNSUPPORTED;
    if (!Mode(parts.size() == 2 ? parts[1] : "", &metadata_mode_)) return RC_UNSUPPORTED;
    if (android::base::GetUintProperty<unsigned>("ro.crypto.dm_default_key.options_format.version",
                                                 2) != 2)
      return RC_UNSUPPORTED;
  }
  return RC_OK;
}
int Storage::PrepareKey(Hal& hal, View persistent, KeyMode mode, Bytes* ephemeral) {
  if (mode == KeyMode::Raw) {
    ephemeral->assign(persistent.begin(), persistent.end());
    return RC_OK;
  }
  if (mode == KeyMode::WrappedV0) return hal.ExportStorageKey(persistent, ephemeral);
  unique_fd fd(open(data_.blk_device.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0)
    return Diagnostic(RC_IO_ERROR, Checkpoint::StorageExport, CodeSource::Errno, errno);
  ephemeral->resize(128);
  struct blk_crypto_prepare_key_arg arg{};
  arg.lt_key_ptr = reinterpret_cast<uintptr_t>(persistent.data());
  arg.lt_key_size = persistent.size();
  arg.eph_key_ptr = reinterpret_cast<uintptr_t>(ephemeral->data());
  arg.eph_key_size = ephemeral->size();
  if (ioctl(fd.get(), BLKCRYPTOPREPAREKEY, &arg) != 0) {
    const int code = errno;
    Clear(*ephemeral);
    return Diagnostic(RC_IO_ERROR, Checkpoint::StorageExport, CodeSource::Errno, code);
  }
  if (!arg.eph_key_size || arg.eph_key_size > 128) {
    Clear(*ephemeral);
    return Diagnostic(RC_IO_ERROR, Checkpoint::StorageExport);
  }
  ephemeral->resize(arg.eph_key_size);
  return RC_OK;
}
int Storage::Mount(Hal& hal) {
  std::string source;
  bool ro = false;
  if (Mounted("/data", &source, &ro)) {
    // Do not snapshot a live writable Android filesystem or guess which mapping owns it.
    if (!ro || !ValidMountedFilesystem("/data", data_.fs_type)) return RC_IO_ERROR;
    if (!metadata_encrypted_ && !SameDevice(source, data_.blk_device)) return RC_IO_ERROR;
    if (metadata_encrypted_) {
      std::string expected;
      if (!android::dm::DeviceMapper::Instance().GetDmDevicePathByName("recovery-crypto-data",
                                                                       &expected) ||
          !SameDevice(source, expected))
        return RC_IO_ERROR;
    }
    data_device_ = source;
    return RC_OK;
  }
  if (!metadata_encrypted_) return ReadOnlyMount(data_, data_.blk_device, media_writes_);
  // An already-mounted metadata partition may be writable; read no state through any other device.
  if (Mounted("/metadata", &source, &ro)) {
    if (!SameDevice(source, metadata_.blk_device) || !ValidMountedFilesystem("/metadata", "ext4"))
      return RC_IO_ERROR;
  } else {
    int result = ReadOnlyMount(metadata_, metadata_.blk_device);
    if (result != RC_OK) return result;
  }
  Bytes persistent, ephemeral;
  int result = RetrieveExistingKey(hal, data_.metadata_key_dir + "/key", {}, &persistent);
  if (result != RC_OK) return result;
  if (metadata_mode_ == KeyMode::Raw &&
      persistent.size() != (metadata_cipher_ == "aes-xts-plain64" ? 64 : 32))
    return RC_IO_ERROR;
  result = PrepareKey(hal, persistent, metadata_mode_, &ephemeral);
  if (result != RC_OK) return result;
  unique_fd fd(open(data_.blk_device.c_str(), O_RDONLY | O_CLOEXEC));
  uint64_t bytes = 0;
  if (fd.get() < 0 || ioctl(fd.get(), BLKGETSIZE64, &bytes) != 0 || bytes < 4096)
    return RC_IO_ERROR;
  Bytes encoded;
  encoded.reserve(ephemeral.size() * 2);
  for (uint8_t b : ephemeral) {
    encoded.push_back("0123456789abcdef"[b >> 4]);
    encoded.push_back("0123456789abcdef"[b & 15]);
  }
  auto& dm = android::dm::DeviceMapper::Instance();
  constexpr const char* name = "recovery-crypto-data";
  // Never reuse/remove an unidentified existing mapping left by another process.
  if (dm.GetState(name) != android::dm::DmDeviceState::INVALID) return RC_IO_ERROR;
  // libdm logs table.DebugString() on load failure, including the key. Use a locked,
  // zeroed ioctl buffer for table loading instead; libdm only creates/deletes/waits.
  Bytes parameters;
  auto append = [&](const std::string& value) {
    parameters.insert(parameters.end(), value.begin(), value.end());
  };
  append(metadata_cipher_ + " ");
  parameters.insert(parameters.end(), encoded.begin(), encoded.end());
  append(" 0 " + data_.blk_device + " 0 ");
  // The reviewed vold/kernel ABI uses the wrappedkey_v0 dm option for both
  // wrapped-key generations; the fscrypt ioctl flags still distinguish them.
  append(metadata_mode_ == KeyMode::Raw ? "2 sector_size:4096 iv_large_sectors"
                                        : "3 sector_size:4096 iv_large_sectors wrappedkey_v0");
  parameters.push_back(0);
  size_t target_size = (sizeof(dm_target_spec) + parameters.size() + 7) & ~size_t{ 7 };
  size_t size = sizeof(dm_ioctl) + target_size;
  recovery_crypto::SecretMemory request;
  if (!request.data() || size > static_cast<size_t>(sysconf(_SC_PAGESIZE))) return RC_IO_ERROR;
  unique_fd control(open("/dev/device-mapper", O_RDWR | O_CLOEXEC));
  if (control.get() < 0) return RC_IO_ERROR;
  if (!dm.CreateEmptyDevice(name)) return RC_IO_ERROR;
  auto init = [&](dm_ioctl* io, size_t count) {
    InitializeDmIo(io, count);
    strcpy(io->name, name);
  };
  auto* io = static_cast<dm_ioctl*>(request.data());
  init(io, size);
  io->target_count = 1;
  io->flags = (media_writes_ ? 0 : DM_READONLY_FLAG) | DM_SECURE_DATA_FLAG;
  auto* target =
      reinterpret_cast<dm_target_spec*>(reinterpret_cast<uint8_t*>(io) + sizeof(dm_ioctl));
  target->length = (bytes / 512) & ~uint64_t{ 7 };
  target->next = target_size;
  strcpy(target->target_type, "default-key");
  memcpy(target + 1, parameters.data(), parameters.size());
  if (ioctl(control.get(), DM_TABLE_LOAD, io) != 0) {
    dm.DeleteDevice(name);
    return RC_IO_ERROR;
  }
  init(io, sizeof(dm_ioctl));
  if (ioctl(control.get(), DM_DEV_SUSPEND, io) != 0 ||
      !dm.WaitForDevice(name, std::chrono::seconds(5), &data_device_)) {
    dm.DeleteDevice(name);
    return RC_IO_ERROR;
  }
  result = ReadOnlyMount(data_, data_device_, media_writes_);
  if (result != RC_OK) dm.DeleteDevice(name);
  return result;
}
static bool GetPolicy(int fd, fscrypt_get_policy_ex_arg* policy) {
  memset(policy, 0, sizeof(*policy));
  policy->policy_size = sizeof(policy->policy);
  return ioctl(fd, FS_IOC_GET_ENCRYPTION_POLICY_EX, policy) == 0;
}
int Storage::Install(Hal& hal, View persistent, const std::string& directory) {
  if (persistent.empty() || persistent.size() > 65536)
    return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptKeyShape);
  Bytes key;
  int result = PrepareKey(hal, persistent, mode_, &key);
  if (result != RC_OK) return result;
  if (key.empty() || key.size() > 128 ||
      (mode_ == KeyMode::Raw && key.size() != 32 && key.size() != 64))
    return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptKeyShape);
  auto policy_fd = OpenDirectory(directory);
  if (policy_fd.get() < 0)
    return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptPolicy, CodeSource::Errno, errno);
  auto mount_fd = OpenDirectory("/data");
  if (mount_fd.get() < 0)
    return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptPolicy, CodeSource::Errno, errno);
  fscrypt_get_policy_ex_arg policy{};
  if (!GetPolicy(policy_fd.get(), &policy))
    return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptPolicy, CodeSource::Errno, errno);
  bool v1 = policy.policy.version == FSCRYPT_POLICY_V1;
  if ((v1 ? 1u : policy.policy.version == FSCRYPT_POLICY_V2 ? 2u : 0u) != policy_version_)
    return Diagnostic(RC_UNSUPPORTED, Checkpoint::FscryptPolicy);
  Bytes buffer(sizeof(fscrypt_add_key_arg) + key.size(), 0);
  auto* arg = reinterpret_cast<fscrypt_add_key_arg*>(buffer.data());
  arg->raw_size = key.size();
  memcpy(arg->raw, key.data(), key.size());
  if (mode_ == KeyMode::WrappedV0) arg->__flags = __FSCRYPT_ADD_KEY_FLAG_HW_WRAPPED;
  if (mode_ == KeyMode::Wrapped) arg->flags = FSCRYPT_ADD_KEY_FLAG_HW_WRAPPED;
  if (v1) {
    size_t length = mode_ == KeyMode::WrappedV0 ? key.size() / 2 : key.size();
    Bytes first(SHA512_DIGEST_LENGTH), second(SHA512_DIGEST_LENGTH);
    SHA512(key.data(), length, first.data());
    SHA512(first.data(), first.size(), second.data());
    if (CRYPTO_memcmp(second.data(), policy.policy.v1.master_key_descriptor,
                      FSCRYPT_KEY_DESCRIPTOR_SIZE) != 0)
      return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptDescriptor);
    arg->key_spec.type = FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR;
    memcpy(arg->key_spec.u.descriptor, second.data(), FSCRYPT_KEY_DESCRIPTOR_SIZE);
  } else
    arg->key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
  if (ioctl(mount_fd.get(), FS_IOC_ADD_ENCRYPTION_KEY, arg) != 0)
    return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptAddKey, CodeSource::Errno, errno);
  if (!v1 && CRYPTO_memcmp(arg->key_spec.u.identifier, policy.policy.v2.master_key_identifier,
                           FSCRYPT_KEY_IDENTIFIER_SIZE) != 0)
    return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptIdentifier);
  fscrypt_get_key_status_arg status{};
  status.key_spec = arg->key_spec;
  if (ioctl(mount_fd.get(), FS_IOC_GET_ENCRYPTION_KEY_STATUS, &status) != 0)
    return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptStatus, CodeSource::Errno, errno);
  if (status.status != FSCRYPT_KEY_STATUS_PRESENT)
    return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptStatus);
  return Diagnostic(RC_OK, Checkpoint::FscryptStatus);
}
int Storage::LoadDe(Hal& hal, uint32_t user) {
  Bytes system;
  int result = RetrieveExistingKey(hal, "/data/unencrypted/key", {}, &system);
  if (result != RC_OK) return result;
  result = Install(hal, system, "/data/system");
  if (result != RC_OK) return result;
  Bytes de;
  result =
      RetrieveExistingKey(hal, "/data/misc/vold/user_keys/de/" + std::to_string(user), {}, &de);
  if (result != RC_OK) return result;
  return Install(hal, de, "/data/system_de/" + std::to_string(user));
}
int Storage::LoadCe(Hal& hal, uint32_t user, View secret) {
  if (secret.empty()) return Diagnostic(RC_IO_ERROR, Checkpoint::FscryptKeyShape);
  std::string root = "/data/misc/vold/user_keys/ce/" + std::to_string(user);
  std::vector<std::string> directories;
  if (!ListDirectories(root, &directories))
    return Diagnostic(RC_EXISTING_KEY_MISSING, Checkpoint::CeKeyDirectories);
  std::sort(directories.begin(), directories.end(), std::greater<>());
  bool attempted = false;
  for (const auto& directory : directories) {
    // Current vold rotates into cx%010u. Keep older numeric c* names readable too,
    // without accepting arbitrary directory names or invoking key fixation.
    size_t digits = directory.starts_with("cx") ? 2 : 1;
    if (directory != "current" && (directory.size() <= digits || directory[0] != 'c' ||
                                   !std::all_of(directory.begin() + digits, directory.end(),
                                                [](char c) { return c >= '0' && c <= '9'; })))
      continue;
    attempted = true;
    Bytes ce;
    int result = RetrieveExistingKey(hal, root + "/" + directory, secret, &ce);
    if (result == RC_KEY_UPGRADE_REQUIRED || result == RC_UNSUPPORTED) return result;
    if (result != RC_OK) continue;
    result = Install(hal, ce, "/data/media/" + std::to_string(user));
    if (result == RC_OK) return result;
    if (result == RC_KEY_UPGRADE_REQUIRED || result == RC_UNSUPPORTED) return result;
  }
  return attempted ? RC_IO_ERROR : Diagnostic(RC_EXISTING_KEY_MISSING, Checkpoint::CeKeyDirectories);
}
void Storage::EnableMediaWrites(uint32_t user) {
  if (!media_writes_ || user != 0) return;
  // Called only after existing CE key installation and kernel status checks.
  // MTP never triggers authentication or a remount from a host command.
  std::string source;
  bool ro = false;
  if (!Mounted("/data", &source, &ro) || !SameDevice(source, data_device_) ||
      !ValidMountedFilesystem("/data", data_.fs_type)) return;
  unique_fd block(open(source.c_str(), O_RDONLY | O_CLOEXEC));
  int readonly = 1;
  if (block.get() < 0 || ioctl(block.get(), BLKROGET, &readonly) || readonly) return;
  std::string options;
  for (const auto& option : android::base::Split(data_.fs_options, ",")) {
    if (option.empty() || option == "ro" || option == "noload" || option == "norecovery" ||
        option == "disable_roll_forward" || option == "discard" || option.starts_with("discard=")) continue;
    if (!options.empty()) options += ",";
    options += option;
  }
  const auto flags = (data_.flags & ~MS_RDONLY) | MS_REMOUNT | MS_NOATIME | MS_NOSUID | MS_NODEV | MS_NOEXEC;
  if (ro && mount(source.c_str(), "/data", data_.fs_type.c_str(), flags, options.c_str()))
    LOG(WARNING) << "Recovery media remount refused; MTP remains read-only";
}
}  // namespace recovery_crypto::android17
