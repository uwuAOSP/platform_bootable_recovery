/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "image_flash_policy.h"
#include <gtest/gtest.h>

namespace {
using recovery_image::BootloaderState;
using recovery_image::GetBootloaderState;

TEST(ImageFlashPolicy, OrangeWithoutOtherPropertiesIsUnlocked) {
  EXPECT_EQ(BootloaderState::Unlocked, GetBootloaderState("", "", "orange"));
}
TEST(ImageFlashPolicy, ExplicitUnlockSignalsAreRecognized) {
  EXPECT_EQ(BootloaderState::Unlocked, GetBootloaderState("0", "", ""));
  EXPECT_EQ(BootloaderState::Unlocked, GetBootloaderState("", "unlocked", ""));
  EXPECT_EQ(BootloaderState::Unlocked, GetBootloaderState("0", "unlocked", "orange"));
  EXPECT_EQ(BootloaderState::Unlocked, GetBootloaderState("0", "unlocked", "red"));
}
TEST(ImageFlashPolicy, MissingAndUnrecognizedStatesDoNotAuthorizeWrites) {
  EXPECT_EQ(BootloaderState::Unknown, GetBootloaderState("", "", ""));
  EXPECT_EQ(BootloaderState::Unknown, GetBootloaderState("", "", "red"));
  EXPECT_EQ(BootloaderState::Unknown, GetBootloaderState("unknown", "", "orange"));
  EXPECT_EQ(BootloaderState::Unknown, GetBootloaderState("0", "unknown", "orange"));
  EXPECT_EQ(BootloaderState::Unknown, GetBootloaderState("0", "", "unknown"));
}
TEST(ImageFlashPolicy, LockedSignalsTakePrecedenceOverUnlockSignals) {
  EXPECT_EQ(BootloaderState::Locked, GetBootloaderState("1", "", "orange"));
  EXPECT_EQ(BootloaderState::Locked, GetBootloaderState("0", "locked", "orange"));
  EXPECT_EQ(BootloaderState::Locked, GetBootloaderState("0", "unlocked", "green"));
  EXPECT_EQ(BootloaderState::Locked, GetBootloaderState("", "unlocked", "yellow"));
}
}  // namespace
