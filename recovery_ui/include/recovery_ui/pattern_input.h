/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace recovery_ui {
// The caller owns locked credential memory. UI reads/draws through this view;
// it never copies the sequence into strings, logs or a second credential buffer.
class PatternInput {
 public:
  virtual ~PatternInput() = default;
  virtual unsigned GridSize() const { return 3; }
  virtual size_t Size() const = 0;
  virtual uint8_t Cell(size_t index) const = 0;
  virtual bool Append(uint8_t cell) = 0;
  virtual void Clear() = 0;
  bool Contains(uint8_t cell) const {
    for (size_t i = 0; i < Size(); ++i) if (Cell(i) == cell) return true;
    return false;
  }
  bool Select(uint8_t cell) {
    const unsigned grid = GridSize();
    if (grid < 3 || grid > 6 || cell >= grid * grid || Size() >= grid * grid || Contains(cell))
      return false;
    if (Size()) {
      const auto previous = Cell(Size() - 1);
      if (previous >= grid * grid) return false;
      int x1 = previous % grid, y1 = previous / grid, x2 = cell % grid, y2 = cell / grid;
      int dx = x2 - x1, dy = y2 - y1;
      // uwuAOSP LockPatternView fills all skipped cells on a row, column or
      // 45-degree diagonal. Other slopes must not invent intermediate cells.
      if (dx == 0 || dy == 0 || std::abs(dx) == std::abs(dy)) {
        int sx = (dx > 0) - (dx < 0), sy = (dy > 0) - (dy < 0);
        for (x1 += sx, y1 += sy; x1 != x2 || y1 != y2; x1 += sx, y1 += sy) {
          uint8_t middle = y1 * grid + x1;
          if (!Contains(middle) && !Append(middle)) return false;
        }
      }
    }
    return Append(cell);
  }
};
}  // namespace recovery_ui
