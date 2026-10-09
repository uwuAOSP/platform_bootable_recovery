/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "protocol.h"
#include <sys/stat.h>

namespace recovery_crypto {
bool ValidRequest(const Request& r) {
  if (r.protocol != kProtocol || r.user_id > 99999 ||
      r.credential_type > RC_CREDENTIAL_PATTERN || r.length > RECOVERY_CRYPTO_MAX_CREDENTIAL ||
      r.pattern_size < RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE ||
      r.pattern_size > RECOVERY_CRYPTO_MAX_PATTERN_SIZE ||
      (r.credential_type != RC_CREDENTIAL_PATTERN &&
       r.pattern_size != RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE)) {
    return false;
  }
  if (r.operation == kPrepare) return r.length == 0 && r.credential_type == RC_CREDENTIAL_NONE;
  if (r.operation != kUnlock) return false;
  if (r.credential_type == RC_CREDENTIAL_NONE) return r.length == 0;
  if (r.length == 0) return false;
  const uint32_t count = r.pattern_size * r.pattern_size;
  if (r.credential_type == RC_CREDENTIAL_PATTERN && (r.length < 4 || r.length > count)) return false;
  uint64_t pattern_cells = 0;
  for (uint32_t i = 0; i < r.length; ++i) {
    auto c = r.credential[i];
    if (r.credential_type == RC_CREDENTIAL_PIN && (c < '0' || c > '9')) return false;
    if (r.credential_type == RC_CREDENTIAL_PASSWORD && (c < 32 || c > 126)) return false;
    if (r.credential_type == RC_CREDENTIAL_PATTERN) {
      if (c >= count || (pattern_cells & (uint64_t{1} << c))) return false;
      pattern_cells |= uint64_t{1} << c;
    }
  }
  return true;
}
bool ValidReply(const Reply& r) {
  if (r.protocol != kProtocol || (r.kind != kProgress && r.kind != kResult) ||
      r.status > static_cast<uint32_t>(Status::StorageLocked) ||
      r.stage > static_cast<uint32_t>(Stage::Verify) || r.credential_type > RC_CREDENTIAL_PATTERN ||
      r.pattern_size < RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE ||
      r.pattern_size > RECOVERY_CRYPTO_MAX_PATTERN_SIZE ||
      (r.credential_type != RC_CREDENTIAL_PATTERN &&
       r.pattern_size != RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE)) {
    return false;
  }
  return r.status != static_cast<uint32_t>(Status::Throttled) || r.retry_seconds != 0;
}
Status BackendStatus(int32_t code) {
  switch (code) {
    case RC_OK: return Status::Ready;
    case RC_UNSUPPORTED: return Status::Unsupported;
    case RC_SERVICES_UNAVAILABLE: return Status::ServicesUnavailable;
    case RC_EXISTING_KEY_MISSING: return Status::MissingKey;
    case RC_KEY_UPGRADE_REQUIRED: return Status::UpgradeRequired;
    case RC_WRONG_CREDENTIAL: return Status::WrongCredential;
    case RC_RETRY: return Status::Throttled;
    case RC_IO_ERROR: return Status::IoError;
    default: return Status::InvalidBackend;
  }
}
bool TrustedFile(const char* path) {
  struct stat st{};
  // Refuse writable files/symlinks. The device tree installs the backend in
  // the Recovery ramdisk, never loaded from encrypted userdata or a ZIP.
  return lstat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
      !(st.st_mode & (S_IWGRP | S_IWOTH));
}
}  // namespace recovery_crypto
