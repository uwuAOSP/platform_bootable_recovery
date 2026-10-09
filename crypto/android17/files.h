/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <android-base/unique_fd.h>
#include <string>
#include <vector>
#include "secure_bytes.h"
namespace recovery_crypto::android17 {
android::base::unique_fd OpenDirectory(const std::string& path);
bool ReadFile(const std::string& path, Bytes* bytes, size_t limit = 1024 * 1024,
              bool* missing = nullptr);
bool ListDirectories(const std::string& path, std::vector<std::string>* names);
bool IsTrustedFile(const std::string& path);
}  // namespace recovery_crypto::android17
