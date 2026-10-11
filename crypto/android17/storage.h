/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <fstab/fstab.h>
#include "hal.h"
namespace recovery_crypto::android17 {
enum class KeyMode { Raw, WrappedV0, Wrapped };
class Storage final {
 public:
  int Configure(const Config& config);
  int Mount(Hal& hal);
  int LoadDe(Hal& hal, uint32_t user);
  int LoadCe(Hal& hal, uint32_t user, View secret);
  void EnableMediaWrites(uint32_t user);

 private:
  int PrepareKey(Hal& hal, View persistent, KeyMode mode, Bytes* ephemeral);
  int Install(Hal& hal, View persistent, const std::string& policy_directory);
  android::fs_mgr::Fstab fstab_;
  android::fs_mgr::FstabEntry data_, metadata_;
  unsigned policy_version_ = 0;
  KeyMode mode_ = KeyMode::Raw, metadata_mode_ = KeyMode::Raw;
  std::string metadata_cipher_, data_device_;
  bool metadata_encrypted_ = false, media_writes_ = false;
};
}  // namespace recovery_crypto::android17
