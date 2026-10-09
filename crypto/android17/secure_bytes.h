/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <openssl/mem.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace recovery_crypto::android17 {
template <class T>
struct WipingAllocator : std::allocator<T> {
  using value_type = T;
  template <class U>
  struct rebind {
    using other = WipingAllocator<U>;
  };
  WipingAllocator() = default;
  template <class U>
  WipingAllocator(const WipingAllocator<U>&) {}
  void deallocate(T* p, size_t n) {
    OPENSSL_cleanse(p, n * sizeof(T));
    std::allocator<T>::deallocate(p, n);
  }
};
using Bytes = std::vector<uint8_t, WipingAllocator<uint8_t>>;
using View = std::span<const uint8_t>;
inline View AsBytes(const char* s) {
  return { reinterpret_cast<const uint8_t*>(s), strlen(s) };
}
inline void Clear(Bytes& b) {
  if (!b.empty()) OPENSSL_cleanse(b.data(), b.size());
  b.clear();
}
// Binder's generated interfaces require the default allocator. Always wipe these copies.
struct BinderBytes : std::vector<uint8_t> {
  using std::vector<uint8_t>::vector;
  BinderBytes() = default;
  explicit BinderBytes(View v) : std::vector<uint8_t>(v.begin(), v.end()) {}
  ~BinderBytes() {
    if (!empty()) OPENSSL_cleanse(data(), size());
  }
};
}  // namespace recovery_crypto::android17
