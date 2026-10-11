/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "recovery_ui/screen_ui.h"
#include "recovery_ui/m3e.h"
#include "recovery_ui/m3e_install.h"
#include "recovery_ui/m3e_design.h"
#include "recovery_ui/m3e_terminal.h"
#include "recovery_ui/m3e_password.h"
#include "recovery_ui/terminal_session.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <aidl/android/hardware/health/BatteryStatus.h>

#include <android-base/chrono_utils.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include <health/utils.h>
#include <healthd/BatteryMonitor.h>

#include "minui/minui.h"
#include "otautil/paths.h"
#include "recovery_ui/device.h"
#include "recovery_ui/ui.h"
#include "recovery_ui/m3e_pattern.h"


// M3E presentation uses minui only; installation code and confirmation flow stay upstream.
class M3eCanvas : public recovery_m3e::Canvas {
 public:
  void Fill(recovery_m3e::Rect b, recovery_m3e::Color c) override {
    if (b.w <= 0 || b.h <= 0) return;
    int x1 = std::max(0, b.x), y1 = std::max(0, b.y);
    int x2 = std::min(gr_fb_width(), b.x + b.w), y2 = std::min(gr_fb_height(), b.y + b.h);
    if (x2 <= x1 || y2 <= y1) return;
    gr_color(c.r, c.g, c.b, 255);
    gr_fill(x1, y1, x2, y2);
  }
  void Text(int x, int y, const std::string& text, recovery_m3e::Font kind,
            recovery_m3e::Color color, bool bold) override {
    if (text.empty()) return;
    auto bitmap = recovery_m3e::RasterText(text, recovery_m3e::FontPixels(kind, gr_fb_width()), bold,
        recovery_m3e::Monospace(kind), recovery_m3e::FaceFor(kind));
    Mask({x, y, bitmap.width, bitmap.height}, bitmap.alpha, color);
  }
  void Mask(recovery_m3e::Rect b, const std::vector<uint8_t>& alpha,
            recovery_m3e::Color color) override {
    int left = std::max(0, b.x), top = std::max(0, b.y);
    int width = std::min(gr_fb_width(), b.x + b.w) - left;
    int height = std::min(gr_fb_height(), b.y + b.h) - top;
    if (width <= 0 || height <= 0) return;
    auto surface = GRSurface::Create(width, height, width, 1);
    if (!surface) return;
    for (int row = 0; row < height; ++row) {
      memcpy(surface->data() + row * surface->row_bytes,
             alpha.data() + (top - b.y + row) * b.w + left - b.x, width);
    }
    gr_color(color.r, color.g, color.b, 255);
    gr_texticon(left, top, surface.get());
  }
};
static void M3eSetColor(recovery_m3e::Color c) { gr_color(c.r, c.g, c.b, 255); }

void ScreenRecoveryUI::DrawTerminalLocked() {
  M3eCanvas canvas;
  terminal_state_->view.Update(terminal_state_->output,ScreenWidth(),ScreenHeight());
  recovery_m3e::terminal::Draw(canvas,ScreenWidth(),ScreenHeight(),terminal_input_,terminal_state_->view,
      terminal_symbols_,terminal_shift_,terminal_focus_);
  recovery_m3e::DrawPageFooter(canvas,ScreenWidth(),ScreenHeight(),title_lines_);
}

void ScreenRecoveryUI::ShowTerminal() {
  if (IsKeyInterrupted()) return;
  recovery_ui::TerminalSession session;
  int columns=std::max(20,ScreenWidth()/recovery_m3e::Dp(ScreenWidth(),7));
  bool opened=session.Open(24,columns);
  std::string start_error=opened?"":std::string("Unable to start terminal: ")+strerror(errno)+"\n";
  {
    std::lock_guard<std::mutex> lock(updateMutex);
    menu_.reset();transition_menu_.reset();menu_transition_=false;
    terminal_visible_=true;terminal_shift_=terminal_symbols_=false;terminal_focus_=-1;
    terminal_state_=std::make_unique<recovery_m3e::terminal::State>();
    terminal_state_->output.Append(start_error.data(),start_error.size());
    auto bounds=recovery_m3e::terminal::OutputBounds(ScreenWidth(),ScreenHeight());
    SetTouchMoveCoalescing(true,Point(bounds.x,bounds.y),Point(bounds.x+bounds.w,bounds.y+bounds.h));
    terminal_input_.clear();gesture_input_=true;FlushKeys();update_screen_locked();
  }
  auto& output=terminal_state_->output;
  std::atomic<bool> stopped{false};
  std::thread reader;
  if (opened) reader=std::thread([&] {
    char bytes[2048];
    while (!stopped) {
      ssize_t count=session.Read(bytes,sizeof(bytes));
      if (count==0) continue;
      std::lock_guard<std::mutex> lock(updateMutex);
      if (count<0) {
        const std::string ended="\n[Shell exited. Tap Back to return.]\n";
        output.Append(ended.data(),ended.size());
      } else output.Append(bytes,count);
      update_screen_locked();
      if (count<0) break;
    }
  });
  bool done=false,dragging=false,moved=false;int pressed=-2;Point origin,last;
  while (!done) {
    auto event=WaitInputEvent();
    std::lock_guard<std::mutex> lock(updateMutex);
    auto keys=recovery_m3e::terminal::Keyboard(ScreenWidth(),ScreenHeight(),terminal_symbols_,terminal_shift_);
    auto send=[&](const std::string& value) {
      if (!session.Send(value)) {
        const std::string failed="\n[Input could not be sent to the shell.]\n";
        output.Append(failed.data(),failed.size());
      }
    };
    auto invoke=[&](int index) {
      if (index==-1) {done=true;return;}
      if (index<0 || index>=static_cast<int>(keys.size()))return;
      const auto& key=keys[index];
      if (!key.value.empty()) {
        if (terminal_input_.size()<4096)terminal_input_+=key.value;
      } else if (key.label=="ABC" && terminal_symbols_)terminal_symbols_=false;
      else if (key.label=="123")terminal_symbols_=true;
      else if (key.label=="ABC" || key.label=="abc" || key.label=="More" || key.label=="Less")terminal_shift_=!terminal_shift_;
      else if (key.label=="Space") {if(terminal_input_.size()<4096)terminal_input_+=' ';}
      else if (key.label=="Del") {if(!terminal_input_.empty())terminal_input_.pop_back();}
      else if (key.label=="Enter") {terminal_state_->view.Bottom();send(terminal_input_+"\n");terminal_input_.clear();}
      else if (key.label=="^C") {send(std::string(1,3));terminal_input_.clear();}
      else if (key.label=="Clear") {output.Clear();terminal_state_->view= recovery_m3e::terminal::Viewport{};}
    };
    if (event.type()==EventType::EXTRA) {
      if (event.key()==static_cast<int>(KeyError::INTERRUPTED))done=true;
      continue;
    }
    if (event.type()==EventType::TOUCH || event.type()==EventType::TOUCH_DOWN ||
        event.type()==EventType::TOUCH_MOVE || event.type()==EventType::TOUCH_UP) {
      auto point=TouchPoint(event.pos());auto back=recovery_m3e::design::Back(ScreenWidth());
      int action=recovery_m3e::terminal::HitKey(keys,point.x(),point.y());
      if(action<0)action=-2;
      if(recovery_m3e::InRounded(back,back.h/2,point.x(),point.y()))action=-1;
      auto& view=terminal_state_->view;view.Update(output,ScreenWidth(),ScreenHeight());
      if(event.type()==EventType::TOUCH)invoke(action);
      else if(event.type()==EventType::TOUCH_DOWN) {
        origin=last=point;pressed=action;moved=false;
        dragging=view.Bounds().Contains(point.x(),point.y());
        if(action>=-1)terminal_focus_=action;
      } else {
        int64_t dx=point.x()-origin.x(),dy=point.y()-origin.y();int slop=recovery_m3e::Dp(ScreenWidth(),16);
        if(dx*dx+dy*dy>int64_t(slop)*slop) {pressed=-2;moved=true;}
        if(dragging) {
          view.ScrollPixels(last.y()-point.y());last=point;
        }
        if(event.type()==EventType::TOUCH_UP) {
          if(!moved && pressed!=-2 && action==pressed)invoke(action);
          pressed=-2;dragging=false;
        }
      }
    } else if (event.type()==EventType::KEY) {
      int key=event.key();
      if (key==KEY_BACK || key==KEY_ESC)done=true;
      else if (key==KEY_VOLUMEUP || key==KEY_UP)terminal_focus_=terminal_focus_<0?static_cast<int>(keys.size())-1:terminal_focus_-1;
      else if (key==KEY_VOLUMEDOWN || key==KEY_DOWN)terminal_focus_=terminal_focus_+1>=static_cast<int>(keys.size())?-1:terminal_focus_+1;
      else if (key==KEY_POWER)invoke(terminal_focus_);
      else if (key==KEY_PAGEUP)terminal_state_->view.Scroll(-std::max(1,terminal_state_->view.Visible()));
      else if (key==KEY_PAGEDOWN)terminal_state_->view.Scroll(std::max(1,terminal_state_->view.Visible()));
      else if (key==KEY_HOME)terminal_state_->view.Top();
      else if (key==KEY_END)terminal_state_->view.Bottom();
      else if (key==KEY_ENTER) {terminal_state_->view.Bottom();send(terminal_input_+"\n");terminal_input_.clear();}
      else if (key==KEY_BACKSPACE) {if(!terminal_input_.empty())terminal_input_.pop_back();}
      else if (key==KEY_SPACE && terminal_input_.size()<4096)terminal_input_+=' ';
    }
    update_screen_locked();
  }
  stopped=true;if(reader.joinable())reader.join();session.Close();
  {
    std::lock_guard<std::mutex> lock(updateMutex);
    SetTouchMoveCoalescing(false,Point(),Point());
    gesture_input_=false;discard_touch_until_press_=true;
    terminal_visible_=false;terminal_input_.clear();terminal_state_.reset();
    menu_transition_=true;FlushKeys();
  }
}

enum DirectRenderManager {
    DRM_INNER,
    DRM_OUTER,
};

// Return the current time as a double (including fractions of a second).
static double now() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec / 1000000.0;
}

Menu::Menu(size_t initial_selection, const DrawInterface& draw_func)
    : selection_(initial_selection), draw_funcs_(draw_func) {}

int Menu::selection() const {
  return selection_;
}

TextMenu::TextMenu(bool wrappable, size_t /*max_length*/,
                   const std::vector<std::string>& headers, const std::vector<std::string>& items,
                   size_t initial_selection, int /*char_height*/, const DrawInterface& draw_funcs)
    : Menu(initial_selection, draw_funcs),
      wrappable_(wrappable),
      calibrated_height_(false),
      max_display_items_(1),
      text_headers_(headers),
      menu_start_(0) {

  size_t items_count = items.size();
  for (size_t i = 0; i < items_count; ++i) {
    text_items_.emplace_back(items[i]);
  }

  CHECK(!text_items_.empty());
}

const std::vector<std::string>& TextMenu::text_headers() const {
  return text_headers_;
}

std::string TextMenu::TextItem(size_t index) const {
  CHECK_LT(index, text_items_.size());

  return text_items_[index];
}

size_t TextMenu::MenuStart() const {
  return menu_start_;
}

size_t TextMenu::MenuEnd() const {
  return std::min(ItemsCount(), menu_start_ + max_display_items_);
}

size_t TextMenu::ItemsCount() const {
  return text_items_.size();
}

bool TextMenu::ItemsOverflow(std::string* cur_selection_str) const {
  if (ItemsCount() <= max_display_items_) {
    return false;
  }

  *cur_selection_str =
      android::base::StringPrintf("Current item: %d/%zu", selection_ + 1, ItemsCount());
  return true;
}

// TODO(xunchang) modify the function parameters to button up & down.
int TextMenu::Select(int sel) {
  CHECK_LE(ItemsCount(), static_cast<size_t>(std::numeric_limits<int>::max()));
  int count = ItemsCount();

  int min = IsMain() ? 0 : -1; // -1 is back arrow

  if (sel < min) {
    selection_ = wrappable() ? count - 1 : min;
  } else if (sel >= count) {
    selection_ = wrappable() ? min : count - 1;
  } else {
    selection_ = sel;
  }

  if (selection_ >= 0 && max_display_items_ > 0) {
    if (selection_ < menu_start_) {
      menu_start_ = selection_;
    } else if (static_cast<size_t>(selection_) >= MenuEnd()) {
      menu_start_ = selection_ - max_display_items_ + 1;
    }
  }

  return selection_;
}

