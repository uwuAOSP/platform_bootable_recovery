/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "install/image_flash.h"
#include "install/crypto.h"
#include "image_format.h"
#include "image_flash_policy.h"
#include "recovery_crypto/session.h"
#include "recovery_mtp/controller.h"
#include "recovery_utils/roots.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <libsnapshot/snapshot.h>
#include <openssl/evp.h>
#include <linux/fs.h>
#include <linux/memfd.h>
#include <fcntl.h>
#include <mntent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace {
using android::base::GetProperty;
using android::base::unique_fd;
using Stage = RecoveryUI::InstallStage;
constexpr uint64_t kMaxImage = 512ULL * 1024 * 1024;
constexpr size_t kBuffer = 256 * 1024;
struct Target {
  std::string name, base, path;
  dev_t device = 0;
  uint64_t size = 0;
};
size_t Select(Device* device, const std::vector<std::string>& headers,
              const std::vector<std::string>& items) {
  return device->GetUI()->ShowMenu(headers, items, 0, true,
      std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2));
}
void Notice(Device* device, const std::string& text) {
  Select(device, {"Flash partition image", text}, {"Back"});
}
const char* FlashBlocker(Device* device) {
  // No unlock operation or AVB changes are performed by this feature.
  const auto flash_locked = GetProperty("ro.boot.flash.locked", "");
  const auto vbmeta_state = GetProperty("ro.boot.vbmeta.device_state", "");
  const auto verified_boot_state = GetProperty("ro.boot.verifiedbootstate", "");
  const auto state = recovery_image::GetBootloaderState(flash_locked, vbmeta_state, verified_boot_state);
  LOG(INFO) << "Recovery image flash boot state: flash.locked='" << flash_locked
            << "' vbmeta.device_state='" << vbmeta_state
            << "' verifiedbootstate='" << verified_boot_state << "'";
  if (state == recovery_image::BootloaderState::Locked) return "Bootloader is locked. Cannot flash a partition image.";
  if (state != recovery_image::BootloaderState::Unlocked) return "Cannot confirm bootloader unlock status. No partition will be written.";
  if (device->GetReason().value_or("") == "update_in_progress") {
    return "An OTA update is pending. Cannot flash a partition image.";
  }
  if (android::base::GetBoolProperty("ro.virtual_ab.enabled", false)) {
    if (ensure_path_mounted("/metadata") != 0) return "Cannot verify OTA snapshot state. No partition will be written.";
    auto manager = android::snapshot::SnapshotManager::New();
    if (!manager) return "Cannot verify OTA snapshot state. No partition will be written.";
    if (manager->GetUpdateState() != android::snapshot::UpdateState::None) {
      return "An OTA update is pending. Cannot flash a partition image.";
    }
  }
  return nullptr;
}
bool NotMounted(dev_t device) {
  FILE* mounts = setmntent("/proc/mounts", "r");
  if (!mounts) return false;
  bool result = true;
  while (auto entry = getmntent(mounts)) {
    struct stat st{};
    if (!stat(entry->mnt_fsname, &st) && S_ISBLK(st.st_mode) && st.st_rdev == device) {
      result = false;
      break;
    }
  }
  endmntent(mounts);
  return result;
}
bool Inspect(const std::string& name, Target* target) {
  std::string base;
  if (!recovery_image::PhysicalName(name, &base)) return false;
  for (const auto* directory : {"/dev/block/by-name/", "/dev/block/bootdevice/by-name/"}) {
    char* resolved = realpath((std::string(directory) + name).c_str(), nullptr);
    if (!resolved) continue;
    std::string path(resolved);
    free(resolved);
    if (!android::base::StartsWith(path, "/dev/block/")) continue;
    unique_fd fd(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    struct stat st{};
    uint64_t size = 0;
    if (fd.get() < 0 || fstat(fd.get(), &st)) {
      PLOG(WARNING) << "Recovery image: cannot inspect partition " << name;
      continue;
    }
    if (!S_ISBLK(st.st_mode)) continue;
    if (ioctl(fd.get(), BLKGETSIZE64, &size)) {
      PLOG(WARNING) << "Recovery image: cannot read partition capacity " << name;
      continue;
    }
    if (!size || size > kMaxImage || !NotMounted(st.st_rdev)) continue;
    // A real partition has this sysfs file. Whole disks, loop and dm/snapshot
    // devices do not; no logical partition or super container is accepted.
    const auto sysfs = "/sys/dev/block/" + std::to_string(major(st.st_rdev)) + ":" +
                       std::to_string(minor(st.st_rdev)) + "/partition";
    // Only its existence is needed. R_OK additionally asks SELinux for file
    // read permission, which enforcing Recovery need not have for this marker.
    if (access(sysfs.c_str(), F_OK)) {
      PLOG(WARNING) << "Recovery image: cannot confirm physical partition " << name;
      continue;
    }
    *target = {name, base, path, st.st_rdev, size};
    return true;
  }
  return false;
}
std::vector<Target> Targets() {
  std::vector<Target> targets;
  for (const auto* base : {"boot", "init_boot", "vendor_boot", "recovery", "dtbo", "vbmeta", "vbmeta_system"}) {
    bool slotted = false;
    for (const auto* suffix : {"_a", "_b"}) {
      Target target;
      if (Inspect(std::string(base) + suffix, &target)) {
        slotted = true;
        targets.push_back(target);
      }
    }
    Target target;
    if (!slotted && Inspect(base, &target)) targets.push_back(target);
  }
  return targets;
}
// Open every path component without following symlinks, retaining the chosen
// source FD. The browser root never permits access outside media or /tmp.
unique_fd OpenImage(const std::string& root, const std::string& path) {
  if (!android::base::StartsWith(path, root + "/") ||
      !android::base::EndsWithIgnoreCase(path, ".img")) return unique_fd();
  unique_fd fd(open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  const auto parts = android::base::Split(path.substr(root.size() + 1), "/");
  for (size_t i = 0; i < parts.size(); ++i) {
    if (parts[i].empty() || parts[i] == "." || parts[i] == ".." || fd.get() < 0) return unique_fd();
    fd.reset(openat(fd.get(), parts[i].c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC |
        (i + 1 < parts.size() ? O_DIRECTORY : O_NONBLOCK)));
  }
  return fd;
}
bool EnoughMemory(uint64_t size) {
  std::string info;
  if (!android::base::ReadFileToString("/proc/meminfo", &info)) {
    PLOG(ERROR) << "Recovery image: cannot read /proc/meminfo for staging";
    return false;
  }
  const auto required_kb = (size + 64ULL * 1024 * 1024) / 1024;
  for (const auto& line : android::base::Split(info, "\n")) {
    unsigned long long kb = 0;
    if (sscanf(line.c_str(), "MemAvailable: %llu kB", &kb) == 1) {
      if (kb > required_kb) return true;
      LOG(ERROR) << "Recovery image: insufficient RAM for sealed copy; available="
                 << kb << " KiB required=" << required_kb << " KiB";
      return false;
    }
  }
  LOG(ERROR) << "Recovery image: /proc/meminfo has no readable MemAvailable value";
  return false;
}
bool Digest(int fd, uint64_t size, std::array<uint8_t, 32>* digest, RecoveryUI* ui = nullptr) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || !EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr)) return false;
  std::vector<uint8_t> buffer(kBuffer);
  for (uint64_t offset = 0; offset < size;) {
    const size_t bytes = std::min<uint64_t>(buffer.size(), size - offset);
    if (!android::base::ReadFullyAtOffset(fd, buffer.data(), bytes, offset) ||
        !EVP_DigestUpdate(context.get(), buffer.data(), bytes)) return false;
    offset += bytes;
    if (ui) ui->SetProgress(static_cast<float>(offset) / size);
  }
  unsigned int length = 0;
  return EVP_DigestFinal_ex(context.get(), digest->data(), &length) && length == digest->size();
}
bool StageImage(int input, uint64_t size, unique_fd* staged) {
  if (!EnoughMemory(size)) return false;
  staged->reset(memfd_create("recovery-image", MFD_CLOEXEC | MFD_ALLOW_SEALING));
  if (staged->get() < 0) { PLOG(ERROR) << "Recovery image: memfd_create"; return false; }
  std::vector<uint8_t> buffer(kBuffer);
  for (uint64_t offset = 0; offset < size;) {
    const size_t bytes = std::min<uint64_t>(buffer.size(), size - offset);
    if (!android::base::ReadFullyAtOffset(input, buffer.data(), bytes, offset) ||
        !android::base::WriteFully(staged->get(), buffer.data(), bytes)) {
      PLOG(ERROR) << "Recovery image: stage source bytes";
      return false;
    }
    offset += bytes;
  }
  struct stat after{};
  if (fstat(input, &after) || after.st_size < 0 || static_cast<uint64_t>(after.st_size) != size) return false;
  return fcntl(staged->get(), F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0;
}
bool DtboPayloadsValid(int image, const uint8_t* header) {
  const uint64_t total = recovery_image::Be32(header + 4);
  const uint64_t entry_size = recovery_image::Be32(header + 12);
  const uint64_t count = recovery_image::Be32(header + 16);
  const uint64_t offset = recovery_image::Be32(header + 20);
  const uint64_t table_end = offset + entry_size * count;
  std::array<uint8_t, 32> entry{};
  std::array<uint8_t, 40> dtb{};
  for (uint64_t i = 0; i < count; ++i) {
    if (!android::base::ReadFullyAtOffset(image, entry.data(), entry.size(), offset + i * entry_size) ||
        !recovery_image::ValidDtboEntry(entry.data(), total, table_end) ||
        !android::base::ReadFullyAtOffset(image, dtb.data(), dtb.size(), recovery_image::Be32(entry.data() + 4)) ||
        recovery_image::Be32(dtb.data()) != 0xd00dfeed || recovery_image::Be32(dtb.data() + 4) < dtb.size() ||
        recovery_image::Be32(dtb.data() + 4) > recovery_image::Be32(entry.data())) return false;
  }
  return true;
}
bool WriteAndVerify(int image, const Target& target, uint64_t size, Device* device, bool* started) {
  auto ui = device->GetUI();
  if (const auto* blocker = FlashBlocker(device)) {
    LOG(ERROR) << "Recovery image flash blocked: " << blocker;
    ui->Print("%s\n", blocker);
    return false;
  }
  Target current;
  if (!Inspect(target.name, &current) || current.path != target.path ||
      current.device != target.device || current.size != target.size || size > current.size) return false;
  std::array<uint8_t, 32> expected{}, actual{};
  if (!Digest(image, size, &expected)) return false;
  unique_fd output(open(current.path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC | O_EXCL));
  struct stat st{};
  uint64_t capacity = 0;
  if (output.get() < 0 || fstat(output.get(), &st) || !S_ISBLK(st.st_mode) || st.st_rdev != target.device ||
      ioctl(output.get(), BLKGETSIZE64, &capacity) || capacity != target.size) return false;
  if (size < capacity) {
    std::array<uint8_t, 64> footer{};
    if (capacity < footer.size() ||
        !android::base::ReadFullyAtOffset(output.get(), footer.data(), footer.size(), capacity - footer.size())) return false;
    if (!std::memcmp(footer.data(), "AVBf", 4)) {
      LOG(ERROR) << "Recovery image: refusing shorter image over existing AVB footer; use a full partition-sized image";
      ui->Print("%s\n", "Existing AVB footer requires a full partition-sized image.");
      return false;
    }
  }
  // Verify cache invalidation is permitted before the first partition write.
  // A missing ioctl grant must not leave a flashed but unverified partition.
  if (ioctl(output.get(), BLKFLSBUF)) {
    PLOG(ERROR) << "Recovery image: cache invalidation preflight for " << target.name;
    return false;
  }
  ui->SetInstallStage(Stage::FLASH_WRITING);
  ui->SetProgressType(RecoveryUI::DETERMINATE);
  ui->ShowProgress(1, 0);
  std::vector<uint8_t> buffer(kBuffer);
  for (uint64_t offset = 0; offset < size;) {
    const size_t bytes = std::min<uint64_t>(buffer.size(), size - offset);
    if (!android::base::ReadFullyAtOffset(image, buffer.data(), bytes, offset)) return false;
    *started = true;
    if (!android::base::WriteFully(output.get(), buffer.data(), bytes)) return false;
    offset += bytes;
    ui->SetProgress(static_cast<float>(offset) / size);
  }
  if (fsync(output.get())) return false;
  // Invalidate the block page cache so the read-back isn't just cached writes.
  if (ioctl(output.get(), BLKFLSBUF)) return false;
  ui->SetInstallStage(Stage::FLASH_VERIFYING);
  ui->SetProgressType(RecoveryUI::DETERMINATE);
  ui->ShowProgress(1, 0);
  const bool verified = Digest(output.get(), size, &actual, ui) && actual == expected;
  if (verified) {
    std::string hex;
    constexpr char digits[] = "0123456789abcdef";
    for (auto byte : actual) { hex += digits[byte >> 4]; hex += digits[byte & 15]; }
    LOG(INFO) << "Recovery image flash verified: " << target.name << " bytes=" << size << " sha256=" << hex;
  }
  return verified;
}
void Result(Device* device, bool success, bool started) {
  auto ui = device->GetUI();
  const auto stage = success ? Stage::FLASH_SUCCESS : Stage::FLASH_ERROR;
  ui->SetInstallStage(stage);
  if (!success) ui->Print("%s\n", started ? "The selected partition may be incomplete. Reflash before rebooting."
                                         : "No partition was written.");
  for (;;) {
    if (Select(device, {"Flash result"}, {"Continue", "View recovery logs"}) != 1) break;
    ui->SetInstallStage(Stage::NONE);
    fflush(stdout);
    ui->ShowFile("/tmp/recovery.log");
    ui->SetInstallStage(stage);
  }
  ui->SetInstallStage(Stage::NONE);
}
bool ChooseFlashTarget(Device* device, const std::vector<Target>& targets,
                       const std::vector<std::string>& items, const std::string& path,
                       uint64_t size, Target* selected) {
  const auto filename = path.substr(path.find_last_of('/') + 1);
  for (;;) {
    if (device->GetUI()->IsKeyInterrupted()) return false;
    const auto picked = Select(device,
        {"Choose target partition", "Current slot: " + GetProperty("ro.boot.slot_suffix", "none")}, items);
    if (picked >= targets.size()) return false;
    const auto& target = targets[picked];
    for (;;) {
      if (device->GetUI()->IsKeyInterrupted()) return false;
      const auto review = Select(device,
          {"Review image flash", "This overwrites the selected partition. It does not switch slots.",
           "Image compatibility and rollback protection are your responsibility."}, {"Cancel", "Continue"});
      if (review != 1) break;  // Back to target selection.
      const auto confirmation = Select(device,
          {"Confirm image flash", filename.substr(0, 72), "Target: " + target.name,
           "Image bytes: " + std::to_string(size)}, {"Cancel", "Flash selected partition"});
      if (confirmation != 1) continue;  // Back to review; no write or USB change.
      *selected = target;
      return true;
    }
  }
}

// False returns to the file picker; true means an operation reached its result.
bool FlashImage(Device* device, const std::string& root, const std::string& path) {
  auto ui = device->GetUI();
  unique_fd input = OpenImage(root, path);
  struct stat st{};
  if (input.get() < 0 || fstat(input.get(), &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      static_cast<uint64_t>(st.st_size) > kMaxImage) {
    Notice(device, "Image is unreadable, empty or exceeds 512 MiB.");
    return false;
  }
  const uint64_t size = st.st_size;
  std::array<uint8_t, 32> selected_digest{};
  if (!Digest(input.get(), size, &selected_digest)) {
    Notice(device, "Cannot read the complete image. Finish uploading it first.");
    return false;
  }
  std::array<uint8_t, 4096> header{};
  const auto header_bytes = std::min<uint64_t>(header.size(), size);
  if (!android::base::ReadFullyAtOffset(input.get(), header.data(), header_bytes, 0)) {
    Notice(device, "Image is unreadable, empty or exceeds 512 MiB.");
    return false;
  }
  std::vector<Target> targets;
  std::vector<std::string> items;
  for (const auto& target : Targets()) {
    if (size <= target.size && recovery_image::ValidImage(target.base, header.data(), header_bytes, size)) {
      targets.push_back(target);
      items.push_back(target.name);
    }
  }
  if (targets.empty()) {
    Notice(device, "No compatible physical partition. Sparse and logical images require fastbootd.");
    return false;
  }
  Target target;
  if (!ChooseFlashTarget(device, targets, items, path, size, &target)) return false;

  // Nothing above changes USB or writes a block device. Stop MTP only after
  // explicit confirmation, then use a sealed copy for all validation/writes.
  if (!recovery_mtp::Stop()) { Notice(device, "Could not stop USB file transfer. Please retry."); return false; }
  ui->SetProgressType(RecoveryUI::EMPTY);
  ui->SetInstallStage(Stage::FLASH_PREPARING);
  unique_fd staged;
  bool started = false;
  bool success = StageImage(input.get(), size, &staged);
  if (success) {
    std::array<uint8_t, 32> staged_digest{};
    success = android::base::ReadFullyAtOffset(staged.get(), header.data(), header_bytes, 0) &&
        recovery_image::ValidImage(target.base, header.data(), header_bytes, size) &&
        Digest(staged.get(), size, &staged_digest) && selected_digest == staged_digest;
    if (success && target.base == "dtbo") success = DtboPayloadsValid(staged.get(), header.data());
    if (!success) LOG(ERROR) << "Recovery image: source changed or format validation failed before writing";
  }
  if (success) success = WriteAndVerify(staged.get(), target, size, device, &started);
  staged.reset();
  input.reset();
  if (!success) {
    LOG(ERROR) << "Recovery image flash failed for " << target.name << "; write_started=" << started;
    ui->SetInstallStage(Stage::FLASH_ERROR);
    ui->Print("%s\n", "Image preparation, write or verification failed. Check the recovery log.");
  }
  Result(device, success, started);
  recovery_mtp::Start();
  return true;
}
}  // namespace

void FlashPartitionImage(Device* device) {
  if (const auto* blocker = FlashBlocker(device)) {
    LOG(ERROR) << "Recovery image flash blocked: " << blocker;
    Notice(device, blocker);
    return;
  }
  for (;;) {
    if (device->GetUI()->IsKeyInterrupted()) return;
    const auto source = Select(device, {"Flash partition image"},
        {"Internal storage", "Choose IMG from /tmp", "Cancel"});
    std::string root;
    if (source == 0) {
      if (!UnlockRecoveryStorage(device)) continue;
      root = recovery_crypto::UserStoragePath(0);
    } else if (source == 1) {
      root = "/tmp";
    } else {
      return;  // Back to update methods.
    }
    for (;;) {
      if (device->GetUI()->IsKeyInterrupted()) return;
      const auto path = ChooseRecoveryStorageFile(device, root, ".img", "Choose partition image");
      if (path.empty()) break;  // Back to source selection.
      if (FlashImage(device, root, path)) return;
    }
  }
}
