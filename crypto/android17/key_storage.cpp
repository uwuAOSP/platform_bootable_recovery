// SPDX-License-Identifier: Apache-2.0
// Existing-key-only port of vold KeyStorage.cpp; never invokes its upgrade/creation paths.
// Copyright (C) 2016 The Android Open Source Project
// Copyright (C) 2026 The uwuAOSP Project
#include "key_storage.h"
#include "diagnostic.h"
#include "files.h"
#include "primitives.h"
namespace recovery_crypto::android17 {
int RetrieveExistingKey(Hal& hal, const std::string& directory, View ce_secret, Bytes* key) {
  Bytes version, discardable, encrypted, blob, upgraded;
  bool missing = false;
  if (!ReadFile(directory + "/version", &version, 16))
    return Diagnostic(RC_EXISTING_KEY_MISSING, Checkpoint::StoredKeyRead);
  if (version != Bytes{ '1' }) return Diagnostic(RC_UNSUPPORTED, Checkpoint::StoredKeyRead);
  if (!ReadFile(directory + "/encrypted_key", &encrypted, 65536))
    return Diagnostic(RC_EXISTING_KEY_MISSING, Checkpoint::StoredKeyRead);
  if (!ReadFile(directory + "/secdiscardable", &discardable, 16384, &missing) && !missing)
    return Diagnostic(RC_IO_ERROR, Checkpoint::StoredKeyRead);
  // An absent discardable is legitimate for modern software-wrapped CE keys, never DE/metadata.
  if (ce_secret.empty() && discardable.size() != 16384)
    return Diagnostic(RC_EXISTING_KEY_MISSING, Checkpoint::StoredKeyRead);
  if (!discardable.empty() && discardable.size() != 16384)
    return Diagnostic(RC_IO_ERROR, Checkpoint::StoredKeyRead);
  Bytes discard_hash;
  if (!discardable.empty())
    discard_hash = PersonalizedHash("Android secdiscardable SHA512", discardable);
  auto app_id = Concat(discard_hash, ce_secret);
  if (!ce_secret.empty()) {
    auto wrapping = PersonalizedHash("Android key wrapping key generation SHA512", app_id);
    wrapping.resize(32);
    return Diagnostic(GcmDecrypt(wrapping, encrypted, key) ? RC_OK : RC_IO_ERROR,
                      Checkpoint::StoredKeyDecrypt);
  }
  // A pending upgraded blob has ambiguous commit status. Leave both versions untouched.
  if (ReadFile(directory + "/keymaster_key_blob_upgraded", &upgraded, 65536, &missing))
    return Diagnostic(RC_KEY_UPGRADE_REQUIRED, Checkpoint::StoredKeyRead);
  if (!missing) return Diagnostic(RC_IO_ERROR, Checkpoint::StoredKeyRead);
  if (!ReadFile(directory + "/keymaster_key_blob", &blob, 65536))
    return Diagnostic(RC_EXISTING_KEY_MISSING, Checkpoint::StoredKeyRead);
  return hal.Decrypt(blob, encrypted, app_id, nullptr, key);
}
}  // namespace recovery_crypto::android17