int TextMenu::SelectVisible(int relative_sel) {
  int sel = relative_sel;
  if (menu_start_ > 0) {
    sel += menu_start_;
  }

  return Select(sel);
}

int TextMenu::Scroll(int updown) {
  if (max_display_items_ == 0) return selection_;
  if ((updown > 0 && menu_start_ + max_display_items_ < ItemsCount()) ||
      (updown < 0 && menu_start_ > 0)) {
    menu_start_ += updown;

    /* We can receive a kInvokeItem event from a different source than touch,
       like from Power button. For this reason, selection should not get out of
       the screen. Constrain it to the first or last visible item of the list */
    if (selection_ < menu_start_) {
      selection_ = menu_start_;
    } else if (selection_ >= menu_start_ + max_display_items_) {
      selection_ = menu_start_ + max_display_items_ - 1;
    }
  }
  return selection_;
}

bool TextMenu::DashboardCandidate() const {
  static const std::vector<std::string> main_items{
    "Apply update", "Terminal", "Settings", "Reboot options", "Factory reset"};
  return text_headers_.empty() && text_items_ == main_items;
}
std::string TextMenu::PageTitle() const {
  if (text_headers_.empty() && text_items_ == std::vector<std::string>{
      "Reboot system now", "Reboot to bootloader", "Enter recovery", "Power off"}) return "FastbootD";
  if (text_headers_.size() == 1) {
    if (text_headers_[0] == "ADB Sideload" || text_headers_[0] == "Install result" ||
        text_headers_[0] == "Flash result" || text_headers_[0] == "Flash partition image") return text_headers_[0];
    if (text_headers_[0] == "Settings" || text_headers_[0] == "Language" ||
        text_headers_[0] == "Reboot options" || text_headers_[0] == "Advanced tools") return text_headers_[0];
    if (text_headers_[0] == "Advanced options") return "Tools";
    if (text_headers_[0] == "Apply update") return "Install update";
    if (text_headers_[0] == "Factory reset") return "Factory reset";
  }
  if (std::find(text_items_.begin(), text_items_.end(), " Format data") != text_items_.end() ||
      std::find(text_items_.begin(), text_items_.end(), "Factory data reset") != text_items_.end())
    return "Factory reset";
  return IsMain() ? "Recovery" : "Confirm or select";
}
int TextMenu::DrawHeader(int x, int y) const {
  if (text_headers_ == std::vector<std::string>{"Install result"} ||
      text_headers_ == std::vector<std::string>{"Flash result"} ||
      text_headers_ == std::vector<std::string>{"Flash partition image"}) return 0;
  if (text_headers_.size() == 1 && (text_headers_[0] == "Advanced options" ||
      text_headers_[0] == "Apply update" || text_headers_[0] == "Factory reset" ||
      text_headers_[0] == "Settings" || text_headers_[0] == "Language" ||
      text_headers_[0] == "Reboot options" || text_headers_[0] == "Advanced tools")) return 0;
  return draw_funcs_.DrawMenuPrompt(x, y, text_headers_);
}
void TextMenu::SetViewport(int width, int height) {
  screen_width_ = width;
  SetMenuHeight(height);
}
void TextMenu::SetMenuHeight(int height) {
  viewport_height_ = std::max(0, height);
  dashboard_ = DashboardCandidate() && screen_width_ > 0 &&
      recovery_m3e::design::Home(screen_width_, 0, viewport_height_).valid;
  int row = draw_funcs_.MenuItemHeight(), gap = draw_funcs_.MenuItemSpacing();
  auto page = recovery_m3e::design::MenuPage(DashboardCandidate(), PageTitle());
  int reserved = recovery_m3e::design::SeparatePower(page) ? recovery_m3e::Dp(screen_width_,27) : 0;
  size_t visible = dashboard_ ? 5 : recovery_m3e::VisibleCount(viewport_height_ - reserved, row, gap);
  if (!calibrated_height_ || visible != max_display_items_) {
    max_display_items_ = visible;
    if (max_display_items_ > 0)
      menu_start_ = std::max(0, selection_ - static_cast<int>(max_display_items_) + 1);
    calibrated_height_ = true;
  }
  if (dashboard_) menu_start_ = 0;
}
int TextMenu::DrawItems(int /*x*/, int y, int screen_width, bool long_press) const {
  if (dashboard_) return draw_funcs_.DrawDashboard(y, screen_width, viewport_height_, selection_, long_press);
  int offset = 0;
  int height = draw_funcs_.MenuItemHeight(), spacing = draw_funcs_.MenuItemSpacing();
  auto page = recovery_m3e::design::MenuPage(DashboardCandidate(), PageTitle());
  for (size_t i = MenuStart(); i < MenuEnd(); ++i) {
    bool selected = static_cast<int>(i) == selection();
    bool separate = recovery_m3e::design::SeparatePower(page) && i + 1 == ItemsCount();
    if (i > MenuStart()) offset += recovery_m3e::design::ExtraGap(screen_width, page, separate);
    draw_funcs_.DrawMenuCard(y + offset, screen_width, TextItem(i), selected, selected && long_press,
        i == 0 || separate, i + 1 == ItemsCount() || (recovery_m3e::design::SeparatePower(page) && i + 2 == ItemsCount()));
    offset += height + spacing;
  }
  if (MenuEnd() > MenuStart()) offset -= spacing;
  std::string unused;
  if (ItemsOverflow(&unused) && max_display_items_ > 0) {
    int thumb = std::max(8, static_cast<int>(offset * max_display_items_ / ItemsCount()));
    int travel = std::max(0, offset - thumb);
    int thumb_y = y + travel * menu_start_ / (ItemsCount() - max_display_items_);
    draw_funcs_.SetColor(UIElement::SCROLLBAR);
    draw_funcs_.DrawScrollBar(thumb_y, thumb);
  }
  return offset;
}
int TextMenu::HitTest(int x, int y, int screen_width) const {
  if (y < 0 || y >= viewport_height_) return -1;
  recovery_m3e::Metrics m(screen_width);
  if (dashboard_) return recovery_m3e::design::HitHome(screen_width, viewport_height_, x, y);
  auto page = recovery_m3e::design::MenuPage(DashboardCandidate(), PageTitle());
  if (page != recovery_m3e::design::Page::None)
    return recovery_m3e::design::HitList(screen_width,page,MenuEnd()-MenuStart(),MenuStart(),ItemsCount(),x,y);
  return recovery_m3e::HitRow(m, 0, MenuEnd() - MenuStart(),
                             selection_ - static_cast<int>(MenuStart()), x, y,
                             static_cast<int>(MenuStart()), static_cast<int>(ItemsCount()));
}

GraphicMenu::GraphicMenu(const GRSurface* graphic_headers,
                         const std::vector<const GRSurface*>& graphic_items,
                         size_t initial_selection, const DrawInterface& draw_funcs)
    : Menu(initial_selection, draw_funcs) {
  graphic_headers_ = graphic_headers->Clone();
  graphic_items_.reserve(graphic_items.size());
  for (const auto& item : graphic_items) {
    graphic_items_.emplace_back(item->Clone());
  }
}

// Define destructor out-of-line so that GRSurface is defined for unique_ptr.
GraphicMenu::~GraphicMenu() = default;

int GraphicMenu::Select(int sel) {
  CHECK_LE(graphic_items_.size(), static_cast<size_t>(std::numeric_limits<int>::max()));
  int count = graphic_items_.size();

  // Wraps the selection at boundary if the menu is not scrollable.
  if (sel < 0) {
    selection_ = count - 1;
  } else if (sel >= count) {
    selection_ = 0;
  } else {
    selection_ = sel;
  }

  return selection_;
}

int GraphicMenu::DrawHeader(int x, int y) const {
  draw_funcs_.SetColor(UIElement::HEADER);
  draw_funcs_.DrawTextIcon(x, y, graphic_headers_.get());
  return graphic_headers_->height;
}

int GraphicMenu::DrawItems(int x, int y, int screen_width, bool long_press) const {
  int offset = 0;
  for (size_t i = 0; i < graphic_items_.size(); ++i) {
    const auto& item = graphic_items_[i];
    if (offset + static_cast<int>(item->height) > viewport_height_) break;
    bool selected = static_cast<int>(i) == selection_;
    if (selected) {
      draw_funcs_.SetColor(long_press ? UIElement::MENU_SEL_BG_ACTIVE : UIElement::MENU_SEL_BG);
      draw_funcs_.DrawHighlightBar(x, y + offset, screen_width - 2 * x, item->height);
    }
    draw_funcs_.SetColor(selected ? UIElement::MENU_SEL_FG : UIElement::MENU);
    draw_funcs_.DrawTextIcon(x, y + offset, item.get());
    offset += item->height;
  }
  return offset;
}
int GraphicMenu::HitTest(int x, int y, int screen_width) const {
  recovery_m3e::Metrics m(screen_width);
  if (x < m.inset || x >= screen_width - m.inset || y < 0 || y >= viewport_height_) return -1;
  int offset = 0;
  for (size_t i = 0; i < graphic_items_.size(); ++i) {
    offset += graphic_items_[i]->height;
    if (offset > viewport_height_) break;
    if (y < offset) return i;
  }
  return -1;
}
bool GraphicMenu::HasVisibleItems() const {
  int bottom = 0;
  for (int i = 0; i <= selection_ && i < static_cast<int>(graphic_items_.size()); ++i)
    bottom += graphic_items_[i]->height;
  return selection_ >= 0 && bottom > 0 && bottom <= viewport_height_;
}

size_t GraphicMenu::ItemsCount() const {
  return graphic_items_.size();
}

bool GraphicMenu::Validate(size_t max_width, size_t max_height, const GRSurface* graphic_headers,
                           const std::vector<const GRSurface*>& graphic_items) {
  int offset = 0;
  if (!ValidateGraphicSurface(max_width, max_height, offset, graphic_headers)) {
    return false;
  }
  offset += graphic_headers->height;

  for (const auto& item : graphic_items) {
    if (!ValidateGraphicSurface(max_width, max_height, offset, item)) {
      return false;
    }
    offset += item->height;
  }

  return true;
}

bool GraphicMenu::ValidateGraphicSurface(size_t max_width, size_t max_height, int y,
                                         const GRSurface* surface) {
  if (!surface) {
    fprintf(stderr, "Graphic surface can not be null\n");
    return false;
  }

  if (surface->pixel_bytes != 1 || surface->width != surface->row_bytes) {
    fprintf(stderr, "Invalid graphic surface, pixel bytes: %zu, width: %zu row_bytes: %zu\n",
            surface->pixel_bytes, surface->width, surface->row_bytes);
    return false;
  }

  if (surface->width > max_width || surface->height > max_height - y) {
    fprintf(stderr,
            "Graphic surface doesn't fit into the screen. width: %zu, height: %zu, max_width: %zu,"
            " max_height: %zu, vertical offset: %d\n",
            surface->width, surface->height, max_width, max_height, y);
    return false;
  }

  return true;
}

MenuDrawFunctions::MenuDrawFunctions(const DrawInterface& wrappee)
    : wrappee_(wrappee) {
}

int MenuDrawFunctions::DrawTextLine(int x, int y, const std::string& line, bool bold) const {
  gr_text(gr_menu_font(), x, y + MenuItemPadding(), line.c_str(), bold);
  return 2 * MenuItemPadding() + MenuCharHeight();
}

int MenuDrawFunctions::DrawTextLines(int x, int y, const std::vector<std::string>& lines) const {
  int offset = 0;
  for (const auto& line : lines) {
    offset += DrawTextLine(x, y + offset, line, false);
  }
  return offset;
}

int MenuDrawFunctions::DrawWrappedTextLines(int x, int y, const std::vector<std::string>& lines) const {
  const int padding = MenuItemPadding() / 2;

  // Keep symmetrical margins based on the given offset (i.e. x).
  size_t text_cols = (gr_fb_width() - x * 2) / MenuCharWidth();
  int offset = 0;
  for (const auto& line : lines) {
    size_t next_start = 0;
    while (next_start < line.size()) {
      std::string sub = line.substr(next_start, text_cols + 1);
      if (sub.size() <= text_cols) {
        next_start += sub.size();
      } else {
        // Line too long and must be wrapped to text_cols columns.
        size_t last_space = sub.find_last_of(" \t\n");
        if (last_space == std::string::npos) {
          // No space found, just draw as much as we can.
          sub.resize(text_cols);
          next_start += text_cols;
        } else {
          sub.resize(last_space);
          next_start += last_space + 1;
        }
      }
      offset += DrawTextLine(x, y + offset, sub, false) - (2 * MenuItemPadding() - padding);
    }
  }
  if (!lines.empty()) {
    offset += 2 * MenuItemPadding() - padding;
  }
  return offset;
}

