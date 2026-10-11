/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "diagnostic.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include "files.h"
#include "recovery_crypto/backend.h"

namespace recovery_crypto::android17 {
static const char* Name(Checkpoint point) {
  switch (point) {
    case Checkpoint::Services: return "services";
    case Checkpoint::Metadata: return "metadata";
    case Checkpoint::DeKeys: return "de_keys";
    case Checkpoint::CredentialType: return "credential_type";
    case Checkpoint::SpUnlock: return "sp_unlock";
    case Checkpoint::CeLoad: return "ce_load";
    case Checkpoint::CredentialFormat: return "credential_format";
    case Checkpoint::ProtectorKey: return "protector_key";
    case Checkpoint::Stretch: return "stretch";
    case Checkpoint::GatekeeperInput: return "gatekeeper_input";
    case Checkpoint::GatekeeperVerify: return "gatekeeper_verify";
    case Checkpoint::GatekeeperToken: return "gatekeeper_token";
    case Checkpoint::WeaverRead: return "weaver_read";
    case Checkpoint::SpDiscardable: return "sp_discardable";
    case Checkpoint::SpSoftwareDecrypt: return "sp_software_decrypt";
    case Checkpoint::SpFormat: return "sp_format";
    case Checkpoint::SpHandle: return "sp_handle";
    case Checkpoint::SpDerive: return "sp_derive";
    case Checkpoint::KeymintInput: return "keymint_input";
    case Checkpoint::KeymintBegin: return "keymint_begin";
    case Checkpoint::KeymintFinish: return "keymint_finish";
    case Checkpoint::KeymintOutput: return "keymint_output";
    case Checkpoint::SecureClock: return "secureclock";
    case Checkpoint::StorageExport: return "storage_export";
    case Checkpoint::StoredKeyRead: return "stored_key_read";
    case Checkpoint::StoredKeyDecrypt: return "stored_key_decrypt";
    case Checkpoint::FscryptPolicy: return "fscrypt_policy";
    case Checkpoint::FscryptKeyShape: return "fscrypt_key_shape";
    case Checkpoint::FscryptDescriptor: return "fscrypt_descriptor";
    case Checkpoint::FscryptAddKey: return "fscrypt_add_key";
    case Checkpoint::FscryptIdentifier: return "fscrypt_identifier";
    case Checkpoint::FscryptStatus: return "fscrypt_status";
    case Checkpoint::CeKeyDirectories: return "ce_key_directories";
  }
  return nullptr;
}
std::string FormatDiagnostic(int result, Checkpoint point, CodeSource source, int32_t code) {
  const char* name = Name(point);
  const char* facility = nullptr;
  switch (source) {
    case CodeSource::None: facility = "none"; break;
    case CodeSource::Errno: facility = "errno"; break;
    case CodeSource::BinderException: facility = "binder"; break;
    case CodeSource::Hal: facility = "hal"; break;
  }
  if (!name || !facility || result < RC_OK || result > RC_IO_ERROR ||
      (source == CodeSource::None && code != 0))
    return {};
  char line[160];
  int length = snprintf(line, sizeof(line),
                        "[recovery_crypto] point=%s result=%d source=%s code=%d\n",
                        name, result, facility, static_cast<int>(code));
  return length > 0 && length < static_cast<int>(sizeof(line)) ? std::string(line, length)
                                                             : std::string();
}
int Diagnostic(int result, Checkpoint point, CodeSource source, int32_t code) {
  const int saved_errno = errno;
  const auto line = FormatDiagnostic(result, point, source, code);
  if (!line.empty()) {
    // Opening through the directory helper also rejects symlink path components.
    auto directory = OpenDirectory("/tmp");
    android::base::unique_fd fd(directory.get() < 0 ? -1 :
        openat(directory.get(), "recovery.log", O_WRONLY | O_APPEND | O_CLOEXEC |
                                                  O_NOFOLLOW | O_NONBLOCK));
    struct stat st{};
    if (fd.get() >= 0 && fstat(fd.get(), &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
        st.st_size >= 0 && st.st_size < 4 * 1024 * 1024) {
      size_t offset = 0;
      for (int attempt = 0; attempt < 3 && offset < line.size(); ++attempt) {
        auto count = write(fd.get(), line.data() + offset, line.size() - offset);
        if (count > 0) offset += count;
        else if (count == 0 || errno != EINTR) break;
      }
    }
  }
  errno = saved_errno;
  return result;
}
}  // namespace recovery_crypto::android17
