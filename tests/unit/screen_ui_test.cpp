/*
 * Copyright (C) 2018 The Android Open Source Project
 * Copyright (C) 2026 The uwuAOSP Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stddef.h>
#include <stdio.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <gtest/gtest.h>
#include <gtest/gtest_prod.h>

#include "common/test_constants.h"
#include "minui/minui.h"
#include "otautil/paths.h"
#include "private/resources.h"
#include "recovery_ui/device.h"
#include "recovery_ui/screen_ui.h"
#include "recovery_ui/m3e_pattern.h"

static const std::vector<std::string> HEADERS{ "header" };
static const std::vector<std::string> ITEMS{ "item1", "item2", "item3", "item4", "1234567890" };

// Synthetic cells only; these fixtures never read a user's credential or a HAL.
class TestPattern : public recovery_ui::PatternInput {
 public:
  explicit TestPattern(unsigned grid = 3) : grid_(grid) {}
  unsigned GridSize() const override { return grid_; }
  size_t Size() const override { return cells.size(); }
  uint8_t Cell(size_t index) const override { return cells[index]; }
  bool Append(uint8_t cell) override {
    if (cells.size() == grid_ * grid_) return false;
    cells.push_back(cell);
    return true;
  }
  void Clear() override { cells.clear(); }
  std::vector<uint8_t> cells;
 private:
  unsigned grid_;
};

TEST(PatternInputTest, InsertsOnlyUnvisitedMidpointsAndRejectsDuplicates) {
  TestPattern pattern;
  ASSERT_TRUE(pattern.Select(0));
  ASSERT_TRUE(pattern.Select(2));
  EXPECT_EQ(pattern.cells, (std::vector<uint8_t>{0, 1, 2}));
  EXPECT_FALSE(pattern.Select(1));
  EXPECT_FALSE(pattern.Select(9));
  ASSERT_TRUE(pattern.Select(8));
  EXPECT_EQ(pattern.cells, (std::vector<uint8_t>{0, 1, 2, 5, 8}));
  pattern.Clear();
  pattern.Select(4);
  pattern.Select(0);
  pattern.Select(8);
  EXPECT_EQ(pattern.cells, (std::vector<uint8_t>{4, 0, 8}));
}

TEST(PatternInputTest, SparseMotionKeepsCrossedDotsInOrder) {
  using namespace recovery_m3e;
  SetScaleBasis(360, 800);
  auto layout = PatternBounds(Metrics(360), 180, 776);
  TestPattern pattern;
  auto [x0, y0] = layout.Dot(0);
  auto [x2, y2] = layout.Dot(2);
  pattern.Select(0);
  TracePattern(pattern, layout, x0, y0, x2, y2);
  EXPECT_EQ(pattern.cells, (std::vector<uint8_t>{0, 1, 2}));
  auto [x8, y8] = layout.Dot(8);
  TracePattern(pattern, layout, x2, y2, x8, y8);
  EXPECT_EQ(pattern.cells, (std::vector<uint8_t>{0, 1, 2, 5, 8}));
  pattern.Clear();
  pattern.Select(0);
  TracePattern(pattern, layout, x0, y0, x8, y8);
  EXPECT_EQ(pattern.cells, (std::vector<uint8_t>{0, 4, 8}));
  TracePattern(pattern, layout, x8, y8, x0, y0);
  EXPECT_EQ(pattern.cells, (std::vector<uint8_t>{0, 4, 8}));
}

TEST(PatternInputTest, ControlsRemainOutsideGridOnPortraitAndLandscape) {
  using namespace recovery_m3e;
  for (const auto& dimensions : std::vector<std::pair<int, int>>{{360, 800}, {1220, 2712},
                                                               {800, 360}, {240, 320}}) {
    auto [width, height] = dimensions;
    SetScaleBasis(width, height);
    Metrics m(width);
    int top = Dp(width, 180), bottom = height - Dp(width, 24);
    for (unsigned n = 3; n <= 6; ++n) {
      auto layout = PatternBounds(m, top, bottom, n);
      for (const auto& b : {layout.grid, layout.unlock, layout.clear, layout.cancel}) {
        EXPECT_GT(b.w, 0);
        EXPECT_GT(b.h, 0);
        EXPECT_GE(b.x, 0);
        EXPECT_GE(b.y, top);
        EXPECT_LE(b.x + b.w, width);
        EXPECT_LE(b.y + b.h, bottom);
      }
      for (int cell = 0; cell < layout.CellCount(); ++cell) {
        auto [x, y] = layout.Dot(cell);
        EXPECT_EQ(layout.HitDot(x, y), cell);
        EXPECT_EQ(layout.HitAction(x, y), -2);  // Empty space must not mean Back.
      }
      for (const auto& b : {layout.unlock, layout.clear, layout.cancel}) {
        EXPECT_TRUE(b.x >= layout.grid.x + layout.grid.w ||
                    b.y >= layout.grid.y + layout.grid.h);
      }
      EXPECT_EQ(layout.HitAction(layout.unlock.x + 1, layout.unlock.y + 1), layout.UnlockAction());
      EXPECT_EQ(layout.HitAction(layout.clear.x + 1, layout.clear.y + 1), layout.ClearAction());
      EXPECT_EQ(layout.HitAction(layout.cancel.x + 1, layout.cancel.y + 1), layout.CancelAction());
    }
  }
  SetScaleBasis(0, 0);
}

TEST(PatternInputTest, LargerGridsFillAllStraightGapsInOrder) {
  using namespace recovery_m3e;
  SetScaleBasis(360, 800);
  for (unsigned n = 3; n <= 6; ++n) {
    TestPattern input(n);
    auto layout = PatternBounds(Metrics(360), 180, 776, n);
    auto [x0, y0] = layout.Dot(0);
    auto [xl, yl] = layout.Dot(n * n - 1);
    ASSERT_TRUE(input.Select(0));
    TracePattern(input, layout, x0, y0, xl, yl);
    std::vector<uint8_t> diagonal;
    for (unsigned i = 0; i < n; ++i) diagonal.push_back(i * (n + 1));
    EXPECT_EQ(input.cells, diagonal);
    input.Clear();
    input.Select(0);
    input.Select(n - 1);
    std::vector<uint8_t> border;
    for (unsigned i = 0; i < n; ++i) border.push_back(i);
    EXPECT_EQ(input.cells, border);
    input.Select(n * n - 1);
    for (unsigned i = 1; i < n; ++i) border.push_back(i * n + n - 1);
    EXPECT_EQ(input.cells, border);
    EXPECT_FALSE(input.Select(n * n));
    EXPECT_FALSE(input.Select(n - 1));
    input.Clear();
    input.Select(0);
    input.Select(n + 2);  // A non-axis/non-diagonal jump must not fill a gap.
    EXPECT_EQ(input.cells, (std::vector<uint8_t>{0, static_cast<uint8_t>(n + 2)}));
  }
  TestPattern invalid(7);
  EXPECT_FALSE(invalid.Select(0));
  SetScaleBasis(0, 0);
}

// TODO(xunchang) check if some draw functions are called when drawing menus.
class MockDrawFunctions : public DrawInterface {
  void SetColor(UIElement /* element */) const override {}
  void DrawHighlightBar(int /* x */, int /* y */, int /* width */,
                        int /* height */) const override {}
  int DrawHorizontalRule(int /* y */) const override {
    return 0;
  }
  int DrawTextLine(int /* x */, int /* y */, const std::string& /* line */,
                   bool /* bold */) const override {
    return 0;
  }
  void DrawSurface(const GRSurface* /* surface */, int /* sx */, int /* sy */, int /* w */,
                   int /* h */, int /* dx */, int /* dy */) const override {}
  void DrawFill(int /* x */, int /* y */, int /* w */, int /* h */) const override {}
  void DrawTextIcon(int /* x */, int /* y */, const GRSurface* /* surface */) const override {}
  int DrawTextLines(int /* x */, int /* y */,
                    const std::vector<std::string>& /* lines */) const override {
    return 0;
  }
  int DrawWrappedTextLines(int /* x */, int /* y */,
                           const std::vector<std::string>& /* lines */) const override {
    return 0;
  }
};

