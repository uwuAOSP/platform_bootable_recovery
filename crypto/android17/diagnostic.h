/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <cstdint>
#include <string>

namespace recovery_crypto::android17 {
// No free-form strings, user IDs, paths, credentials, lengths, tokens or blobs.
enum class Checkpoint {
  Services, Metadata, DeKeys, CredentialType, SpUnlock, CeLoad,
  CredentialFormat, ProtectorKey, Stretch, GatekeeperInput, GatekeeperVerify, GatekeeperToken,
  WeaverRead, SpDiscardable, SpSoftwareDecrypt, SpFormat, SpHandle, SpDerive,
  KeymintInput, KeymintBegin, KeymintFinish, KeymintOutput, SecureClock,
  StorageExport, StoredKeyRead, StoredKeyDecrypt, FscryptPolicy, FscryptKeyShape,
  FscryptDescriptor, FscryptAddKey, FscryptIdentifier, FscryptStatus, CeKeyDirectories,
};
enum class CodeSource { None, Errno, BinderException, Hal };
std::string FormatDiagnostic(int result, Checkpoint point,
                             CodeSource source = CodeSource::None, int32_t code = 0);
// Best-effort append to an existing root-owned regular /tmp/recovery.log.
// Does not create a log, follow symlinks, require logd, or alter the result/errno.
int Diagnostic(int result, Checkpoint point,
               CodeSource source = CodeSource::None, int32_t code = 0);
}  // namespace recovery_crypto::android17
