/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "m3e_font.h"
#include "m3e_cjk.h"
#include "m3e_design_fonts.h"
#include "m3e_strings.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>
namespace recovery_m3e {
enum class Font { Body, Menu, Title, Small, Heading, DesignTitle, DesignBrand, Code, Caption, Command, Instruction, Source, DesignBody, DesignMenu, DesignSmall, DesignHeading, DesignRecoveryTitle, DesignPageTitle };
enum class Face { Legacy, Flex, Outfit, Code };
inline Face FaceFor(Font font) {
  switch(font) {
    case Font::DesignBrand: return Face::Outfit;
    case Font::Code: case Font::Caption: case Font::Command: return Face::Code;
    case Font::Body: case Font::Menu: case Font::Title: case Font::Small: case Font::Heading: return Face::Legacy;
    default: return Face::Flex;
  }
}
inline bool Monospace(Font font) {return font==Font::Code || font==Font::Caption || font==Font::Command;}
// Scale basis: the shorter screen side, set once the framebuffer size is known.
// Layout widths stay full-screen; only dp scaling is capped, so portrait screens
// (basis == width) are unchanged while landscape and tablet screens scale by
// their height instead of growing with the long side. 0 means unset.
inline int& ScaleBasisStorage() { static int basis = 0; return basis; }
inline void SetScaleBasis(int width, int height) { ScaleBasisStorage() = std::max(0, std::min(width, height)); }
// Width used for dp scaling: never wider than the basis.
inline int ScaleWidth(int width) {
  int basis = ScaleBasisStorage();
  return basis > 0 ? std::min(width, basis) : width;
}
inline int Dp(int width, float dp) {
  return std::max(1, static_cast<int>(std::lround(ScaleWidth(width) * dp / 360.f)));
}
inline int FontPixels(Font font, int width) {
  switch(font) {
    case Font::DesignTitle: return Dp(width,GetLanguage()==Language::Chinese?18:22);
    case Font::DesignRecoveryTitle: return Dp(width,28);
    case Font::DesignPageTitle: return Dp(width,22);
    case Font::DesignBody: return Dp(width,GetLanguage()==Language::Chinese?13:14);
    case Font::DesignMenu: return Dp(width,GetLanguage()==Language::Chinese?16:18);
    case Font::DesignSmall: return Dp(width,12);
    case Font::DesignHeading: return Dp(width,GetLanguage()==Language::Chinese?22:25);
    case Font::DesignBrand: return Dp(width,28);
    case Font::Code: return Dp(width,11);
    case Font::Caption: return Dp(width,9);
    case Font::Command: return Dp(width,14);
    case Font::Instruction: return Dp(width,11);
    case Font::Source: return Dp(width,16);
    case Font::Small: return Dp(width,12);
    case Font::Body: return Dp(width,14);
    case Font::Menu: return Dp(width,18);
    case Font::Heading: return Dp(width,25);
    case Font::Title: return Dp(width,44);
  }
  return Dp(width,14);
}
// All widths, wrapping and rasterization use the same UTF-8 codepoint boundaries.
inline uint32_t NextCodepoint(const std::string& text,size_t& pos) {
  if(pos>=text.size()) return 0;
  auto byte=[&](size_t i){return static_cast<unsigned char>(text[i]);};
  unsigned char first=byte(pos++);
  if(first<0x80) return first;
  int tail=first>=0xc2 && first<=0xdf?1:first>=0xe0 && first<=0xef?2:first>=0xf0 && first<=0xf4?3:0;
  if(tail==0 || pos+tail>text.size()) return 0xfffd;
  uint32_t value=first & ((1<<(6-tail))-1);
  for(int i=0;i<tail;++i) if((byte(pos+i)&0xc0)!=0x80) return 0xfffd;
  for(int i=0;i<tail;++i) value=(value<<6)|(byte(pos++)&0x3f);
  if((tail==1 && value<0x80)||(tail==2 && value<0x800)||(tail==3 && value<0x10000)||
     value>0x10ffff||(value>=0xd800 && value<=0xdfff)) return 0xfffd;
  return value;
}
inline int GlyphIndex(uint32_t ch) {
  if(ch>=32 && ch<=126) return ch-32;
  const auto* end=fontdata::kCjkCodes+fontdata::kCjkCount;
  const auto* found=std::lower_bound(fontdata::kCjkCodes,end,ch);
  return found!=end && *found==ch ? 95+static_cast<int>(found-fontdata::kCjkCodes) : '?'-32;
}
inline const fontdata::Glyph& GlyphAt(int index,bool bold) {
  if(index<95) return bold?fontdata::kBold[index]:fontdata::kRegular[index];
  return bold?fontdata::kCjkBold[index-95]:fontdata::kCjkRegular[index-95];
}
inline const fontdata::Glyph& GlyphFor(uint32_t ch,bool bold,Face face=Face::Legacy) {
  int index=GlyphIndex(ch);
  if(index>=95 || face==Face::Legacy) return GlyphAt(index,bold);
  switch(face) {
    case Face::Flex: return bold?fontdata::kFlexBold[index]:fontdata::kFlexRegular[index];
    case Face::Outfit: return bold?fontdata::kOutfitBold[index]:fontdata::kOutfitRegular[index];
    case Face::Code: return bold?fontdata::kCodeBold[index]:fontdata::kCodeRegular[index];
    default: return GlyphAt(index,bold);
  }
}
inline int TextAscent(Face face=Face::Legacy) {
  int ascent=face==Face::Flex?fontdata::kFlexAscent:face==Face::Outfit?fontdata::kOutfitAscent:
      face==Face::Code?fontdata::kCodeAscent:fontdata::kAscent;
  // Preserve the original page baseline when the design adds new CJK glyphs.
  return std::max(ascent,face==Face::Legacy?fontdata::kLegacyCjkAscent:fontdata::kCjkAscent);
}
inline int TextDescent(Face face=Face::Legacy) {
  int descent=face==Face::Flex?fontdata::kFlexDescent:face==Face::Outfit?fontdata::kOutfitDescent:
      face==Face::Code?fontdata::kCodeDescent:fontdata::kDescent;
  return std::max(descent,face==Face::Legacy?fontdata::kLegacyCjkDescent:fontdata::kCjkDescent);
}
inline int TextWidth(const std::string& text,int pixels,bool bold=false,bool mono=false,Face face=Face::Legacy) {
  if(mono)face=Face::Code;
  double advance=0;size_t pos=0;
  while(pos<text.size()) {
    auto ch=NextCodepoint(text,pos);
    advance+=GlyphFor(ch,bold,face).advance;
  }
  return static_cast<int>(std::ceil(advance*pixels/(64.0*fontdata::kSize)));
}
inline int LineHeight(int pixels,Face face=Face::Legacy) {
  int height=TextAscent(face)+TextDescent(face);
  return static_cast<int>(std::ceil(pixels*double(height)/fontdata::kSize));
}
inline std::string FitText(std::string text,int width,int pixels,bool bold=false,bool mono=false,Face face=Face::Legacy) {
  if(TextWidth(text,pixels,bold,mono,face)<=width) return text;
  if(TextWidth("...",pixels,bold,mono,face)>width) return {};
  std::vector<size_t> boundaries{0};size_t pos=0;
  while(pos<text.size()) {NextCodepoint(text,pos);boundaries.push_back(pos);}
  while(boundaries.size()>1) {
    boundaries.pop_back();
    std::string candidate=text.substr(0,boundaries.back())+"...";
    if(TextWidth(candidate,pixels,bold,mono,face)<=width) return candidate;
  }
  return "...";
}
inline std::vector<std::string> WrapText(const std::string& text,int width,int pixels,bool bold=false,bool mono=false,Face face=Face::Legacy) {
  std::vector<std::string> lines;size_t pos=0;
  while(pos<text.size()) {
    size_t end=pos,last_space=std::string::npos;
    while(end<text.size() && text[end]!='\n') {
      size_t next=end;NextCodepoint(text,next);
      if(TextWidth(text.substr(pos,next-pos),pixels,bold,mono,face)>width) break;
      if(text[end]==' ') last_space=end;
      end=next;
    }
    if(end==pos && text[pos]!='\n') NextCodepoint(text,end);
    if(end<text.size() && text[end]!='\n' && last_space!=std::string::npos && last_space>pos) end=last_space;
    lines.push_back(text.substr(pos,end-pos));pos=end;
    if(pos<text.size() && text[pos]=='\n') ++pos;
    while(pos<text.size() && text[pos]==' ') ++pos;
  }
  return lines;
}
inline const std::vector<uint8_t>& LegacyGlyphMask(uint32_t ch,bool bold) {
  static const auto masks=[] {
    constexpr int count=95+fontdata::kCjkCount;
    std::array<std::vector<uint8_t>,2*count> result;
    for(int style=0;style<2;++style) for(int index=0;index<count;++index) {
      const auto& g=GlyphAt(index,style);
      const auto* source=index<95?(style?fontdata::kBoldData:fontdata::kRegularData):
                                  (style?fontdata::kCjkBoldData:fontdata::kCjkRegularData);
      auto& dest=result[style*count+index];dest.reserve(g.w*g.h);
      for(uint32_t i=g.offset;i<g.offset+g.length;i+=2) dest.insert(dest.end(),source[i],source[i+1]);
    }
    return result;
  }();
  return masks[(bold?95+fontdata::kCjkCount:0)+GlyphIndex(ch)];
}
inline std::vector<uint8_t> DecodeGlyph(const fontdata::Glyph& glyph,const uint8_t* source) {
  std::vector<uint8_t> mask;mask.reserve(glyph.w*glyph.h);
  for(uint32_t i=glyph.offset;i<glyph.offset+glyph.length;i+=2)mask.insert(mask.end(),source[i],source[i+1]);
  return mask;
}
inline const std::vector<uint8_t>& GlyphMask(uint32_t ch,bool bold,Face face=Face::Legacy) {
  if(GlyphIndex(ch)>=95 || face==Face::Legacy)return LegacyGlyphMask(ch,bold);
  static const auto masks=[] {
    std::array<std::vector<uint8_t>,3*2*95> result;
    const uint8_t* sources[]={fontdata::kFlexRegularData,fontdata::kFlexBoldData,
      fontdata::kOutfitRegularData,fontdata::kOutfitBoldData,fontdata::kCodeRegularData,fontdata::kCodeBoldData};
    for(int family=0;family<3;++family)for(int style=0;style<2;++style)for(int index=0;index<95;++index)
      result[(family*2+style)*95+index]=DecodeGlyph(GlyphFor(index+32,style,static_cast<Face>(family+1)),sources[family*2+style]);
    return result;
  }();
  return masks[((static_cast<int>(face)-1)*2+bold)*95+GlyphIndex(ch)];
}
inline int FontLineHeight(Font font,int width) {return LineHeight(FontPixels(font,width),FaceFor(font));}
struct TextBitmap { int width,height; std::vector<uint8_t> alpha; };
inline TextBitmap RasterText(const std::string& text,int pixels,bool bold,bool mono=false,Face face=Face::Legacy) {
  if(mono)face=Face::Code;
  TextBitmap result{std::max(1,TextWidth(text,pixels,bold,mono,face)+2),LineHeight(pixels,face),{}};
  result.alpha.resize(result.width*result.height);
  const double scale=double(pixels)/fontdata::kSize;
  double cursor=0;
  size_t pos=0;
  while(pos<text.size()) {
    uint32_t ch=NextCodepoint(text,pos);
    const auto& g=GlyphFor(ch,bold,face);
    const auto& mask=GlyphMask(ch,bold,face);
    double advance=g.advance*scale/64;
    double scale_x=scale;
    const int left=std::lround(cursor+g.left*scale);
    const int top=std::lround((TextAscent(face)+g.top)*scale);
    int w=std::ceil(g.w*scale_x), h=std::ceil(g.h*scale);
    auto sample=[&](int x,int y)->double {
      return x<0 || y<0 || x>=g.w || y>=g.h ? 0 : mask[y*g.w+x];
    };
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
      int dx=left+x,dy=top+y;
      if(dx<0 || dy<0 || dx>=result.width || dy>=result.height) continue;
      double sx=(x+0.5)/scale_x-0.5, sy=(y+0.5)/scale-0.5;
      int ix=std::floor(sx), iy=std::floor(sy);
      double ax=sx-ix,ay=sy-iy;
      double value=(sample(ix,iy)*(1-ax)+sample(ix+1,iy)*ax)*(1-ay)
                  +(sample(ix,iy+1)*(1-ax)+sample(ix+1,iy+1)*ax)*ay;
      auto& dest=result.alpha[dy*result.width+dx];
      dest=std::max(dest,static_cast<uint8_t>(std::clamp(std::lround(value),0L,255L)));
    }
    cursor+=advance;
  }
  return result;
}
} // namespace recovery_m3e
