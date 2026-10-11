/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "config.h"
#include <map>
#include <set>
#include <sstream>
#include "files.h"
namespace recovery_crypto::android17 {
bool ReadConfig(Config* c) {
  constexpr const char* path = "/system/etc/recovery.crypto.conf";
  Bytes content;
  if (!IsTrustedFile(path) || !ReadFile(path, &content, 8192)) return false;
  std::string text(content.begin(), content.end());
  std::istringstream input(text);
  std::string line;
  std::map<std::string, std::string> fields;
  const std::set<std::string> allowed = { "profile_version",
                                          "platform_sdk",
                                          "fstab",
                                          "keymint_service",
                                          "security_level",
                                          "gatekeeper_transport",
                                          "gatekeeper_instance",
                                          "weaver_transport",
                                          "weaver_instance",
                                          "secureclock_service",
                                          "sharedsecret_services",
                                          "storage_binding",
                                          "sharedsecret_hidl_instances" };
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    auto split = line.find('=');
    if (split == std::string::npos) return false;
    auto key = line.substr(0, split), value = line.substr(split + 1);
    if (!allowed.count(key) || fields.count(key) || value.size() > 1024 ||
        value.find_first_of(" \t\r\n\0", 0, 5) != std::string::npos)
      return false;
    fields.emplace(key, value);
  }
  if (fields["profile_version"] != "1" || fields["platform_sdk"] != "37" ||
      fields["storage_binding"] != "none")
    return false;
  c->fstab = fields["fstab"];
  c->keymint_service = fields["keymint_service"];
  if (!c->fstab.starts_with("/system/etc/") || !IsTrustedFile(c->fstab) ||
      !c->keymint_service.starts_with("android.hardware.security.keymint.IKeyMintDevice/"))
    return false;
  if (fields["security_level"] == "tee")
    c->security_level = 1;
  else if (fields["security_level"] == "strongbox")
    c->security_level = 2;
  else
    return false;
  auto transport = [&](const char* kind, std::string* t, std::string* instance) {
    *t = fields[std::string(kind) + "_transport"];
    *instance = fields[std::string(kind) + "_instance"];
    return (*t == "none" && instance->empty()) ||
           ((*t == "aidl" || *t == "hidl") && !instance->empty() &&
            instance->find('/') == std::string::npos && instance->find('.') == std::string::npos);
  };
  if (!transport("gatekeeper", &c->gatekeeper_transport, &c->gatekeeper_instance) ||
      !transport("weaver", &c->weaver_transport, &c->weaver_instance))
    return false;
  c->secureclock_service = fields["secureclock_service"];
  if (!c->secureclock_service.empty() &&
      !c->secureclock_service.starts_with("android.hardware.security.secureclock.ISecureClock/"))
    return false;
  std::istringstream services(fields["sharedsecret_services"]);
  std::string service;
  while (std::getline(services, service, ',')) {
    if (!service.starts_with("android.hardware.security.sharedsecret.ISharedSecret/") ||
        c->sharedsecret_services.size() >= 4)
      return false;
    for (const auto& previous : c->sharedsecret_services)
      if (previous == service) return false;
    c->sharedsecret_services.push_back(service);
  }
  std::istringstream legacy(fields["sharedsecret_hidl_instances"]);
  std::set<std::string> instances;
  while (std::getline(legacy, service, ',')) {
    if (service != "4.0/default" && service != "4.1/default" && service != "4.0/strongbox" &&
        service != "4.1/strongbox")
      return false;
    auto instance = service.substr(4);
    if (!instances.insert(instance).second ||
        c->sharedsecret_services.size() + c->sharedsecret_hidl_instances.size() >= 4)
      return false;
    c->sharedsecret_hidl_instances.push_back(service);
  }
  return !c->sharedsecret_services.empty();
}
}  // namespace recovery_crypto::android17
