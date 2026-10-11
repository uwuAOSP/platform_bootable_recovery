/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A product may select the generic Android 17 backend or implement this ABI in
// a Recovery-only device-tree module. Install exactly one with stem
// librecovery_crypto_backend. Device-specific sources stay outside this project.
#define RECOVERY_CRYPTO_BACKEND_ABI 1u
#define RECOVERY_CRYPTO_MAX_CREDENTIAL 128u
#define RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE 3u
#define RECOVERY_CRYPTO_MAX_PATTERN_SIZE 6u

enum recovery_crypto_result {
  RC_OK = 0,
  RC_UNSUPPORTED = 1,
  RC_SERVICES_UNAVAILABLE = 2,
  RC_EXISTING_KEY_MISSING = 3,
  RC_KEY_UPGRADE_REQUIRED = 4,
  RC_WRONG_CREDENTIAL = 5,
  RC_RETRY = 6,
  RC_IO_ERROR = 7,
};

enum recovery_crypto_credential {
  RC_CREDENTIAL_NONE = 0,
  RC_CREDENTIAL_PIN = 1,
  RC_CREDENTIAL_PASSWORD = 2,
  RC_CREDENTIAL_PATTERN = 3,
};

struct recovery_crypto_backend_v1 {
  uint32_t abi_version;
  uint32_t struct_size;
  // All operations concern EXISTING keys only. Never format, generate, delete,
  // fixate, reenroll or rewrite stored credentials/keys on an unlock attempt.
  // Implement bounded Binder lookups; never waitForService without a deadline.
  int32_t (*prepare_services)(uint32_t user_id);
  int32_t (*mount_metadata)(uint32_t user_id);
  int32_t (*load_de_keys)(uint32_t user_id);
  int32_t (*get_credential_type)(uint32_t user_id, uint32_t* credential_type);
  // The credential is NOT the vold CE secret. Resolve the current platform's
  // Synthetic Password protector, authenticate with Gatekeeper/Weaver, derive
  // the fbe-key subkey, and load the existing CE key. Preserve hardware retry
  // limits. retry_seconds must be nonzero when returning RC_RETRY.
  // PIN/password: UTF-8 bytes (UI currently supports ASCII passwords).
  // Pattern: ordered zero-based row-major cell bytes, including skipped cells.
  // The grid defaults to 3x3; larger grids require the optional size export below.
  // NONE: null pointer, zero length; still resolve the no-LSKF SP protector.
  // Do not retain or print credentials, auth tokens or key material.
  int32_t (*unlock_ce)(uint32_t user_id, uint32_t credential_type,
                       const uint8_t* credential, size_t length, uint32_t* retry_seconds);
  // Release transient memory/handles. Leave mounted userdata and installed
  // fscrypt keys usable by the Recovery file picker; do not revoke them here.
  void (*finish)(void);
};

typedef const struct recovery_crypto_backend_v1* (*recovery_crypto_get_backend_fn)(void);
// Export this unmangled function from the device's shared library.
const struct recovery_crypto_backend_v1* recovery_crypto_get_backend_v1(void);

// Optional separate export, leaving the existing v1 struct layout unchanged.
// Called after get_credential_type succeeds for a pattern, before asking for
// input. Read the user's saved grid size without authenticating or changing it.
// Return RC_OK and a size in 3..6; other sizes must return RC_UNSUPPORTED.
// A backend without this export retains the legacy 3x3 behavior.
typedef int32_t (*recovery_crypto_get_pattern_size_fn)(uint32_t user_id, uint32_t* size);
int32_t recovery_crypto_get_pattern_size_v1(uint32_t user_id, uint32_t* size);

#ifdef __cplusplus
}
#endif
