/*
 * Copyright (C) 2015 The Android Open Source Project
 * Copyright (C) 2019 The LineageOS Project
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

#include "recovery_ui/device.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <android-base/logging.h>

#include "otautil/boot_state.h"
#include "recovery_ui/ui.h"

// Navigation identifiers are local to this menu model, never passed to the installer.
enum class MenuRoute { Language = 1000, Reboot, Maintenance, Chinese, English };
using menu_action_t = std::pair<std::string, int>;
static std::vector<std::string> g_main_header{};
static std::vector<menu_action_t> g_main_actions{
  { "Apply update", Device::APPLY_UPDATE },
  { "Terminal", Device::OPEN_TERMINAL },
  { "Settings", Device::MENU_ADVANCED },
  { "Reboot options", static_cast<int>(MenuRoute::Reboot) },
  { "Factory reset", Device::MENU_WIPE },
};
static std::vector<std::string> g_settings_header{ "Settings" };
static std::vector<menu_action_t> g_settings_actions{
  { "Language", static_cast<int>(MenuRoute::Language) },
  { "View recovery logs", Device::VIEW_RECOVERY_LOGS },
  { "Advanced tools", static_cast<int>(MenuRoute::Maintenance) },
};
static std::vector<std::string> g_reboot_header{ "Reboot options" };
static std::vector<menu_action_t> g_reboot_actions{
  { "Reboot system now", Device::REBOOT },
  { "Enter fastboot", Device::ENTER_FASTBOOT },
  { "Reboot to bootloader", Device::REBOOT_BOOTLOADER },
  { "Reboot to recovery", Device::REBOOT_RECOVERY },
  { "Power off", Device::SHUTDOWN },
};
static std::vector<std::string> g_maintenance_header{ "Advanced tools" };
static std::vector<menu_action_t> g_maintenance_actions{
  { "Mount/unmount system", Device::MOUNT_SYSTEM },
  { "Enable ADB", Device::ENABLE_ADB },
  { "Run graphics test", Device::RUN_GRAPHICS_TEST },
  { "Run locale test", Device::RUN_LOCALE_TEST },
  { "Enter rescue", Device::ENTER_RESCUE },
};
static std::vector<std::string> g_language_header{ "Language" };
static std::vector<menu_action_t> g_language_actions{
  { "简体中文", static_cast<int>(MenuRoute::Chinese) },
  { "English", static_cast<int>(MenuRoute::English) },
};
static std::vector<std::string> g_wipe_header{ "Factory reset" };
static std::vector<menu_action_t> g_wipe_actions{
  { "Format data/factory reset", Device::WIPE_DATA },
  { "Format cache partition", Device::WIPE_CACHE },
  { "Format system partition", Device::WIPE_SYSTEM },
};
static std::vector<menu_action_t>* current_menu_ = &g_main_actions;
static std::vector<menu_action_t>* reboot_parent_ = &g_main_actions;
static std::vector<std::string> g_menu_items;
static void PopulateMenuItems() {
  g_menu_items.clear();
  std::transform(current_menu_->cbegin(), current_menu_->cend(), std::back_inserter(g_menu_items),
                 [](const auto& entry) { return entry.first; });
}
Device::Device(RecoveryUI* ui) : ui_(ui) { ui->SetDevice(this); PopulateMenuItems(); }
Device::~Device() = default;
void Device::ResetUI(RecoveryUI* ui) { ui_.reset(ui); }
void Device::GoHome() { current_menu_ = &g_main_actions; PopulateMenuItems(); }
void Device::GoBack() {
  if (current_menu_ == &g_reboot_actions) current_menu_ = reboot_parent_;
  else if (current_menu_ == &g_language_actions ||
      current_menu_ == &g_maintenance_actions) current_menu_ = &g_settings_actions;
  else current_menu_ = &g_main_actions;
  PopulateMenuItems();
}
static void RemoveMenuItemForAction(std::vector<menu_action_t>& menu, int action) {
  menu.erase(std::remove_if(menu.begin(), menu.end(),
      [action](const auto& entry) { return entry.second == action; }), menu.end());
}
void Device::RemoveMenuItemForAction(Device::BuiltinAction action) {
  ::RemoveMenuItemForAction(g_wipe_actions, action);
  ::RemoveMenuItemForAction(g_reboot_actions, action);
  ::RemoveMenuItemForAction(g_settings_actions, action);
  ::RemoveMenuItemForAction(g_maintenance_actions, action);
  // A user build removes every maintenance action during startup. An empty
  // optional submenu is valid; hide its entry instead of aborting recovery.
  if (g_maintenance_actions.empty()) {
    ::RemoveMenuItemForAction(g_settings_actions, static_cast<int>(MenuRoute::Maintenance));
    if (current_menu_ == &g_maintenance_actions) {
      current_menu_ = &g_settings_actions;
    }
  }
  PopulateMenuItems();
}
const std::vector<std::string>& Device::GetMenuItems() { return g_menu_items; }
const std::vector<std::string>& Device::GetMenuHeaders() {
  if (current_menu_ == &g_wipe_actions) return g_wipe_header;
  if (current_menu_ == &g_settings_actions) return g_settings_header;
  if (current_menu_ == &g_reboot_actions) return g_reboot_header;
  if (current_menu_ == &g_maintenance_actions) return g_maintenance_header;
  if (current_menu_ == &g_language_actions) return g_language_header;
  return g_main_header;
}
Device::BuiltinAction Device::InvokeMenuItem(size_t menu_position) {
  if (menu_position >= current_menu_->size()) return NO_ACTION;
  int action = (*current_menu_)[menu_position].second;
  if (action == static_cast<int>(MenuRoute::Chinese) || action == static_cast<int>(MenuRoute::English)) {
    const std::string code = action == static_cast<int>(MenuRoute::Chinese) ? "zh-CN" : "en-US";
    if (!ui_->SetUiLanguage(code)) ui_->Print("Recovery UI does not support this language switch.\n");
    // Return through FinishRecovery so the preference can be saved outside encrypted userdata.
    return NO_ACTION;
  }
  if (action == MENU_WIPE) current_menu_ = &g_wipe_actions;
  else if (action == MENU_ADVANCED) current_menu_ = &g_settings_actions;
  else if (action == static_cast<int>(MenuRoute::Language)) current_menu_ = &g_language_actions;
  else if (action == static_cast<int>(MenuRoute::Reboot)) {
    reboot_parent_ = current_menu_;
    current_menu_ = &g_reboot_actions;
  }
  else if (action == static_cast<int>(MenuRoute::Maintenance)) current_menu_ = &g_maintenance_actions;
  else return static_cast<BuiltinAction>(action);
  PopulateMenuItems();
  return MENU_BASE;
}

int Device::HandleMenuKey(int key, bool visible) {
  if (!visible) {
    return kNoAction;
  }

  switch (key) {
    case KEY_RIGHTSHIFT:
    case KEY_DOWN:
    case KEY_VOLUMEDOWN:
    case KEY_MENU:
    case BTN_NORTH:
    case BTN_DPAD_DOWN:
      return kHighlightDown;

    case KEY_UP:
    case KEY_VOLUMEUP:
    case KEY_SEARCH:
    case BTN_WEST:
    case BTN_DPAD_UP:
      return kHighlightUp;

    case KEY_HOME:
      return kHighlightFirst;
    case KEY_END:
      return kHighlightLast;

    case KEY_PAGEUP:
    case KEY_SCROLLUP:
      return kScrollUp;
    case KEY_PAGEDOWN:
    case KEY_SCROLLDOWN:
      return kScrollDown;

    case KEY_ENTER:
    case KEY_POWER:
    case BTN_MOUSE:
    case KEY_SEND:
    case BTN_SOUTH:
    case BTN_START:
      return kInvokeItem;

    case KEY_HOMEPAGE:
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
      return kGoHome;

    case KEY_BACKSPACE:
    case KEY_BACK:
    case KEY_ESC:
      return kGoBack;

    case KEY_AGAIN:
      return kDoSideload;

    case KEY_REFRESH:
      return kRefresh;

    default:
      // If you have all of the above buttons, any other buttons
      // are ignored. Otherwise, any button cycles the highlight.
      return ui_->HasThreeButtons() ? kNoAction : kHighlightDown;
  }
}

void Device::SetBootState(const BootState* state) {
  boot_state_ = state;
}

std::optional<std::string> Device::GetReason() const {
  return boot_state_ ? std::make_optional(boot_state_->reason()) : std::nullopt;
}

std::optional<std::string> Device::GetStage() const {
  return boot_state_ ? std::make_optional(boot_state_->stage()) : std::nullopt;
}