constexpr int kDefaultMarginHeight = 0;
constexpr int kDefaultMarginWidth = 0;
constexpr int kDefaultAnimationFps = 30;

ScreenRecoveryUI::ScreenRecoveryUI()
    : margin_width_(
          android::base::GetIntProperty("ro.recovery.ui.margin_width", kDefaultMarginWidth)),
      margin_height_(
          android::base::GetIntProperty("ro.recovery.ui.margin_height", kDefaultMarginHeight)),
      animation_fps_(
          android::base::GetIntProperty("ro.recovery.ui.animation_fps", kDefaultAnimationFps)),
      density_(static_cast<float>(android::base::GetIntProperty("ro.sf.lcd_density", 160)) / 160.f),
      blank_unblank_on_init_(
          android::base::GetBoolProperty("ro.recovery.ui.blank_unblank_on_init", false)),
      current_icon_(NONE),
      current_frame_(0),
      intro_done_(false),
      progressBarType(EMPTY),
      progressScopeStart(0),
      progressScopeSize(0),
      progress(0),
      progressScopeTime(0),
      progressScopeDuration(0),
      pagesIdentical(false),
      text_cols_(0),
      text_rows_(0),
      text_(nullptr),
      text_col_(0),
      text_row_(0),
      show_text(false),
      show_text_ever(false),
      file_viewer_text_(nullptr),
      stage(-1),
      max_stage(-1),
      locale_(""),
      rtl_locale_(false),
      batt_capacity_(0),
      charging_(false),
      is_graphics_available(false) {}

ScreenRecoveryUI::~ScreenRecoveryUI() {
  batt_monitor_thread_stopped_ = true;
  if (batt_monitor_thread_.joinable()) {
    batt_monitor_thread_.join();
  }

  progress_thread_stopped_ = true;
  if (progress_thread_.joinable()) {
    progress_thread_.join();
  }
  // No-op if gr_init() (via Init()) was not called or had failed.
  gr_exit();
}

const GRSurface* ScreenRecoveryUI::GetCurrentFrame() const {
  if (current_icon_ == INSTALLING_UPDATE || current_icon_ == ERASING) {
    return intro_done_ ? loop_frames_[current_frame_].get() : intro_frames_[current_frame_].get();
  }
  return error_icon_.get();
}

const GRSurface* ScreenRecoveryUI::GetCurrentText() const {
  switch (current_icon_) {
    case ERASING:
      return erasing_text_.get();
    case ERROR:
      return error_text_.get();
    case INSTALLING_UPDATE:
      return installing_text_.get();
    case NO_COMMAND:
      return no_command_text_.get();
    case NONE:
      abort();
  }
}

int ScreenRecoveryUI::PixelsFromDp(int dp) const {
  return dp * density_;
}

// Here's the intended layout:

//          | portrait    large        landscape      large
// ---------+-------------------------------------------------
//      gap |
// icon     |                   (200dp)
//      gap |    68dp      68dp             56dp      112dp
// text     |                    (14sp)
//      gap |    32dp      32dp             26dp       52dp
// progress |                     (2dp)
//      gap |

// Note that "baseline" is actually the *top* of each icon (because that's how our drawing routines
// work), so that's the more useful measurement for calling code. We use even top and bottom gaps.

enum Layout { PORTRAIT = 0, PORTRAIT_LARGE = 1, LANDSCAPE = 2, LANDSCAPE_LARGE = 3, LAYOUT_MAX };
enum Dimension { TEXT = 0, ICON = 1, DIMENSION_MAX };
static constexpr int kLayouts[LAYOUT_MAX][DIMENSION_MAX] = {
  { 32, 68 },   // PORTRAIT
  { 32, 68 },   // PORTRAIT_LARGE
  { 26, 56 },   // LANDSCAPE
  { 52, 112 },  // LANDSCAPE_LARGE
};

int ScreenRecoveryUI::GetAnimationBaseline() const {
  return GetTextBaseline() - PixelsFromDp(kLayouts[layout_][ICON]) -
         gr_get_height(loop_frames_[0].get());
}

int ScreenRecoveryUI::GetTextBaseline() const {
  return GetProgressBaseline() - PixelsFromDp(kLayouts[layout_][TEXT]) -
         gr_get_height(installing_text_.get());
}

int ScreenRecoveryUI::GetProgressBaseline() const {
  int elements_sum = gr_get_height(loop_frames_[0].get()) + PixelsFromDp(kLayouts[layout_][ICON]) +
                     gr_get_height(installing_text_.get()) + PixelsFromDp(kLayouts[layout_][TEXT]) +
                     gr_get_height(progress_bar_fill_.get());
  int bottom_gap = (ScreenHeight() - elements_sum) / 2;
  return ScreenHeight() - bottom_gap - gr_get_height(progress_bar_fill_.get());
}

// Clear the screen and draw the currently selected background icon (if any).
// Should only be called with updateMutex locked.
void ScreenRecoveryUI::draw_background_locked() {
  pagesIdentical = false;
  gr_color(0, 0, 0, 255);
  gr_clear();
  if (current_icon_ != NONE) {
    if (max_stage != -1) {
      int stage_height = gr_get_height(stage_marker_empty_.get());
      int stage_width = gr_get_width(stage_marker_empty_.get());
      int x = (ScreenWidth() - max_stage * gr_get_width(stage_marker_empty_.get())) / 2;
      int y = ScreenHeight() - stage_height - margin_height_;
      for (int i = 0; i < max_stage; ++i) {
        const auto& stage_surface = (i < stage) ? stage_marker_fill_ : stage_marker_empty_;
        DrawSurface(stage_surface.get(), 0, 0, stage_width, stage_height, x, y);
        x += stage_width;
      }
    }

    const auto& text_surface = GetCurrentText();
    int text_x = (ScreenWidth() - gr_get_width(text_surface)) / 2;
    int text_y = GetTextBaseline();
    M3eSetColor(recovery_m3e::theme::text);
    DrawTextIcon(text_x, text_y, text_surface);
  }
}

// Draws the animation and progress bar (if any) on the screen. Does not flip pages. Should only be
// called with updateMutex locked.
void ScreenRecoveryUI::draw_foreground_locked() {
  if (current_icon_ != NONE) {
    const auto& frame = GetCurrentFrame();
    int frame_width = gr_get_width(frame);
    int frame_height = gr_get_height(frame);
    int frame_x = (ScreenWidth() - frame_width) / 2;
    int frame_y = GetAnimationBaseline();
    if (frame_x >= 0 && frame_y >= 0 && (frame_x + frame_width) < ScreenWidth() &&
        (frame_y + frame_height) < ScreenHeight())
      DrawSurface(frame, 0, 0, frame_width, frame_height, frame_x, frame_y);
  }

  if (progressBarType != EMPTY) {
    int width = gr_get_width(progress_bar_empty_.get());
    int height = gr_get_height(progress_bar_empty_.get());

    int progress_x = (ScreenWidth() - width) / 2;
    int progress_y = GetProgressBaseline();

    // Erase behind the progress bar (in case this was a progress-only update)
    gr_color(0, 0, 0, 255);
    DrawFill(progress_x, progress_y, width, height);

    if (progressBarType == DETERMINATE) {
      float p = progressScopeStart + progress * progressScopeSize;
      int pos = static_cast<int>(p * width);

      if (rtl_locale_) {
        // Fill the progress bar from right to left.
        if (pos > 0) {
          DrawSurface(progress_bar_fill_.get(), width - pos, 0, pos, height,
                      progress_x + width - pos, progress_y);
        }
        if (pos < width - 1) {
          DrawSurface(progress_bar_empty_.get(), 0, 0, width - pos, height, progress_x, progress_y);
        }
      } else {
        // Fill the progress bar from left to right.
        if (pos > 0) {
          DrawSurface(progress_bar_fill_.get(), 0, 0, pos, height, progress_x, progress_y);
        }
        if (pos < width - 1) {
          DrawSurface(progress_bar_empty_.get(), pos, 0, width - pos, height, progress_x + pos,
                      progress_y);
        }
      }
    }
  }
}

// All Recovery pages share the SVG palette, including fastbootd.
void ScreenRecoveryUI::SetColor(UIElement e) const {
  const auto p = recovery_m3e::Palette::ForMode(fastbootd_logo_enabled_);
  switch (e) {
    case UIElement::BATTERY_LOW: M3eSetColor(p.error); break;
    case UIElement::HEADER: M3eSetColor(p.text); break;
    case UIElement::INFO: M3eSetColor(p.text); break;
    case UIElement::SCROLLBAR: M3eSetColor(p.secondary); break;
    case UIElement::MENU: M3eSetColor(p.text); break;
    case UIElement::MENU_BG: M3eSetColor(p.card); break;
    case UIElement::MENU_SEL_BG: M3eSetColor(p.selected); break;
    case UIElement::MENU_SEL_BG_ACTIVE: M3eSetColor(p.pressed); break;
    case UIElement::MENU_SEL_FG: M3eSetColor(p.on_primary); break;
    case UIElement::LOG: M3eSetColor(p.secondary); break;
    case UIElement::TEXT_FILL: gr_color(p.background.r, p.background.g, p.background.b, 230); break;
    default: M3eSetColor(p.text); break;
  }
}

void ScreenRecoveryUI::SelectAndShowBackgroundText(const std::vector<std::string>& locales_entries,
                                                   size_t sel) {
  SetLocale(locales_entries[sel]);
  std::vector<std::string> text_name = { "erasing_text", "error_text", "installing_text",
                                         "installing_security_text", "no_command_text" };
  std::unordered_map<std::string, std::unique_ptr<GRSurface>> surfaces;
  for (const auto& name : text_name) {
    auto text_image = LoadLocalizedBitmap(name);
    if (!text_image) {
      Print("Failed to load %s\n", name.c_str());
      return;
    }
    surfaces.emplace(name, std::move(text_image));
  }

  std::lock_guard<std::mutex> lg(updateMutex);
  gr_color(0, 0, 0, 255);
  gr_clear();

  int text_y = margin_height_;
  int text_x = margin_width_;
  int line_spacing = gr_sys_font()->char_height;  // Put some extra space between images.
  // Write the header and descriptive texts.
  SetColor(UIElement::INFO);
  std::string header = "Show background text image";
  text_y += DrawTextLine(text_x, text_y, header, true);
  std::string locale_selection = android::base::StringPrintf(
      "Current locale: %s, %zu/%zu", locales_entries[sel].c_str(), sel + 1, locales_entries.size());
  // clang-format off
  std::vector<std::string> instruction = {
    locale_selection,
    "Use volume up/down to switch locales and power to exit."
  };
  // clang-format on
  text_y += DrawWrappedTextLines(text_x, text_y, instruction);

  // Iterate through the text images and display them in order for the current locale.
  for (const auto& p : surfaces) {
    text_y += line_spacing;
    SetColor(UIElement::LOG);
    text_y += DrawTextLine(text_x, text_y, p.first, false);
    M3eSetColor(recovery_m3e::theme::text);
    gr_texticon(text_x, text_y, p.second.get());
    text_y += gr_get_height(p.second.get());
  }
  // Update the whole screen.
  gr_flip();
}

void ScreenRecoveryUI::DrawPatternPageLocked() {
  using namespace recovery_m3e;
  Metrics m(ScreenWidth());
  M3eCanvas canvas;
  auto palette = Palette::ForMode(false);
  int top = std::max(margin_height_, Dp(m.width, 24));
  int y = DrawHeader(canvas, m, top, true, pattern_focus_ == -1, false,
                     0, 0, {}, palette, "Unlock internal storage");
  Label(canvas, m, m.inset, y, m.width - 2 * m.inset,
        "Draw your lock-screen pattern", Font::Body, palette.secondary);
  y += LineHeight(FontPixels(Font::Body, m.width)) + Dp(m.width, 6);
  Label(canvas, m, m.inset, y, m.width - 2 * m.inset,
        "Connect at least 4 dots, then tap Unlock", Font::Small, palette.secondary);
  y += LineHeight(FontPixels(Font::Small, m.width)) + Dp(m.width, 20);
  auto layout = PatternBounds(m, y, ScreenHeight() - std::max(margin_height_, Dp(m.width, 24)),
                              pattern_input_->GridSize());
  DrawPattern(canvas, m, layout, *pattern_input_, pattern_dragging_,
              pattern_finger_.x(), pattern_finger_.y(), pattern_focus_, palette);
}

