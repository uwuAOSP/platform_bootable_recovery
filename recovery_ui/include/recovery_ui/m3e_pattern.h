/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "m3e.h"
#include "pattern_input.h"

namespace recovery_m3e {
struct PatternLayout {
  Rect grid, unlock, clear, cancel;
  int radius;
  unsigned size = 3;
  int CellCount() const { return size * size; }
  int UnlockAction() const { return CellCount(); }
  int ClearAction() const { return CellCount() + 1; }
  int CancelAction() const { return CellCount() + 2; }
  std::pair<int, int> Dot(int cell) const {
    const int n = static_cast<int>(size);
    return {grid.x + grid.w * (2 * (cell % n) + 1) / (2 * n),
            grid.y + grid.h * (2 * (cell / n) + 1) / (2 * n)};
  }
  int HitDot(int x, int y) const {
    for (int cell = 0; cell < CellCount(); ++cell) {
      auto [cx, cy] = Dot(cell);
      int64_t dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy <= int64_t(radius) * radius) return cell;
    }
    return -1;
  }
  int HitAction(int x, int y) const {
    if (unlock.Contains(x, y)) return UnlockAction();
    if (clear.Contains(x, y)) return ClearAction();
    if (cancel.Contains(x, y)) return CancelAction();
    return -2;
  }
};
inline PatternLayout PatternBounds(const Metrics& m, int top, int bottom, unsigned size = 3) {
  int gap = Dp(m.width, 12), button = Dp(m.width, 48);
  int width = m.width - 2 * m.inset;
  int height = std::max(1, bottom - top);
  PatternLayout l{};
  l.size = std::clamp(size, 3u, 6u);
  if (height < Dp(m.width, 350) && width >= Dp(m.width, 460)) {
    int side = std::min(height, (width - gap) / 2);
    l.grid = {m.inset, top + (height - side) / 2, side, side};
    int x = m.inset + side + gap, w = width - side - gap;
    int bh = std::min(button, std::max(1, (height - 2 * gap) / 3));
    int y = top + (height - 3 * bh - 2 * gap) / 2;
    l.unlock = {x, y, w, bh};
    l.clear = {x, y + bh + gap, w, bh};
    l.cancel = {x, y + 2 * (bh + gap), w, bh};
  } else {
    button = std::min(button, std::max(1, height / 5));
    int side = std::max(1, std::min(width, height - 2 * button - 2 * gap));
    l.grid = {(m.width - side) / 2, top, side, side};
    int y = bottom - 2 * button - gap;
    l.unlock = {m.inset, y, width, button};
    l.clear = {m.inset, y + button + gap, (width - gap) / 2, button};
    l.cancel = {l.clear.x + l.clear.w + gap, l.clear.y, width - l.clear.w - gap, button};
  }
  l.radius = std::max(1, std::min(Dp(m.width, 28), l.grid.w / (3 * int(l.size))));
  return l;
}
// Intersect the complete motion segment with dot hit circles. Sparse input
// frames still select crossed dots, in motion order, including diagonal strokes.
inline void TracePattern(recovery_ui::PatternInput& input, const PatternLayout& l,
                         int x1, int y1, int x2, int y2) {
  std::array<std::pair<double, int>, 36> hits{};
  size_t count = 0;
  double dx = x2 - x1, dy = y2 - y1, length2 = dx * dx + dy * dy;
  for (int cell = 0; cell < l.CellCount(); ++cell) {
    if (input.Contains(cell)) continue;
    auto [cx, cy] = l.Dot(cell);
    double t = length2 ? std::clamp(((cx - x1) * dx + (cy - y1) * dy) / length2, 0.0, 1.0) : 0;
    double ex = x1 + t * dx - cx, ey = y1 + t * dy - cy;
    if (ex * ex + ey * ey <= l.radius * l.radius) hits[count++] = {t, cell};
  }
  std::sort(hits.begin(), hits.begin() + count);
  for (size_t i = 0; i < count; ++i) input.Select(hits[i].second);
}
inline void PatternButton(Canvas& c, const Metrics& m, Rect b, const char* label,
                          bool primary, bool enabled, bool focused, const Palette& p) {
  Color bg = primary && enabled ? p.mint : p.surface;
  Surface(c, b, b.h / 2, bg, focused, p.text, Dp(m.width, 2));
  int pixels = FontPixels(Font::Menu, m.width);
  auto text = FitText(Tr(label), b.w - Dp(m.width, 16), pixels, true);
  c.Text(b.x + (b.w - TextWidth(text, pixels, true)) / 2,
         b.y + (b.h - LineHeight(pixels)) / 2, text, Font::Menu,
         !enabled ? p.secondary : primary ? p.on_primary : p.text, true);
}
inline void DrawPattern(Canvas& c, const Metrics& m, const PatternLayout& l,
                        const recovery_ui::PatternInput& input, bool dragging,
                        int finger_x, int finger_y, int focus, const Palette& p) {
  Rounded(c, l.grid, Dp(m.width, 32), p.card);
  for (size_t i = 1; i < input.Size(); ++i) {
    auto [x1, y1] = l.Dot(input.Cell(i - 1));
    auto [x2, y2] = l.Dot(input.Cell(i));
    Line(c, x1, y1, x2, y2, Dp(m.width, 4), p.primary);
  }
  if (dragging && input.Size()) {
    auto [x, y] = l.Dot(input.Cell(input.Size() - 1));
    Line(c, x, y, std::clamp(finger_x, l.grid.x, l.grid.x + l.grid.w - 1),
         std::clamp(finger_y, l.grid.y, l.grid.y + l.grid.h - 1), Dp(m.width, 3), p.outline);
  }
  for (int cell = 0; cell < l.CellCount(); ++cell) {
    auto [x, y] = l.Dot(cell);
    bool selected = input.Contains(cell);
    int r = std::min(l.radius, Dp(m.width, selected ? 19 : 7));
    if (focus == cell) Surface(c, {x - l.radius, y - l.radius, 2 * l.radius, 2 * l.radius},
                              l.radius, p.card, true, p.text, Dp(m.width, 2));
    if (selected) {
      Rounded(c, {x - r, y - r, 2 * r, 2 * r}, r, p.outline);
      r = std::min(r, Dp(m.width, 9));
    }
    Rounded(c, {x - r, y - r, 2 * r, 2 * r}, r, selected ? p.primary : p.secondary);
  }
  PatternButton(c, m, l.unlock, "Unlock storage", true, input.Size() >= 4 && !dragging,
                focus == l.UnlockAction(), p);
  PatternButton(c, m, l.clear, "Clear input", false, true, focus == l.ClearAction(), p);
  PatternButton(c, m, l.cancel, "Cancel", false, true, focus == l.CancelAction(), p);
}
}  // namespace recovery_m3e
