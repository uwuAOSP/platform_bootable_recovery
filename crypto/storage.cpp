/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "recovery_crypto/session.h"
#include <android-base/unique_fd.h>
#include <linux/fscrypt.h>
#include <mntent.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace recovery_crypto {
std::string UserStoragePath(uint32_t user_id) { return "/data/media/" + std::to_string(user_id); }
bool VerifyUserStorage(uint32_t user_id) {
  if (user_id > 99999) return false;
  // /data must be a real mount; never report a directory on the ramdisk as
  // decrypted userdata. Do not read device-mapper tables containing keys.
  FILE* mounts = setmntent("/proc/mounts", "r");
  if (!mounts) return false;
  bool mounted = false;
  while (auto entry = getmntent(mounts)) {
    if (!strcmp(entry->mnt_dir, "/data") &&
        (!strcmp(entry->mnt_type, "f2fs") || !strcmp(entry->mnt_type, "ext4"))) {
      mounted = true;
      break;
    }
  }
  endmntent(mounts);
  if (!mounted) return false;
  android::base::unique_fd root(open("/data", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (root.get() < 0) return false;
  android::base::unique_fd media(openat(root.get(), "media", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (media.get() < 0) return false;
  auto user = std::to_string(user_id);
  android::base::unique_fd fd(openat(media.get(), user.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.get() < 0) return false;
  struct fscrypt_get_policy_ex_arg policy{};
  policy.policy_size = sizeof(policy.policy);
  if (ioctl(fd.get(), FS_IOC_GET_ENCRYPTION_POLICY_EX, &policy) < 0) return false;
  struct fscrypt_get_key_status_arg key{};
  if (policy.policy.version == FSCRYPT_POLICY_V2) {
    key.key_spec.type = FSCRYPT_KEY_SPEC_TYPE_IDENTIFIER;
    memcpy(key.key_spec.u.identifier, policy.policy.v2.master_key_identifier,
           FSCRYPT_KEY_IDENTIFIER_SIZE);
  } else if (policy.policy.version == FSCRYPT_POLICY_V1) {
    key.key_spec.type = FSCRYPT_KEY_SPEC_TYPE_DESCRIPTOR;
    memcpy(key.key_spec.u.descriptor, policy.policy.v1.master_key_descriptor,
           FSCRYPT_KEY_DESCRIPTOR_SIZE);
  } else {
    return false;
  }
  if (ioctl(fd.get(), FS_IOC_GET_ENCRYPTION_KEY_STATUS, &key) < 0 ||
      key.status != FSCRYPT_KEY_STATUS_PRESENT) return false;
  // fdopendir owns the descriptor on success. Restore cleanup on failure.
  const int directory_fd = fd.release();
  DIR* directory = fdopendir(directory_fd);
  if (!directory) {
    close(directory_fd);
    return false;
  }
  errno = 0;
  // Access, rather than nonempty output, is the success criterion. An empty
  // readable directory is valid. No filenames or file contents enter logs.
  while (readdir(directory) != nullptr) {}
  const bool readable = errno == 0;
  closedir(directory);
  return readable;
}
}  // namespace recovery_crypto
