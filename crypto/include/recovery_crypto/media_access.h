/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

namespace recovery_crypto {
// Immutable device-tree opt-in. Legacy VID:PID profiles remain read-only.
// This is configuration, never evidence that authentication succeeded.
inline bool ReadMediaAccess(std::string* vid = nullptr, std::string* pid = nullptr,
                            bool* writable = nullptr) {
  android::base::unique_fd fd(open("/system/etc/recovery.mtp.conf",
                                  O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  struct stat st{};
  if (fd.get() < 0 || fstat(fd.get(), &st) || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
      (st.st_mode & 0022) || st.st_size < 9 || st.st_size > 13) return false;
  char buffer[14];
  const auto count = TEMP_FAILURE_RETRY(read(fd.get(), buffer, sizeof(buffer)));
  if (count != st.st_size) return false;
  std::string text(buffer, count);
  if (text.back() == '\n') text.pop_back();
  if ((text.size() != 9 && text.size() != 12) || text[4] != ':' ||
      text.substr(0, 4).find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
      text.substr(5, 4).find_first_not_of("0123456789abcdefABCDEF") != std::string::npos ||
      (text.size() == 12 && text.substr(9) != ":rw")) return false;
  if (vid) *vid = text.substr(0, 4);
  if (pid) *pid = text.substr(5, 4);
  if (writable) *writable = text.size() == 12;
  return true;
}
inline bool MediaWritesRequested() {
  bool writable = false;
  return ReadMediaAccess(nullptr, nullptr, &writable) && writable;
}
}  // namespace recovery_crypto
