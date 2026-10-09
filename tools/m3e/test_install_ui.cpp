/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
// Host rendering and geometry checks using the same primitives as ScreenRecoveryUI.
#include "recovery_ui/m3e_install.h"
#include "recovery_ui/m3e_design.h"
#include "recovery_ui/m3e_terminal.h"
#include "recovery_ui/m3e_password.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
using namespace recovery_m3e;

#ifdef M3E_INSTALL_ROUTING_TEST
#include "install_routing.inc"
#endif

struct PixelCanvas : Canvas {
  struct Run { Rect bounds; std::string text; Color color; };
  struct MaskRun {Rect bounds,ink;};
  int width,height;bool pixels;std::vector<uint8_t> rgb;std::vector<Run> runs;
  bool text_mask=false;std::vector<MaskRun> symbol_masks;
  PixelCanvas(int w,int h,bool draw=false):width(w),height(h),pixels(draw),rgb(draw?w*h*3:0) {}
  void Fill(Rect b,Color c) override {
    assert(b.x>=0 && b.y>=0 && b.w>=0 && b.h>=0);
    assert(b.x+b.w<=width && b.y+b.h<=height);
    if(!pixels) return;
    for(int y=b.y;y<b.y+b.h;++y)for(int x=b.x;x<b.x+b.w;++x) {
      size_t i=(y*width+x)*3;rgb[i]=c.r;rgb[i+1]=c.g;rgb[i+2]=c.b;
    }
  }
  void Text(int x,int y,const std::string& s,Font f,Color c,bool bold) override {
    if(s.empty()) return;
    Rect b{x,y,TextWidth(s,FontPixels(f,width),bold,Monospace(f),FaceFor(f)),FontLineHeight(f,width)};
    assert(x>=0 && y>=0 && x+b.w<=width && y+b.h<=height);
    runs.push_back({b,s,c});
    if(!pixels) return;
    auto raster=RasterText(s,FontPixels(f,width),bold,Monospace(f),FaceFor(f));
    text_mask=true;
    Mask({x,y,raster.width,raster.height},raster.alpha,c);
    text_mask=false;
  }
  void TextMask(Rect b,const std::vector<uint8_t>& alpha,Color c) override {
    text_mask=true;Mask(b,alpha,c);text_mask=false;
  }
  void Mask(Rect b,const std::vector<uint8_t>& alpha,Color c) override {
    assert(b.x>=0 && b.y>=0 && b.w>=0 && b.h>=0 && b.x+b.w<=width && b.y+b.h<=height);
    assert(alpha.size()==static_cast<size_t>(b.w*b.h));
    if(!text_mask) {
      int left=b.w,top=b.h,right=0,bottom=0;
      for(int y=0;y<b.h;++y)for(int x=0;x<b.w;++x)if(alpha[y*b.w+x]) {
        left=std::min(left,x);top=std::min(top,y);right=std::max(right,x+1);bottom=std::max(bottom,y+1);
      }
      symbol_masks.push_back({b,{b.x+left,b.y+top,right-left,bottom-top}});
    }
    if(!pixels)return;
    for(int sy=0;sy<b.h;++sy)for(int sx=0;sx<b.w;++sx) {
      int dx=b.x+sx,dy=b.y+sy;if(dx<0||dy<0||dx>=width||dy>=height)continue;
      int a=alpha[sy*b.w+sx];size_t i=(dy*width+dx)*3;
      rgb[i]=(rgb[i]*(255-a)+c.r*a+127)/255;
      rgb[i+1]=(rgb[i+1]*(255-a)+c.g*a+127)/255;
      rgb[i+2]=(rgb[i+2]*(255-a)+c.b*a+127)/255;
    }
  }
  bool Has(const std::string& s) const {
    return std::any_of(runs.begin(),runs.end(),[&](const Run& r){return r.text==s;});
  }
  void Blit(const std::string& path,Rect b) {
    if(!pixels || !b.w) return;
    std::ifstream f(path,std::ios::binary);std::string magic;int w,h,max;
    f>>magic>>w>>h>>max;f.get();assert(magic=="P6" && max==255 && w==b.w && h==b.h);
    std::vector<uint8_t> source(w*h*3);f.read(reinterpret_cast<char*>(source.data()),source.size());
    assert(f.good());
    for(int y=0;y<h;++y)std::copy_n(source.data()+y*w*3,w*3,rgb.data()+((b.y+y)*width+b.x)*3);
  }
  void Write(const std::string& path) const {
    std::ofstream f(path,std::ios::binary);f<<"P6\n"<<width<<" "<<height<<"\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()),rgb.size());assert(f.good());
  }
};
constexpr InstallStage stages[]={InstallStage::WAITING,InstallStage::VERIFYING,
    InstallStage::INSTALLING,InstallStage::SUCCESS,InstallStage::ERROR,InstallStage::CANCELLED};
