// SPDX-License-Identifier: Apache-2.0
// Format/derivation ported from AOSP SyntheticPasswordCrypto, SP800Derive and vold KeyStorage.
// Copyright (C) 2017-2018 The Android Open Source Project
// Copyright (C) 2026 The uwuAOSP Project
#include "primitives.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <array>
#include <climits>
#include <cstring>
#include <memory>
namespace recovery_crypto::android17 {
Bytes Concat(View a, View b) {
  Bytes out(a.begin(), a.end());
  out.insert(out.end(), b.begin(), b.end());
  return out;
}
Bytes PersonalizedHash(std::string_view label, View data) {
  if (label.size() > SHA512_CBLOCK) return {};
  std::array<uint8_t, SHA512_CBLOCK> prefix{};
  memcpy(prefix.data(), label.data(), label.size());
  SHA512_CTX ctx;
  SHA512_Init(&ctx);
  SHA512_Update(&ctx, prefix.data(), prefix.size());
  SHA512_Update(&ctx, data.data(), data.size());
  Bytes out(SHA512_DIGEST_LENGTH);
  SHA512_Final(out.data(), &ctx);
  OPENSSL_cleanse(&ctx, sizeof(ctx));
  return out;
}
static void Be32(Bytes& out, uint32_t n) {
  for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(n >> shift));
}
Bytes SpSubkey(uint8_t version, View sp, std::string_view label) {
  if (version == 1 || version == 2) return PersonalizedHash(label, sp);
  if (version != 3 || sp.empty()) return {};
  constexpr std::string_view context = "android-synthetic-password-personalization-context";
  Bytes input;
  Be32(input, 1);
  input.insert(input.end(), label.begin(), label.end());
  input.push_back(0);
  input.insert(input.end(), context.begin(), context.end());
  Be32(input, context.size() * 8);
  Be32(input, 256);
  Bytes out(32);
  unsigned length = 0;
  if (!HMAC(EVP_sha256(), sp.data(), sp.size(), input.data(), input.size(), out.data(), &length) ||
      length != 32)
    return {};
  return out;
}
bool Stretch(View password, View salt, unsigned n, unsigned r, unsigned p, Bytes* out) {
  // Never use attacker-controlled shift counts or permit unbounded scrypt allocations.
  if (n < 1 || n > 20 || r > 8 || p > 8 || salt.empty() || salt.size() > 64) return false;
  uint64_t N = uint64_t{ 1 } << n, R = uint64_t{ 1 } << r, P = uint64_t{ 1 } << p;
  if (N * R > 262144 || R * P > 4096) return false;
  out->resize(32);
  if (EVP_PBE_scrypt(reinterpret_cast<const char*>(password.data()), password.size(), salt.data(),
                     salt.size(), N, R, P, 64 * 1024 * 1024, out->data(), out->size()) != 1) {
    Clear(*out);
    return false;
  }
  return true;
}
bool GcmDecrypt(View key, View blob, Bytes* out) {
  Clear(*out);
  if (key.size() != 32 || blob.size() < 28 || blob.size() > 1024 * 1024) return false;
  auto ctx = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>(EVP_CIPHER_CTX_new(),
                                                                             EVP_CIPHER_CTX_free);
  if (!ctx) return false;
  size_t size = blob.size() - 28;
  Bytes plain(size + 16);
  int count = 0, last = 0;
  bool ok =
      EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.data(), blob.data()) == 1 &&
      EVP_DecryptUpdate(ctx.get(), plain.data(), &count, blob.data() + 12, size) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16,
                          const_cast<uint8_t*>(blob.data() + blob.size() - 16)) == 1 &&
      EVP_DecryptFinal_ex(ctx.get(), plain.data() + count, &last) == 1;
  if (!ok || count < 0 || last < 0 || static_cast<size_t>(count + last) != size) return false;
  plain.resize(size);
  *out = std::move(plain);
  return true;
}
}  // namespace recovery_crypto::android17
