/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "database.h"
#include <MtpDescriptors.h>
#include <MtpServer.h>
#include <MtpStorage.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <recovery_crypto/session.h>
#include <recovery_crypto/media_access.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#include <string>

int main() {
  using android::base::GetProperty;
  using android::base::SetProperty;
  struct rlimit core{0, 0};
  if (setrlimit(RLIMIT_CORE, &core) != 0) return 1;
  android::base::unique_fd null(open("/dev/null", O_RDWR | O_CLOEXEC));
  if (null.get() < 0) return 1;
  for (int fd = 0; fd <= 2; ++fd) if (dup2(null.get(), fd) < 0) return 1;
  // Do not trust a UI flag or init property as proof that CE keys are present.
  if (GetProperty("sys.usb.config", "") != "mtp,adb" ||
      !recovery_crypto::VerifyUserStorage(0)) return 2;
  constexpr char root[] = "/data/media/0";
  recovery_mtp::Database database(root);
  if (!database.valid()) return 2;
  database.EnableWrites(recovery_crypto::MediaWritesRequested());
  android::base::unique_fd control(open(android::FFS_MTP_EP0, O_RDWR | O_CLOEXEC));
  if (control.get() < 0 || !android::writeDescriptors(control.get(), false)) return 3;
  const auto manufacturer = GetProperty("ro.product.manufacturer", "Android");
  const auto model = GetProperty("ro.product.model", "Recovery");
  const auto serial = GetProperty("ro.serialno", "recovery");
  android::MtpStorage storage(recovery_mtp::kStorageId, root, database.recoveryStorageWritable() ? "Internal storage" : "Internal storage (read-only)", false, 0);
  android::MtpServer server(&database, control.release(), false, manufacturer.c_str(),
                            model.c_str(), "1.0", serial.c_str());
  server.addStorage(&storage);
  // Bind the gadget only after descriptor creation; keep ep0 open throughout.
  if (!SetProperty("sys.usb.ffs.mtp.ready", "1")) return 3;
  server.run();
  // Leave readiness set on exit so init can distinguish a crashed/unplugged
  // daemon from the initial stopped service and restore ADB automatically.
  return 0;
}
