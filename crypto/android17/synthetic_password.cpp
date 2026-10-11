// SPDX-License-Identifier: Apache-2.0
// Native unlock-only port of the pinned Android 17 SyntheticPasswordManager/Crypto.
// Copyright (C) 2017-2018 The Android Open Source Project
// Copyright (C) 2026 The uwuAOSP Project
#include "synthetic_password.h"
#include <algorithm>
#include <cstdio>
#include "diagnostic.h"
#include "files.h"
#include "primitives.h"
#include "sqlite_snapshot.h"
namespace recovery_crypto::android17 {
static uint32_t Be(View v, size_t p) {
  return (uint32_t(v[p]) << 24) | (uint32_t(v[p + 1]) << 16) | (uint32_t(v[p + 2]) << 8) | v[p + 3];
}
bool ParsePasswordData(View v, PasswordData* data) {
  if (v.size() < 15 || v.size() > 8192) return false;
  // Android's parser explicitly discards the high short (Android 14 beta compatibility).
  data->type = static_cast<int16_t>(Be(v, 0));
  if (data->type != -1 && data->type != 1 && data->type != 2 && data->type != 3 && data->type != 4)
    return false;
  data->n = v[4];
  data->r = v[5];
  data->p = v[6];
  if (data->n < 1 || data->n > 20 || data->r > 8 || data->p > 8) return false;
  uint32_t salt = Be(v, 7);
  if (!salt || salt > 64 || size_t(salt) + 15 > v.size()) return false;
  size_t offset = 11 + salt;
  uint32_t handle = Be(v, offset);
  offset += 4;
  if (handle > 4096 || handle > v.size() - offset ||
      (v.size() - offset - handle != 0 && v.size() - offset - handle != 4))
    return false;
  data->salt.assign(v.begin() + 11, v.begin() + 11 + salt);
  data->handle.assign(v.begin() + offset, v.begin() + offset + handle);
  return true;
}
std::string SyntheticPassword::State(const char* name, bool global) const {
  char file[64];
  snprintf(file, sizeof(file), "/%016llx.%s",
           static_cast<unsigned long long>(global ? 0 : protector_), name);
  return "/data/system_de/" + std::to_string(user_) + "/spblob" + file;
}
int SyntheticPassword::Load(uint32_t user, uint32_t* credential_type) {
  user_ = user;
  if (user > 10000 || !ReadProtectorId(user, &protector_)) return RC_EXISTING_KEY_MISSING;
  if (!ReadFile(State("spblob"), &blob_, 8192)) return RC_EXISTING_KEY_MISSING;
  if (blob_.size() < 58 || blob_[0] < 1 || blob_[0] > 3 || blob_[1] != 0) return RC_UNSUPPORTED;
  Bytes raw, slot;
  bool missing = false;
  if (ReadFile(State("pwd"), &raw, 8192, &missing)) {
    password_.emplace();
    if (!ParsePasswordData(raw, &*password_)) return RC_UNSUPPORTED;
    switch (password_->type) {
      case -1:
        type_ = RC_CREDENTIAL_NONE;
        break;
      case 1:
        type_ = RC_CREDENTIAL_PATTERN;
        break;
      case 3:
        type_ = RC_CREDENTIAL_PIN;
        break;
      case 4:
        type_ = RC_CREDENTIAL_PASSWORD;
        break;
      case 2: {
        int64_t quality = 0;
        if (!ReadPasswordQuality(user, &quality)) return RC_UNSUPPORTED;
        if (quality == 0x20000 || quality == 0x30000)
          type_ = RC_CREDENTIAL_PIN;
        else if (quality == 0x40000 || quality == 0x50000 || quality == 0x60000)
          type_ = RC_CREDENTIAL_PASSWORD;
        else
          return RC_UNSUPPORTED;
        break;
      }
      default:
        return RC_UNSUPPORTED;
    }
  } else {
    if (!missing) return RC_IO_ERROR;
    // Do not infer an empty LSKF from a lost PasswordData file alone.
    int64_t quality = -1;
    if (!ReadPasswordQuality(user, &quality) || quality != 0) return RC_EXISTING_KEY_MISSING;
    type_ = RC_CREDENTIAL_NONE;
  }
  if (ReadFile(State("weaver"), &slot, 5, &missing)) {
    if (slot.size() != 5 || slot[0] != 1 || Be(slot, 1) > INT32_MAX) return RC_UNSUPPORTED;
    weaver_slot_ = Be(slot, 1);
  } else if (!missing)
    return RC_IO_ERROR;
  if (type_ != RC_CREDENTIAL_NONE && !weaver_slot_ && (!password_ || password_->handle.empty()))
    return RC_EXISTING_KEY_MISSING;
  if (type_ == RC_CREDENTIAL_PATTERN && !ReadPatternSize(user, &pattern_size_))
    return RC_UNSUPPORTED;
  *credential_type = type_;
  return RC_OK;
}
bool EncodePatternCredential(View credential, uint32_t size, Bytes* encoded) {
  Clear(*encoded);
  if (size < RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE || size > RECOVERY_CRYPTO_MAX_PATTERN_SIZE ||
      credential.size() < 4 || credential.size() > size * size)
    return false;
  bool used[RECOVERY_CRYPTO_MAX_PATTERN_SIZE * RECOVERY_CRYPTO_MAX_PATTERN_SIZE]{};
  for (uint8_t cell : credential) {
    if (cell >= size * size || used[cell]) {
      OPENSSL_cleanse(used, sizeof(used));
      Clear(*encoded);
      return false;
    }
    used[cell] = true;
    // LockPatternUtils.patternToByteArray: row * gridSize + column + '1'.
    // Cells above index 8 are still individual bytes, not decimal strings.
    encoded->push_back(cell + '1');
  }
  OPENSSL_cleanse(used, sizeof(used));
  return true;
}
int SyntheticPassword::Unlock(Hal& hal, uint32_t type, View credential, Bytes* fbe_key,
                              uint32_t* retry) {
  *retry = 0;
  Clear(*fbe_key);
  if (!protector_ || blob_.size() < 2 || type != type_ ||
      credential.size() > RECOVERY_CRYPTO_MAX_CREDENTIAL)
    return Diagnostic(RC_IO_ERROR, Checkpoint::CredentialFormat);
  // Read the database before authenticating: SP keys have a short auth timeout,
  // and a large snapshot must not consume that window after Gatekeeper succeeds.
  Bytes key;
  int level = 0;
  if (!ReadProtectorKey(protector_, &key, &level))
    return Diagnostic(RC_EXISTING_KEY_MISSING, Checkpoint::ProtectorKey);
  if (level != hal.security_level()) return Diagnostic(RC_UNSUPPORTED, Checkpoint::ProtectorKey);
  Diagnostic(RC_OK, Checkpoint::ProtectorKey);
  Bytes password;
  if (type == RC_CREDENTIAL_NONE) {
    if (!credential.empty()) return Diagnostic(RC_IO_ERROR, Checkpoint::CredentialFormat);
    auto value = AsBytes("default-password");
    password.assign(value.begin(), value.end());
  } else if (type == RC_CREDENTIAL_PATTERN) {
    if (!EncodePatternCredential(credential, pattern_size_, &password))
      return Diagnostic(RC_IO_ERROR, Checkpoint::CredentialFormat);
  } else {
    if (credential.empty() ||
        (type == RC_CREDENTIAL_PIN && !std::all_of(credential.begin(), credential.end(),
                                                   [](uint8_t c) { return c >= '0' && c <= '9'; })))
      return Diagnostic(RC_IO_ERROR, Checkpoint::CredentialFormat);
    password.assign(credential.begin(), credential.end());
  }
  Bytes stretched;
  if (password_) {
    if (!Stretch(password, password_->salt, password_->n, password_->r, password_->p, &stretched))
      return Diagnostic(RC_UNSUPPORTED, Checkpoint::Stretch);
  } else {
    stretched = password;
    stretched.resize(32, 0);
  }
  Clear(password);
  Authentication auth;
  Bytes secret;
  if (weaver_slot_) {
    Bytes value;
    int result = hal.ReadWeaver(*weaver_slot_, stretched, &value, retry);
    if (result != RC_OK) return result;
    auto hash = PersonalizedHash("weaver-pwd", value);
    secret = Concat(stretched, hash);
  } else {
    if (password_ && !password_->handle.empty()) {
      auto gk_password = PersonalizedHash("user-gk-authentication", stretched);
      int result =
          hal.VerifyGatekeeper(100000 + user_, password_->handle, gk_password, &auth, retry);
      if (result != RC_OK) return result;
    } else if (type != RC_CREDENTIAL_NONE)
      return Diagnostic(RC_EXISTING_KEY_MISSING, Checkpoint::GatekeeperVerify);
    Bytes discardable;
    if (!ReadFile(State("secdis"), &discardable, 16384))
      return Diagnostic(RC_EXISTING_KEY_MISSING, Checkpoint::SpDiscardable);
    if (discardable.size() != 16384) return Diagnostic(RC_IO_ERROR, Checkpoint::SpDiscardable);
    auto hash = PersonalizedHash("secdiscardable-transform", discardable);
    secret = Concat(stretched, hash);
  }
  auto wrapping = PersonalizedHash("application-id", secret);
  wrapping.resize(32);
  Bytes intermediate, sp;
  int result = RC_IO_ERROR;
  if (blob_[0] == 1) {
    if (!GcmDecrypt(wrapping, View(blob_).subspan(2), &intermediate))
      return Diagnostic(RC_IO_ERROR, Checkpoint::SpSoftwareDecrypt);
    result = hal.Decrypt(key, intermediate, {}, &auth, &sp);
  } else {
    result = hal.Decrypt(key, View(blob_).subspan(2), {}, &auth, &intermediate);
    if (result == RC_OK && !GcmDecrypt(wrapping, intermediate, &sp))
      return Diagnostic(RC_IO_ERROR, Checkpoint::SpSoftwareDecrypt);
  }
  if (result != RC_OK) return result;
  // Android's synthetic password is the ASCII hex of a personalized SHA-512 digest.
  // It is NOT decoded to raw bytes before deriving its subkeys.
  if (sp.size() != 128 || !std::all_of(sp.begin(), sp.end(), [](uint8_t c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
      }))
    return Diagnostic(RC_UNSUPPORTED, Checkpoint::SpFormat);
  Diagnostic(RC_OK, Checkpoint::SpFormat);
  Bytes handle;
  bool missing = false;
  if (ReadFile(State("handle", true), &handle, 4096, &missing)) {
    auto gk_password = SpSubkey(blob_[0], sp, "sp-gk-authentication");
    Authentication refreshed;
    result = hal.VerifyGatekeeper(user_, handle, gk_password, &refreshed, retry);
    if (result != RC_OK) return Diagnostic(result, Checkpoint::SpHandle);
  } else if (!missing)
    return Diagnostic(RC_IO_ERROR, Checkpoint::SpHandle);
  *fbe_key = SpSubkey(blob_[0], sp, "fbe-key");
  return Diagnostic(fbe_key->size() == (blob_[0] == 3 ? 32u : 64u) ? RC_OK : RC_IO_ERROR,
                    Checkpoint::SpDerive);
}
}  // namespace recovery_crypto::android17
