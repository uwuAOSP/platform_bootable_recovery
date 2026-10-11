/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "m3e_terminal.h"

namespace recovery_m3e::password {
enum class Action { Character, Symbols, Shift, Space, Delete, Clear, Cancel, Unlock };
struct Key {Rect bounds;std::string label;Action action;uint8_t character=0;};
struct Layout {Rect field;std::vector<Key> keys;int instruction;bool compact;};
inline Layout Keyboard(int width,int height,bool pin,bool symbols,bool shift) {
  int pad=Dp(width,18),gap=Dp(width,3),kh=Dp(width,38),top=height-4*(kh+gap)-Dp(width,10);
  Layout layout;auto& keys=layout.keys;
  if(pin) {
    int kw=(width-2*pad-2*gap)/3;
    for(int row=0;row<4;++row)for(int col=0;col<3;++col) {
      int index=row*3+col;uint8_t digit=index<9?'1'+index:'0';
      std::string label=index==9?"Clear input":index==11?"Delete last character":std::string(1,digit);
      auto action=index==9?Action::Clear:index==11?Action::Delete:Action::Character;
      keys.push_back({{pad+col*(kw+gap),top+row*(kh+gap),kw,kh},label,action,digit});
    }
  } else {
    auto letters=terminal::Keyboard(width,height,symbols,shift,false);
    for(size_t i=0;i+7<letters.size();++i)
      keys.push_back({letters[i].bounds,letters[i].label,Action::Character,static_cast<uint8_t>(letters[i].value[0])});
    const std::array<std::string,5> labels{{symbols?"ABC":"123",symbols?(shift?"Less":"More"):(shift?"abc":"ABC"),"Space","Delete last character","Clear input"}};
    const Action actions[]={Action::Symbols,Action::Shift,Action::Space,Action::Delete,Action::Clear};
    int kw=(width-2*pad-4*gap)/5;
    for(int i=0;i<5;++i)keys.push_back({{pad+i*(kw+gap),top+3*(kh+gap),kw,kh},labels[i],actions[i]});
  }
  int action_top=top-Dp(width,46),half=(width-2*pad-Dp(width,8))/2;
  keys.push_back({{pad,action_top,half,kh},"Cancel",Action::Cancel});
  keys.push_back({{pad+half+Dp(width,8),action_top,width-2*pad-half-Dp(width,8),kh},"Unlock storage",Action::Unlock});
  int first=design::Back(width).y+design::Back(width).h+Dp(width,16);
  int field_top=first+FontLineHeight(Font::DesignBody,width)+Dp(width,8),field_height=Dp(width,56);
  layout.compact=field_top+field_height+Dp(width,26)>action_top;
  if(layout.compact) {field_top=first-Dp(width,4);field_height=Dp(width,40);}
  layout.field={pad,field_top,width-2*pad,field_height};layout.instruction=first;
  return layout;
}
inline int HitKey(const Layout& layout,int x,int y) {
  for(size_t i=0;i<layout.keys.size();++i)if(InRounded(layout.keys[i].bounds,3,x,y))return i;
  return -1;
}
inline void Draw(Canvas& c,int width,int height,bool pin,size_t length,bool symbols,bool shift,int focus) {
  c.Fill({0,0,width,height},design::background);Metrics m(width);
  auto back=design::Back(width);Surface(c,back,back.h/2,design::surface,focus==-1,design::text,Dp(width,1));
  design::Symbol(c,Inset(back,Dp(width,14)),design::Glyph::Back);
  Label(c,m,back.x+back.w+Dp(width,14),back.y,width-back.x-back.w-Dp(width,30),
      "Unlock internal storage",Font::DesignPageTitle,design::text);
  auto layout=Keyboard(width,height,pin,symbols,shift);auto field=layout.field;
  if(!layout.compact)Label(c,m,field.x,layout.instruction,field.w,
      pin?"Enter your lock-screen PIN":"Enter your lock-screen password",Font::DesignBody,design::text);
  Surface(c,field,Dp(width,12),design::surface,true,design::green,Dp(width,1));
  int pad=Dp(width,16),diameter=Dp(width,6),pitch=Dp(width,12);
  size_t visible=std::min(length,static_cast<size_t>(std::max(0,(field.w-2*pad)/pitch)));
  for(size_t i=0;i<visible;++i)Rounded(c,{field.x+pad+static_cast<int>(i)*pitch,
      field.y+(field.h-diameter)/2,diameter,diameter},diameter/2,design::text);
  if(!length)Label(c,m,field.x+pad,field.y+(field.h-FontLineHeight(Font::DesignBody,width))/2,
      field.w-2*pad,pin?"Enter your lock-screen PIN":"Enter your lock-screen password",Font::DesignBody,design::muted);
  for(size_t i=0;i<layout.keys.size();++i) {
    const auto& key=layout.keys[i];auto b=key.bounds;bool unlock=key.action==Action::Unlock;
    Surface(c,b,Dp(width,8),unlock && length?theme::mint:design::surface,focus==static_cast<int>(i),design::green,Dp(width,1));
    std::string label=key.action==Action::Delete?"Del":key.action==Action::Clear?"Clear":key.label;
    if(GetLanguage()==Language::Chinese && key.action==Action::Delete)label="删除";
    else if(GetLanguage()==Language::Chinese && key.action==Action::Clear)label=Tr("Clear input");
    else label=Tr(label);
    Font font=key.action==Action::Character?Font::DesignMenu:Font::DesignSmall;
    label=FitText(label,b.w-Dp(width,4),FontPixels(font,width),false,false,Face::Flex);
    int tw=TextWidth(label,FontPixels(font,width),false,false,Face::Flex);
    c.Text(b.x+(b.w-tw)/2,b.y+(b.h-FontLineHeight(font,width))/2,label,font,design::text,false);
  }
}
} // namespace recovery_m3e::password