class ScreenUITest : public testing::Test {
 protected:
  MockDrawFunctions draw_funcs_;
};

TEST_F(ScreenUITest, StartPhoneMenuSmoke) {
  TextMenu menu(false, 10, 20, HEADERS, ITEMS, 0, 20, draw_funcs_);
  ASSERT_FALSE(menu.scrollable());
  ASSERT_EQ(HEADERS[0], menu.text_headers()[0]);
  ASSERT_EQ(5u, menu.ItemsCount());

  std::string message;
  ASSERT_FALSE(menu.ItemsOverflow(&message));
  for (size_t i = 0; i < menu.ItemsCount(); i++) {
    ASSERT_EQ(ITEMS[i], menu.TextItem(i));
  }

  ASSERT_EQ(0, menu.selection());
}

TEST_F(ScreenUITest, StartWearMenuSmoke) {
  TextMenu menu(true, 10, 8, HEADERS, ITEMS, 1, 20, draw_funcs_);
  ASSERT_TRUE(menu.scrollable());
  ASSERT_EQ(HEADERS[0], menu.text_headers()[0]);
  ASSERT_EQ(5u, menu.ItemsCount());

  std::string message;
  ASSERT_FALSE(menu.ItemsOverflow(&message));
  for (size_t i = 0; i < menu.ItemsCount() - 1; i++) {
    ASSERT_EQ(ITEMS[i], menu.TextItem(i));
  }
  // Test of the last item is truncated
  ASSERT_EQ("12345678", menu.TextItem(4));
  ASSERT_EQ(1, menu.selection());
}

