/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "recovery_crypto/session.h"

namespace recovery_crypto {
constexpr uint32_t kProtocol = 2;
constexpr uint32_t kPrepare = 1;
constexpr uint32_t kUnlock = 2;
constexpr uint32_t kProgress = 1;
constexpr uint32_t kResult = 2;
constexpr int kWorkerFd = 3;
constexpr int kDeadlineSeconds = 45;
constexpr const char* kWorkerPath = "/system/bin/recovery_crypto_worker";
#if defined(__LP64__)
constexpr const char* kBackendPath = "/system/lib64/librecovery_crypto_backend.so";
#else
constexpr const char* kBackendPath = "/system/lib/librecovery_crypto_backend.so";
#endif
// Fixed-size packets; no user-supplied paths, free-form output or shell parsing.
struct Request {
  uint32_t protocol = kProtocol;
  uint32_t operation = 0;
  uint32_t user_id = 0;
  uint32_t credential_type = RC_CREDENTIAL_NONE;
  uint32_t length = 0;
  uint32_t pattern_size = RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE;
  uint8_t credential[RECOVERY_CRYPTO_MAX_CREDENTIAL]{};
};
struct Reply {
  uint32_t protocol = kProtocol;
  uint32_t kind = kResult;
  uint32_t status = static_cast<uint32_t>(Status::Unsupported);
  uint32_t stage = static_cast<uint32_t>(Stage::Services);
  uint32_t credential_type = RC_CREDENTIAL_NONE;
  uint32_t retry_seconds = 0;
  uint32_t pattern_size = RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE;
};
bool ValidReply(const Reply& reply);
bool ValidRequest(const Request& request);
Status BackendStatus(int32_t code);
bool TrustedFile(const char* path);
}  // namespace recovery_crypto
