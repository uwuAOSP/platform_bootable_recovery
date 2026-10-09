/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <libdm/dm.h>
#include <linux/dm-ioctl.h>
#include <cstddef>
#include <cstring>

namespace recovery_crypto::android17 {
inline void InitializeDmIo(dm_ioctl* io, size_t size) {
  memset(io, 0, size);
  // Use libdm's stable request ABI, as DeviceMapper::InitIo does. The UAPI
  // DM_VERSION_MINOR describes the build headers, not the device's kernel.
  // A newer requested minor makes an older kernel reject even TABLE_LOAD.
  io->version[0] = DM_VERSION0;
  io->version[1] = DM_VERSION1;
  io->version[2] = DM_VERSION2;
  io->data_size = size;
  io->data_start = sizeof(dm_ioctl);
}
}  // namespace recovery_crypto::android17