TEST_F(ScreenUITest, StartPhoneMenuItemsOverflow) {
  TextMenu menu(false, 1, 20, HEADERS, ITEMS, 0, 20, draw_funcs_);
  ASSERT_FALSE(menu.scrollable());
  ASSERT_EQ(1u, menu.ItemsCount());

  std::string message;
  ASSERT_FALSE(menu.ItemsOverflow(&message));
  for (size_t i = 0; i < menu.ItemsCount(); i++) {
    ASSERT_EQ(ITEMS[i], menu.TextItem(i));
  }

  ASSERT_EQ(0u, menu.MenuStart());
  ASSERT_EQ(1u, menu.MenuEnd());
}

TEST_F(ScreenUITest, StartWearMenuItemsOverflow) {
  TextMenu menu(true, 1, 20, HEADERS, ITEMS, 0, 20, draw_funcs_);
  ASSERT_TRUE(menu.scrollable());
  ASSERT_EQ(5u, menu.ItemsCount());

  std::string message;
  ASSERT_TRUE(menu.ItemsOverflow(&message));
  ASSERT_EQ("Current item: 1/5", message);

  for (size_t i = 0; i < menu.ItemsCount(); i++) {
    ASSERT_EQ(ITEMS[i], menu.TextItem(i));
  }

  ASSERT_EQ(0u, menu.MenuStart());
  ASSERT_EQ(1u, menu.MenuEnd());
}

