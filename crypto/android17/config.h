/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <string>
#include <vector>
namespace recovery_crypto::android17 {
struct Config {
  std::string fstab, keymint_service, gatekeeper_transport, gatekeeper_instance;
  std::string weaver_transport, weaver_instance, secureclock_service;
  std::vector<std::string> sharedsecret_services;
  // Explicit highest-version HIDL Keymaster participants, e.g. 4.1/default.
  std::vector<std::string> sharedsecret_hidl_instances;
  int security_level = 1;
};
bool ReadConfig(Config* config);
}  // namespace recovery_crypto::android17
