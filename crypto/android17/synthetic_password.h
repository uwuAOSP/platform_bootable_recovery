/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <optional>
#include "hal.h"
namespace recovery_crypto::android17 {
struct PasswordData {
  int type = -1;
  unsigned n = 0, r = 0, p = 0;
  Bytes salt, handle;
};
bool ParsePasswordData(View bytes, PasswordData* data);
bool EncodePatternCredential(View cells, uint32_t size, Bytes* encoded);
class SyntheticPassword final {
 public:
  int Load(uint32_t user, uint32_t* credential_type);
  uint32_t PatternSize() const { return pattern_size_; }
  int Unlock(Hal& hal, uint32_t credential_type, View credential, Bytes* fbe_key, uint32_t* retry);

 private:
  uint32_t user_ = 0, type_ = RC_CREDENTIAL_NONE;
  uint32_t pattern_size_ = RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE;
  uint64_t protector_ = 0;
  Bytes blob_;
  std::optional<PasswordData> password_;
  std::optional<uint32_t> weaver_slot_;
  std::string State(const char* name, bool global = false) const;
};
}  // namespace recovery_crypto::android17