void ScreenRecoveryUI::DrawPasswordPageLocked() {
  M3eCanvas canvas;
  recovery_m3e::password::Draw(canvas,ScreenWidth(),ScreenHeight(),password_input_->NumericOnly(),
      password_input_->Size(),password_symbols_,password_shift_,password_focus_);
}

bool ScreenRecoveryUI::ReadPassword(recovery_ui::PasswordInput& input) {
  using namespace recovery_m3e;
  input.Clear();
  if (IsKeyInterrupted()) return false;
  bool previous_text;
  {
    std::lock_guard<std::mutex> lock(updateMutex);
    previous_text=show_text;show_text=true;
    password_input_=&input;password_symbols_=password_shift_=false;password_focus_=-2;
    gesture_input_=true;FlushKeys();update_screen_locked();
  }
  bool accepted=false,done=false;int pressed=-2;Point origin;
  while (!done) {
    auto event=WaitInputEvent();
    std::lock_guard<std::mutex> lock(updateMutex);
    if (event.type()==EventType::EXTRA) {
      if (event.key()==static_cast<int>(KeyError::INTERRUPTED) ||
          event.key()==static_cast<int>(KeyError::TIMED_OUT)) done=true;
      continue;
    }
    auto layout=password::Keyboard(ScreenWidth(),ScreenHeight(),input.NumericOnly(),password_symbols_,password_shift_);
    auto invoke=[&](int index) {
      if (index==-1) {done=true;return;}
      if (index<0 || index>=static_cast<int>(layout.keys.size())) return;
      const auto& key=layout.keys[index];
      switch (key.action) {
        case password::Action::Character: input.Append(key.character);break;
        case password::Action::Symbols: password_symbols_=!password_symbols_;password_shift_=false;break;
        case password::Action::Shift: password_shift_=!password_shift_;break;
        case password::Action::Space: if(!input.NumericOnly())input.Append(' ');break;
        case password::Action::Delete: input.EraseLast();break;
        case password::Action::Clear: input.Clear();break;
        case password::Action::Cancel: done=true;break;
        case password::Action::Unlock: if(input.Size())accepted=done=true;break;
      }
      if(key.action==password::Action::Symbols || key.action==password::Action::Shift) {
        auto changed=password::Keyboard(ScreenWidth(),ScreenHeight(),input.NumericOnly(),password_symbols_,password_shift_);
        for(size_t i=0;i<changed.keys.size();++i)if(changed.keys[i].action==key.action)password_focus_=i;
      }
    };
    if (event.type()==EventType::KEY) {
      int key=event.key();
      if (key==KEY_BACK || key==KEY_ESC) done=true;
      else if (key==KEY_BACKSPACE || key==KEY_DELETE) input.EraseLast();
      else if (key==KEY_UP || key==KEY_VOLUMEUP || key==KEY_LEFT)
        password_focus_=password_focus_<=-1?static_cast<int>(layout.keys.size())-1:password_focus_-1;
      else if (key==KEY_DOWN || key==KEY_VOLUMEDOWN || key==KEY_RIGHT)
        password_focus_=password_focus_+1>=static_cast<int>(layout.keys.size())?-1:password_focus_+1;
      else if (key==KEY_POWER) {
        if(password_focus_==-2)password_focus_=0;else invoke(password_focus_);
      } else if (key==KEY_ENTER) {
        if(password_focus_>=0)invoke(password_focus_);else if(input.Size())accepted=done=true;
      } else if (key==KEY_SPACE && !input.NumericOnly())input.Append(' ');
      else if (key>=KEY_1 && key<=KEY_0)input.Append(key==KEY_0?'0':'1'+key-KEY_1);
    } else if (event.type()==EventType::TOUCH_DOWN || event.type()==EventType::TOUCH_MOVE ||
               event.type()==EventType::TOUCH_UP || event.type()==EventType::TOUCH) {
      auto point=TouchPoint(event.pos());auto back=design::Back(ScreenWidth());
      int action=password::HitKey(layout,point.x(),point.y());
      if(action<0)action=-2;
      if(InRounded(back,back.h/2,point.x(),point.y()))action=-1;
      if(event.type()==EventType::TOUCH)invoke(action);
      else if(event.type()==EventType::TOUCH_DOWN) {
        pressed=action;origin=point;password_focus_=action;
      } else {
        int64_t dx=point.x()-origin.x(),dy=point.y()-origin.y();int slop=Dp(ScreenWidth(),16);
        if(dx*dx+dy*dy>int64_t(slop)*slop)pressed=-2;
        if(event.type()==EventType::TOUCH_UP) {
          if(pressed!=-2 && action==pressed)invoke(action);
          pressed=-2;
        }
      }
    }
    if (!done && event.type()!=EventType::TOUCH_MOVE)update_screen_locked();
  }
  {
    std::lock_guard<std::mutex> lock(updateMutex);
    gesture_input_=false;discard_touch_until_press_=true;FlushKeys();
    password_input_=nullptr;password_symbols_=password_shift_=false;password_focus_=-2;
    if(!accepted)input.Clear();
    show_text=previous_text;
    if(!menu_)menu_=std::move(transition_menu_);
    menu_transition_=false;update_screen_locked();update_screen_locked();
    transition_menu_=std::move(menu_);menu_transition_=true;
  }
  return accepted;
}

bool ScreenRecoveryUI::ReadPattern(recovery_ui::PatternInput& input) {
  using namespace recovery_m3e;
  if (IsKeyInterrupted() || input.GridSize() < 3 || input.GridSize() > 6) {
    input.Clear(); return false;
  }
  {
    std::lock_guard<std::mutex> lock(updateMutex);
    input.Clear();
    pattern_input_ = &input;
    pattern_dragging_ = false;
    pattern_focus_ = -2;
    gesture_input_ = true;
    FlushKeys();
    update_screen_locked();
  }
  int pressed_action = -2;
  Point pressed_point;
  bool accepted = false, done = false;
  auto last_draw = std::chrono::steady_clock::now();
  while (!done) {
    auto event = WaitInputEvent();
    std::lock_guard<std::mutex> lock(updateMutex);
    if (event.type() == EventType::EXTRA) {
      if (event.key() == static_cast<int>(KeyError::INTERRUPTED)) done = true;
      continue;
    }
    Metrics m(ScreenWidth());
    int top = std::max(margin_height_, Dp(m.width, 24));
    int y = HeaderBottom(m, top, false) + LineHeight(FontPixels(Font::Body, m.width)) +
            LineHeight(FontPixels(Font::Small, m.width)) + Dp(m.width, 26);
    auto layout = PatternBounds(m, y, ScreenHeight() - std::max(margin_height_, Dp(m.width, 24)),
                                input.GridSize());
    auto invoke = [&](int action) {
      if (action == -1 || action == layout.CancelAction()) done = true;
      else if (action == layout.ClearAction()) { input.Clear(); pattern_dragging_ = false; }
      else if (action == layout.UnlockAction() && !pattern_dragging_ && input.Size() >= 4) {
        accepted = done = true;
      } else if (action >= 0 && action < layout.CellCount()) input.Select(action);
    };
    if (event.type() == EventType::KEY) {
      if (event.key() == KEY_BACK || event.key() == KEY_ESC) done = true;
      else if (event.key() == KEY_BACKSPACE || event.key() == KEY_DELETE) invoke(layout.ClearAction());
      else if (!pattern_dragging_ && (event.key() == KEY_UP || event.key() == KEY_VOLUMEUP)) {
        pattern_focus_ = pattern_focus_ <= -1 ? layout.CancelAction() : pattern_focus_ - 1;
      } else if (!pattern_dragging_ && (event.key() == KEY_DOWN || event.key() == KEY_VOLUMEDOWN)) {
        pattern_focus_ = pattern_focus_ < -1 || pattern_focus_ == layout.CancelAction() ?
                         -1 : pattern_focus_ + 1;
      } else if (!pattern_dragging_ && (event.key() == KEY_POWER || event.key() == KEY_ENTER)) {
        if (pattern_focus_ == -2) pattern_focus_ = 0;
        else invoke(pattern_focus_);
      }
    } else if (event.type() == EventType::TOUCH_DOWN || event.type() == EventType::TOUCH_MOVE ||
               event.type() == EventType::TOUCH_UP) {
      auto point = TouchPoint(event.pos());
      auto back = BackBounds(m, top);
      int action = back.Contains(point.x(), point.y()) ? -1 : layout.HitAction(point.x(), point.y());
      if (event.type() == EventType::TOUCH_DOWN) {
        pressed_action = action;
        pressed_point = point;
        pattern_focus_ = -2;
        int cell = layout.HitDot(point.x(), point.y());
        pattern_dragging_ = cell >= 0;
        if (pattern_dragging_) { input.Clear(); input.Select(cell); }
        pattern_finger_ = point;
      } else {
        if (pattern_dragging_) {
          TracePattern(input, layout, pattern_finger_.x(), pattern_finger_.y(), point.x(), point.y());
          pattern_finger_ = point;
          if (event.type() == EventType::TOUCH_UP) pattern_dragging_ = false;
        } else {
          int64_t dx = point.x() - pressed_point.x(), dy = point.y() - pressed_point.y();
          int slop = Dp(m.width, 16);
          if (dx * dx + dy * dy > int64_t(slop) * slop) pressed_action = -2;
          if (event.type() == EventType::TOUCH_UP && pressed_action != -2 && action == pressed_action)
            invoke(action);
        }
        if (event.type() == EventType::TOUCH_UP) pressed_action = -2;
      }
    }
    // Consume every motion sample, but bound expensive text/full-page redraws.
    auto time = std::chrono::steady_clock::now();
    if (!done && (event.type() != EventType::TOUCH_MOVE ||
                  time - last_draw >= std::chrono::milliseconds(33))) {
      update_screen_locked();
      last_draw = time;
    }
  }
  {
    std::lock_guard<std::mutex> lock(updateMutex);
    gesture_input_ = false;
    discard_touch_until_press_ = true;
    FlushKeys();
    pattern_input_ = nullptr;
    pattern_dragging_ = false;
    pattern_finger_ = {};
    pattern_focus_ = -2;
    if (!accepted) input.Clear();
    // Replace sensitive pattern pixels with the invoking menu, rather than
    // flashing a logo while the caller returns to its parent.
    menu_ = std::move(transition_menu_);
    menu_transition_ = false;
    update_screen_locked();
    update_screen_locked();
    transition_menu_ = std::move(menu_);
    menu_transition_ = true;
  }
  return accepted;
}

void ScreenRecoveryUI::CheckBackgroundTextImages() {
  // Load a list of locales embedded in one of the resource files.
  std::vector<std::string> locales_entries = get_locales_in_png("installing_text");
  if (locales_entries.empty()) {
    Print("Failed to load locales from the resource files\n");
    return;
  }
  std::string saved_locale = locale_;
  size_t selected = 0;
  SelectAndShowBackgroundText(locales_entries, selected);

  FlushKeys();
  while (true) {
    InputEvent evt = WaitInputEvent();
    if (evt.type() == EventType::EXTRA) {
      if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) break;
    }
    if (evt.type() == EventType::KEY) {
      if (evt.key() == KEY_POWER || evt.key() == KEY_ENTER) {
        break;
      } else if (evt.key() == KEY_UP || evt.key() == KEY_VOLUMEUP) {
        selected = (selected == 0) ? locales_entries.size() - 1 : selected - 1;
        SelectAndShowBackgroundText(locales_entries, selected);
      } else if (evt.key() == KEY_DOWN || evt.key() == KEY_VOLUMEDOWN) {
        selected = (selected == locales_entries.size() - 1) ? 0 : selected + 1;
        SelectAndShowBackgroundText(locales_entries, selected);
      }
    }
  }

  SetLocale(saved_locale);
}

int ScreenRecoveryUI::ScreenWidth() const {
  return gr_fb_width();
}

int ScreenRecoveryUI::M3eScaleWidth() const {
  return recovery_m3e::ScaleWidth(ScreenWidth());
}

int ScreenRecoveryUI::MenuItemHeight() const {
  return recovery_m3e::Metrics(M3eScaleWidth()).row_height;
}

