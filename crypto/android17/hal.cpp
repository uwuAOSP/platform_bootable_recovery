/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hal.h"
#include <aidl/android/hardware/security/keymint/BeginResult.h>
#include <aidl/android/hardware/security/keymint/ErrorCode.h>
#include <aidl/android/hardware/security/keymint/IKeyMintOperation.h>
#include <aidl/android/hardware/security/sharedsecret/ISharedSecret.h>
#include <android/binder_manager.h>
#include <android/hardware/keymaster/4.0/IKeymasterDevice.h>
#include <android/hardware/keymaster/4.1/IKeymasterDevice.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>
#include "diagnostic.h"
#include "primitives.h"
namespace recovery_crypto::android17 {
namespace gk = aidl::android::hardware::gatekeeper;
namespace wv = aidl::android::hardware::weaver;
namespace oldgk = android::hardware::gatekeeper::V1_0;
namespace oldwv = android::hardware::weaver::V1_0;
template <class T>
static std::shared_ptr<T> Lookup(const std::string& name) {
  // checkService does not lazily start unreviewed services. Parent kills hung Binder operations.
  for (int i = 0; i < 20; ++i) {
    ndk::SpAIBinder binder(AServiceManager_checkService(name.c_str()));
    if (binder.get()) return T::fromBinder(binder);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return nullptr;
}
static int Error(const ndk::ScopedAStatus& status, Checkpoint point) {
  if (status.isOk()) return RC_OK;
  if (status.getExceptionCode() != EX_SERVICE_SPECIFIC)
    return Diagnostic(RC_SERVICES_UNAVAILABLE, point, CodeSource::BinderException,
                      status.getExceptionCode());
  auto error = static_cast<km::ErrorCode>(status.getServiceSpecificError());
  return Diagnostic(error == km::ErrorCode::KEY_REQUIRES_UPGRADE ? RC_KEY_UPGRADE_REQUIRED :
                                                                 RC_IO_ERROR,
                    point, CodeSource::Hal, status.getServiceSpecificError());
}
static int Negotiate(const Config& c) {
  namespace ss = aidl::android::hardware::security::sharedsecret;
  namespace legacy = android::hardware::keymaster::V4_0;
  // Another negotiator could have additional participants. Never replace its live agreement.
  ndk::SpAIBinder running(
      AServiceManager_checkService("android.system.keystore2.IKeystoreService/default"));
  if (running.get()) return RC_SERVICES_UNAVAILABLE;
  std::vector<std::shared_ptr<ss::ISharedSecret>> devices;
  std::vector<android::sp<legacy::IKeymasterDevice>> old_devices;
  std::vector<ss::SharedSecretParameters> parameters;
  for (const auto& name : c.sharedsecret_services) {
    auto d = Lookup<ss::ISharedSecret>(name);
    ss::SharedSecretParameters p;
    if (!d || !d->getSharedSecretParameters(&p).isOk() || p.nonce.size() != 32 ||
        (!p.seed.empty() && p.seed.size() != 32))
      return RC_SERVICES_UNAVAILABLE;
    devices.push_back(d);
    parameters.push_back(std::move(p));
  }
  for (const auto& name : c.sharedsecret_hidl_instances) {
    android::sp<legacy::IKeymasterDevice> device;
    const auto instance = name.substr(4);
    if (name.starts_with("4.1/"))
      device = android::hardware::keymaster::V4_1::IKeymasterDevice::tryGetService(instance);
    else
      device = legacy::IKeymasterDevice::tryGetService(instance);
    if (!device) return RC_SERVICES_UNAVAILABLE;
    bool hardware_ok = false;
    auto hardware = device->getHardwareInfo([&](legacy::SecurityLevel level,
                                                const android::hardware::hidl_string&,
                                                const android::hardware::hidl_string&) {
      hardware_ok = level == (instance == "strongbox" ? legacy::SecurityLevel::STRONGBOX
                                                      : legacy::SecurityLevel::TRUSTED_ENVIRONMENT);
    });
    if (!hardware.isOk() || !hardware_ok) return RC_SERVICES_UNAVAILABLE;
    ss::SharedSecretParameters p;
    bool ok = false;
    auto status = device->getHmacSharingParameters(
        [&](legacy::ErrorCode error, const legacy::HmacSharingParameters& value) {
          if (error != legacy::ErrorCode::OK || (value.seed.size() != 0 && value.seed.size() != 32))
            return;
          p.seed.assign(value.seed.begin(), value.seed.end());
          p.nonce.assign(value.nonce.data(), value.nonce.data() + value.nonce.size());
          ok = p.nonce.size() == 32;
        });
    if (!status.isOk() || !ok) return RC_SERVICES_UNAVAILABLE;
    old_devices.push_back(device);
    parameters.push_back(std::move(p));
  }
  // Match keystore2: retain every participant (including equal parameters),
  // sort the combined list once, then require every checksum to agree.
  std::sort(parameters.begin(), parameters.end(), [](const auto& a, const auto& b) {
    return a.seed != b.seed ? a.seed < b.seed : a.nonce < b.nonce;
  });
  BinderBytes reference;
  for (const auto& device : devices) {
    BinderBytes check;
    if (!device->computeSharedSecret(parameters, &check).isOk() || check.size() != 32)
      return RC_SERVICES_UNAVAILABLE;
    if (reference.empty())
      reference.assign(check.begin(), check.end());
    else if (CRYPTO_memcmp(reference.data(), check.data(), 32) != 0)
      return RC_SERVICES_UNAVAILABLE;
  }
  android::hardware::hidl_vec<legacy::HmacSharingParameters> old_parameters;
  old_parameters.resize(parameters.size());
  for (size_t i = 0; i < parameters.size(); ++i) {
    old_parameters[i].seed.resize(parameters[i].seed.size());
    std::copy(parameters[i].seed.begin(), parameters[i].seed.end(), old_parameters[i].seed.begin());
    std::copy(parameters[i].nonce.begin(), parameters[i].nonce.end(),
              old_parameters[i].nonce.data());
  }
  for (const auto& device : old_devices) {
    bool ok = false;
    auto status = device->computeSharedHmac(
        old_parameters,
        [&](legacy::ErrorCode error, const android::hardware::hidl_vec<uint8_t>& check) {
          ok = error == legacy::ErrorCode::OK && check.size() == 32 && reference.size() == 32 &&
               CRYPTO_memcmp(reference.data(), check.data(), 32) == 0;
        });
    if (!status.isOk() || !ok) return RC_SERVICES_UNAVAILABLE;
  }
  return RC_OK;
}
int Hal::Connect(const Config& c) {
  keymint_ = Lookup<km::IKeyMintDevice>(c.keymint_service);
  km::KeyMintHardwareInfo info;
  if (!keymint_ || !keymint_->getHardwareInfo(&info).isOk() ||
      static_cast<int>(info.securityLevel) != c.security_level)
    return RC_SERVICES_UNAVAILABLE;
  security_level_ = c.security_level;
  timestamp_required_ = info.timestampTokenRequired;
  if (!c.secureclock_service.empty())
    clock_ =
        Lookup<aidl::android::hardware::security::secureclock::ISecureClock>(c.secureclock_service);
  if (timestamp_required_ && !clock_) return RC_SERVICES_UNAVAILABLE;
  int result = Negotiate(c);
  if (result != RC_OK) return result;
  if (c.gatekeeper_transport == "aidl") {
    gatekeeper_ =
        Lookup<gk::IGatekeeper>("android.hardware.gatekeeper.IGatekeeper/" + c.gatekeeper_instance);
    if (!gatekeeper_) return RC_SERVICES_UNAVAILABLE;
  } else if (c.gatekeeper_transport == "hidl") {
    old_gatekeeper_ = oldgk::IGatekeeper::tryGetService(c.gatekeeper_instance);
    if (!old_gatekeeper_) return RC_SERVICES_UNAVAILABLE;
  }
  if (c.weaver_transport == "aidl") {
    weaver_ = Lookup<wv::IWeaver>("android.hardware.weaver.IWeaver/" + c.weaver_instance);
    if (!weaver_) return RC_SERVICES_UNAVAILABLE;
  } else if (c.weaver_transport == "hidl") {
    old_weaver_ = oldwv::IWeaver::tryGetService(c.weaver_instance);
    if (!old_weaver_) return RC_SERVICES_UNAVAILABLE;
  }
  return RC_OK;
}
int Hal::Decrypt(View keyblob, View ciphertext, View app_id, const Authentication* auth,
                 Bytes* out) {
  Clear(*out);
  if (!keymint_ || keyblob.empty() || ciphertext.size() < 28 || ciphertext.size() > 65536)
    return Diagnostic(RC_IO_ERROR, Checkpoint::KeymintInput);
  using V = km::KeyParameterValue;
  std::vector<km::KeyParameter> params = {
    { km::Tag::BLOCK_MODE, V::make<V::blockMode>(km::BlockMode::GCM) },
    { km::Tag::PADDING, V::make<V::paddingMode>(km::PaddingMode::NONE) },
    { km::Tag::MAC_LENGTH, V::make<V::integer>(128) },
    { km::Tag::NONCE,
      V::make<V::blob>(std::vector<uint8_t>(ciphertext.begin(), ciphertext.begin() + 12)) }
  };
  if (!app_id.empty())
    params.push_back({ km::Tag::APPLICATION_ID,
                       V::make<V::blob>(std::vector<uint8_t>(app_id.begin(), app_id.end())) });
  BinderBytes key(keyblob), input(ciphertext.subspan(12)), plain;
  km::BeginResult begin;
  std::optional<km::HardwareAuthToken> no_token;
  const auto& token = auth ? auth->token : no_token;
  auto status = keymint_->begin(km::KeyPurpose::DECRYPT, key, params, token, &begin);
  for (auto& p : params)
    if (p.value.getTag() == V::blob) {
      auto& v = p.value.get<V::blob>();
      if (!v.empty()) OPENSSL_cleanse(v.data(), v.size());
    }
  int result = Error(status, Checkpoint::KeymintBegin);
  if (result != RC_OK) return result;
  if (!begin.operation) return Diagnostic(RC_IO_ERROR, Checkpoint::KeymintBegin);
  std::optional<aidl::android::hardware::security::secureclock::TimeStampToken> timestamp;
  if (timestamp_required_ && token) {
    timestamp.emplace();
    status = clock_->generateTimeStamp(begin.challenge, &*timestamp);
    if (!status.isOk()) {
      begin.operation->abort();
      return Diagnostic(RC_SERVICES_UNAVAILABLE, Checkpoint::SecureClock,
                        status.getExceptionCode() == EX_SERVICE_SPECIFIC ? CodeSource::Hal :
                                                                         CodeSource::BinderException,
                        status.getExceptionCode() == EX_SERVICE_SPECIFIC ?
                            status.getServiceSpecificError() : status.getExceptionCode());
    }
  }
  // One bounded finish handles the complete ciphertext+tag; never publish unauthenticated update
  // output.
  status = begin.operation->finish(input, std::nullopt, token, timestamp, std::nullopt, &plain);
  if (timestamp && !timestamp->mac.empty())
    OPENSSL_cleanse(timestamp->mac.data(), timestamp->mac.size());
  result = Error(status, Checkpoint::KeymintFinish);
  if (result != RC_OK) {
    begin.operation->abort();
    return result;
  }
  if (plain.size() != ciphertext.size() - 28)
    return Diagnostic(RC_IO_ERROR, Checkpoint::KeymintOutput);
  out->assign(plain.begin(), plain.end());
  return RC_OK;
}
int Hal::ExportStorageKey(View persistent, Bytes* ephemeral) {
  BinderBytes in(persistent), out;
  auto status = keymint_->convertStorageKeyToEphemeral(in, &out);
  int result = Error(status, Checkpoint::StorageExport);
  if (result != RC_OK) return result;
  if (out.empty() || out.size() > 128) return Diagnostic(RC_IO_ERROR, Checkpoint::StorageExport);
  ephemeral->assign(out.begin(), out.end());
  return RC_OK;
}
static uint64_t Little64(View v, size_t p) {
  uint64_t n = 0;
  for (int i = 7; i >= 0; --i) n = (n << 8) | v[p + i];
  return n;
}
static uint64_t Big64(View v, size_t p) {
  uint64_t n = 0;
  for (int i = 0; i < 8; ++i) n = (n << 8) | v[p + i];
  return n;
}
static int Retry(uint64_t milliseconds, uint32_t* retry) {
  if (!milliseconds) return RC_IO_ERROR;
  *retry = static_cast<uint32_t>(std::min<uint64_t>((milliseconds + 999) / 1000, UINT32_MAX));
  return RC_RETRY;
}
bool ValidGatekeeperInput(uint32_t user, View handle) {
  // AOSP GateKeeper::Verify accepts legacy version 0; rejecting it here stops
  // before the hardware can authenticate an existing enrollment. Keep the SID
  // read's size guard and existing upper version bound. This does not enroll or
  // upgrade a handle, and the HAL must still verify its signature/password.
  return user <= INT32_MAX && handle.size() >= 9 && handle.size() <= 4096 && handle[0] <= 3;
}
int Hal::VerifyGatekeeper(uint32_t user, View handle, View password, Authentication* auth,
                          uint32_t* retry) {
  if (!ValidGatekeeperInput(user, handle))
    return Diagnostic(RC_IO_ERROR, Checkpoint::GatekeeperInput);
  BinderBytes h(handle), p(password);
  if (gatekeeper_) {
    gk::GatekeeperVerifyResponse response;
    auto status = gatekeeper_->verify(user, 0, h, p, &response);
    if (!status.isOk()) {
      if (status.getExceptionCode() == EX_SERVICE_SPECIFIC) {
        if (status.getServiceSpecificError() == gk::IGatekeeper::ERROR_GENERAL_FAILURE)
          return Diagnostic(RC_WRONG_CREDENTIAL, Checkpoint::GatekeeperVerify,
                            CodeSource::Hal, status.getServiceSpecificError());
        if (status.getServiceSpecificError() == gk::IGatekeeper::ERROR_NOT_IMPLEMENTED)
          return Diagnostic(RC_UNSUPPORTED, Checkpoint::GatekeeperVerify,
                            CodeSource::Hal, status.getServiceSpecificError());
      }
      return Diagnostic(RC_SERVICES_UNAVAILABLE, Checkpoint::GatekeeperVerify,
                        status.getExceptionCode() == EX_SERVICE_SPECIFIC ? CodeSource::Hal :
                                                                         CodeSource::BinderException,
                        status.getExceptionCode() == EX_SERVICE_SPECIFIC ?
                            status.getServiceSpecificError() : status.getExceptionCode());
    }
    if (response.statusCode == gk::IGatekeeper::ERROR_RETRY_TIMEOUT)
      return Diagnostic(Retry(response.timeoutMs > 0 ? response.timeoutMs : 0, retry),
                        Checkpoint::GatekeeperVerify, CodeSource::Hal, response.statusCode);
    if (response.statusCode == gk::IGatekeeper::ERROR_GENERAL_FAILURE)
      return Diagnostic(RC_WRONG_CREDENTIAL, Checkpoint::GatekeeperVerify,
                        CodeSource::Hal, response.statusCode);
    if (response.statusCode == gk::IGatekeeper::ERROR_NOT_IMPLEMENTED)
      return Diagnostic(RC_UNSUPPORTED, Checkpoint::GatekeeperVerify,
                        CodeSource::Hal, response.statusCode);
    if (response.statusCode != gk::IGatekeeper::STATUS_OK &&
        response.statusCode != gk::IGatekeeper::STATUS_REENROLL)
      return Diagnostic(RC_IO_ERROR, Checkpoint::GatekeeperVerify, CodeSource::Hal, response.statusCode);
    auth->token = std::move(response.hardwareAuthToken);
  } else if (old_gatekeeper_) {
    int result = RC_SERVICES_UNAVAILABLE;
    android::hardware::hidl_vec<uint8_t> hh, pp;
    hh.setToExternal(h.data(), h.size());
    pp.setToExternal(p.data(), p.size());
    auto status =
        old_gatekeeper_->verify(user, 0, hh, pp, [&](const oldgk::GatekeeperResponse& response) {
          if (response.code == oldgk::GatekeeperStatusCode::ERROR_RETRY_TIMEOUT) {
            result = Diagnostic(Retry(response.timeout, retry), Checkpoint::GatekeeperVerify,
                                CodeSource::Hal, static_cast<int32_t>(response.code));
            return;
          }
          if (response.code == oldgk::GatekeeperStatusCode::ERROR_GENERAL_FAILURE) {
            result = Diagnostic(RC_WRONG_CREDENTIAL, Checkpoint::GatekeeperVerify,
                                CodeSource::Hal, static_cast<int32_t>(response.code));
            return;
          }
          if (response.code == oldgk::GatekeeperStatusCode::ERROR_NOT_IMPLEMENTED) {
            result = Diagnostic(RC_UNSUPPORTED, Checkpoint::GatekeeperVerify,
                                CodeSource::Hal, static_cast<int32_t>(response.code));
            return;
          }
          if (response.code != oldgk::GatekeeperStatusCode::STATUS_OK &&
              response.code != oldgk::GatekeeperStatusCode::STATUS_REENROLL) {
            result = Diagnostic(RC_IO_ERROR, Checkpoint::GatekeeperVerify,
                                CodeSource::Hal, static_cast<int32_t>(response.code));
            return;
          }
          View raw(response.data.data(), response.data.size());
          if (raw.size() != 69 || raw[0] != 0) {
            result = Diagnostic(RC_IO_ERROR, Checkpoint::GatekeeperToken);
            return;
          }
          km::HardwareAuthToken t;
          t.challenge = Little64(raw, 1);
          t.userId = Little64(raw, 9);
          t.authenticatorId = Little64(raw, 17);
          uint32_t type = (uint32_t(raw[25]) << 24) | (uint32_t(raw[26]) << 16) |
                          (uint32_t(raw[27]) << 8) | raw[28];
          t.authenticatorType = static_cast<km::HardwareAuthenticatorType>(type);
          t.timestamp.milliSeconds = Big64(raw, 29);
          t.mac.assign(raw.begin() + 37, raw.end());
          auth->token = std::move(t);
          result = RC_OK;
        });
    if (!status.isOk()) return Diagnostic(RC_SERVICES_UNAVAILABLE, Checkpoint::GatekeeperVerify);
    if (result != RC_OK) return Diagnostic(result, Checkpoint::GatekeeperVerify);
  } else
    return Diagnostic(RC_SERVICES_UNAVAILABLE, Checkpoint::GatekeeperVerify);
  if (!auth->token || auth->token->mac.size() != 32 || auth->token->challenge != 0 ||
      static_cast<uint64_t>(auth->token->userId) != Little64(handle, 1) ||
      auth->token->authenticatorType != km::HardwareAuthenticatorType::PASSWORD)
    return Diagnostic(RC_IO_ERROR, Checkpoint::GatekeeperToken);
  // STATUS_REENROLL is accepted without enrolling, changing the handle or writing FRP.
  return Diagnostic(RC_OK, Checkpoint::GatekeeperVerify);
}
int Hal::ReadWeaver(uint32_t slot, View stretched, Bytes* secret, uint32_t* retry) {
  uint32_t slots = 0, key_size = 0, value_size = 0;
  if (weaver_) {
    wv::WeaverConfig config;
    auto status = weaver_->getConfig(&config);
    if (!status.isOk())
      return Diagnostic(RC_SERVICES_UNAVAILABLE, Checkpoint::WeaverRead,
                        status.getExceptionCode() == EX_SERVICE_SPECIFIC ? CodeSource::Hal :
                                                                         CodeSource::BinderException,
                        status.getExceptionCode() == EX_SERVICE_SPECIFIC ?
                            status.getServiceSpecificError() : status.getExceptionCode());
    if (config.slots < 1 || config.keySize < 1 || config.valueSize < 1)
      return Diagnostic(RC_IO_ERROR, Checkpoint::WeaverRead);
    slots = config.slots;
    key_size = config.keySize;
    value_size = config.valueSize;
  } else if (old_weaver_) {
    bool ok = false;
    auto status = old_weaver_->getConfig([&](oldwv::WeaverStatus s, const oldwv::WeaverConfig& c) {
      ok = s == oldwv::WeaverStatus::OK;
      if (!ok)
        Diagnostic(RC_SERVICES_UNAVAILABLE, Checkpoint::WeaverRead, CodeSource::Hal,
                   static_cast<int32_t>(s));
      slots = c.slots;
      key_size = c.keySize;
      value_size = c.valueSize;
    });
    if (!status.isOk() || !ok)
      return Diagnostic(RC_SERVICES_UNAVAILABLE, Checkpoint::WeaverRead);
  } else
    return Diagnostic(RC_SERVICES_UNAVAILABLE, Checkpoint::WeaverRead);
  if (slot >= slots || !key_size || key_size > 64 || !value_size || value_size > 4096)
    return Diagnostic(RC_IO_ERROR, Checkpoint::WeaverRead);
  auto hash = PersonalizedHash("weaver-key", stretched);
  BinderBytes key(View(hash).first(key_size));
  if (weaver_) {
    wv::WeaverReadResponse response;
    auto status = weaver_->read(slot, key, &response);
    if (!status.isOk())
      return Diagnostic(RC_SERVICES_UNAVAILABLE, Checkpoint::WeaverRead,
                        status.getExceptionCode() == EX_SERVICE_SPECIFIC ? CodeSource::Hal :
                                                                         CodeSource::BinderException,
                        status.getExceptionCode() == EX_SERVICE_SPECIFIC ?
                            status.getServiceSpecificError() : status.getExceptionCode());
    if (response.timeout < 0) {
      if (!response.value.empty()) OPENSSL_cleanse(response.value.data(), response.value.size());
      return Diagnostic(RC_IO_ERROR, Checkpoint::WeaverRead);
    }
    int result = RC_IO_ERROR;
    if (response.status == wv::WeaverReadStatus::OK && response.value.size() == value_size &&
        response.timeout == 0) {
      secret->assign(response.value.begin(), response.value.end());
      result = RC_OK;
    } else if (response.status == wv::WeaverReadStatus::THROTTLE ||
               response.status == wv::WeaverReadStatus::INCORRECT_KEY) {
      result = response.timeout ? Retry(response.timeout, retry)
               : response.status == wv::WeaverReadStatus::INCORRECT_KEY ? RC_WRONG_CREDENTIAL
                                                                        : RC_IO_ERROR;
    }
    if (!response.value.empty()) OPENSSL_cleanse(response.value.data(), response.value.size());
    return Diagnostic(result, Checkpoint::WeaverRead, CodeSource::Hal,
                      static_cast<int32_t>(response.status));
  }
  int result = RC_SERVICES_UNAVAILABLE;
  android::hardware::hidl_vec<uint8_t> k;
  k.setToExternal(key.data(), key.size());
  auto status = old_weaver_->read(
      slot, k, [&](oldwv::WeaverReadStatus s, const oldwv::WeaverReadResponse& r) {
        if (s == oldwv::WeaverReadStatus::OK && r.value.size() == value_size && r.timeout == 0) {
          secret->assign(r.value.begin(), r.value.end());
          result = RC_OK;
        } else if (s == oldwv::WeaverReadStatus::THROTTLE ||
                   s == oldwv::WeaverReadStatus::INCORRECT_KEY)
          result = r.timeout                                     ? Retry(r.timeout, retry)
                   : s == oldwv::WeaverReadStatus::INCORRECT_KEY ? RC_WRONG_CREDENTIAL
                                                                 : RC_IO_ERROR;
        else
          result = RC_IO_ERROR;
        Diagnostic(result, Checkpoint::WeaverRead, CodeSource::Hal, static_cast<int32_t>(s));
      });
  return status.isOk() ? result : Diagnostic(RC_SERVICES_UNAVAILABLE, Checkpoint::WeaverRead);
}
}  // namespace recovery_crypto::android17
