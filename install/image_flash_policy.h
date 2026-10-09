/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <string_view>

namespace recovery_image {
enum class BootloaderState { Unknown, Locked, Unlocked };

inline BootloaderState GetBootloaderState(std::string_view flash_locked,
                                         std::string_view vbmeta_state,
                                         std::string_view verified_boot_state) {
  // Never override explicit locked evidence, including contradictory signals.
  if (flash_locked == "1" || vbmeta_state == "locked" ||
      verified_boot_state == "green" || verified_boot_state == "yellow") {
    return BootloaderState::Locked;
  }
  if ((!flash_locked.empty() && flash_locked != "0") ||
      (!vbmeta_state.empty() && vbmeta_state != "unlocked") ||
      (!verified_boot_state.empty() && verified_boot_state != "orange" &&
       verified_boot_state != "red")) {
    return BootloaderState::Unknown;
  }
  // AOSP defines orange as UNLOCKED. Some bootloaders supply only this
  // property in Recovery. Empty properties alone never authorize a write.
  if (flash_locked == "0" || vbmeta_state == "unlocked" || verified_boot_state == "orange") {
    return BootloaderState::Unlocked;
  }
  return BootloaderState::Unknown;
}
}  // namespace recovery_image
