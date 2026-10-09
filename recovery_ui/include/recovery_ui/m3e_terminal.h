/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "m3e_design.h"

namespace recovery_m3e::terminal {
struct Key {Rect bounds;std::string label,value;};
inline std::vector<Key> Keyboard(int width,int height,bool symbols,bool shift,bool footer=true) {
  int bottom=footer?PageFooterTop(width,height):height;
  int pad=Dp(width,10),gap=Dp(width,3),kh=Dp(width,38),top=bottom-4*(kh+gap)-Dp(width,10);
  std::vector<Key> keys;
  const std::array<std::string,3> letters{{"qwertyuiop","asdfghjkl","zxcvbnm"}};
  const std::array<std::string,3> punctuation{{"1234567890","/.-_~|&;$","<>*?=()"}};
  const std::array<std::string,3> more{{"\"'`\\[]{}!#","@%:+,^()","$&|;-_/"}};
  for(int row=0;row<3;++row) {
    auto chars=symbols?(shift?more[row]:punctuation[row]):letters[row];int count=chars.size(),kw=(width-2*pad-(count-1)*gap)/count;
    int left=(width-count*kw-(count-1)*gap)/2;
    for(int i=0;i<count;++i) {
      std::string value=chars.substr(i,1);if(!symbols && shift)value[0]=static_cast<char>(value[0]-'a'+'A');
      keys.push_back({{left+i*(kw+gap),top+row*(kh+gap),kw,kh},value,value});
    }
  }
  const std::array<std::string,7> labels{{symbols?"ABC":"123",symbols?(shift?"Less":"More"):(shift?"abc":"ABC"),"Space","Del","Enter","^C","Clear"}};
  int kw=(width-2*pad-6*gap)/7;
  for(int i=0;i<7;++i)keys.push_back({{pad+i*(kw+gap),top+3*(kh+gap),kw,kh},labels[i],{}});
  return keys;
}
inline int HitKey(const std::vector<Key>& keys,int x,int y) {
  for(size_t i=0;i<keys.size();++i)if(InRounded(keys[i].bounds,3,x,y))return i;
  return -1;
}
// Consume terminal control characters without exposing escape sequences in the log view.
class Output {
 public:
  void Append(const char* bytes,size_t count) {
    for(size_t i=0;i<count;++i) {
      unsigned char ch=bytes[i];
      if(escape_) {
        if(ch=='[' || ch==']') {sequence_=true;continue;}
        if(!sequence_ || (ch>=0x40 && ch<=0x7e)) {escape_=sequence_=false;}
        continue;
      }
      if(ch==27) {escape_=true;sequence_=false;}
      else if(ch=='\n') {if(lines.back().size()>4096)lines.back().resize(4096);lines.emplace_back();column_=0;}
      else if(ch=='\r')column_=0;
      else if(ch=='\b') {if(column_>0)--column_;}
      else if(ch=='\t') {for(int n=4-column_%4;n>0;--n)Put(' ');}
      else if(ch>=32 && ch!=127)Put(ch);
    }
    if(lines.size()>256) {first_line_+=lines.size()-256;lines.erase(lines.begin(),lines.end()-256);}
    ++revision_;
  }
  void Clear() {lines={""};column_=0;first_line_=0;escape_=sequence_=false;++revision_;}
  uint64_t FirstLine() const {return first_line_;}
  uint64_t Revision() const {return revision_;}
  std::vector<std::string> lines{""};
 private:
  void Put(char ch) {
    auto& line=lines.back();if(column_>=4096)return;
    if(column_<line.size())line[column_]=ch;else line+=ch;
    ++column_;
  }
  size_t column_=0;bool escape_=false,sequence_=false;
  uint64_t first_line_=0,revision_=0;
};

inline Rect OutputBounds(int width,int height) {
  auto back=design::Back(width);int pad=Dp(width,12),top=back.y+back.h+pad;
  int input_y=Keyboard(width,height,false,false).front().bounds.y-Dp(width,32);
  return {pad,top,width-2*pad,std::max(0,input_y-top-Dp(width,5))};
}
// Keep the first visible logical line and its wrapped row anchored as output arrives.
class Viewport {
 public:
  struct Row {std::string text;uint64_t line;size_t part;};
  void Update(const Output& output,int width,int height) {
    if(width==width_ && height==height_ && output.Revision()==revision_)return;
    Row anchor=rows_.empty()?Row{"",output.FirstLine(),0}:rows_[std::min(First(),static_cast<int>(rows_.size())-1)];
    int inset=offset_%line_height_;
    width_=width;height_=height;revision_=output.Revision();bounds_=OutputBounds(width,height);
    line_height_=FontLineHeight(Font::Code,width);visible_=bounds_.h/line_height_;
    rows_.clear();rasters_.clear();
    for(size_t i=0;i<output.lines.size();++i) {
      if(output.lines[i].empty()) {rows_.push_back({"",output.FirstLine()+i,0});continue;}
      size_t part=0;
      for(auto& row:WrapText(output.lines[i],bounds_.w,FontPixels(Font::Code,width),false,true,Face::Code))
        rows_.push_back({std::move(row),output.FirstLine()+i,part++});
    }
    if(following_)offset_=MaximumPixels();
    else {
      auto found=std::lower_bound(rows_.begin(),rows_.end(),anchor,[](const Row& a,const Row& b) {
        return a.line<b.line || (a.line==b.line && a.part<b.part);
      });
      bool retained=found!=rows_.end() && found->line==anchor.line && found->part==anchor.part;
      offset_=std::min(MaximumPixels(),static_cast<int>(found-rows_.begin())*line_height_+(retained?inset:0));
    }
  }
  void Scroll(int rows) {
    ScrollPixels(rows*line_height_);
  }
  void ScrollPixels(int pixels) {
    offset_=std::clamp(offset_+pixels,0,MaximumPixels());following_=offset_==MaximumPixels();
  }
  void Bottom() {offset_=MaximumPixels();following_=true;}
  void Top() {offset_=0;following_=MaximumPixels()==0;}
  const std::vector<Row>& Rows() const {return rows_;}
  Rect Bounds() const {return bounds_;}
  int First() const {return offset_/line_height_;}
  int Offset() const {return offset_;}
  int Visible() const {return visible_;}
  int LineHeight() const {return line_height_;}
  int MaximumPixels() const {return std::max(0,static_cast<int>(rows_.size())*line_height_-bounds_.h);}
  int Maximum() const {return MaximumPixels()/line_height_;}
  bool Following() const {return following_;}
  const TextBitmap& Raster(int row) const {
    for(auto& entry:rasters_)if(entry.first==row)return entry.second;
    // Retain only the visible region and its neighbours, rather than rasterizing all history.
    rasters_.erase(std::remove_if(rasters_.begin(),rasters_.end(),[&](const auto& entry) {
      return entry.first<First()-1 || entry.first>First()+visible_+2;
    }),rasters_.end());
    rasters_.emplace_back(row,RasterText(rows_[row].text,FontPixels(Font::Code,width_),false,true,Face::Code));
    return rasters_.back().second;
  }
 private:
  std::vector<Row> rows_;Rect bounds_{};
  mutable std::vector<std::pair<int,TextBitmap>> rasters_;
  int width_=0,height_=0,offset_=0,visible_=0,line_height_=1;
  uint64_t revision_=0;bool following_=true;
};
struct State {Output output;Viewport view;};
inline void Draw(Canvas& c,int width,int height,const std::string& input,
                 const Viewport& view,bool symbols,bool shift,int focus) {
  Metrics m(width);c.Fill({0,0,width,height},design::background);
  auto back=design::Back(width);Surface(c,back,back.h/2,design::surface,focus==-1,design::text,Dp(width,1));
  design::Symbol(c,Inset(back,Dp(width,14)),design::Glyph::Back);
  Label(c,m,back.x+back.w+Dp(width,14),back.y+Dp(width,3),width-back.x-back.w-Dp(width,30),"Terminal",Font::DesignMenu,design::text);
  auto keys=Keyboard(width,height,symbols,shift);int pad=Dp(width,12);
  int input_y=keys.front().bounds.y-Dp(width,32),top=view.Bounds().y-view.Offset()%view.LineHeight();
  int bottom=view.Bounds().y+view.Bounds().h;
  for(int i=view.First();i<static_cast<int>(view.Rows().size()) && top<bottom;++i,top+=view.LineHeight()) {
    if(view.Rows()[i].text.empty())continue;
    const auto& bitmap=view.Raster(i);
    int start=std::max(0,view.Bounds().y-top),end=std::min(bitmap.height,bottom-top);
    int width=std::min(bitmap.width,view.Bounds().w);
    if(end<=start || width<=0)continue;
    if(start==0 && end==bitmap.height && width==bitmap.width)
      c.TextMask({pad,top,width,bitmap.height},bitmap.alpha,design::text);
    else {
      std::vector<uint8_t> clipped(width*(end-start));
      for(int y=start;y<end;++y)std::copy_n(bitmap.alpha.data()+y*bitmap.width,width,clipped.data()+(y-start)*width);
      c.TextMask({pad,top+start,width,end-start},clipped,design::text);
    }
  }
  if(view.MaximumPixels()>0 && view.Visible()>0) {
    int track=view.Bounds().h;
    int thumb=std::clamp(static_cast<int>(int64_t(track)*track/(view.Rows().size()*view.LineHeight())),std::min(Dp(width,12),track),track);
    int y=view.Bounds().y+static_cast<int>(int64_t(track-thumb)*view.Offset()/view.MaximumPixels());
    Rounded(c,{width-Dp(width,5),y,Dp(width,2),thumb},Dp(width,1),design::muted);
  }
  Rounded(c,{pad,input_y,width-2*pad,Dp(width,26)},Dp(width,6),design::surface);
  std::string tail=input;while(!tail.empty() && TextWidth(tail+"_",FontPixels(Font::Code,width),false,true,Face::Code)>width-4*pad)tail.erase(0,1);
  c.Text(2*pad,input_y+Dp(width,5),tail+"_",Font::Code,design::text,false);
  for(size_t i=0;i<keys.size();++i) {
    auto b=keys[i].bounds;Surface(c,b,Dp(width,5),design::surface,focus==static_cast<int>(i),design::green,Dp(width,1));
    auto label=FitText(keys[i].label,b.w-Dp(width,2),FontPixels(Font::Caption,width),false,true);
    int tw=TextWidth(label,FontPixels(Font::Caption,width),false,true);
    c.Text(b.x+(b.w-tw)/2,b.y+(b.h-FontLineHeight(Font::Caption,width))/2,label,Font::Caption,design::text,false);
  }
}
inline void Draw(Canvas& c,int width,int height,const std::string& input,
                 const std::vector<std::string>& output,bool symbols,bool shift,int focus) {
  Output buffer;buffer.lines=output;Viewport view;view.Update(buffer,width,height);
  Draw(c,width,height,input,view,symbols,shift,focus);
}
} // namespace recovery_m3e::terminal
