/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "image_format.h"
#include <array>
#include <gtest/gtest.h>

namespace {
using recovery_image::ValidImage;
void PutLe(std::array<uint8_t, 4096>& h, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) h[offset + i] = value >> (8 * i);
}
void PutBe(std::array<uint8_t, 4096>& h, size_t offset, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) h[offset + i] = value >> (8 * (3 - i));
}
TEST(ImageFlashFormat, ExactPhysicalAllowlist) {
  for (auto name : {"boot", "boot_a", "boot_b", "recovery_b", "init_boot_a", "dtbo", "vbmeta_system_b"}) {
    EXPECT_TRUE(recovery_image::PhysicalName(name)) << name;
  }
  for (auto name : {"userdata", "super", "system_a", "vendor", "persist", "modem_a", "xbl_a",
                    "../boot_a", "boot_a_b", "boot_c", "boot_backup", ""}) {
    EXPECT_FALSE(recovery_image::PhysicalName(name)) << name;
  }
}
TEST(ImageFlashFormat, BootV4SectionsAndRecoveryWithoutKernel) {
  std::array<uint8_t, 4096> h{};
  std::memcpy(h.data(), "ANDROID!", 8);
  PutLe(h, 8, 4096); PutLe(h, 12, 8192); PutLe(h, 20, 1584); PutLe(h, 40, 4);
  EXPECT_TRUE(ValidImage("boot", h.data(), h.size(), 16384));
  EXPECT_FALSE(ValidImage("boot", h.data(), h.size(), 16383));
  EXPECT_FALSE(ValidImage("init_boot", h.data(), h.size(), 16384));
  PutLe(h, 8, 0);
  EXPECT_TRUE(ValidImage("recovery", h.data(), h.size(), 12288));
  EXPECT_TRUE(ValidImage("init_boot", h.data(), h.size(), 12288));
  EXPECT_FALSE(ValidImage("boot", h.data(), h.size(), 12288));
  PutLe(h, 1580, 1024);
  EXPECT_FALSE(ValidImage("recovery", h.data(), h.size(), 12288));
  EXPECT_TRUE(ValidImage("recovery", h.data(), h.size(), 13312));
  EXPECT_FALSE(ValidImage("recovery", h.data(), 256, 13312));
  PutLe(h, 40, 5);
  EXPECT_FALSE(ValidImage("recovery", h.data(), h.size(), 13312));
}
TEST(ImageFlashFormat, LegacyBootPageBounds) {
  std::array<uint8_t, 4096> h{};
  std::memcpy(h.data(), "ANDROID!", 8);
  PutLe(h, 8, 2048); PutLe(h, 16, 2048); PutLe(h, 36, 2048);
  EXPECT_TRUE(ValidImage("boot", h.data(), h.size(), 6144));
  PutLe(h, 36, 0);
  EXPECT_FALSE(ValidImage("boot", h.data(), h.size(), 6144));
  PutLe(h, 36, 2049);
  EXPECT_FALSE(ValidImage("boot", h.data(), h.size(), 6144));
  PutLe(h, 36, 1024);
  EXPECT_FALSE(ValidImage("boot", h.data(), h.size(), 6144));
}
TEST(ImageFlashFormat, VendorBootV4TableExtent) {
  std::array<uint8_t, 4096> h{};
  std::memcpy(h.data(), "VNDRBOOT", 8);
  PutLe(h, 8, 4); PutLe(h, 12, 4096); PutLe(h, 24, 8192);
  PutLe(h, 2096, 2128); PutLe(h, 2100, 4096);
  PutLe(h, 2112, 108); PutLe(h, 2116, 1); PutLe(h, 2120, 108); PutLe(h, 2124, 16);
  EXPECT_TRUE(ValidImage("vendor_boot", h.data(), h.size(), 20496));
  EXPECT_FALSE(ValidImage("vendor_boot", h.data(), h.size(), 20495));
  PutLe(h, 2116, 2);
  EXPECT_FALSE(ValidImage("vendor_boot", h.data(), h.size(), 20496));
  PutLe(h, 2116, 1); PutLe(h, 2120, 0);
  EXPECT_FALSE(ValidImage("vendor_boot", h.data(), h.size(), 20496));
}
TEST(ImageFlashFormat, DtboBigEndianAndTableBounds) {
  std::array<uint8_t, 4096> h{};
  PutBe(h, 0, 0xd7b7ab1e); PutBe(h, 4, 4096); PutBe(h, 8, 32);
  PutBe(h, 12, 32); PutBe(h, 16, 1); PutBe(h, 20, 32);
  EXPECT_TRUE(ValidImage("dtbo", h.data(), h.size(), 4096));
  EXPECT_FALSE(ValidImage("dtbo", h.data(), h.size(), 4095));
  PutBe(h, 16, UINT32_MAX);
  EXPECT_FALSE(ValidImage("dtbo", h.data(), h.size(), 4096));
  PutBe(h, 16, 1); PutBe(h, 20, UINT32_MAX);
  EXPECT_FALSE(ValidImage("dtbo", h.data(), h.size(), 4096));
}
TEST(ImageFlashFormat, VbmetaOverflowAndSparseRejection) {
  std::array<uint8_t, 4096> h{};
  std::memcpy(h.data(), "AVB0", 4);
  PutBe(h, 16, 64); PutBe(h, 24, 128);
  EXPECT_TRUE(ValidImage("vbmeta", h.data(), h.size(), 448));
  EXPECT_FALSE(ValidImage("vbmeta", h.data(), h.size(), 447));
  PutBe(h, 12, UINT32_MAX); PutBe(h, 16, UINT32_MAX);
  EXPECT_FALSE(ValidImage("vbmeta", h.data(), h.size(), 448));
  PutLe(h, 0, 0xed26ff3a);
  for (auto base : {"boot", "recovery", "vendor_boot", "dtbo", "vbmeta", "init_boot"})
    EXPECT_FALSE(ValidImage(base, h.data(), h.size(), 4096));
}
TEST(ImageFlashFormat, DtboPayloadMustNotOverlapTableOrEscapeFile) {
  std::array<uint8_t, 4096> entry{};
  PutBe(entry, 0, 128); PutBe(entry, 4, 64);
  EXPECT_TRUE(recovery_image::ValidDtboEntry(entry.data(), 4096, 64));
  PutBe(entry, 4, 32);
  EXPECT_FALSE(recovery_image::ValidDtboEntry(entry.data(), 4096, 64));
  PutBe(entry, 4, 4000);
  EXPECT_FALSE(recovery_image::ValidDtboEntry(entry.data(), 4096, 64));
  PutBe(entry, 0, 0);
  EXPECT_FALSE(recovery_image::ValidDtboEntry(entry.data(), 4096, 64));
}
}  // namespace
