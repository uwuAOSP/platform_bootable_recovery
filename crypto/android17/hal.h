/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <aidl/android/hardware/gatekeeper/IGatekeeper.h>
#include <aidl/android/hardware/security/keymint/HardwareAuthToken.h>
#include <aidl/android/hardware/security/keymint/IKeyMintDevice.h>
#include <aidl/android/hardware/security/secureclock/ISecureClock.h>
#include <aidl/android/hardware/weaver/IWeaver.h>
#include <android/hardware/gatekeeper/1.0/IGatekeeper.h>
#include <android/hardware/weaver/1.0/IWeaver.h>
#include <recovery_crypto/backend.h>
#include <optional>
#include "config.h"
#include "secure_bytes.h"
namespace recovery_crypto::android17 {
namespace km = aidl::android::hardware::security::keymint;
// Pure bounds/format check, independent of HAL lookup or authentication.
bool ValidGatekeeperInput(uint32_t user, View handle);
struct Authentication {
  std::optional<km::HardwareAuthToken> token;
  ~Authentication() {
    if (token && !token->mac.empty()) OPENSSL_cleanse(token->mac.data(), token->mac.size());
  }
};
class Hal final {
 public:
  int Connect(const Config& c);
  int Decrypt(View keyblob, View ciphertext, View app_id, const Authentication* auth, Bytes* out);
  int ExportStorageKey(View persistent, Bytes* ephemeral);
  int VerifyGatekeeper(uint32_t user, View handle, View password, Authentication* auth,
                       uint32_t* retry);
  int ReadWeaver(uint32_t slot, View stretched, Bytes* secret, uint32_t* retry);
  int security_level() const {
    return security_level_;
  }

 private:
  std::shared_ptr<km::IKeyMintDevice> keymint_;
  std::shared_ptr<aidl::android::hardware::security::secureclock::ISecureClock> clock_;
  std::shared_ptr<aidl::android::hardware::gatekeeper::IGatekeeper> gatekeeper_;
  std::shared_ptr<aidl::android::hardware::weaver::IWeaver> weaver_;
  android::sp<android::hardware::gatekeeper::V1_0::IGatekeeper> old_gatekeeper_;
  android::sp<android::hardware::weaver::V1_0::IWeaver> old_weaver_;
  bool timestamp_required_ = false;
  int security_level_ = 0;
};
}  // namespace recovery_crypto::android17
