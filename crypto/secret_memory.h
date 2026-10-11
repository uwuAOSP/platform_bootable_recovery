/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <sys/mman.h>
#include <unistd.h>
#include <cstddef>

namespace recovery_crypto {
inline void Wipe(void* address, size_t size) {
  volatile unsigned char* p = static_cast<volatile unsigned char*>(address);
  while (size-- != 0) *p++ = 0;
}
// One anonymous page, locked and excluded from process dumps. Failure is fatal
// to this unlock attempt only, never a fallback to plaintext credential files.
class SecretMemory final {
 public:
  SecretMemory() {
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return;
    size_ = static_cast<size_t>(page_size);
    void* p = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    if (mlock(p, size_) != 0 || madvise(p, size_, MADV_DONTDUMP) != 0) {
      munlock(p, size_);
      munmap(p, size_);
      return;
    }
    data_ = p;
  }
  ~SecretMemory() {
    if (data_) { Wipe(data_, size_); munlock(data_, size_); munmap(data_, size_); }
  }
  SecretMemory(const SecretMemory&) = delete;
  SecretMemory& operator=(const SecretMemory&) = delete;
  void* data() const { return data_; }
 private:
  void* data_ = nullptr;
  size_t size_ = 0;
};
}  // namespace recovery_crypto
