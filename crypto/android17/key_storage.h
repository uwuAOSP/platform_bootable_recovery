/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <string>
#include "hal.h"
namespace recovery_crypto::android17 {
int RetrieveExistingKey(Hal& hal, const std::string& directory, View ce_secret, Bytes* key);
}
