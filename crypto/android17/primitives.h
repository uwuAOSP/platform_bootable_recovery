/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <string_view>
#include "secure_bytes.h"
namespace recovery_crypto::android17 {
Bytes PersonalizedHash(std::string_view label, View data);
Bytes SpSubkey(uint8_t version, View sp, std::string_view label);
bool Stretch(View password, View salt, unsigned n, unsigned r, unsigned p, Bytes* out);
bool GcmDecrypt(View key, View blob, Bytes* out);
Bytes Concat(View a, View b);
}  // namespace recovery_crypto::android17