const char* names[]={"waiting","verifying","installing","success","error","cancelled"};

#ifdef M3E_INSTALL_ROUTING_TEST
#include "fastboot_flow.inc"
#include "password_flow.inc"
#include "terminal_flow.inc"
#endif

void Render(const std::string& out,int w,int h,bool zh,int index,bool pixels=false) {
  SetScaleBasis(w,h);SetLanguage(zh?Language::Chinese:Language::English);
  PixelCanvas c(w,h,pixels);Metrics m(w);auto p=Palette::ForMode(false);
  auto stage=stages[index];int rows=stage==InstallStage::WAITING?1:index>=3?2:0;
  int top=Dp(w,24),bottom=h-top;
  int y=DrawInstallHeader(c,m,top,bottom,rows,false,{},p);
  int logo_width=pixels?800:Dp(w,200),logo_height=pixels?568:Dp(w,142);
  bool detailed=stage==InstallStage::VERIFYING || stage==InstallStage::INSTALLING;
  auto layout=InstallationLayout(m,y,bottom,rows,logo_width,logo_height,detailed);
  assert(layout.panel.y>=y && layout.panel.y+layout.panel.h<=bottom);
  if(layout.logo.w) {
    assert(layout.logo.x==(w-logo_width)/2 && layout.logo.y==y);
    assert(layout.panel.y==y+logo_height+Dp(w,16));
    if(pixels)c.Blit(out+"/logo.ppm",layout.logo);
  } else {
    assert(layout.panel.y==y); // No empty gap when the bitmap cannot fit.
  }
  auto without_logo=InstallationLayout(m,y,bottom,rows,0,0,detailed);
  assert(without_logo.logo.w==0 && without_logo.panel.y==y);
  auto oversized=InstallationLayout(m,y,bottom,rows,w+1,h+1,detailed);
  assert(oversized.logo.w==0 && oversized.panel.y==y);
  assert(layout.panel.h==without_logo.panel.h); // Artwork cannot reduce readable status space.
  if(w==1220 && h==2712)assert((layout.logo.w>0)==!detailed);
  if(!CompactInstallHeader(m,top,bottom,rows)) {
    assert(c.Has("uwuAOSP"));
  }
  c.runs.clear();
  std::vector<std::string> logs;
  if(stage==InstallStage::VERIFYING) logs={"Verifying update package..."};
  if(stage==InstallStage::INSTALLING) logs={"Installing update...","Step 1/2",
      "Opening partition system_a","Applying operations to system_a",
      "Completed 154/230 operations","Opening partition vendor_a",
      "Applying operations to vendor_a","Completed 84/100 operations",
      "Verifying partition vendor_a","Step 2/2","Finalizing update..."};
  if(stage==InstallStage::SUCCESS) logs={"Install completed with status 0."};
  if(stage==InstallStage::ERROR) logs={"signature verification failed","Installation aborted."};
  DrawInstallPanel(c,m,layout.panel,stage,0.42,true,false,logs,p);
  assert(!c.runs.empty()); // A readable result is required even on compact screens.
  for(const auto& run:c.runs) {
    assert(run.bounds.x>=layout.panel.x && run.bounds.y>=layout.panel.y);
    assert(run.bounds.x+run.bounds.w<=layout.panel.x+layout.panel.w);
    assert(run.bounds.y+run.bounds.h<=layout.panel.y+layout.panel.h);
  }
  if(stage==InstallStage::WAITING) assert(c.Has("adb sideload <filename>"));
  if(stage==InstallStage::VERIFYING || stage==InstallStage::INSTALLING) assert(c.Has("42%"));
  else assert(!c.Has("42%"));
  assert(c.Has("100%")== (stage==InstallStage::SUCCESS));
  if(detailed && w==1220 && h==2712 && stage==InstallStage::INSTALLING) {
    assert(c.Has("Opening partition system_a") && c.Has("Finalizing update..."));
    assert(std::count_if(c.runs.begin(),c.runs.end(),[](const auto& r) {
      return r.text.find("partition")!=std::string::npos;
    })>=3);
  }
  if(rows) {
    assert(layout.menu_y>=layout.panel.y+layout.panel.h);
    assert(VisibleCount(bottom-layout.menu_y,m.row_height,m.gap)>=rows);
    for(int i=0;i<rows;++i) {
      int row_y=layout.menu_y+i*m.Pitch();
      DrawCard(c,m,row_y,rows==1?"Cancel":i==0?"Continue":"View recovery logs",i==0,false,p,i==0,i==rows-1);
      assert(HitRow(m,0,rows,0,w/2,row_y+m.row_height/2-layout.menu_y)==i);
    }
  }
  DrawBattery(c,m,top,87,false,p);
  if(pixels)c.Write(out+"/"+names[index]+(zh?"-zh.ppm":"-en.ppm"));
}
void CheckProgress() {
  SetScaleBasis(360,800);SetLanguage(Language::English);Metrics m(360);Palette p;
  for(double value:{-1.0,0.0,0.42,1.0,5.0,std::numeric_limits<double>::quiet_NaN()}) {
    PixelCanvas c(360,800);
    DrawInstallPanel(c,m,{24,24,312,188},InstallStage::INSTALLING,value,true,false,{},p);
    assert(c.Has(InstallPercent(value)));
  }
  PixelCanvas unknown(360,800);
  DrawInstallPanel(unknown,m,{24,24,312,188},InstallStage::INSTALLING,0.9,false,true,{},p);
  for(const auto& r:unknown.runs)assert(r.text.find('%')==std::string::npos);
  assert(std::string(InstallTitle(InstallStage::INSTALLING,true))=="Installing security update");
  for(auto stage:{InstallStage::ERROR,InstallStage::SUCCESS,InstallStage::CANCELLED}) {
    PixelCanvas c(360,800);DrawInstallPanel(c,m,{24,24,312,188},stage,0.42,true,false,{},p);
    auto expected=p.text; // Status titles use the common text color; the bar uses the accent.
    assert(c.runs[0].color.r==expected.r && c.runs[0].color.g==expected.g && c.runs[0].color.b==expected.b);
  }
  for(auto stage:{InstallStage::FLASH_PREPARING,InstallStage::FLASH_WRITING,
      InstallStage::FLASH_VERIFYING,InstallStage::FLASH_SUCCESS,InstallStage::FLASH_ERROR}) {
    assert(recovery_ui::IsFlashStage(stage));
    PixelCanvas c(360,800);DrawInstallPanel(c,m,{24,24,312,188},stage,0.42,true,false,{},p);
    assert(!c.runs.empty());
    // Long English titles may be ellipsized to leave room for the percentage.
    assert(c.runs[0].text.rfind(std::string(InstallTitle(stage,false)).substr(0,8),0)==0);
    assert(c.Has("100%")== (stage==InstallStage::FLASH_SUCCESS));
    if(stage==InstallStage::FLASH_WRITING || stage==InstallStage::FLASH_VERIFYING) assert(c.Has("42%"));
    if(stage==InstallStage::FLASH_ERROR) assert(c.runs[0].color.r==p.text.r);
    assert(!c.Has("adb sideload <filename>"));
  }
}
void RenderList(const std::string& out,int w,int h,bool zh,int count,int first_index=0,
                bool pixels=false) {
  SetScaleBasis(w,h);SetLanguage(zh?Language::Chinese:Language::English);
  Metrics m(w);auto p=Palette::ForMode(false);PixelCanvas c(w,h,pixels);
  c.Fill({0,0,w,h},p.background);
  int top=Dp(w,24);
  int visible=std::min(count-first_index,VisibleCount(h-2*top,m.row_height,m.gap));
  const std::vector<std::string> items{"Apply from ADB","Choose ZIP from internal storage",
                                       "Flash partition image","Choose from USB"};
  // Render, select and press every visible item, including scrolled list windows.
  for(int selected=0;selected<visible;++selected)for(bool active:{false,true}) {
    for(int row=0;row<visible;++row) {
      int index=first_index+row,y=top+row*m.Pitch();
      DrawCard(c,m,y,items[index],row==selected,active && row==selected,p,index==0,index==count-1);
      assert(HitRow(m,top,visible,selected,w/2,y+m.row_height/2,first_index,count)==row);
      Rect card=m.Card(y);int x=card.x+m.InnerRadius()+Dp(w,1);
      for(bool bottom:{false,true}) {
        int edge=bottom?card.y+card.h-1:card.y;
        bool outer=bottom?index==count-1:index==0;
        assert(HitRow(m,top,visible,selected,x,edge,first_index,count)==(outer?-1:row));
        if(pixels) {
          size_t pixel=(edge*w+x)*3;
          bool painted=c.rgb[pixel]!=p.background.r || c.rgb[pixel+1]!=p.background.g ||
                       c.rgb[pixel+2]!=p.background.b;
          assert(painted==!outer);
        }
      }
      if(row+1<visible)assert(HitRow(m,top,visible,selected,w/2,y+m.row_height,
                                    first_index,count)==-1);
    }
  }
  if(pixels)c.Write(out+"/grouped-list-"+std::to_string(count)+(zh?"-zh.ppm":"-en.ppm"));
}
void RenderDesign(const std::string& out,int w,int h,bool zh,int page,bool pixels=false) {
  SetScaleBasis(w,h);SetLanguage(zh?Language::Chinese:Language::English);
  PixelCanvas c(w,h,pixels);c.Fill({0,0,w,h},design::background);
  const std::vector<std::string> details{"Product name - diting","Version 17.0.130 (2026-10-05)"};
  if(page>=7) {
    password::Draw(c,w,h,page==7,8,false,false,-2);
#ifdef M3E_INSTALL_ROUTING_TEST
    PasswordFlow::Check(w,h);
#endif
  }
  else if(page==6) {
#ifdef M3E_INSTALL_ROUTING_TEST
    FastbootFlow::Render(c,w,h);
#else
    design::Header(c,w,h,design::Page::Fastboot);design::FastbootStatus(c,w,h);
    design::Footer(c,w,h,details);
#endif
  }
  else if(page>=3 && page<=4)design::Adb(c,w,h,page==3,0.04,true,
      page==3?std::vector<std::string>{}:std::vector<std::string>{"Finding package...","Verifying package...","Installing updates","Step 1/2"},details);
  else if(page==5) {
    terminal::Draw(c,w,h,"ls /system/bin",{"# pwd","/","# ls /system/bin"},false,false,-1);
    DrawPageFooter(c,w,h,details);
#ifdef M3E_INSTALL_ROUTING_TEST
    if(!pixels)TerminalFlow::Check(w,h,out);
#endif
  }
  else {
    auto kind=page==0?design::Page::Home:page==1?design::Page::Reboot:design::Page::Sources;
    design::Header(c,w,h,kind);int top=design::MenuTop(w,h,kind),available=design::FooterTop(w,h)-Dp(w,10)-top;
    if(page==0 && design::Home(w,top,available).valid) {
      design::Dashboard(c,w,top,available,-1,false);
      for(const char* label:{"Install or update","Terminal","Power","Reset"})assert(c.Has(Tr(label)));
      for(int i=0;i<5;++i) {
        auto b=design::Home(w,0,available).buttons[i];assert(design::HitHome(w,available,b.x+b.w/2,b.y+b.h/2)==i);
      }
    } else {
      auto m=design::LayoutMetrics(w);
      std::vector<std::string> items=page==1?std::vector<std::string>{"Reboot system now","Enter fastboot","Reboot to bootloader","Reboot to recovery","Power off"}:
          page==2?std::vector<std::string>{"Apply from ADB","Choose ZIP from internal storage","Flash partition image","Choose from USB"}:
          std::vector<std::string>{"Apply update","Terminal","Settings","Reboot options","Factory reset"};
      int count=std::min(static_cast<int>(items.size()),VisibleCount(available-(page==1?Dp(w,27):0),m.row_height,m.gap));
      int y=top;
      for(int i=0;i<count;++i) {
        bool last=i+1==static_cast<int>(items.size()),separate=page==1 && last;
        if(i>0)y+=design::ExtraGap(w,kind,separate);
        design::Card(c,w,y,items[i],false,false,i==0 || separate,last || (page==1 && i+2==static_cast<int>(items.size())));
        assert(design::HitList(w,kind,count,0,items.size(),w/2,y-top+m.row_height/2)==i);
        if(i>0)assert(design::HitList(w,kind,count,0,items.size(),w/2,y-top-1)==-1);
        y+=m.Pitch();
      }
    }
    design::Footer(c,w,h,details);
  }
  if(page!=5 && page<7)design::Battery(c,w,82,false);
  if(page<=6) {
    auto label=std::find_if(c.runs.begin(),c.runs.end(),[](const PixelCanvas::Run& run){
      return run.text==Tr("Recovery version");
    });
    assert(label!=c.runs.end() && label+1!=c.runs.end());
    const auto& version=*(label+1);
    assert(label->bounds.x+label->bounds.w==w-Dp(w,24));
    assert(version.bounds.x+version.bounds.w==label->bounds.x+label->bounds.w);
    assert(version.bounds.y==label->bounds.y+FontLineHeight(Font::Code,w));
    assert(c.Has("uwuAOSP recovery") && c.Has("codename: diting"));
  }
  // Check visible icon alignment in every complete page, including small screens.
  for(const auto& mask:c.symbol_masks) {
    assert(mask.ink.w>0 && mask.ink.h>0);
    assert(std::abs((mask.ink.x+mask.ink.w/2.0)-(mask.bounds.x+mask.bounds.w/2.0))<=1);
    assert(std::abs((mask.ink.y+mask.ink.h/2.0)-(mask.bounds.y+mask.bounds.h/2.0))<=1);
  }
  // Check the font selection as part of the complete bilingual page flow.
  assert(FaceFor(Font::DesignBrand)==Face::Outfit && FaceFor(Font::DesignRecoveryTitle)==Face::Flex);
  assert(FaceFor(Font::Code)==Face::Code && FaceFor(Font::DesignMenu)==Face::Flex);
  assert(TextWidth("iiii",FontPixels(Font::Code,w),false,true)==TextWidth("WWWW",FontPixels(Font::Code,w),false,true));
  assert(design::text.r==230 && design::text.g==230 && design::text.b==230);
  if(pixels)c.Write(out+"/design-"+std::to_string(page)+(zh?"-zh.ppm":"-en.ppm"));
}
int main(int argc,char** argv) {
  assert(argc==2);std::string out=argv[1];
  CheckProgress();
#ifdef M3E_INSTALL_ROUTING_TEST
  CheckRouting();
#endif
  for(auto [w,h]:std::vector<std::pair<int,int>>{{1220,2712},{720,1280},{360,640},
        {320,480},{1280,720},{320,240},{1600,2560}})
    for(bool zh:{false,true}) {
      for(int i=0;i<6;++i)Render(out,w,h,zh,i);
      for(int count:{1,2,4})for(int first=0;first<count;++first)RenderList(out,w,h,zh,count,first);
      for(int page=0;page<9;++page)RenderDesign(out,w,h,zh,page);
    }
  for(bool zh:{false,true})for(int i=0;i<6;++i)Render(out,1220,2712,zh,i,true);
  for(bool zh:{false,true})for(int count:{1,2,4})RenderList(out,1220,2712,zh,count,0,true);
  for(bool zh:{false,true})for(int page=0;page<9;++page)RenderDesign(out,1220,2712,zh,page,true);
  std::cout<<"PASS: six stages, two languages, seven screen sizes, grouped lists with selection, scrolling and matching touch geometry\n";
}
