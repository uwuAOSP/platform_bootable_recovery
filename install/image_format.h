/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace recovery_image {
inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24;
}
inline uint64_t Be64(const uint8_t* p) {
  return uint64_t(Be32(p)) << 32 | Be32(p + 4);
}
inline uint64_t Align(uint64_t n, uint32_t page) {
  return (n + page - 1) / page * page;
}
inline bool PageSize(uint32_t page) {
  return page >= 512 && page <= 65536 && (page & (page - 1)) == 0;
}
inline bool PhysicalName(const std::string& name, std::string* base = nullptr) {
  auto value = name;
  if (value.size() > 2 && (value.ends_with("_a") || value.ends_with("_b"))) value.resize(value.size() - 2);
  if (value != "boot" && value != "init_boot" && value != "vendor_boot" &&
      value != "recovery" && value != "dtbo" && value != "vbmeta" && value != "vbmeta_system") return false;
  if (base) *base = value;
  return true;
}

// Validate required on-disk sections, not only the magic or the filename.
// Padding and AVB footers are allowed. This is not signature/rollback validation.
inline bool ValidImage(const std::string& base, const uint8_t* h, size_t available, uint64_t size) {
  if (available < 256 || size < 256 || Le32(h) == 0xed26ff3a) return false;  // sparse
  if (base == "boot" || base == "recovery" || base == "init_boot") {
    if (std::memcmp(h, "ANDROID!", 8)) return false;
    const auto version = Le32(h + 40);
    const auto kernel = Le32(h + 8);
    const auto ramdisk = Le32(h + (version >= 3 ? 12 : 16));
    if (base == "boot" && !kernel) return false;
    if (base == "init_boot" && (version != 4 || kernel || !ramdisk)) return false;
    if (base == "recovery" && !ramdisk) return false;
    uint64_t required;
    if (version == 3 || version == 4) {
      const uint32_t header_size = version == 4 ? 1584 : 1580;
      if (available < header_size || Le32(h + 20) != header_size) return false;
      required = 4096 + Align(kernel, 4096) + Align(ramdisk, 4096);
      if (version == 4) required += Le32(h + 1580);
    } else if (version <= 2) {
      const uint32_t header_size = version == 2 ? 1660 : version == 1 ? 1648 : 1632;
      const auto page = Le32(h + 36);
      if (available < header_size || !PageSize(page) || page < header_size) return false;
      required = uint64_t(page) + Align(kernel, page) + Align(ramdisk, page) + Align(Le32(h + 24), page);
      if (version >= 1) {
        if (Le32(h + 1644) != header_size) return false;
        const auto recovery_dtbo_size = Le32(h + 1632);
        const uint64_t offset = uint64_t(Le32(h + 1636)) | uint64_t(Le32(h + 1640)) << 32;
        if (recovery_dtbo_size && offset != required) return false;
        required += Align(recovery_dtbo_size, page);
      }
      if (version == 2) required += Align(Le32(h + 1648), page);
    } else return false;
    return required <= size;
  }
  if (base == "vendor_boot") {
    if (std::memcmp(h, "VNDRBOOT", 8)) return false;
    const auto version = Le32(h + 8);
    const auto page = Le32(h + 12);
    const uint32_t header_size = version == 4 ? 2128 : 2112;
    if ((version != 3 && version != 4) || !PageSize(page) || available < header_size ||
        Le32(h + 2096) != header_size) return false;
    uint64_t required = Align(header_size, page) + Align(Le32(h + 24), page) + Align(Le32(h + 2100), page);
    if (version == 4) {
      const uint64_t table_size = Le32(h + 2112);
      const uint64_t entries = Le32(h + 2116);
      const auto entry_size = Le32(h + 2120);
      if ((entries && entry_size != 108) || entries * entry_size != table_size) return false;
      required += Align(table_size, page) + Le32(h + 2124);
    }
    return required <= size;
  }
  if (base == "dtbo") {
    const uint64_t total = Be32(h + 4), header_size = Be32(h + 8);
    const uint64_t entry_size = Be32(h + 12), entries = Be32(h + 16), offset = Be32(h + 20);
    return Be32(h) == 0xd7b7ab1e && header_size >= 32 && header_size <= total &&
        entry_size >= 32 && entries && entries <= 4096 && offset >= header_size && offset <= total &&
        entries * entry_size <= total - offset && total <= size;
  }
  if (base == "vbmeta" || base == "vbmeta_system") {
    if (std::memcmp(h, "AVB0", 4)) return false;
    const uint64_t auth = Be64(h + 12), aux = Be64(h + 20);
    return auth <= size - 256 && aux <= size - 256 - auth;
  }
  return false;
}
inline bool ValidDtboEntry(const uint8_t* entry, uint64_t total, uint64_t table_end) {
  const uint64_t bytes = Be32(entry), offset = Be32(entry + 4);
  return bytes >= 40 && offset >= table_end && offset <= total && bytes <= total - offset;
}
}  // namespace recovery_image
