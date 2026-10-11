/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <memory>
#include "config.h"
#include "diagnostic.h"
#include "hal.h"
#include "storage.h"
#include "synthetic_password.h"
namespace recovery_crypto::android17 {
struct Session {
  uint32_t user = 0;
  unsigned stage = 0;
  Config config;
  Hal hal;
  Storage storage;
  SyntheticPassword protector;
};
static std::unique_ptr<Session> session;
static int32_t Prepare(uint32_t user) {
  if (user > 10000) return RC_UNSUPPORTED;
  session = std::make_unique<Session>();
  session->user = user;
  if (!ReadConfig(&session->config)) return RC_UNSUPPORTED;
  int result = session->storage.Configure(session->config);
  if (result != RC_OK) return result;
  result = session->hal.Connect(session->config);
  if (result == RC_OK) session->stage = 1;
  return Diagnostic(result, Checkpoint::Services);
}
static bool Stage(uint32_t user, unsigned stage) {
  return session && session->user == user && session->stage == stage;
}
static int32_t Mount(uint32_t user) {
  if (!Stage(user, 1)) return RC_IO_ERROR;
  int result = session->storage.Mount(session->hal);
  if (result == RC_OK) session->stage = 2;
  return Diagnostic(result, Checkpoint::Metadata);
}
static int32_t De(uint32_t user) {
  if (!Stage(user, 2)) return RC_IO_ERROR;
  int result = session->storage.LoadDe(session->hal, user);
  if (result == RC_OK) session->stage = 3;
  return Diagnostic(result, Checkpoint::DeKeys);
}
static int32_t Type(uint32_t user, uint32_t* type) {
  if (!Stage(user, 3) || !type) return RC_IO_ERROR;
  int result = session->protector.Load(user, type);
  if (result == RC_OK) session->stage = 4;
  return Diagnostic(result, Checkpoint::CredentialType);
}
static int32_t Ce(uint32_t user, uint32_t type, const uint8_t* credential, size_t length,
                  uint32_t* retry) {
  if (!Stage(user, 4) || !retry || (length && !credential))
    return Diagnostic(RC_IO_ERROR, Checkpoint::CredentialFormat);
  Bytes secret;
  int result =
      session->protector.Unlock(session->hal, type, { credential, length }, &secret, retry);
  Diagnostic(result, Checkpoint::SpUnlock);
  if (result != RC_OK) return result;
  result = session->storage.LoadCe(session->hal, user, secret);
  if (result == RC_OK) {
    session->stage = 5;
    // Optional export permission is separate from authentication success.
    // Remount failures retain the decrypted read-only browser/download path.
    session->storage.EnableMediaWrites(user);
  }
  return Diagnostic(result, Checkpoint::CeLoad);
}
static void Finish() {
  session.reset();
}
static const recovery_crypto_backend_v1 backend{ RECOVERY_CRYPTO_BACKEND_ABI,
                                                 sizeof(recovery_crypto_backend_v1),
                                                 Prepare,
                                                 Mount,
                                                 De,
                                                 Type,
                                                 Ce,
                                                 Finish };
}  // namespace recovery_crypto::android17
extern "C" __attribute__((visibility("default"))) const recovery_crypto_backend_v1*
recovery_crypto_get_backend_v1() {
  return &recovery_crypto::android17::backend;
}
extern "C" __attribute__((visibility("default"))) int32_t
recovery_crypto_get_pattern_size_v1(uint32_t user, uint32_t* size) {
  using namespace recovery_crypto::android17;
  if (!Stage(user, 4) || !size) return RC_IO_ERROR;
  *size = session->protector.PatternSize();
  return RC_OK;
}