int ScreenRecoveryUI::MenuItemSpacing() const {
  return recovery_m3e::Metrics(M3eScaleWidth()).gap;
}

int ScreenRecoveryUI::ScreenHeight() const {
  return gr_fb_height();
}

void ScreenRecoveryUI::DrawSurface(const GRSurface* surface, int sx, int sy, int w, int h, int dx,
                                   int dy) const {
  gr_blit(surface, sx, sy, w, h, dx, dy);
}

int ScreenRecoveryUI::DrawHorizontalRule(int y) const {
  gr_fill(0, y + 4, ScreenWidth(), y + 6);
  return 8;
}

void ScreenRecoveryUI::DrawHighlightBar(int x, int y, int width, int height) const {
  if (y + height > ScreenHeight())
    height = ScreenHeight() - y;
  gr_fill(x, y, x + width, y + height);
}

void ScreenRecoveryUI::DrawScrollBar(int y, int height) const {
  recovery_m3e::Metrics m(ScreenWidth(), MenuCharWidth(), MenuCharHeight());
  int x = ScreenWidth() - m.inset / 2;
  int width = std::max(3, m.inset / 8);
  gr_fill(x - width, y, x, y + height);
}
int ScreenRecoveryUI::DrawDashboard(int y, int width, int available, int selected, bool active) const {
  M3eCanvas canvas;
  return recovery_m3e::design::Dashboard(canvas,width,y,available,selected,active);
}
int ScreenRecoveryUI::DrawMenuPrompt(int /*x*/, int y, const std::vector<std::string>& lines) const {
  M3eCanvas canvas;
  return recovery_m3e::DrawPrompt(canvas, recovery_m3e::Metrics(ScreenWidth()), y, lines,
                                   recovery_m3e::Palette::ForMode(fastbootd_logo_enabled_)) - y;
}
void ScreenRecoveryUI::DrawMenuCard(int y, int width, const std::string& label,
                                    bool selected, bool active, bool first, bool last) const {
  M3eCanvas canvas;
  if (IsDesignMenuLocked()) {
    recovery_m3e::design::Card(canvas,width,y,label,selected,active,first,last);
    return;
  }
  recovery_m3e::Metrics m(width, MenuCharWidth(), MenuCharHeight());
  recovery_m3e::DrawCard(canvas, m, y, label, selected, active,
                         recovery_m3e::Palette::ForMode(fastbootd_logo_enabled_), first, last);
}

void ScreenRecoveryUI::DrawFill(int x, int y, int w, int h) const {
  gr_fill(x, y, w, h);
}

void ScreenRecoveryUI::DrawTextIcon(int x, int y, const GRSurface* surface) const {
  gr_texticon(x, y, surface);
}

int ScreenRecoveryUI::DrawTextLine(int x, int y, const std::string& line, bool bold) const {
  gr_text(gr_sys_font(), x, y, line.c_str(), bold);
  return char_height_ + 4;
}

int ScreenRecoveryUI::DrawTextLines(int x, int y, const std::vector<std::string>& lines) const {
  int offset = 0;
  for (const auto& line : lines) {
    offset += DrawTextLine(x, y + offset, line, false);
  }
  return offset;
}

int ScreenRecoveryUI::DrawWrappedTextLines(int x, int y,
                                           const std::vector<std::string>& lines) const {
  // Keep symmetrical margins based on the given offset (i.e. x).
  size_t text_cols = (ScreenWidth() - x * 2) / char_width_;
  int offset = 0;
  for (const auto& line : lines) {
    size_t next_start = 0;
    while (next_start < line.size()) {
      std::string sub = line.substr(next_start, text_cols + 1);
      if (sub.size() <= text_cols) {
        next_start += sub.size();
      } else {
        // Line too long and must be wrapped to text_cols columns.
        size_t last_space = sub.find_last_of(" \t\n");
        if (last_space == std::string::npos) {
          // No space found, just draw as much as we can.
          sub.resize(text_cols);
          next_start += text_cols;
        } else {
          sub.resize(last_space);
          next_start += last_space + 1;
        }
      }
      offset += DrawTextLine(x, y + offset, sub, false);
    }
  }
  return offset;
}

void ScreenRecoveryUI::SetTitle(const std::vector<std::string>& lines) {
  title_lines_ = lines;
}

std::vector<std::string> ScreenRecoveryUI::GetMenuHelpMessage() const {
  // clang-format off
  static std::vector<std::string> REGULAR_HELP{
    "Use the volume up/down keys to navigate.",
    "Use the power key to select.",
  };
  static std::vector<std::string> LONG_PRESS_HELP{
    "Any button cycles highlight.",
    "Long-press activates.",
  };
  static const std::vector<std::string> NO_HELP = {};
  // clang-format on
  return HasTouchScreen() ? NO_HELP : HasThreeButtons() ? REGULAR_HELP : LONG_PRESS_HELP;
}

// Redraws everything on the screen. Does not flip pages. Should only be called with updateMutex
// locked.
bool ScreenRecoveryUI::IsDesignMenuLocked() const {
  if (!menu_ || pattern_input_ || password_input_) return false;
  auto page = recovery_m3e::design::MenuPage(menu_->DashboardCandidate(),menu_->PageTitle());
  return fastbootd_logo_enabled_ ? page == recovery_m3e::design::Page::Fastboot :
      page != recovery_m3e::design::Page::None && page != recovery_m3e::design::Page::Fastboot;
}
bool ScreenRecoveryUI::IsDesignAdbLocked() const {
  return IsInstallPageLocked() && recovery_m3e::design::AdbPage(m3e_adb_sideload_,m3e_install_stage_);
}
void ScreenRecoveryUI::draw_screen_locked() {
  if (password_input_) { DrawPasswordPageLocked(); return; }
  if (terminal_visible_) { DrawTerminalLocked(); return; }
  if (pattern_input_) {
    M3eSetColor(recovery_m3e::Palette::ForMode(false).background);
    gr_clear();
    DrawPatternPageLocked();
    draw_battery_capacity_locked();
    return;
  }
  if (IsInstallPageLocked()) {
    M3eSetColor(recovery_m3e::theme::background);
    gr_clear();
    DrawInstallPageLocked();
    draw_battery_capacity_locked();
    return;
  }
  if (!show_text) {
    draw_background_locked();
    draw_foreground_locked();
    return;
  }

  M3eSetColor(IsDesignMenuLocked() ? recovery_m3e::design::background :
      recovery_m3e::Palette::ForMode(fastbootd_logo_enabled_).background);
  gr_clear();

  draw_menu_and_text_buffer_locked(GetMenuHelpMessage());
  draw_battery_capacity_locked();
}

// Draws the menu and text buffer on the screen. Should only be called with updateMutex locked.
void ScreenRecoveryUI::draw_menu_and_text_buffer_locked(const std::vector<std::string>& /*help_message*/) {
  if (menu_) {
    M3eCanvas canvas;
    if (IsDesignMenuLocked()) {
      using namespace recovery_m3e;
      auto page=design::MenuPage(menu_->DashboardCandidate(),menu_->PageTitle());
      design::Header(canvas,ScreenWidth(),ScreenHeight(),page,menu_->selection()==-1);
      if (page==design::Page::Fastboot) design::FastbootStatus(canvas,ScreenWidth(),ScreenHeight());
      menu_start_y_=design::MenuTop(ScreenWidth(),ScreenHeight(),page);
      m3e_menu_bottom_=design::FooterTop(ScreenWidth(),ScreenHeight())-Dp(ScreenWidth(),10);
      menu_->SetViewport(ScreenWidth(),std::max(0,m3e_menu_bottom_-menu_start_y_));
      menu_->DrawItems(Dp(ScreenWidth(),18),menu_start_y_,ScreenWidth(),IsLongPress());
      design::Footer(canvas,ScreenWidth(),ScreenHeight(),title_lines_);
      return;
    }
    recovery_m3e::Metrics m(ScreenWidth());
    auto palette = recovery_m3e::Palette::ForMode(fastbootd_logo_enabled_);
    int top = std::max(margin_height_, recovery_m3e::Dp(ScreenWidth(), 24));
    int bottom = recovery_m3e::PageContentBottom(ScreenWidth(),ScreenHeight());
    bool dashboard = menu_->DashboardCandidate() &&
        bottom - recovery_m3e::HeaderBottom(m, top, true) >= recovery_m3e::DashboardMinimum(m);
    int y = recovery_m3e::DrawHeader(canvas, m, top, !menu_->IsMain(), menu_->selection() == -1,
        fastbootd_logo_enabled_, char_width_, char_height_, title_lines_, palette,
        menu_->PageTitle(), dashboard);
    y += menu_->DrawHeader(m.inset, y);
    menu_start_y_ = y;
    m3e_menu_bottom_ = bottom;
    menu_->SetViewport(ScreenWidth(), std::max(0, m3e_menu_bottom_ - menu_start_y_));
    menu_->DrawItems(m.inset, y, ScreenWidth(), IsLongPress());
    recovery_m3e::DrawPageFooter(canvas,ScreenWidth(),ScreenHeight(),title_lines_);
    return;
  }
  // No menu is active while services start or an action returns to its parent.
  // Only the explicitly opened file viewer should render a full text page.
  if (!file_viewer_text_ || text_ != file_viewer_text_) {
    DrawStatusPageLocked();
    return;
  }
  // Preserve the complete output when the user explicitly opens a log file.
  SetColor(UIElement::LOG);
  int row = text_row_;
  size_t count = 0;
  for (int ty = ScreenHeight() - margin_height_ - char_height_; ty >= margin_height_ && count < text_rows_;
       ty -= char_height_, ++count) {
    DrawTextLine(margin_width_, ty, text_[row], false);
    --row;
    if (row < 0) row = text_rows_ - 1;
  }
}

void ScreenRecoveryUI::DrawStatusPageLocked() {
  // The display resource is composited onto black for minui RGB compatibility.
  // Keep transitions free of headers, status messages, controls and overlays.
  gr_color(0, 0, 0, 255);
  gr_clear();
  if (!m3e_logo_) return;
  int logo_width = gr_get_width(m3e_logo_.get());
  int logo_height = gr_get_height(m3e_logo_.get());
  int width = std::min(ScreenWidth(), logo_width);
  int height = std::min(ScreenHeight(), logo_height);
  DrawSurface(m3e_logo_.get(), (logo_width - width) / 2, (logo_height - height) / 2,
      width, height, (ScreenWidth() - width) / 2, (ScreenHeight() - height) / 2);
}