TEST_F(ScreenUITest, PhoneMenuSelectSmoke) {
  int sel = 0;
  TextMenu menu(false, 10, 20, HEADERS, ITEMS, sel, 20, draw_funcs_);
  // Mimic down button 10 times (2 * items size)
  for (int i = 0; i < 10; i++) {
    sel = menu.Select(++sel);
    ASSERT_EQ(sel, menu.selection());

    // Wraps the selection for unscrollable menu when it reaches the boundary.
    int expected = (i + 1) % 5;
    ASSERT_EQ(expected, menu.selection());

    ASSERT_EQ(0u, menu.MenuStart());
    ASSERT_EQ(5u, menu.MenuEnd());
  }

  // Mimic up button 10 times
  for (int i = 0; i < 10; i++) {
    sel = menu.Select(--sel);
    ASSERT_EQ(sel, menu.selection());

    int expected = (9 - i) % 5;
    ASSERT_EQ(expected, menu.selection());

    ASSERT_EQ(0u, menu.MenuStart());
    ASSERT_EQ(5u, menu.MenuEnd());
  }
}

TEST_F(ScreenUITest, WearMenuSelectSmoke) {
  int sel = 0;
  TextMenu menu(true, 10, 20, HEADERS, ITEMS, sel, 20, draw_funcs_);
  // Mimic pressing down button 10 times (2 * items size)
  for (int i = 0; i < 10; i++) {
    sel = menu.Select(++sel);
    ASSERT_EQ(sel, menu.selection());

    // Stops the selection at the boundary if the menu is scrollable.
    int expected = std::min(i + 1, 4);
    ASSERT_EQ(expected, menu.selection());

    ASSERT_EQ(0u, menu.MenuStart());
    ASSERT_EQ(5u, menu.MenuEnd());
  }

  // Mimic pressing up button 10 times
  for (int i = 0; i < 10; i++) {
    sel = menu.Select(--sel);
    ASSERT_EQ(sel, menu.selection());

    int expected = std::max(3 - i, 0);
    ASSERT_EQ(expected, menu.selection());

    ASSERT_EQ(0u, menu.MenuStart());
    ASSERT_EQ(5u, menu.MenuEnd());
  }
}

TEST_F(ScreenUITest, WearMenuSelectItemsOverflow) {
  int sel = 1;
  TextMenu menu(true, 3, 20, HEADERS, ITEMS, sel, 20, draw_funcs_);
  ASSERT_EQ(5u, menu.ItemsCount());

  // Scroll the menu to the end, and check the start & end of menu.
  for (int i = 0; i < 3; i++) {
    sel = menu.Select(++sel);
    ASSERT_EQ(i + 2, sel);
    ASSERT_EQ(static_cast<size_t>(i), menu.MenuStart());
    ASSERT_EQ(static_cast<size_t>(i + 3), menu.MenuEnd());
  }

  // Press down button one more time won't change the MenuStart() and MenuEnd().
  sel = menu.Select(++sel);
  ASSERT_EQ(4, sel);
  ASSERT_EQ(2u, menu.MenuStart());
  ASSERT_EQ(5u, menu.MenuEnd());

  // Scroll the menu to the top.
  // The expected menu sel, start & ends are:
  // sel 3, start 2, end 5
  // sel 2, start 2, end 5
  // sel 1, start 1, end 4
  // sel 0, start 0, end 3
  for (int i = 0; i < 4; i++) {
    sel = menu.Select(--sel);
    ASSERT_EQ(3 - i, sel);
    ASSERT_EQ(static_cast<size_t>(std::min(3 - i, 2)), menu.MenuStart());
    ASSERT_EQ(static_cast<size_t>(std::min(6 - i, 5)), menu.MenuEnd());
  }

  // Press up button one more time won't change the MenuStart() and MenuEnd().
  sel = menu.Select(--sel);
  ASSERT_EQ(0, sel);
  ASSERT_EQ(0u, menu.MenuStart());
  ASSERT_EQ(3u, menu.MenuEnd());
}

