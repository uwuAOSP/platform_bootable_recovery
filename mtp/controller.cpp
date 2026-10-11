/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "recovery_mtp/controller.h"
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <recovery_crypto/session.h>
#include <recovery_crypto/media_access.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <chrono>
#include <algorithm>
#include <cctype>

namespace recovery_mtp {
using android::base::GetProperty;
using android::base::SetProperty;
using android::base::WaitForProperty;
using namespace std::chrono_literals;
namespace {
bool attempted = false;
bool Config(const char* config) {
  return SetProperty("sys.usb.config", config) &&
         WaitForProperty("sys.usb.state", config, 3s);
}
bool Enabled(std::string* configured_pid = nullptr) {
  struct stat st{};
  if (lstat("/system/bin/recovery_mtp", &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != 0 || (st.st_mode & 0022) != 0 || (st.st_mode & 0100) == 0 ||
      GetProperty("sys.usb.configfs", "") != "1") return false;
  std::string configured_vid, pid;
  if (!recovery_crypto::ReadMediaAccess(&configured_vid, &pid)) return false;
  std::transform(configured_vid.begin(), configured_vid.end(), configured_vid.begin(), [](unsigned char ch) { return std::toupper(ch); });
  std::transform(pid.begin(), pid.end(), pid.begin(), [](unsigned char ch) { return std::toupper(ch); });
  auto vid = GetProperty("ro.recovery.usb.vid", "");
  std::transform(vid.begin(), vid.end(), vid.begin(), [](unsigned char ch) { return std::toupper(ch); });
  if (vid.substr(0, 2) == "0X") vid.erase(0, 2);
  if (configured_vid != vid) return false;
  if (configured_pid) *configured_pid = pid;
  return true;
}
}  // namespace
bool Start() {
  std::string pid;
  if (!Enabled(&pid)) return false;
  if (!recovery_crypto::VerifyUserStorage(0)) {
    Stop();
    return false;
  }
  if (GetProperty("sys.usb.state", "") == "mtp,adb" &&
      GetProperty("init.svc.recovery-mtp", "") == "running") return true;
  if (GetProperty("sys.usb.config", "") != "adb" || attempted) return false;
  attempted = true;
  SetProperty("sys.usb.config.recovery_mtp.failed", "0");
  if (SetProperty("sys.usb.config.recovery_mtp.pid", pid) &&
      SetProperty("sys.usb.config.recovery_mtp.prepare", "1") &&
      WaitForProperty("sys.usb.config.recovery_mtp.prepared", "1", 3s) &&
      Config("none") && Config("mtp,adb") &&
      GetProperty("init.svc.recovery-mtp", "") == "running") {
    LOG(INFO) << "Recovery MTP: unlocked media shared; ADB retained";
    return true;
  }
  LOG(WARNING) << "Recovery MTP unavailable; restoring ADB";
  Stop();
  if (GetProperty("sys.usb.config", "") == "none") Config("adb");
  attempted = true;  // Avoid retrying a failed gadget on every menu navigation.
  return false;
}
bool Stop() {
  if (!Enabled() && GetProperty("init.svc.recovery-mtp", "") != "running") return true;
  const auto config = GetProperty("sys.usb.config", "");
  const bool restore_adb = config == "mtp,adb";
  SetProperty("sys.usb.config.recovery_mtp.failed", "0");
  // ctl.stop is synchronous in init; do not let a daemon retain media FDs
  // across unmount, wipe, installer or fastboot handover.
  if (GetProperty("init.svc.recovery-mtp", "") == "running") {
    if (restore_adb && !Config("none")) return false;
    if (!SetProperty("ctl.stop", "recovery-mtp") ||
        !WaitForProperty("init.svc.recovery-mtp", "stopped", 6s)) return false;
  } else if (restore_adb && !Config("none")) {
    return false;
  }
  // The daemon is stopped before flushing. A partial upload is never published.
  if (recovery_crypto::VerifyUserStorage(0)) {
    android::base::unique_fd media(open("/data/media/0", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (media.get() < 0 || syncfs(media.get()) != 0) return false;
  }
  if (restore_adb && !Config("adb")) return false;
  attempted = false;
  return true;
}
}  // namespace recovery_mtp