void ScreenRecoveryUI::draw_battery_capacity_locked() {
  if (is_battery_less || (!menu_ && !IsInstallPageLocked() && !pattern_input_)) return;
  M3eCanvas canvas;
  if (IsDesignMenuLocked() || IsDesignAdbLocked()) {
    recovery_m3e::design::Battery(canvas,ScreenWidth(),batt_capacity_,charging_);
    return;
  }
  recovery_m3e::Metrics m(ScreenWidth());
  int top = std::max(margin_height_, recovery_m3e::Dp(ScreenWidth(), 24));
  recovery_m3e::DrawBattery(canvas, m, top, batt_capacity_, charging_,
                             recovery_m3e::Palette::ForMode(fastbootd_logo_enabled_));
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with updateMutex locked.
bool ScreenRecoveryUI::ShouldHoldMenuFrameLocked() const {
  return menu_transition_ && !menu_ && show_text && !terminal_visible_ &&
      m3e_install_stage_ == InstallStage::NONE && !pattern_input_ && !password_input_ &&
      !(file_viewer_text_ && text_ == file_viewer_text_);
}

void ScreenRecoveryUI::update_screen_locked() {
  if (ShouldHoldMenuFrameLocked()) return;
  if (password_input_) DrawPasswordPageLocked();
  else draw_screen_locked();
  gr_flip();
}

// Updates only the progress bar, if possible, otherwise redraws the screen.
// Should only be called with updateMutex locked.
void ScreenRecoveryUI::update_progress_locked() {
  if (ShouldHoldMenuFrameLocked()) return;
  if (password_input_) DrawPasswordPageLocked();
  else if (IsInstallPageLocked() || show_text || !pagesIdentical) {
    draw_screen_locked();  // Must redraw the whole screen
    pagesIdentical = true;
  } else {
    draw_foreground_locked();  // Draw only the progress bar and overlays
  }
  gr_flip();
}

void ScreenRecoveryUI::BattMonitorThreadLoop() {
  using aidl::android::hardware::health::BatteryStatus;
  using android::hardware::health::InitHealthdConfig;

  auto config = std::make_unique<healthd_config>();
  InitHealthdConfig(config.get());

  auto batt_monitor = std::make_unique<android::BatteryMonitor>();
  batt_monitor->init(config.get());

  bool is_first_call = true;

  while (!batt_monitor_thread_stopped_) {
    bool redraw = false;
    {
      std::lock_guard<std::mutex> lg(updateMutex);

      auto charge_status = static_cast<BatteryStatus>(batt_monitor->getChargeStatus());
      // Treat unknown status as on charger.
      bool charging = (charge_status != BatteryStatus::DISCHARGING &&
                       charge_status != BatteryStatus::NOT_CHARGING &&
                       charge_status != BatteryStatus::FULL);
      if (charging_ != charging) {
        charging_ = charging;
        redraw = true;
      }

      android::BatteryProperty prop;
      android::base::Timer t;
      android::status_t status;
      while (t.duration() < 5s) {
        status = batt_monitor->getProperty(android::BATTERY_PROP_CAPACITY, &prop);
        if (status == android::OK || !is_first_call) {
          break;
        }

        LOG(WARNING) << "Trying again for reinitializing battery info";
        if (redraw) update_screen_locked();
        batt_monitor->init(config.get());
        std::this_thread::sleep_for(100ms);
      }
      is_first_call = false;

      // If we can't read battery percentage, it may be a device without battery. In this
      // situation, use 100 as a fake battery percentage.
      if (status != android::OK) {
        prop.valueInt64 = 100;
      }

      int32_t batt_capacity = static_cast<int32_t>(prop.valueInt64);
      if (batt_capacity_ != batt_capacity) {
        batt_capacity_ = batt_capacity;
        redraw = true;
      }

      if (redraw) update_screen_locked();
    }
    std::this_thread::sleep_for(5s);
  }
}

void ScreenRecoveryUI::ProgressThreadLoop() {
  double interval = 1.0 / animation_fps_;
  while (!progress_thread_stopped_) {
    double start = now();
    bool redraw = false;
    {
      std::lock_guard<std::mutex> lg(updateMutex);

      // update the installation animation, if active
      // skip this if we have a text overlay (too expensive to update)
      if ((current_icon_ == INSTALLING_UPDATE || current_icon_ == ERASING) && !show_text) {
        if (!intro_done_) {
          if (current_frame_ == intro_frames_.size() - 1) {
            intro_done_ = true;
            current_frame_ = 0;
          } else {
            ++current_frame_;
          }
        } else {
          current_frame_ = (current_frame_ + 1) % loop_frames_.size();
        }

        redraw = true;
      }

      // move the progress bar forward on timed intervals, if configured
      int duration = progressScopeDuration;
      if (progressBarType == DETERMINATE && duration > 0) {
        double elapsed = now() - progressScopeTime;
        float p = 1.0 * elapsed / duration;
        if (p > 1.0) p = 1.0;
        if (p > progress) {
          progress = p;
          redraw = true;
        }
      }

      if (redraw) update_progress_locked();
    }

    double end = now();
    // minimum of 20ms delay between frames
    double delay = interval - (end - start);
    if (delay < 0.02) delay = 0.02;
    usleep(static_cast<useconds_t>(delay * 1000000));
  }
}

std::unique_ptr<GRSurface> ScreenRecoveryUI::LoadBitmap(const std::string& filename) {
  GRSurface* surface;
  if (auto result = res_create_display_surface(filename.c_str(), &surface); result < 0) {
    LOG(ERROR) << "Failed to load bitmap " << filename << " (error " << result << ")";
    return nullptr;
  }
  return std::unique_ptr<GRSurface>(surface);
}

std::unique_ptr<GRSurface> ScreenRecoveryUI::LoadLocalizedBitmap(const std::string& filename) {
  GRSurface* surface;
  auto result = res_create_localized_alpha_surface(filename.c_str(), locale_.c_str(), &surface);
  if (result == 0) {
    return std::unique_ptr<GRSurface>(surface);
  }

  result = res_create_localized_alpha_surface(filename.c_str(), DEFAULT_LOCALE, &surface);
  if (result == 0) {
    return std::unique_ptr<GRSurface>(surface);
  }

  return nullptr;
}

static char** Alloc2d(size_t rows, size_t cols) {
  char** result = new char*[rows];
  for (size_t i = 0; i < rows; ++i) {
    result[i] = new char[cols];
    memset(result[i], 0, cols);
  }
  return result;
}

// Choose the right background string to display during update.
void ScreenRecoveryUI::SetSystemUpdateText(bool security_update) {
  m3e_security_update_ = security_update;
  if (security_update) {
    installing_text_ = LoadLocalizedBitmap("installing_security_text");
  } else {
    installing_text_ = LoadLocalizedBitmap("installing_text");
  }
  Redraw();
}

bool ScreenRecoveryUI::InitTextParams() {
  // gr_init() would return successfully on font initialization failure.
  if (gr_sys_font() == nullptr) {
    return false;
  }
  gr_font_size(gr_sys_font(), &char_width_, &char_height_);
  gr_font_size(gr_menu_font(), &menu_char_width_, &menu_char_height_);
  text_rows_ = (ScreenHeight() - margin_height_ * 2) / char_height_;
  text_cols_ = (ScreenWidth() - margin_width_ * 2) / char_width_;
  return true;
}

bool ScreenRecoveryUI::LoadWipeDataMenuText() {
  // Ignores the errors since the member variables will stay as nullptr.
  cancel_wipe_data_text_ = LoadLocalizedBitmap("cancel_wipe_data_text");
  factory_data_reset_text_ = LoadLocalizedBitmap("factory_data_reset_text");
  try_again_text_ = LoadLocalizedBitmap("try_again_text");
  wipe_data_confirmation_text_ = LoadLocalizedBitmap("wipe_data_confirmation_text");
  wipe_data_menu_header_text_ = LoadLocalizedBitmap("wipe_data_menu_header_text");
  return true;
}

static bool InitGraphics() {
  // Default timeout is 5 seconds, same as init wait for file
  // 10ms increments
  const unsigned timeout =
      android::base::GetIntProperty("ro.recovery.ui.graphics_timeout_ms", 5000) / 10;
  for (auto retry = timeout; retry > 0; --retry) {
    if (gr_init() == 0) {
      if (retry < timeout) {
        // Log message like init wait for file completion log for consistency.
        LOG(WARNING) << "wait for 'graphics' took " << ((timeout - retry) * 10) << "ms";
      }
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  // Log message like init wait for file timeout log for consistency.
  LOG(ERROR) << "timeout wait for 'graphics' took " << (timeout * 10) << "ms";
  return false;
}

bool ScreenRecoveryUI::Init(const std::string& locale) {
  RecoveryUI::Init(locale);

  if (!InitGraphics()) {
    return false;
  }
  is_graphics_available = true;

  if (!InitTextParams()) {
    return false;
  }
  menu_draw_funcs_ = std::make_unique<MenuDrawFunctions>(*this);

  if (blank_unblank_on_init_) {
    gr_fb_blank(true);
    gr_fb_blank(false);
  }

  // Scale M3E dp values by the shorter side so landscape/tablet screens stay proportional.
  recovery_m3e::SetScaleBasis(gr_fb_width(), gr_fb_height());

  // Are we portrait or landscape?
  layout_ = (gr_fb_width() > gr_fb_height()) ? LANDSCAPE : PORTRAIT;
  // Are we the large variant of our base layout?
  if (gr_fb_height() > PixelsFromDp(800)) ++layout_;

  text_ = Alloc2d(text_rows_, text_cols_ + 1);
  file_viewer_text_ = Alloc2d(text_rows_, text_cols_ + 1);

  text_col_ = text_row_ = 0;

  // Set up the locale info.
  SetLocale(locale);
  recovery_m3e::SetLanguage(recovery_m3e::LanguageForLocale(locale));

  error_icon_ = LoadBitmap("icon_error");
  m3e_logo_ = LoadBitmap("uwu_recovery_m3e");

  progress_bar_empty_ = LoadBitmap("progress_empty");
  progress_bar_fill_ = LoadBitmap("progress_fill");
  stage_marker_empty_ = LoadBitmap("stage_empty");
  stage_marker_fill_ = LoadBitmap("stage_fill");

  erasing_text_ = LoadLocalizedBitmap("erasing_text");
  no_command_text_ = LoadLocalizedBitmap("no_command_text");
  error_text_ = LoadLocalizedBitmap("error_text");

  back_icon_ = LoadBitmap("ic_back");
  back_icon_sel_ = LoadBitmap("ic_back_sel");
  lineage_logo_ = LoadBitmap("logo_image");
  if (android::base::GetBoolProperty("ro.boot.dynamic_partitions", false) ||
      android::base::GetBoolProperty("ro.fastbootd.available", true)) {
    fastbootd_logo_ = LoadBitmap("fastbootd");
  }

  // Background text for "installing_update" could be "installing update" or
  // "installing security update". It will be set after Init() according to the commands in BCB.
  installing_text_.reset();

  LoadWipeDataMenuText();

  LoadAnimation();

  is_battery_less = android::base::GetBoolProperty("ro.recovery.batteryless", false);
  if (!is_battery_less)
    // Keep the battery capacity updated.
    batt_monitor_thread_ = std::thread(&ScreenRecoveryUI::BattMonitorThreadLoop, this);

  // Keep the progress bar updated, even when the process is otherwise busy.
  progress_thread_ = std::thread(&ScreenRecoveryUI::ProgressThreadLoop, this);

  // set the callback for hall sensor event
  (void)ev_sync_sw_state([this](auto&& a, auto&& b) { return this->SetSwCallback(a, b);});

  return true;
}

bool ScreenRecoveryUI::SetUiLanguage(const std::string& code) {
  if (!recovery_m3e::SupportedLocale(code)) return false;
  // Load into temporary owners before taking the redraw lock.
  auto load = [&](const char* name) {
    GRSurface* surface = nullptr;
    if (res_create_localized_alpha_surface(name, code.c_str(), &surface) != 0) return std::unique_ptr<GRSurface>();
    return std::unique_ptr<GRSurface>(surface);
  };
  auto erasing = load("erasing_text"), error = load("error_text"), no_command = load("no_command_text");
  auto cancel = load("cancel_wipe_data_text"), reset = load("factory_data_reset_text");
  auto confirm = load("wipe_data_confirmation_text"), wipe = load("wipe_data_menu_header_text");
  auto retry = load("try_again_text");
  auto installing = load(m3e_security_update_ ? "installing_security_text" : "installing_text");
  std::lock_guard<std::mutex> lg(updateMutex);
  SetLocale(code);
  recovery_m3e::SetLanguage(recovery_m3e::LanguageForLocale(code));
  m3e_pending_locale_ = code;
  if (erasing) erasing_text_ = std::move(erasing);
  if (error) error_text_ = std::move(error);
  if (no_command) no_command_text_ = std::move(no_command);
  if (cancel) cancel_wipe_data_text_ = std::move(cancel);
  if (reset) factory_data_reset_text_ = std::move(reset);
  if (confirm) wipe_data_confirmation_text_ = std::move(confirm);
  if (wipe) wipe_data_menu_header_text_ = std::move(wipe);
  if (retry) try_again_text_ = std::move(retry);
  if (installing) installing_text_ = std::move(installing);
  update_screen_locked();
  return true;
}
std::string ScreenRecoveryUI::ConsumeLanguagePreference() {
  std::lock_guard<std::mutex> lg(updateMutex);
  std::string pending = m3e_pending_locale_;
  m3e_pending_locale_.clear();
  return pending;
}
std::string ScreenRecoveryUI::GetLocale() const {
  return locale_;
}

void ScreenRecoveryUI::LoadAnimation() {
  std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(Paths::Get().resource_dir().c_str()),
                                                closedir);
  dirent* de;
  std::vector<std::string> intro_frame_names;
  std::vector<std::string> loop_frame_names;

  while ((de = readdir(dir.get())) != nullptr) {
    int value, num_chars;
    if (sscanf(de->d_name, "intro%d%n.png", &value, &num_chars) == 1) {
      intro_frame_names.emplace_back(de->d_name, num_chars);
    } else if (sscanf(de->d_name, "loop%d%n.png", &value, &num_chars) == 1) {
      loop_frame_names.emplace_back(de->d_name, num_chars);
    }
  }

  size_t intro_frames = intro_frame_names.size();
  size_t loop_frames = loop_frame_names.size();

  // It's okay to not have an intro.
  if (intro_frames == 0) intro_done_ = true;
  // But you must have an animation.
  if (loop_frames == 0) abort();

  std::sort(intro_frame_names.begin(), intro_frame_names.end());
  std::sort(loop_frame_names.begin(), loop_frame_names.end());

  intro_frames_.clear();
  intro_frames_.reserve(intro_frames);
  for (const auto& frame_name : intro_frame_names) {
    intro_frames_.emplace_back(LoadBitmap(frame_name));
  }

  loop_frames_.clear();
  loop_frames_.reserve(loop_frames);
  for (const auto& frame_name : loop_frame_names) {
    loop_frames_.emplace_back(LoadBitmap(frame_name));
  }
}

void ScreenRecoveryUI::SetBackground(Icon icon) {
  std::lock_guard<std::mutex> lg(updateMutex);

  current_icon_ = icon;
  if (icon == INSTALLING_UPDATE || icon == ERASING || icon == ERROR) menu_transition_ = false;
  update_screen_locked();
}

void ScreenRecoveryUI::SetInstallStage(InstallStage stage) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (stage == InstallStage::WAITING || m3e_install_stage_ == InstallStage::NONE) {
    m3e_install_logs_.clear();
  }
  if (stage == InstallStage::WAITING) m3e_adb_sideload_ = true;
  else if (stage != InstallStage::VERIFYING && stage != InstallStage::INSTALLING) m3e_adb_sideload_ = false;
  m3e_install_stage_ = stage;
  if (stage != InstallStage::NONE) menu_transition_ = false;
  update_screen_locked();
}

bool ScreenRecoveryUI::IsInstallPageLocked() const {
  if (m3e_install_stage_ == InstallStage::NONE) return false;
  if (!menu_) return true;
  // Confirmation menus always take priority over the installation dashboard.
  const auto title = menu_->PageTitle();
  return title == "Install result" || title == "Flash result" ||
      (recovery_m3e::design::AdbPage(m3e_adb_sideload_,m3e_install_stage_) && title == "ADB Sideload");
}

void ScreenRecoveryUI::DrawInstallPageLocked() {
  M3eCanvas canvas;
  if (IsDesignAdbLocked()) {
    recovery_m3e::design::Adb(canvas,ScreenWidth(),ScreenHeight(),m3e_install_stage_ == InstallStage::WAITING,
        progressScopeStart + progress * progressScopeSize,
        progressBarType == DETERMINATE && progressScopeSize > 0,m3e_install_logs_,title_lines_,
        menu_ && menu_->selection() == -1);
    if (menu_) {
      // The SVG uses the back control to cancel, rather than a separate Cancel card.
      menu_start_y_ = m3e_menu_bottom_ = ScreenHeight();
      menu_->SetViewport(ScreenWidth(),0);
      menu_->Select(-1);
    }
    return;
  }
  recovery_m3e::Metrics m(ScreenWidth());
  auto palette = recovery_m3e::Palette::ForMode(false);
  int top = std::max(margin_height_, recovery_m3e::Dp(ScreenWidth(), 24));
  int bottom = recovery_m3e::PageContentBottom(ScreenWidth(),ScreenHeight());
  int rows = menu_ ? std::min<size_t>(2, menu_->ItemsCount()) : 0;
  int y = recovery_m3e::DrawInstallHeader(canvas, m, top, bottom, rows,
      menu_ && menu_->selection() == -1, title_lines_, palette,
      recovery_ui::IsFlashStage(m3e_install_stage_) ? "Flash partition image" : "Install update");
  auto layout = recovery_m3e::InstallationLayout(m, y, bottom, rows,
      gr_get_width(m3e_logo_.get()), gr_get_height(m3e_logo_.get()),
      m3e_install_stage_ == InstallStage::VERIFYING ||
      m3e_install_stage_ == InstallStage::INSTALLING);
  if (layout.logo.w > 0) {
    DrawSurface(m3e_logo_.get(), 0, 0, layout.logo.w, layout.logo.h,
        layout.logo.x, layout.logo.y);
  }
  double fraction = progressScopeStart + progress * progressScopeSize;
  recovery_m3e::DrawInstallPanel(canvas, m, layout.panel, m3e_install_stage_, fraction,
      progressBarType == DETERMINATE && progressScopeSize > 0, m3e_security_update_,
      m3e_install_logs_, palette);
  if (menu_) {
    menu_start_y_ = layout.menu_y;
    m3e_menu_bottom_ = bottom;
    menu_->SetViewport(ScreenWidth(), std::max(0, bottom - menu_start_y_));
    menu_->DrawItems(m.inset, menu_start_y_, ScreenWidth(), IsLongPress());
  }
  recovery_m3e::DrawPageFooter(canvas,ScreenWidth(),ScreenHeight(),title_lines_);
}

void ScreenRecoveryUI::SetProgressType(ProgressType type) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (progressBarType != type) {
    progressBarType = type;
  }
  progressScopeStart = 0;
  progressScopeSize = 0;
  progress = 0;
  progressScopeDuration = 0;
  update_progress_locked();
}