TEST_F(ScreenUITest, GraphicMenuSelection) {
  auto image = GRSurface::Create(50, 50, 50, 1);
  auto header = image->Clone();
  std::vector<const GRSurface*> items = {
    image.get(),
    image.get(),
    image.get(),
  };
  GraphicMenu menu(header.get(), items, 0, draw_funcs_);

  ASSERT_EQ(0, menu.selection());

  int sel = 0;
  for (int i = 0; i < 3; i++) {
    sel = menu.Select(++sel);
    ASSERT_EQ((i + 1) % 3, sel);
    ASSERT_EQ(sel, menu.selection());
  }

  sel = 0;
  for (int i = 0; i < 3; i++) {
    sel = menu.Select(--sel);
    ASSERT_EQ(2 - i, sel);
    ASSERT_EQ(sel, menu.selection());
  }
}

TEST_F(ScreenUITest, GraphicMenuValidate) {
  auto image = GRSurface::Create(50, 50, 50, 1);
  auto header = image->Clone();
  std::vector<const GRSurface*> items = {
    image.get(),
    image.get(),
    image.get(),
  };

  ASSERT_TRUE(GraphicMenu::Validate(200, 200, header.get(), items));

  // Menu exceeds the horizontal boundary.
  auto wide_surface = GRSurface::Create(300, 50, 300, 1);
  ASSERT_FALSE(GraphicMenu::Validate(299, 200, wide_surface.get(), items));

  // Menu exceeds the vertical boundary.
  items.emplace_back(image.get());
  ASSERT_FALSE(GraphicMenu::Validate(200, 249, header.get(), items));
}

static constexpr int kMagicAction = 101;

enum class KeyCode : int {
  TIMEOUT = -1,
  NO_OP = 0,
  UP = 1,
  DOWN = 2,
  ENTER = 3,
  MAGIC = 1001,
  LAST,
};

static const std::map<KeyCode, int> kKeyMapping{
  // clang-format off
  { KeyCode::NO_OP, Device::kNoAction },
  { KeyCode::UP, Device::kHighlightUp },
  { KeyCode::DOWN, Device::kHighlightDown },
  { KeyCode::ENTER, Device::kInvokeItem },
  { KeyCode::MAGIC, kMagicAction },
  // clang-format on
};

class TestableScreenRecoveryUI : public ScreenRecoveryUI {
 public:
  int WaitKey() override;

  void SetKeyBuffer(const std::vector<KeyCode>& buffer);

  int KeyHandler(int key, bool visible) const;

 private:
  FRIEND_TEST(DISABLED_ScreenRecoveryUITest, Init);
  FRIEND_TEST(DISABLED_ScreenRecoveryUITest, RtlLocale);
  FRIEND_TEST(DISABLED_ScreenRecoveryUITest, RtlLocaleWithSuffix);
  FRIEND_TEST(DISABLED_ScreenRecoveryUITest, LoadAnimation);
  FRIEND_TEST(DISABLED_ScreenRecoveryUITest, LoadAnimation_MissingAnimation);

  std::vector<KeyCode> key_buffer_;
  size_t key_buffer_index_;
};

void TestableScreenRecoveryUI::SetKeyBuffer(const std::vector<KeyCode>& buffer) {
  key_buffer_ = buffer;
  key_buffer_index_ = 0;
}

int TestableScreenRecoveryUI::KeyHandler(int key, bool) const {
  KeyCode key_code = static_cast<KeyCode>(key);
  if (kKeyMapping.find(key_code) != kKeyMapping.end()) {
    return kKeyMapping.at(key_code);
  }
  return Device::kNoAction;
}

int TestableScreenRecoveryUI::WaitKey() {
  if (IsKeyInterrupted()) {
    return static_cast<int>(RecoveryUI::KeyError::INTERRUPTED);
  }

  CHECK_LT(key_buffer_index_, key_buffer_.size());
  return static_cast<int>(key_buffer_[key_buffer_index_++]);
}

class DISABLED_ScreenRecoveryUITest : public ::testing::Test {
 protected:
  const std::string kTestLocale = "en-US";
  const std::string kTestRtlLocale = "ar";
  const std::string kTestRtlLocaleWithSuffix = "ar-EG";

