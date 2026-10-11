/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "install/crypto.h"
#include "install/fuse_install.h"
#include "bootloader_message/bootloader_message.h"
#include "recovery_crypto/session.h"

#include <android-base/strings.h>
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace {
using recovery_crypto::Credential;
using recovery_crypto::Result;
using recovery_crypto::Stage;
using recovery_crypto::Status;
constexpr uint32_t kUser = 0;

size_t Select(Device* device, const std::vector<std::string>& headers,
              const std::vector<std::string>& items) {
  return device->GetUI()->ShowMenu(headers, items, 0, true,
      std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));
}
void ShowError(Device* device, const Result& result) {
  std::vector<std::string> headers{"Unlock internal storage",
      recovery_crypto::StatusMessage(result.status), recovery_crypto::StageMessage(result.stage)};
  if (result.status == Status::Throttled) {
    headers.push_back("Retry after " + std::to_string(result.retry_seconds) + " seconds");
  }
  Select(device, headers, {"Continue"});
}

bool ReadCredential(Device* device, uint32_t type, uint32_t pattern_size, Credential* credential) {
  credential->Clear();
  if (!credential->secure()) { ShowError(device, {Status::IoError}); return false; }
  if (type == RC_CREDENTIAL_PATTERN) {
    class LockedPattern final : public recovery_ui::PatternInput {
     public:
      LockedPattern(Credential& credential, unsigned grid) : credential_(credential), grid_(grid) {}
      unsigned GridSize() const override { return grid_; }
      size_t Size() const override { return credential_.size(); }
      uint8_t Cell(size_t index) const override { return credential_.data()[index]; }
      bool Append(uint8_t cell) override { return credential_.Append(cell); }
      void Clear() override { credential_.Clear(); }
     private:
      Credential& credential_;
      unsigned grid_;
    } pattern(*credential, pattern_size);
    return device->GetUI()->ReadPattern(pattern);
  }
  if (type!=RC_CREDENTIAL_PIN && type!=RC_CREDENTIAL_PASSWORD) return false;
  class LockedPassword final : public recovery_ui::PasswordInput {
   public:
    LockedPassword(Credential& credential,bool numeric) : credential_(credential),numeric_(numeric) {}
    bool NumericOnly() const override { return numeric_; }
    size_t Size() const override { return credential_.size(); }
    bool Append(uint8_t character) override {
      if (numeric_ ? character<'0' || character>'9' : character<32 || character>126) return false;
      return credential_.Append(character);
    }
    void EraseLast() override { credential_.EraseLast(); }
    void Clear() override { credential_.Clear(); }
   private:
    Credential& credential_;
    bool numeric_;
  } input(*credential,type==RC_CREDENTIAL_PIN);
  return device->GetUI()->ReadPassword(input);
}

bool UnlockStorage(Device* device) {
  recovery_crypto::Session session;
  auto ui = device->GetUI();
  ui->SetInstallStage(RecoveryUI::InstallStage::NONE);
  const auto progress = [ui](Stage stage) { ui->Print("%s\n", recovery_crypto::StageMessage(stage)); };
  auto result = session.Prepare(kUser, progress);
  if (result.status == Status::Ready) return true;
  if (result.status != Status::CredentialRequired) { ShowError(device, result); return false; }
  for (;;) {
    Credential credential;
    if (!ReadCredential(device, result.credential_type, result.pattern_size, &credential)) return false;
    result = session.Unlock(credential, progress);
    credential.Clear();
    if (result.status == Status::Ready) return true;
    if (result.status != Status::WrongCredential) { ShowError(device, result); return false; }
    const auto selection = Select(device,
        {"Unlock internal storage", recovery_crypto::StatusMessage(result.status)},
        {"Try again", "Cancel"});
    if (selection != 0) return false;
  }
}

std::string BrowseInternalStorage(Device* device, const std::string& root,
                                 const std::string& extension, const std::string& title) {
  std::string current = root;
  for (;;) {
    DIR* directory = opendir(current.c_str());
    if (!directory) { ShowError(device, {Status::IoError, Stage::Verify}); return {}; }
    std::vector<std::string> directories, files;
    while (auto entry = readdir(directory)) {
      const std::string name(entry->d_name);
      if (name == "." || name == "..") continue;
      struct stat st{};
      if (fstatat(dirfd(directory), name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
      // Avoid filesystem exceptions and symlink loops/escapes; this optional
      // picker cannot browse above the selected user's media root.
      if (S_ISDIR(st.st_mode)) directories.push_back(name + "/");
      else if (S_ISREG(st.st_mode) && android::base::EndsWithIgnoreCase(name, extension)) files.push_back(name);
    }
    closedir(directory);
    std::sort(directories.begin(), directories.end());
    std::sort(files.begin(), files.end());
    std::vector<std::string> items{current == root ? "Cancel" : "../"};
    items.insert(items.end(), directories.begin(), directories.end());
    items.insert(items.end(), files.begin(), files.end());
    const auto picked = Select(device, {title, current}, items);
    if (picked == static_cast<size_t>(Device::kGoHome) ||
        picked == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) return {};
    if (picked == 0 || picked == static_cast<size_t>(Device::kGoBack)) {
      if (current == root) return {};
      current.erase(current.find_last_of('/'));
      continue;
    }
    if (picked >= items.size()) return {};
    std::string chosen = current + "/" + items[picked];
    if (chosen.back() == '/') { chosen.pop_back(); current = chosen; }
    else return chosen;
  }
}
}  // namespace

bool RecoveryCryptoAvailable() { return recovery_crypto::BackendInstalled(); }

std::string ChooseRecoveryStorageFile(Device* device, const std::string& root,
                                     const std::string& extension, const std::string& title) {
  return BrowseInternalStorage(device, root, extension, title);
}

bool UnlockRecoveryStorage(Device* device) {
  if (!RecoveryCryptoAvailable()) return false;
  if (recovery_crypto::VerifyUserStorage(kUser)) return true;
  return UnlockStorage(device);
}

InstallResult ApplyFromEncryptedStorage(Device* device) {
  if (!UnlockRecoveryStorage(device)) return INSTALL_NONE;
  const auto root = recovery_crypto::UserStoragePath(kUser);
  const auto path = ChooseRecoveryStorageFile(device, root, ".zip", "Choose ZIP from internal storage");
  if (path.empty() || path == "@") return INSTALL_NONE;
  // No .map files or symlinks escaping this user's media tree.
  char* resolved = realpath(path.c_str(), nullptr);
  if (!resolved) return INSTALL_ERROR;
  const std::string canonical(resolved);
  free(resolved);
  struct stat st{};
  if (!android::base::StartsWith(canonical, root + "/") ||
      !android::base::EndsWithIgnoreCase(canonical, ".zip") ||
      stat(canonical.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
      !recovery_crypto::VerifyUserStorage(kUser)) return INSTALL_ERROR;
  std::string error;
  if (!update_bootloader_message({}, &error)) return INSTALL_ERROR;
  return InstallWithFuseFromPath(canonical, device);
}