void ScreenRecoveryUI::ShowProgress(float portion, float seconds) {
  std::lock_guard<std::mutex> lg(updateMutex);
  progressBarType = DETERMINATE;
  progressScopeStart += progressScopeSize;
  progressScopeSize = portion;
  progressScopeTime = now();
  progressScopeDuration = seconds;
  progress = 0;
  update_progress_locked();
}

void ScreenRecoveryUI::SetProgress(float fraction) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (!std::isfinite(fraction)) return;
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;
  if (progressBarType == DETERMINATE && fraction > progress) {
    // Skip updates that aren't visibly different.
    int width = IsInstallPageLocked() ? ScreenWidth() : gr_get_width(progress_bar_empty_.get());
    float scale = width * progressScopeSize;
    bool redraw = (int)(progress * scale) != (int)(fraction * scale) ||
        (int)((progressScopeStart + progress * progressScopeSize) * 1000) !=
        (int)((progressScopeStart + fraction * progressScopeSize) * 1000);
    // Always retain the reported value, including changes smaller than one pixel.
    progress = fraction;
    if (redraw) {
      update_progress_locked();
    }
  }
}

void ScreenRecoveryUI::SetStage(int current, int max) {
  std::lock_guard<std::mutex> lg(updateMutex);
  stage = current;
  max_stage = max;
}

void ScreenRecoveryUI::PrintV(const char* fmt, bool copy_to_stdout, va_list ap) {
  std::string str;
  android::base::StringAppendV(&str, fmt, ap);

  if (copy_to_stdout) {
    fputs(str.c_str(), stdout);
  }

  std::lock_guard<std::mutex> lg(updateMutex);
  if (m3e_install_stage_ != InstallStage::NONE) {
    for (const auto& line : android::base::Split(str, "\n")) {
      if (!line.empty()) m3e_install_logs_.push_back(line);
    }
    size_t retained = 128;
    if (m3e_install_logs_.size() > retained) {
      m3e_install_logs_.erase(m3e_install_logs_.begin(), m3e_install_logs_.end() - retained);
    }
  }
  if (text_rows_ > 0 && text_cols_ > 0) {
    for (const char* ptr = str.c_str(); *ptr != '\0'; ++ptr) {
      if (*ptr == '\n' || text_col_ >= text_cols_) {
        text_[text_row_][text_col_] = '\0';
        text_col_ = 0;
        text_row_ = (text_row_ + 1) % text_rows_;
      }
      if (*ptr != '\n') text_[text_row_][text_col_++] = *ptr;
    }
    text_[text_row_][text_col_] = '\0';
    update_screen_locked();
  }
}

void ScreenRecoveryUI::Print(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  PrintV(fmt, true, ap);
  va_end(ap);
}

void ScreenRecoveryUI::PrintOnScreenOnly(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  PrintV(fmt, false, ap);
  va_end(ap);
}

void ScreenRecoveryUI::PutChar(char ch) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (ch != '\n') text_[text_row_][text_col_++] = ch;
  if (ch == '\n' || text_col_ >= text_cols_) {
    text_col_ = 0;
    ++text_row_;
  }
}

void ScreenRecoveryUI::ClearText() {
  std::lock_guard<std::mutex> lg(updateMutex);
  text_col_ = 0;
  text_row_ = 0;
  for (size_t i = 0; i < text_rows_; ++i) {
    memset(text_[i], 0, text_cols_ + 1);
  }
}

void ScreenRecoveryUI::ShowFile(FILE* fp) {
  std::vector<off_t> offsets;
  offsets.push_back(ftello(fp));
  ClearText();

  struct stat sb;
  fstat(fileno(fp), &sb);

  bool show_prompt = false;
  while (true) {
    if (show_prompt) {
      PrintOnScreenOnly("--(%d%% of %d bytes)--",
                        static_cast<int>(100 * (double(ftello(fp)) / double(sb.st_size))),
                        static_cast<int>(sb.st_size));
      Redraw();
      while (show_prompt) {
        show_prompt = false;
        InputEvent evt = WaitInputEvent();
        if (evt.type() == EventType::EXTRA) {
          if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) {
            return;
          }
        }
        if (evt.type() != EventType::KEY) {
          show_prompt = true;
          continue;
        }
        if (evt.key() == KEY_POWER || evt.key() == KEY_ENTER || evt.key() == KEY_BACKSPACE ||
            evt.key() == KEY_BACK || evt.key() == KEY_HOMEPAGE ||
            evt.key() == KEY_ESC || evt.key() == KEY_LEFTMETA || evt.key() == KEY_RIGHTMETA) {
          return;
        } else if (evt.key() == KEY_UP || evt.key() == KEY_VOLUMEUP || evt.key() == KEY_SCROLLUP ||
                   evt.key() == KEY_PAGEUP) {
          if (offsets.size() <= 1) {
            show_prompt = true;
          } else {
            offsets.pop_back();
            fseek(fp, offsets.back(), SEEK_SET);
          }
        } else {
          if (feof(fp)) {
            return;
          }
          offsets.push_back(ftello(fp));
        }
      }
      ClearText();
    }

    int ch = getc(fp);
    if (ch == EOF) {
      while (text_row_ < text_rows_ - 1) PutChar('\n');
      show_prompt = true;
    } else {
      PutChar(ch);
      if (text_col_ == 0 && text_row_ >= text_rows_ - 1) {
        show_prompt = true;
      }
    }
  }
}

void ScreenRecoveryUI::ShowFile(const std::string& filename) {
  std::unique_ptr<FILE, decltype(&fclose)> fp(fopen(filename.c_str(), "re"), fclose);
  if (!fp) {
    Print("  Unable to open %s: %s\n", filename.c_str(), strerror(errno));
    return;
  }

  char** old_text;
  size_t old_text_col, old_text_row;
  {
    std::lock_guard<std::mutex> lock(updateMutex);
    old_text = text_;
    old_text_col = text_col_;
    old_text_row = text_row_;
    // Swap in the explicit text viewer under the same lock as the renderer.
    text_ = file_viewer_text_;
    menu_transition_ = false;
  }
  ClearText();

  ShowFile(fp.get());

  {
    std::lock_guard<std::mutex> lock(updateMutex);
    text_ = old_text;
    text_col_ = old_text_col;
    text_row_ = old_text_row;
    menu_transition_ = true;
    update_screen_locked();
  }
}

std::unique_ptr<Menu> ScreenRecoveryUI::CreateMenu(
    const GRSurface* graphic_header, const std::vector<const GRSurface*>& graphic_items,
    const std::vector<std::string>& text_headers, const std::vector<std::string>& text_items,
    size_t initial_selection) const {
  // Use complete English/Chinese prompts with the native card renderer.
  // Preserve upstream localized graphic resources for other languages.
  if ((locale_.rfind("en", 0) == 0 || locale_.rfind("zh", 0) == 0) && !text_headers.empty() && !text_items.empty()) {
    return CreateMenu(text_headers, text_items, initial_selection);
  }
  // horizontal unusable area: margin width + menu indent
  recovery_m3e::Metrics m(ScreenWidth());
  size_t max_width = std::max(0, ScreenWidth() - 2 * m.inset);
  int top = std::max(margin_height_, recovery_m3e::Dp(ScreenWidth(), 24));
  int bottom = ScreenHeight() - std::max(margin_height_, recovery_m3e::Dp(ScreenWidth(), 24));
  size_t max_height = std::max(0, bottom - recovery_m3e::Dp(ScreenWidth(), 76) -
                                recovery_m3e::HeaderBottom(m, top, false));
  if (GraphicMenu::Validate(max_width, max_height, graphic_header, graphic_items)) {
    return std::make_unique<GraphicMenu>(graphic_header, graphic_items, initial_selection, *this);
  }

  fprintf(stderr, "Failed to initialize graphic menu, falling back to use the text menu.\n");

  return CreateMenu(text_headers, text_items, initial_selection);
}

