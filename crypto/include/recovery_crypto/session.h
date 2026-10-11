/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "recovery_crypto/backend.h"

namespace recovery_crypto {
enum class Stage : uint32_t { Services, Metadata, DeviceKeys, Credential, CredentialKeys, Verify };
enum class Status : uint32_t {
  Ready, CredentialRequired, Unsupported, ServicesUnavailable, MissingKey, UpgradeRequired,
  WrongCredential, Throttled, IoError, InvalidBackend, Timeout, WorkerFailed, StorageLocked,
};
struct Result {
  Status status = Status::Unsupported;
  Stage stage = Stage::Services;
  uint32_t credential_type = RC_CREDENTIAL_NONE;
  uint32_t retry_seconds = 0;
  uint32_t pattern_size = RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE;
};

// No mutable std::string copies, logging or command-line transport of secrets.
class Credential final {
 public:
  Credential();
  ~Credential();
  Credential(const Credential&) = delete;
  Credential& operator=(const Credential&) = delete;
  bool Append(uint8_t byte);
  void EraseLast();
  void Clear();
  const uint8_t* data() const { return bytes_; }
  size_t size() const { return length_; }
  bool Contains(uint8_t byte) const;
  bool secure() const { return locked_; }
 private:
  uint8_t* bytes_ = nullptr;
  size_t length_ = 0;
  bool locked_ = false;
};

// The worker and optional backend use fixed Recovery paths. Presence does not
// certify compatibility. Presence checks do not start services or unlock keys.
bool BackendInstalled();
const char* StatusMessage(Status status);
const char* StageMessage(Stage stage);
// Checks the actual mount, fscrypt policy and kernel key status; no key reads.
bool VerifyUserStorage(uint32_t user_id);
std::string UserStoragePath(uint32_t user_id);

class Session final {
 public:
  Session() = default;
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Result Prepare(uint32_t user_id, const std::function<void(Stage)>& progress = {});
  Result Unlock(const Credential& credential, const std::function<void(Stage)>& progress = {});
 private:
  Result Exchange(uint32_t operation, const Credential* credential,
                  const std::function<void(Stage)>& progress);
  void Stop();
  int fd_ = -1;
  int pid_ = -1;
  uint32_t user_id_ = 0;
  uint32_t credential_type_ = RC_CREDENTIAL_NONE;
  uint32_t pattern_size_ = RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE;
  bool prepared_ = false;
  std::chrono::steady_clock::time_point retry_after_{};
};
}  // namespace recovery_crypto
