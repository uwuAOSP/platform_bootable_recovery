/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace recovery_ui {
// Non-owning view of the caller's credential. Drawing can only read its length.
class PasswordInput {
 public:
  virtual ~PasswordInput() = default;
  virtual bool NumericOnly() const = 0;
  virtual size_t Size() const = 0;
  virtual bool Append(uint8_t character) = 0;
  virtual void EraseLast() = 0;
  virtual void Clear() = 0;
};
}  // namespace recovery_ui