  void SetUp() override {
    has_graphics_ = gr_init() == 0;
    gr_exit();

    if (has_graphics_) {
      ui_ = std::make_unique<TestableScreenRecoveryUI>();
    }

    testdata_dir_ = from_testdata_base("");
    Paths::Get().set_resource_dir(testdata_dir_);
    res_set_resource_dir(testdata_dir_);
  }

  bool has_graphics_;
  std::unique_ptr<TestableScreenRecoveryUI> ui_;
  std::string testdata_dir_;
};

#define RETURN_IF_NO_GRAPHICS                                                 \
  do {                                                                        \
    if (!has_graphics_) {                                                     \
      GTEST_LOG_(INFO) << "Test skipped due to no available graphics device"; \
      return;                                                                 \
    }                                                                         \
  } while (false)

TEST_F(DISABLED_ScreenRecoveryUITest, Init) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestLocale));
  ASSERT_EQ(kTestLocale, ui_->GetLocale());
  ASSERT_FALSE(ui_->rtl_locale_);
  ASSERT_FALSE(ui_->IsTextVisible());
  ASSERT_FALSE(ui_->WasTextEverVisible());
}

TEST_F(DISABLED_ScreenRecoveryUITest, dtor_NotCallingInit) {
  ui_.reset();
  ASSERT_FALSE(ui_);
}

TEST_F(DISABLED_ScreenRecoveryUITest, ShowText) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestLocale));
  ASSERT_FALSE(ui_->IsTextVisible());
  ui_->ShowText(true);
  ASSERT_TRUE(ui_->IsTextVisible());
  ASSERT_TRUE(ui_->WasTextEverVisible());

  ui_->ShowText(false);
  ASSERT_FALSE(ui_->IsTextVisible());
  ASSERT_TRUE(ui_->WasTextEverVisible());
}

TEST_F(DISABLED_ScreenRecoveryUITest, RtlLocale) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestRtlLocale));
  ASSERT_TRUE(ui_->rtl_locale_);
}

TEST_F(DISABLED_ScreenRecoveryUITest, RtlLocaleWithSuffix) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestRtlLocaleWithSuffix));
  ASSERT_TRUE(ui_->rtl_locale_);
}

TEST_F(DISABLED_ScreenRecoveryUITest, ShowMenu) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestLocale));
  ui_->SetKeyBuffer({
      KeyCode::UP,
      KeyCode::DOWN,
      KeyCode::UP,
      KeyCode::DOWN,
      KeyCode::ENTER,
  });
  ASSERT_EQ(3u, ui_->ShowMenu(HEADERS, ITEMS, 3, true,
                              std::bind(&TestableScreenRecoveryUI::KeyHandler, ui_.get(),
                                        std::placeholders::_1, std::placeholders::_2)));

  ui_->SetKeyBuffer({
      KeyCode::UP,
      KeyCode::UP,
      KeyCode::NO_OP,
      KeyCode::NO_OP,
      KeyCode::UP,
      KeyCode::ENTER,
  });
  ASSERT_EQ(2u, ui_->ShowMenu(HEADERS, ITEMS, 0, true,
                              std::bind(&TestableScreenRecoveryUI::KeyHandler, ui_.get(),
                                        std::placeholders::_1, std::placeholders::_2)));
}

TEST_F(DISABLED_ScreenRecoveryUITest, ShowMenu_NotMenuOnly) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestLocale));
  ui_->SetKeyBuffer({
      KeyCode::MAGIC,
  });
  ASSERT_EQ(static_cast<size_t>(kMagicAction),
            ui_->ShowMenu(HEADERS, ITEMS, 3, false,
                          std::bind(&TestableScreenRecoveryUI::KeyHandler, ui_.get(),
                                    std::placeholders::_1, std::placeholders::_2)));
}

TEST_F(DISABLED_ScreenRecoveryUITest, ShowMenu_TimedOut) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestLocale));
  ui_->SetKeyBuffer({
      KeyCode::TIMEOUT,
  });
  ASSERT_EQ(static_cast<size_t>(RecoveryUI::KeyError::TIMED_OUT),
            ui_->ShowMenu(HEADERS, ITEMS, 3, true, nullptr));
}

