/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

namespace recovery_ui {
// Presentation only. Installation decisions continue to use InstallResult and IsTextVisible().
enum class InstallStage {
  NONE, WAITING, VERIFYING, INSTALLING, SUCCESS, ERROR, CANCELLED,
  FLASH_PREPARING, FLASH_WRITING, FLASH_VERIFYING, FLASH_SUCCESS, FLASH_ERROR
};
inline bool IsFlashStage(InstallStage stage) {
  return stage == InstallStage::FLASH_PREPARING || stage == InstallStage::FLASH_WRITING ||
      stage == InstallStage::FLASH_VERIFYING || stage == InstallStage::FLASH_SUCCESS ||
      stage == InstallStage::FLASH_ERROR;
}
}