std::unique_ptr<Menu> ScreenRecoveryUI::CreateMenu(const std::vector<std::string>& text_headers,
                                                   const std::vector<std::string>& text_items,
                                                   size_t initial_selection) const {
  if (text_headers == std::vector<std::string>{"Language"} &&
      text_items == std::vector<std::string>{"简体中文", "English"}) {
    initial_selection = recovery_m3e::GetLanguage() == recovery_m3e::Language::Chinese ? 0 : 1;
  }
  int menu_char_width = MenuCharWidth();
  int menu_char_height = MenuCharHeight();
  int menu_cols = (ScreenWidth() - margin_width_*2 - kMenuIndent) / menu_char_width;
  bool wrap_selection = !HasThreeButtons() && !HasTouchScreen();
  return std::make_unique<TextMenu>(wrap_selection, menu_cols, text_headers, text_items,
                                    initial_selection, menu_char_height, *menu_draw_funcs_);
}

int ScreenRecoveryUI::SelectMenu(int sel) {
  std::lock_guard<std::mutex> lg(updateMutex);
  if (menu_) {
    if (IsDesignAdbLocked()) sel = -1;  // Back is the only control on the SVG sideload page.
    int old_sel = menu_->selection();
    sel = menu_->Select(sel);

    if (sel != old_sel) {
      update_screen_locked();
    }
  }
  return sel;
}

Point ScreenRecoveryUI::TouchPoint(const Point& p) const {
  Point point;

  const auto scaleX = static_cast<double>(p.x()) / gr_fb_width_real();
  const auto scaleY = static_cast<double>(p.y()) / gr_fb_height_real();

  // Correct position for touch rotation
  switch (gr_touch_rotation()) {
    case GRRotation::NONE:
      point.x(ScreenWidth() * scaleX);
      point.y(ScreenHeight() * scaleY);
      break;
    case GRRotation::RIGHT:
      point.x(ScreenWidth() * scaleY);
      point.y(ScreenHeight() - (ScreenHeight() * scaleX));
      break;
    case GRRotation::DOWN:
      point.x(ScreenWidth() - (ScreenWidth() * scaleX));
      point.y(ScreenHeight() - (ScreenHeight() * scaleY));
      break;
    case GRRotation::LEFT:
      point.x(ScreenWidth() - (ScreenWidth() * scaleY));
      point.y(ScreenHeight() * scaleX);
      break;
  }

  // Correct position for overscan
  point.x(point.x() - gr_overscan_offset_x());
  point.y(point.y() - gr_overscan_offset_y());
  return point;
}

int ScreenRecoveryUI::SelectMenu(const Point& p) {
  const Point point = TouchPoint(p);

  int new_sel = Device::kNoAction;
  std::lock_guard<std::mutex> lg(updateMutex);
  if (menu_) {

    auto back = recovery_m3e::PageBackBounds(ScreenWidth());
    if (!menu_->IsMain() && recovery_m3e::InRounded(back, back.h / 2, point.x(), point.y())) {
      return Device::kGoBack;
    }
    if (point.y() < menu_start_y_ || point.y() >= m3e_menu_bottom_) return Device::kNoAction;
    int relative = menu_->HitTest(point.x(), point.y() - menu_start_y_, ScreenWidth());
    if (relative < 0) return Device::kNoAction;
    int old_sel = menu_->selection();
    new_sel = menu_->SelectVisible(relative);
    if (new_sel != old_sel) update_screen_locked();
  }
  return new_sel;
}

int ScreenRecoveryUI::ScrollMenu(int updown) {
  std::lock_guard<std::mutex> lg(updateMutex);
  int sel = Device::kNoAction;
  if (menu_) {
    sel = menu_->Scroll(updown);
    update_screen_locked();
  }
  return sel;
}

size_t ScreenRecoveryUI::ShowMenu(std::unique_ptr<Menu>&& menu, bool menu_only,
                                  const std::function<int(int, bool)>& key_handler,
                                  bool refreshable) {
  // Throw away keys pressed previously, so user doesn't accidentally trigger menu items.
  FlushKeys();

  // If there is a key interrupt in progress, return KeyError::INTERRUPTED without starting the
  // menu.
  if (IsKeyInterrupted()) return static_cast<size_t>(KeyError::INTERRUPTED);

  CHECK(menu != nullptr);

  // Starts and displays the menu
  {
    std::lock_guard<std::mutex> lock(updateMutex);
    menu_ = std::move(menu);
    transition_menu_.reset();
    menu_transition_ = false;
    update_screen_locked();
  }

  int selected = menu_->selection();
  int chosen_item = -1;
  while (chosen_item < 0) {
    InputEvent evt = WaitInputEvent();
    if (evt.type() == EventType::EXTRA) {
      if (evt.key() == static_cast<int>(KeyError::INTERRUPTED)) {
        // WaitKey() was interrupted.
        std::lock_guard<std::mutex> lock(updateMutex);
        transition_menu_ = std::move(menu_);
        menu_transition_ = true;
        return static_cast<size_t>(KeyError::INTERRUPTED);
      }
      if (evt.key() == static_cast<int>(KeyError::TIMED_OUT)) {  // WaitKey() timed out.
        if (WasTextEverVisible()) {
          continue;
        } else {
          LOG(INFO) << "Timed out waiting for key input; rebooting.";
          std::lock_guard<std::mutex> lock(updateMutex);
          menu_.reset();
          update_screen_locked();
          return static_cast<size_t>(KeyError::TIMED_OUT);
        }
      }
    }

    int action = Device::kNoAction;
    if (evt.type() == EventType::TOUCH) {
      int touch_sel = SelectMenu(evt.pos());
      if (touch_sel < 0) {
        action = touch_sel;
      } else {
        action = Device::kInvokeItem;
        selected = touch_sel;
      }
    } else if (evt.type() == EventType::KEY) {
      bool visible = IsTextVisible();
      action = key_handler(evt.key(), visible);
    }

    if (action < 0) {
      switch (action) {
        case Device::kHighlightUp:
          selected = SelectMenu(--selected);
          break;
        case Device::kHighlightDown:
          selected = SelectMenu(++selected);
          break;
        case Device::kHighlightFirst:
          selected = SelectMenu(0);
          break;
        case Device::kHighlightLast:
          selected = SelectMenu(menu_->ItemsCount() - 1);
          break;
        case Device::kScrollUp:
          selected = ScrollMenu(-1);
          break;
        case Device::kScrollDown:
          selected = ScrollMenu(1);
          break;
        case Device::kInvokeItem:
          if (selected >= 0 && !menu_->HasVisibleItems()) break;
          if (selected < 0) {
            chosen_item = Device::kGoBack;
          } else {
            chosen_item = selected;
          }
          break;
        case Device::kNoAction:
          break;
        case Device::kGoBack:
          chosen_item = Device::kGoBack;
          break;
        case Device::kGoHome:
          chosen_item = Device::kGoHome;
          break;
        case Device::kDoSideload:
          chosen_item = Device::kDoSideload;
          break;
        case Device::kRefresh:
          if (refreshable) {
            chosen_item = Device::kRefresh;
          }
          break;
      }
    } else if (!menu_only) {
      chosen_item = action;
    }

    if (chosen_item == Device::kGoBack || chosen_item == Device::kGoHome ||
        chosen_item == Device::kDoSideload || chosen_item == Device::kRefresh) {
      break;
    }
  }

  {
    std::lock_guard<std::mutex> lock(updateMutex);
    transition_menu_ = std::move(menu_);
    menu_transition_ = true;
  }

  return chosen_item;
}

size_t ScreenRecoveryUI::ShowMenu(const std::vector<std::string>& headers,
                                  const std::vector<std::string>& items, size_t initial_selection,
                                  bool menu_only,
                                  const std::function<int(int, bool)>& key_handler,
                                  bool refreshable) {
  auto menu = CreateMenu(headers, items, initial_selection);
  if (menu == nullptr) {
    return initial_selection;
  }

  return ShowMenu(std::move(menu), menu_only, key_handler, refreshable);
}

size_t ScreenRecoveryUI::ShowPromptWipeDataMenu(const std::vector<std::string>& backup_headers,
                                                const std::vector<std::string>& backup_items,
                                                const std::function<int(int, bool)>& key_handler) {
  auto wipe_data_menu = CreateMenu(backup_headers, backup_items, 0);
  if (wipe_data_menu == nullptr) {
    return 0;
  }

  return ShowMenu(std::move(wipe_data_menu), true, key_handler);
}

size_t ScreenRecoveryUI::ShowPromptWipeDataConfirmationMenu(
    const std::vector<std::string>& backup_headers, const std::vector<std::string>& backup_items,
    const std::function<int(int, bool)>& key_handler) {
  auto confirmation_menu =
      CreateMenu(wipe_data_confirmation_text_.get(),
                 { cancel_wipe_data_text_.get(), factory_data_reset_text_.get() }, backup_headers,
                 backup_items, 0);
  if (confirmation_menu == nullptr) {
    return 0;
  }

  return ShowMenu(std::move(confirmation_menu), true, key_handler);
}

bool ScreenRecoveryUI::IsTextVisible() {
  std::lock_guard<std::mutex> lg(updateMutex);
  int visible = show_text;
  return visible;
}

bool ScreenRecoveryUI::WasTextEverVisible() {
  std::lock_guard<std::mutex> lg(updateMutex);
  int ever_visible = show_text_ever;
  return ever_visible;
}

void ScreenRecoveryUI::ShowText(bool visible) {
  std::lock_guard<std::mutex> lg(updateMutex);
  show_text = visible;
  if (show_text) show_text_ever = true;
  update_screen_locked();
}

void ScreenRecoveryUI::Redraw() {
  std::lock_guard<std::mutex> lg(updateMutex);
  update_screen_locked();
}

void ScreenRecoveryUI::KeyLongPress(int) {
  // Redraw so that if we're in the menu, the highlight
  // will change color to indicate a successful long press.
  Redraw();
}

void ScreenRecoveryUI::SetLocale(const std::string& new_locale) {
  locale_ = new_locale;
  rtl_locale_ = false;

  if (!new_locale.empty()) {
    size_t separator = new_locale.find('-');
    // lang has the language prefix prior to the separator, or full string if none exists.
    std::string lang = new_locale.substr(0, separator);

    // A bit cheesy: keep an explicit list of supported RTL languages.
    if (lang == "ar" ||  // Arabic
        lang == "fa" ||  // Persian (Farsi)
        lang == "he" ||  // Hebrew (new language code)
        lang == "iw" ||  // Hebrew (old language code)
        lang == "ur") {  // Urdu
      rtl_locale_ = true;
    }
  }
}

int ScreenRecoveryUI::SetSwCallback(int code, int value) {
  if (!is_graphics_available) { return -1; }
  if (code > SW_MAX) { return -1; }
  if (code != SW_LID) { return 0; }

  /* detect dual display */
  if (!gr_has_multiple_connectors()) { return -1; }

  /* turn off all screen */
  gr_fb_blank(true, DirectRenderManager::DRM_INNER);
  gr_fb_blank(true, DirectRenderManager::DRM_OUTER);
  gr_color(0, 0, 0, 255);
  gr_clear();

  /* turn on the screen */
  gr_fb_blank(false, value);
  gr_flip();

  /* set the retation */
  std::string rotation_str;
  if (value == DirectRenderManager::DRM_OUTER) {
    rotation_str =
      android::base::GetProperty("ro.minui.second_rotation", "ROTATION_NONE");
  } else {
    rotation_str =
      android::base::GetProperty("ro.minui.default_rotation", "ROTATION_NONE");
  }

  if (rotation_str == "ROTATION_RIGHT") {
    gr_rotate(GRRotation::RIGHT);
  } else if (rotation_str == "ROTATION_DOWN") {
    gr_rotate(GRRotation::DOWN);
  } else if (rotation_str == "ROTATION_LEFT") {
    gr_rotate(GRRotation::LEFT);
  } else {  // "ROTATION_NONE" or unknown string
    gr_rotate(GRRotation::NONE);
  }
  Redraw();

  return 0;
}
