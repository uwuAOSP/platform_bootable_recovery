/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "install_status.h"
#include "m3e.h"

namespace recovery_m3e {
using InstallStage = recovery_ui::InstallStage;
struct InstallLayout { Rect logo, panel; int menu_y; };

inline int InstallButtonSpace(const Metrics& m,int rows) {
  return rows>0?rows*m.row_height+(rows-1)*m.gap+Dp(m.width,16):0;
}
inline bool CompactInstallHeader(const Metrics& m,int top,int bottom,int menu_rows) {
  return bottom-HeaderBottom(m,top,false)-InstallButtonSpace(m,menu_rows)<Dp(m.width,188);
}
inline int DrawInstallHeader(Canvas& c,const Metrics& m,int top,int bottom,int menu_rows,
                             bool back_selected,const std::vector<std::string>& details,
                             const Palette& p, const std::string& title = "Install update") {
  if(!CompactInstallHeader(m,top,bottom,menu_rows)) {
    return DrawHeader(c,m,top,menu_rows>0,back_selected,false,0,0,details,p,title,false);
  }
  // Keep both result actions reachable on landscape/compact displays. The back hit area
  // stays identical to the regular header used by ScreenRecoveryUI::SelectMenu.
  auto back=BackBounds(m,top);int x=m.inset;
  if(menu_rows>0) {
    Surface(c,back,back.h/2,p.surface,back_selected,p.text,Dp(m.width,2));
    DrawIcon(c,Inset(back,Dp(m.width,14)),Icon::Back,p.text);
    x=back.x+back.w+Dp(m.width,12);
  }
  Label(c,m,x,back.y+Dp(m.width,13),m.width-m.inset-x-Dp(m.width,90),
        title,Font::Menu,p.text,true);
  return back.y+back.h+Dp(m.width,12);
}

inline InstallLayout InstallationLayout(const Metrics& m,int top,int bottom,int menu_rows,
                                         int logo_width=0,int logo_height=0,bool detailed=false) {
  int gap=Dp(m.width,16);
  int buttons=InstallButtonSpace(m,menu_rows);
  int available=std::max(0,bottom-top-buttons);
  int panel_height=detailed?available:std::min(Dp(m.width,188),available);
  Rect logo{0,0,0,0};
  // Never shrink the status card or hide result actions to make room for artwork.
  if(logo_width>0 && logo_height>0 && logo_width<=m.width-2*m.inset &&
     logo_height<=available-panel_height-gap) {
    logo={(m.width-logo_width)/2,top,logo_width,logo_height};
    top+=logo_height+gap;
  }
  Rect panel{m.inset,top,m.width-2*m.inset,panel_height};
  return {logo,panel,top+panel_height+gap};
}

inline std::string InstallPercent(double fraction) {
  fraction=std::isfinite(fraction)?std::clamp(fraction,0.0,1.0):0.0;
  int tenths=fraction>=1.0?1000:std::min(999,static_cast<int>(fraction*1000+0.000001));
  return std::to_string(tenths/10)+(tenths%10?"."+std::to_string(tenths%10):"")+"%";
}

inline const char* InstallTitle(InstallStage stage,bool security_update) {
  switch(stage) {
    case InstallStage::FLASH_PREPARING: return "Preparing partition image";
    case InstallStage::FLASH_WRITING: return "Writing partition image";
    case InstallStage::FLASH_VERIFYING: return "Verifying partition contents";
    case InstallStage::FLASH_SUCCESS: return "Partition flash complete";
    case InstallStage::FLASH_ERROR: return "Partition flash failed";
    case InstallStage::WAITING: return "Waiting for a package";
    case InstallStage::VERIFYING: return "Verifying update";
    case InstallStage::INSTALLING: return security_update?"Installing security update":"Installing update";
    case InstallStage::SUCCESS: return "Installation complete";
    case InstallStage::ERROR: return "Installation failed";
    case InstallStage::CANCELLED: return "Installation not started";
    default: return "Install update";
  }
}
inline const char* InstallHint(InstallStage stage) {
  switch(stage) {
    case InstallStage::FLASH_PREPARING: return "Creating an immutable copy before writing.";
    case InstallStage::FLASH_WRITING: return "Do not reboot or disconnect power.";
    case InstallStage::FLASH_VERIFYING: return "Comparing the written bytes with the image.";
    case InstallStage::FLASH_SUCCESS: return "The selected partition passed read-back verification.";
    case InstallStage::FLASH_ERROR: return "Open the recovery log for details.";
    case InstallStage::WAITING: return "Send the update package from your computer.";
    case InstallStage::VERIFYING: return "Checking the package signature.";
    case InstallStage::INSTALLING: return "Keep the USB cable connected.";
    case InstallStage::SUCCESS: return "You can return to the menu or read the log.";
    case InstallStage::ERROR: return "Open the recovery log for details.";
    case InstallStage::CANCELLED: return "Cancelled, or no package was received.";
    default: return "";
  }
}
inline void DrawInstallPanel(Canvas& c,const Metrics& m,Rect panel,InstallStage stage,
                             double fraction,bool determinate,bool security_update,
                             const std::vector<std::string>& logs,const Palette& p) {
  if(panel.w<=0 || panel.h<=0) return;
  bool error=stage==InstallStage::ERROR || stage==InstallStage::FLASH_ERROR;
  bool complete=stage==InstallStage::SUCCESS || stage==InstallStage::FLASH_SUCCESS;
  Color accent=error?p.error:complete?p.alert_success:
      stage==InstallStage::CANCELLED?p.alert_warning:p.primary;
  Rounded(c,panel,Dp(m.width,28),error?p.error_surface:p.card);
  bool compact=panel.h<Dp(m.width,140);
  int pad=Dp(m.width,compact?8:16),x=panel.x+pad,y=panel.y+pad;
  int width=panel.w-2*pad,bottom=panel.y+panel.h-pad;
  int title_height=LineHeight(FontPixels(Font::Menu,m.width));
  bool progress_stage=stage==InstallStage::VERIFYING || stage==InstallStage::INSTALLING ||
      stage==InstallStage::FLASH_WRITING || stage==InstallStage::FLASH_VERIFYING;
  fraction=std::isfinite(fraction)?std::clamp(fraction,0.0,1.0):0.0;
  std::string percent=(progress_stage && determinate) || complete?
      InstallPercent(complete?1.0:fraction):"";
  int percent_width=percent.empty()?0:TextWidth(percent,FontPixels(Font::Small,m.width),true);
  if(y+title_height<=bottom) {
    Label(c,m,x,y,width-(percent.empty()?0:percent_width+Dp(m.width,12)),
          InstallTitle(stage,security_update),Font::Menu,p.text,true);
    if(!percent.empty()) c.Text(x+width-percent_width,y+(title_height-LineHeight(FontPixels(Font::Small,m.width)))/2,
                               percent,Font::Small,p.text,true);
    y+=title_height+Dp(m.width,compact?6:10);
  }
  if((progress_stage || complete) && y+Dp(m.width,8)<=bottom) {
    Rect track{x,y,width,Dp(m.width,8)};
    Rounded(c,track,track.h/2,p.outline);
    int filled=complete?width:determinate?static_cast<int>(width*fraction):width/4;
    if(filled>0) Rounded(c,{x,y,filled,track.h},track.h/2,accent);
    y+=track.h+Dp(m.width,12);
  }
  int body_height=LineHeight(FontPixels(Font::Body,m.width));
  if(stage==InstallStage::WAITING && y+body_height<=bottom) {
    Label(c,m,x,y,width,"adb sideload <filename>",Font::Body,p.text,true);
    y+=body_height+Dp(m.width,8);
  }
  auto hint=WrapText(Tr(InstallHint(stage)),width,FontPixels(Font::Body,m.width));
  int count=0;
  for(const auto& line:hint) {
    if(++count>2 || y+body_height>bottom) break;
    c.Text(x,y,line,Font::Body,p.secondary,false);y+=body_height;
  }
  int small_height=FontLineHeight(Font::Small,m.width);
  int log_height=FontLineHeight(Font::Code,m.width);
  if(!logs.empty() && y+Dp(m.width,12)+small_height+log_height<=bottom) {
    y+=Dp(m.width,12);
    Label(c,m,x,y,width,"RECENT OUTPUT",Font::Small,p.secondary,true);y+=small_height;
    std::vector<std::string> rows;
    for(const auto& log:logs) {
      auto wrapped=WrapText(log,width,FontPixels(Font::Code,m.width),false,true);
      rows.insert(rows.end(),wrapped.begin(),wrapped.end());
    }
    size_t lines=(bottom-y)/log_height;
    size_t first=rows.size()>lines?rows.size()-lines:0;
    for(size_t i=first;i<rows.size();++i) {
      c.Text(x,y,rows[i],Font::Code,p.text,false);y+=log_height;
    }
  }
}
}  // namespace recovery_m3e
