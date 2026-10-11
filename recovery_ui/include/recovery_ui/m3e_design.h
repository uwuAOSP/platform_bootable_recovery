/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "m3e.h"
#include "m3e_install.h"
#include "install_status.h"

namespace recovery_m3e::design {
// Only the pages shown in recovery-design-1.svg and fastbootd.svg opt in.
enum class Page { None, Home, Reboot, Sources, Fastboot };
inline Page MenuPage(bool dashboard,const std::string& title) {
  if(dashboard) return Page::Home;
  if(title=="Reboot options") return Page::Reboot;
  if(title=="Install update") return Page::Sources;
  if(title=="FastbootD") return Page::Fastboot;
  return Page::None;
}
inline bool AdbPage(bool adb,recovery_ui::InstallStage stage) {
  return adb && (stage==recovery_ui::InstallStage::WAITING ||
                 stage==recovery_ui::InstallStage::INSTALLING);
}
constexpr Color background=theme::background,surface=theme::surface,text=theme::text;
constexpr Color green=theme::green,muted=theme::muted,track=theme::track;
inline Metrics LayoutMetrics(int width) {
  return Metrics(width);
}
inline Rect Back(int width) {return PageBackBounds(width);}
inline int FooterTop(int width,int height) {return PageFooterTop(width,height);}
inline int TitleTop(int width,int height) {return Dp(width,height<Dp(width,600)?65:112);}
inline int PageTitleTop(int width,int height,Page page) {
  int top=TitleTop(width,height);
  if(page!=Page::Home && page!=Page::Fastboot) {
    auto back=Back(width);top=std::max(top,back.y+back.h+Dp(width,14));
  }
  return top;
}
struct FastbootLayout {Rect status;int heading,menu;bool compact;};
inline FastbootLayout Fastboot(int width,int height) {
  auto m=LayoutMetrics(width);bool compact=height<Dp(width,440);
  int top=TitleTop(width,height)+Dp(width,compact?40:70);
  int gap=Dp(width,compact?5:54),bottom=FooterTop(width,height)-Dp(width,10);
  int minimum=std::max(Dp(width,24),FontLineHeight(Font::DesignMenu,width));
  // Shrink the status card to keep all destinations visible where possible.
  // Short screens retain at least two rows and scroll through the same actions.
  int rows=std::clamp(VisibleCount(bottom-top-gap-minimum-Dp(width,27),m.row_height,m.gap),2,4);
  int reserve=rows*m.row_height+(rows-1)*m.gap+Dp(width,27);
  int panel=std::min(Dp(width,166),std::max(minimum,bottom-top-gap-reserve));
  return {{m.inset,top,width-2*m.inset,panel},top+panel+Dp(width,26),top+panel+gap,compact};
}
inline int MenuTop(int width,int height,Page page) {
  if(page==Page::Fastboot)return Fastboot(width,height).menu;
  return PageTitleTop(width,height,page)+Dp(width,page==Page::Home?71:39);
}
using Glyph=fontdata::Symbol;
inline void Symbol(Canvas& c,Rect b,Glyph glyph,Color color=text,bool filled=false,int pixels=0) {
  DrawSymbol(c,b,glyph,color,filled,pixels);
}
inline void Chevron(Canvas& c,Rect b,Color color=text) {
  // Use the entire round button for positioning, keeping font size separate.
  Symbol(c,b,Glyph::Chevron,color,false,std::min(b.w,b.h)*5/6);
}
inline void Battery(Canvas& c,int width,int capacity,bool charging) {
  Metrics m(width);int h=Dp(width,28),pad=Dp(width,9),icon=Dp(width,18);
  std::string value=capacity>=0 && capacity<=100?std::to_string(capacity)+"%":"--%";
  int tw=TextWidth(value,FontPixels(Font::DesignSmall,width),false,false,Face::Flex),bw=tw+3*pad+icon;
  Rect box{width-Dp(width,32)-bw,Dp(width,32),bw,h};
  Rounded(c,box,h/2,surface);
  c.Text(box.x+pad,box.y+(h-FontLineHeight(Font::DesignSmall,width))/2,value,Font::DesignSmall,text,false);
  Rect cell{box.x+bw-pad-icon,box.y+(h-icon)/2,icon,icon};
  constexpr Glyph levels[]={Glyph::BatteryEmpty,Glyph::Battery1,Glyph::Battery2,
    Glyph::Battery3,Glyph::Battery4,Glyph::Battery5,Glyph::Battery6,Glyph::BatteryFull};
  int level=capacity>=0 && capacity<=100?capacity*7/100:0;
  Symbol(c,cell,charging?ChargingBatterySymbol(capacity):levels[level],text,charging?capacity>=100:level!=0);
}
inline void Footer(Canvas& c,int width,int height,const std::vector<std::string>& details) {
  DrawPageFooter(c,width,height,details);
}
inline void Header(Canvas& c,int width,int height,Page page,bool back_selected=false,
                   const std::string& override_title={}) {
  Metrics m(width);
  if(page!=Page::Home && page!=Page::Fastboot) {
    auto b=Back(width);Surface(c,b,b.h/2,surface,back_selected,text,Dp(width,1));
    Symbol(c,Inset(b,Dp(width,14)),Glyph::Back);
  }
  int y=PageTitleTop(width,height,page);
  if(page==Page::Home || page==Page::Fastboot) {
    int x=Dp(width,29);std::string brand="uwuAOSP";
    for(size_t i=0;i<brand.size();++i) {
      std::string letter=brand.substr(i,1);
      c.Text(x,y,letter,Font::DesignBrand,{static_cast<uint8_t>(147+i*5),static_cast<uint8_t>(232-i*4),255},true);
      x+=TextWidth(letter,FontPixels(Font::DesignBrand,width),true, false, FaceFor(Font::DesignBrand));
    }
    x+=Dp(width,6);
    // These English brand titles retain their spelling and size in every locale.
    c.Text(x,y,FitText(page==Page::Fastboot?"fastbootD":"Recovery",width-x-Dp(width,20),
        FontPixels(Font::DesignRecoveryTitle,width),true,false,Face::Flex),Font::DesignRecoveryTitle,text,true);
  } else {
    std::string title=override_title.empty()?(page==Page::Reboot?"Reboot to...":"Install or update by..."):override_title;
    if(GetLanguage()==Language::Chinese && override_title.empty())title=Tr(page==Page::Reboot?"Reboot options":"Install update");
    Label(c,m,Dp(width,21),y,width-Dp(width,42),title,Font::DesignPageTitle,text,true);
  }
}
inline void FastbootStatus(Canvas& c,int width,int height) {
  auto layout=Fastboot(width,height);auto b=layout.status;Metrics m(width);
  Rounded(c,b,Dp(width,22),surface);
  auto label=FitText(Tr("Ready for operation"),b.w-Dp(width,24),FontPixels(Font::DesignMenu,width),false,false,Face::Flex);
  c.Text(b.x+(b.w-TextWidth(label,FontPixels(Font::DesignMenu,width),false,false,Face::Flex))/2,
      b.y+(b.h-FontLineHeight(Font::DesignMenu,width))/2,label,Font::DesignMenu,text,false);
  if(!layout.compact)Label(c,m,Dp(width,24),layout.heading,width-Dp(width,48),
      GetLanguage()==Language::Chinese?Tr("Reboot options"):"Reboot to...",Font::DesignPageTitle,text);
}
struct HomeLayout {std::array<Rect,5> buttons;int height;bool valid;};
inline HomeLayout Home(int width,int top,int available) {
  int pad=Dp(width,29),gap=Dp(width,17),hero=Dp(width,130),pill=Dp(width,59);
  int total=2*hero+pill+2*gap,w=width-2*pad,half=(w-gap)/2;
  return {{{{pad,top,w,hero},{pad,top+hero+gap,w-pill-Dp(width,12),pill},
    {width-pad-pill,top+hero+gap,pill,pill},{pad,top+hero+pill+2*gap,half,hero},
    {pad+half+gap,top+hero+pill+2*gap,w-half-gap,hero}}},total,available>=total};
}
inline int HitHome(int width,int available,int x,int y) {
  auto layout=Home(width,0,available);if(!layout.valid)return -1;
  for(int i=0;i<5;++i)if(InRounded(layout.buttons[i],Dp(width,i==1 || i==2?30:i==0?22:17),x,y))return i;
  return -1;
}
inline int Dashboard(Canvas& c,int width,int top,int available,int selected,bool active) {
  auto layout=Home(width,top,available);if(!layout.valid)return 0;
  Metrics m(width);int pad=Dp(width,24),ring=Dp(width,1);
  const Color colors[]={theme::purple,theme::cyan,theme::cyan,theme::mint,theme::red};
  const Color arrows[]={theme::purple_pressed,theme::cyan_pressed,theme::cyan_pressed,theme::mint_pressed,theme::red_pressed};
  const char* labels[]={"Install or update","Terminal","","Power","Reset"};
  for(int i=0;i<5;++i) {
    auto b=layout.buttons[i];Color bg=active && selected==i?arrows[i]:colors[i];
    Surface(c,b,Dp(width,i==1 || i==2?30:i==0?22:17),bg,selected==i,text,ring);
    if(i==2) {Symbol(c,Inset(b,Dp(width,18)),Glyph::Gear);continue;}
    int icon=Dp(width,i==1?27:30),iy=i==0?b.y+(b.h-icon)/2:b.y+Dp(width,i==1?16:29);
    Rect ib{b.x+pad,iy,icon,icon};
    if(i==1)Symbol(c,ib,Glyph::Terminal);
    else if(i==3)Symbol(c,ib,Glyph::Refresh);
    else if(i==0)Symbol(c,ib,Glyph::Phone);
    else Symbol(c,ib,Glyph::Trash);
    int circle=Dp(width,27);
    Rect cb{b.x+b.w-Dp(width,i>=3?17:19)-circle,b.y+(i<=1?(b.h-circle)/2:b.h-Dp(width,53)),circle,circle};
    Rounded(c,cb,circle/2,arrows[i]);Chevron(c,cb);
    int tx=b.x+pad+(i==0?Dp(width,46):i==1?Dp(width,42):0);
    int ty=i<=1?b.y+(b.h-FontLineHeight(Font::DesignTitle,width))/2:b.y+b.h-Dp(width,53);
    std::string label=labels[i];
    if(GetLanguage()==Language::Chinese)label=Tr(i==0?"Install update":i==3?"Power":i==4?"Reset":labels[i]);
    Label(c,m,tx,ty,cb.x-tx-Dp(width,4),label,Font::DesignTitle,text);
  }
  return layout.height;
}
inline bool SeparatePower(Page page) {return page==Page::Reboot || page==Page::Fastboot;}
inline int ExtraGap(int width,Page page,bool before_last) {return SeparatePower(page) && before_last?Dp(width,27):0;}
inline CornerRadii Corners(int width,bool first,bool last) {return ListCorners(LayoutMetrics(width),first,last);}
inline void Card(Canvas& c,int width,int y,const std::string& name,bool selected,bool active,
                 bool first,bool last) {
  auto m=LayoutMetrics(width);Rect b=m.Card(y);
  Surface(c,b,Corners(width,first,last),active?track:surface,selected,text,Dp(width,ListButtonStyle::outline));
  int icon=Dp(width,ListButtonStyle::icon_size),pad=Dp(width,ListButtonStyle::padding),ix=b.x+pad,iy=b.y+(b.h-icon)/2;
  Glyph glyph=Glyph::Storage;Color color=text;std::string label=name;bool arrow=false;
  if(name=="Reboot system now") {glyph=Glyph::Android;color=green;label="System";}
  else if(name=="Enter fastboot") {glyph=Glyph::Warning;color={255,0,64};label="FastbootD";}
  else if(name=="Reboot to bootloader") {glyph=Glyph::Chip;label="Bootloader";}
  else if(name=="Reboot to recovery" || name=="Enter recovery") {glyph=Glyph::Refresh;label="Recovery";}
  else if(name=="Apply from ADB") {glyph=Glyph::Android;color=green;label="adb sideload";arrow=true;}
  else if(name=="Choose ZIP from internal storage") {label="package from local storage";arrow=true;}
  else if(name=="Power off")Symbol(c,{ix,iy,icon,icon},Glyph::Power);
  else arrow=true;
  if(name!="Power off")Symbol(c,{ix,iy,icon,icon},glyph,color);
  if(GetLanguage()==Language::Chinese) {
    label=name.rfind("Choose from ",0)==0?"从"+name.substr(12)+"安装":Tr(name);
  }
  int right=b.x+b.w-pad;
  if(arrow) {
    int circle=Dp(width,24);Rect cb{right-circle,b.y+(b.h-circle)/2,circle,circle};
    Rounded(c,cb,circle/2,{54,55,59});Chevron(c,cb);right=cb.x-Dp(width,12);
  }
  int tx=ix+icon+Dp(width,ListButtonStyle::label_gap);
    Font font=label=="package from local storage"?Font::Source:Font::DesignMenu;
    c.Text(tx,b.y+(b.h-FontLineHeight(font,width))/2,
    FitText(label,right-tx,FontPixels(font,width),false,false,FaceFor(font)),font,text,false);
}
inline int HitList(int width,Page page,int count,int first,int total,int x,int y) {
  auto m=LayoutMetrics(width);int top=0;
  for(int row=0;row<count;++row) {
    int index=first+row;bool separate=SeparatePower(page) && index==total-1;
    if(row>0)top+=ExtraGap(width,page,separate);
    bool bottom=index+1==total || (SeparatePower(page) && index+2==total);
    if(InRounded(m.Card(top),Corners(width,index==0 || separate,bottom),x,y))return row;
    top+=m.Pitch();
  }
  return -1;
}
inline void Adb(Canvas& c,int width,int height,bool waiting,double fraction,bool determinate,
                const std::vector<std::string>& logs,const std::vector<std::string>& details,
                bool back_selected=false) {
  Metrics m(width);c.Fill({0,0,width,height},background);
  Header(c,width,height,Page::None,back_selected,"ADB Sideload");
  int inset=Dp(width,18),w=width-2*inset,y=MenuTop(width,height,Page::Sources);
  Label(c,m,inset,y,w,"On your computer, run below command to send package:",Font::Instruction,text);
  y+=Dp(width,16);Label(c,m,inset,y,w,"adb sideload <filename>",Font::Command,text,true);
  y+=Dp(width,26);Label(c,m,inset,y,w,"Status",Font::DesignBody,text);y+=Dp(width,20);
  int bottom=FooterTop(width,height)-Dp(width,10),available=bottom-y;
  int panel_h=std::min(Dp(width,121),std::max(Dp(width,53),available/2));
  Rect panel{inset,y,w,panel_h};Rounded(c,panel,Dp(width,17),surface);
  if(waiting) {
    auto label=FitText(Tr("Waiting for package..."),w-Dp(width,42),FontPixels(Font::DesignMenu,width),false,false,Face::Flex);
    c.Text(inset+(w-TextWidth(label,FontPixels(Font::DesignMenu,width),false,false,Face::Flex))/2,
      y+(panel_h-FontLineHeight(Font::DesignMenu,width))/2,label,Font::DesignMenu,text,false);
  } else {
    double value=std::isfinite(fraction)?std::clamp(fraction,0.0,1.0):0;
    int pad=Dp(width,21),ty=y+Dp(width,12);
    Label(c,m,inset+pad,ty,w-2*pad,"Installing package:",panel_h>=Dp(width,100)?Font::DesignMenu:Font::Code,text);
    if(panel_h>=Dp(width,100)) {
      // ADB's protocol supplies /sideload/package.zip, not the host's original filename.
      Label(c,m,inset+pad,ty+Dp(width,24),w-2*pad,"package.zip",Font::Code,text,true);
    }
    int percent_y=panel.y+panel.h-Dp(width,51);
    Label(c,m,inset+pad,percent_y,w-2*pad,determinate?InstallPercent(value):"...",
      Font::DesignHeading,text);
    Rect bar{inset+pad,panel.y+panel.h-Dp(width,22),w-2*pad,Dp(width,12)};
    Rounded(c,bar,bar.h/2,track);
    if(determinate && value>0)Rounded(c,{bar.x,bar.y,std::max(1,static_cast<int>(bar.w*value)),bar.h},bar.h/2,green);
  }
  y+=panel_h+Dp(width,17);
  int lh=FontLineHeight(Font::Caption,width);
  if(bottom-y>=lh+Dp(width,29)) {
    Label(c,m,inset,y,w,"Logs",Font::DesignBody,text);y+=Dp(width,20);
    Rect box{inset,y,w,bottom-y};Rounded(c,box,Dp(width,17),surface);
    int pad=Dp(width,21),ty=y+Dp(width,12);
    std::vector<std::string> rows;
    for(const auto& log:logs)for(const auto& row:WrapText(log,w-2*pad,FontPixels(Font::Caption,width),false,true))rows.push_back(row);
    int visible=std::max(0,(box.h-Dp(width,24))/lh),start=std::max(0,static_cast<int>(rows.size())-visible);
    for(int i=start;i<static_cast<int>(rows.size());++i) {c.Text(inset+pad,ty,rows[i],Font::Caption,text,false);ty+=lh;}
  }
  Footer(c,width,height,details);
}
} // namespace recovery_m3e::design