TEST_F(DISABLED_ScreenRecoveryUITest, ShowMenu_TimedOut_TextWasEverVisible) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestLocale));
  ui_->ShowText(true);
  ui_->ShowText(false);
  ASSERT_TRUE(ui_->WasTextEverVisible());

  ui_->SetKeyBuffer({
      KeyCode::TIMEOUT,
      KeyCode::DOWN,
      KeyCode::ENTER,
  });
  ASSERT_EQ(4u, ui_->ShowMenu(HEADERS, ITEMS, 3, true,
                              std::bind(&TestableScreenRecoveryUI::KeyHandler, ui_.get(),
                                        std::placeholders::_1, std::placeholders::_2)));
}

TEST_F(DISABLED_ScreenRecoveryUITest, ShowMenuWithInterrupt) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestLocale));
  ui_->SetKeyBuffer({
      KeyCode::UP,
      KeyCode::DOWN,
      KeyCode::UP,
      KeyCode::DOWN,
      KeyCode::ENTER,
  });

  ui_->InterruptKey();
  ASSERT_EQ(static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED),
            ui_->ShowMenu(HEADERS, ITEMS, 3, true,
                          std::bind(&TestableScreenRecoveryUI::KeyHandler, ui_.get(),
                                    std::placeholders::_1, std::placeholders::_2)));

  ui_->SetKeyBuffer({
      KeyCode::UP,
      KeyCode::UP,
      KeyCode::NO_OP,
      KeyCode::NO_OP,
      KeyCode::UP,
      KeyCode::ENTER,
  });
  ASSERT_EQ(static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED),
            ui_->ShowMenu(HEADERS, ITEMS, 0, true,
                          std::bind(&TestableScreenRecoveryUI::KeyHandler, ui_.get(),
                                    std::placeholders::_1, std::placeholders::_2)));
}

TEST_F(DISABLED_ScreenRecoveryUITest, LoadAnimation) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestLocale));
  // Make a few copies of loop00000.png from testdata.
  std::string image_data;
  ASSERT_TRUE(android::base::ReadFileToString(testdata_dir_ + "/loop00000.png", &image_data));

  std::vector<std::string> tempfiles;
  TemporaryDir resource_dir;
  for (const auto& name : { "00002", "00100", "00050" }) {
    tempfiles.push_back(android::base::StringPrintf("%s/loop%s.png", resource_dir.path, name));
    ASSERT_TRUE(android::base::WriteStringToFile(image_data, tempfiles.back()));
  }
  for (const auto& name : { "00", "01" }) {
    tempfiles.push_back(android::base::StringPrintf("%s/intro%s.png", resource_dir.path, name));
    ASSERT_TRUE(android::base::WriteStringToFile(image_data, tempfiles.back()));
  }
  Paths::Get().set_resource_dir(resource_dir.path);

  ui_->LoadAnimation();

  ASSERT_EQ(2u, ui_->intro_frames_.size());
  ASSERT_EQ(3u, ui_->loop_frames_.size());

  for (const auto& name : tempfiles) {
    ASSERT_EQ(0, unlink(name.c_str()));
  }
}

TEST_F(DISABLED_ScreenRecoveryUITest, LoadAnimation_MissingAnimation) {
  RETURN_IF_NO_GRAPHICS;

  ASSERT_TRUE(ui_->Init(kTestLocale));
  // We need a dir that doesn't contain any animation. However, using TemporaryDir will give
  // leftovers since this is a death test where TemporaryDir::~TemporaryDir() won't be called.
  Paths::Get().set_resource_dir("/proc/self");

  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  ASSERT_EXIT(ui_->LoadAnimation(), ::testing::KilledBySignal(SIGABRT), "");
}

#undef RETURN_IF_NO_GRAPHICS
