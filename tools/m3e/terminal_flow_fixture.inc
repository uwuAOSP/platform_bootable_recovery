/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
// Complete ScreenRecoveryUI terminal event replay; host PTY/display I/O substitutes.
// test_install_ui.py inserts the actual production UI methods at the marker below.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>
namespace TerminalFlow {
constexpr int KEY_ESC=1,KEY_BACKSPACE=14,KEY_ENTER=28,KEY_SPACE=57,KEY_HOME=102,
  KEY_UP=103,KEY_PAGEUP=104,KEY_END=107,KEY_DOWN=108,KEY_PAGEDOWN=109,
  KEY_VOLUMEDOWN=114,KEY_VOLUMEUP=115,KEY_POWER=116,KEY_BACK=158;
int width=360,height=640;std::string preview_directory;
struct Pty {
  std::mutex mutex;std::deque<std::string> pending;std::vector<std::string> sent;
  std::string startup;bool fail=false,closed=false;
} pty;
void Feed(const std::string& data) {std::lock_guard<std::mutex> lock(pty.mutex);pty.pending.push_back(data);}
namespace recovery_ui {
class TerminalSession {
 public:
  bool Open(int,int) {
    std::lock_guard<std::mutex> lock(pty.mutex);pty.closed=false;pty.pending.clear();pty.sent.clear();
    if(pty.fail) {errno=ENOENT;return false;}
    pty.pending.push_back(pty.startup);return true;
  }
  std::ptrdiff_t Read(char* bytes,size_t count) {
    {
      std::lock_guard<std::mutex> lock(pty.mutex);
      if(!pty.pending.empty()) {
        auto& next=pty.pending.front();size_t size=std::min(count,next.size());
        std::memcpy(bytes,next.data(),size);next.erase(0,size);
        if(next.empty())pty.pending.pop_front();
        return size;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));return 0;
  }
  bool Send(const std::string& input) {
    std::lock_guard<std::mutex> lock(pty.mutex);pty.sent.push_back(input);
    pty.pending.push_back(input==std::string(1,3)?"^C\n# ":"command result\n# ");return !pty.fail;
  }
  void Close() {std::lock_guard<std::mutex> lock(pty.mutex);pty.closed=true;}
};
}
struct Point {int px=0,py=0;Point()=default;Point(int x,int y):px(x),py(y){}int x() const{return px;}int y() const{return py;}};
struct M3eCanvas : PixelCanvas {
  M3eCanvas():PixelCanvas(TerminalFlow::width,TerminalFlow::height){}
  void TextMask(Rect b,const std::vector<uint8_t>& alpha,Color color) override {
    auto output=terminal::OutputBounds(width,height);
    assert(b.x>=output.x && b.y>=output.y && b.x+b.w<=output.x+output.w && b.y+b.h<=output.y+output.h);
    PixelCanvas::TextMask(b,alpha,color);
  }
};
struct ScreenRecoveryUI {
  enum EventType {KEY,EXTRA,TOUCH,TOUCH_DOWN,TOUCH_MOVE,TOUCH_UP};
  enum class KeyError : int {TIMED_OUT=-1,INTERRUPTED=-2};
  struct InputEvent {
    EventType kind=EXTRA;int code=0;Point point;
    InputEvent()=default;
    InputEvent(EventType t,int c,Point p):kind(t),code(c),point(p){}
    InputEvent(EventType t,Point p):kind(t),point(p){}
    InputEvent(EventType t,KeyError k):kind(t),code(static_cast<int>(k)){}
    EventType type() const{return kind;}int key() const{return code;}Point pos() const{return point;}
  };
  std::mutex updateMutex;bool terminal_visible_=false,terminal_shift_=false,terminal_symbols_=false;
  bool gesture_input_=false,discard_touch_until_press_=false,menu_transition_=false,interrupted=false;
  bool coalesce_touch_moves_=false,coalescing_gesture_=false;Point coalesce_touch_min_,coalesce_touch_max_;
  std::mutex event_queue_mutex;std::condition_variable event_queue_cond;
  InputEvent event_queue[256];int event_queue_len=0;
  int terminal_focus_=-1;std::string terminal_input_;
  std::vector<std::string> title_lines_{"Product name - diting","Version 17.0.130"};
  std::unique_ptr<terminal::State> terminal_state_;
  std::unique_ptr<int> menu_=std::make_unique<int>(1),transition_menu_;
  std::deque<std::function<InputEvent(ScreenRecoveryUI&)>> events;
  int ScreenWidth(){return width;}int ScreenHeight(){return height;}
  Point TouchPoint(Point p){return p;}bool IsKeyInterrupted(){return interrupted;}void FlushKeys(){}
  void update_screen_locked(){if(terminal_visible_)DrawTerminalLocked();}
  void DrawTerminalLocked();void ShowTerminal();void EnqueueGesture(EventType,const Point&);
  void SetTouchMoveCoalescing(bool,const Point&,const Point&);
  InputEvent WaitInputEvent(){
    if(event_queue_len) {
      auto next=event_queue[0];std::move(event_queue+1,event_queue+event_queue_len,event_queue);--event_queue_len;return next;
    }
    assert(!events.empty());auto next=events.front();events.pop_front();return next(*this);
  }
};
// ACTUAL_UI_METHODS
template<class Predicate> void Wait(ScreenRecoveryUI& ui,Predicate predicate,const char* phase) {
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(std::chrono::steady_clock::now()<deadline) {
    {
      std::lock_guard<std::mutex> lock(ui.updateMutex);
      assert(ui.terminal_visible_ && ui.terminal_state_);
      if(predicate(*ui.terminal_state_))return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::lock_guard<std::mutex> lock(ui.updateMutex);
  std::cerr<<"Terminal replay timeout at "<<width<<"x"<<height<<" phase "<<phase
    <<", lines="<<ui.terminal_state_->output.lines.size()<<", first="<<ui.terminal_state_->output.FirstLine()<<"\n";
  assert(false && "Terminal output reader did not reach the expected state");
}
Point KeyPosition(ScreenRecoveryUI& ui,const std::string& label) {
  for(const auto& key:terminal::Keyboard(width,height,ui.terminal_symbols_,ui.terminal_shift_))
    if(key.label==label)return {key.bounds.x+key.bounds.w/2,key.bounds.y+key.bounds.h/2};
  assert(false);return {};
}
void Tap(ScreenRecoveryUI& ui,const std::string& label,bool drag=false) {
  if(drag) {
    ui.events.push_back([=](auto& s){
      auto p=KeyPosition(s,label);s.EnqueueGesture(ScreenRecoveryUI::TOUCH_DOWN,p);
      auto down=s.event_queue[0];s.event_queue_len=0;
      auto away=p;away.py+=Dp(width,40);
      s.EnqueueGesture(ScreenRecoveryUI::TOUCH_MOVE,away);
      s.EnqueueGesture(ScreenRecoveryUI::TOUCH_MOVE,p);
      s.EnqueueGesture(ScreenRecoveryUI::TOUCH_UP,p);
      assert(s.event_queue_len==3);return down;
    });return;
  }
  for(auto kind:{ScreenRecoveryUI::TOUCH_DOWN,ScreenRecoveryUI::TOUCH_UP}) {
    ui.events.push_back([=](auto& s){return ScreenRecoveryUI::InputEvent{kind,0,KeyPosition(s,label)};});
  }
}
void Key(ScreenRecoveryUI& ui,int code) {ui.events.push_back([=](auto&){return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::KEY,code,{}};});}
void Check(int w,int h,const std::string& out) {
  width=w;height=h;preview_directory=out;pty.fail=false;pty.startup.clear();
  for(int i=0;i<80;++i)pty.startup+="row "+std::to_string(i)+" "+std::string(120,'x')+"\n";
  ScreenRecoveryUI ui;terminal::Viewport::Row anchor{};int initial_offset=0,anchor_inset=0;
  ui.events.push_back([](auto& s){
    Wait(s,[](const auto& state){return state.output.lines.size()==81;},"startup");
    assert(s.gesture_input_);
    return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH,0,{width/2,terminal::OutputBounds(width,height).y+2}};
  }); // Output tap never exits.
  ui.events.push_back([](auto&){return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH,0,{width/2,terminal::Keyboard(width,height,false,false).front().bounds.y-Dp(width,20)}};});
  ui.events.push_back([&](auto& s){
    std::lock_guard<std::mutex> lock(s.updateMutex);initial_offset=s.terminal_state_->view.Offset();
    auto b=terminal::OutputBounds(width,height);
    return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH_DOWN,0,{width/2,b.y+b.h/2}};
  });
  ui.events.push_back([](auto&){auto b=terminal::OutputBounds(width,height);
    return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH_MOVE,0,{width/2,b.y+b.h/2+1}};
  });
  ui.events.push_back([&](auto& s){
    std::lock_guard<std::mutex> lock(s.updateMutex);auto& view=s.terminal_state_->view;
    assert(view.Offset()==initial_offset-1 && !view.Following());
    auto b=view.Bounds();return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH_UP,0,{width/2,b.y+b.h/2+1}};
  }); // Even one pixel moves history immediately, without a row-sized dead zone.
  ui.events.push_back([](auto& s){
    auto b=terminal::OutputBounds(width,height);int y=b.y+b.h/2;
    s.EnqueueGesture(ScreenRecoveryUI::TOUCH_DOWN,{width/2,y});
    auto down=s.event_queue[0];s.event_queue_len=0;
    // Replay a busy input producer through the production queue: keep the latest move and finger-up.
    for(int i=0;i<600;++i)s.EnqueueGesture(ScreenRecoveryUI::TOUCH_MOVE,{width/2,y+Dp(width,48)*i/599});
    s.EnqueueGesture(ScreenRecoveryUI::TOUCH_UP,{width/2,y+Dp(width,48)});
    assert(s.event_queue_len==2 && s.event_queue[0].type()==ScreenRecoveryUI::TOUCH_MOVE &&
           s.event_queue[0].pos().y()==y+Dp(width,48) && s.event_queue[1].type()==ScreenRecoveryUI::TOUCH_UP);
    return down;
  });
  ui.events.push_back([&](auto& s){
    {
      std::lock_guard<std::mutex> lock(s.updateMutex);auto& view=s.terminal_state_->view;
      assert(!view.Following() && view.First()<view.Maximum() && s.terminal_input_.empty());
      assert(view.Offset()==initial_offset-1-Dp(width,48));
      anchor=view.Rows()[view.First()];anchor_inset=view.Offset()%view.LineHeight();
      if(w==1220) {
        PixelCanvas c(width,height,true);terminal::Draw(c,width,height,s.terminal_input_,view,false,false,-1);
        DrawPageFooter(c,width,height,s.title_lines_);
        c.Write(preview_directory+"/terminal-history"+(GetLanguage()==Language::Chinese?"-zh.ppm":"-en.ppm"));
      }
    }
    Feed("new output\nmore output\n");
    Wait(s,[](const auto& state){return state.output.lines.size()==83;},"append while scrolled");
    {
      std::lock_guard<std::mutex> lock(s.updateMutex);auto& view=s.terminal_state_->view;
      assert(view.Rows()[view.First()].line==anchor.line && view.Rows()[view.First()].part==anchor.part);
      assert(view.Rows()[view.First()].text==anchor.text && !view.Following());
      assert(view.Offset()%view.LineHeight()==anchor_inset);
    }
    return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::KEY,KEY_HOME,{}};
  });
  Key(ui,KEY_PAGEDOWN);Key(ui,KEY_PAGEUP);
  ui.events.push_back([](auto& s){
    {
      std::lock_guard<std::mutex> lock(s.updateMutex);assert(s.terminal_state_->view.First()==0);
    }
    std::string flood;for(int i=0;i<300;++i)flood+="trimmed history "+std::to_string(i)+"\n";
    Feed(flood);Wait(s,[](const auto& state){return state.output.FirstLine()==127;},"history trim");
    {
      std::lock_guard<std::mutex> lock(s.updateMutex);auto& state=*s.terminal_state_;
      assert(state.view.First()==0 && state.view.Rows().front().line==state.output.FirstLine());
      assert(!state.view.Following());
    }
    return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::KEY,KEY_END,{}};
  });
  ui.events.push_back([](auto& s){
    Feed("follow bottom\n");Wait(s,[](const auto& state){return state.output.FirstLine()==128;},"following bottom");
    {
      std::lock_guard<std::mutex> lock(s.updateMutex);auto& view=s.terminal_state_->view;
      assert(view.Following() && view.First()==view.Maximum());
    }
    return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::KEY,KEY_PAGEUP,{}};
  });
  Tap(ui,"q",true);Tap(ui,"q");Tap(ui,"Enter");
  ui.events.push_back([](auto& s){
    Wait(s,[](const auto& state){return state.output.lines.back()=="# ";},"command response");
    std::lock_guard<std::mutex> lock(s.updateMutex);
    assert(s.terminal_input_.empty() && s.terminal_state_->view.Following());
    return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH,0,KeyPosition(s,"^C")};
  });
  ui.events.push_back([](auto& s){
    Wait(s,[](const auto& state){return state.output.lines.size()>1 && state.output.lines[state.output.lines.size()-2]=="# ^C";},"interrupt response");
    return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH,0,KeyPosition(s,"Clear")};
  });
  ui.events.push_back([](auto& s){
    std::lock_guard<std::mutex> lock(s.updateMutex);auto& state=*s.terminal_state_;
    assert(state.output.lines==std::vector<std::string>{""} && state.view.First()==0 && state.view.Following());
    assert(state.view.Rows().size()==1);
    auto b=design::Back(width);return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH_DOWN,0,{b.x+b.w/2,b.y+b.h/2}};
  });
  ui.events.push_back([](auto&){return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH_MOVE,0,{width/2,height/2}};});
  ui.events.push_back([](auto&){auto b=design::Back(width);return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH_UP,0,{b.x+b.w/2,b.y+b.h/2}};});
  Key(ui,KEY_VOLUMEDOWN);
  Key(ui,KEY_POWER);Key(ui,KEY_BACKSPACE);
  ui.events.push_back([](auto& s){
    std::lock_guard<std::mutex> lock(s.updateMutex);assert(s.terminal_input_.empty());
    auto b=design::Back(width);return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH_DOWN,0,{b.x+b.w/2,b.y+b.h/2}};
  });
  ui.events.push_back([](auto&){return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH_MOVE,0,{width/2,height/2}};});
  ui.events.push_back([](auto&){auto b=design::Back(width);return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH_UP,0,{b.x+b.w/2,b.y+b.h/2}};});
  Key(ui,KEY_BACK);ui.ShowTerminal();
  assert(ui.events.empty() && !ui.terminal_visible_ && !ui.terminal_state_ && !ui.gesture_input_ && !ui.coalesce_touch_moves_);
  assert(ui.discard_touch_until_press_ && ui.menu_transition_ && pty.closed);
  assert(pty.sent==std::vector<std::string>({"q\n",std::string(1,3)}));
  // Startup failure and interrupt paths keep blank taps harmless and restore input mode.
  pty.fail=true;ScreenRecoveryUI failed;
  failed.events.push_back([](auto& s){
    std::lock_guard<std::mutex> lock(s.updateMutex);
    assert(s.terminal_state_->output.lines.front().find("Unable to start terminal:")==0);
    return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH,0,{width/2,height/3}};
  });
  failed.events.push_back([](auto&){auto b=design::Back(width);return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::TOUCH,0,{b.x+b.w/2,b.y+b.h/2}};});
  failed.ShowTerminal();assert(failed.events.empty() && !failed.gesture_input_ && !failed.terminal_state_);
  ScreenRecoveryUI cancelled;cancelled.events.push_back([](auto&){return ScreenRecoveryUI::InputEvent{ScreenRecoveryUI::EXTRA,-2,{}};});
  cancelled.ShowTerminal();assert(!cancelled.terminal_visible_ && !cancelled.gesture_input_ && cancelled.discard_touch_until_press_);
  ScreenRecoveryUI interrupted;interrupted.interrupted=true;interrupted.ShowTerminal();assert(!interrupted.terminal_state_);
  pty.fail=false;
  static bool reported=false;if(!reported) {reported=true;std::cout<<"PASS: actual terminal UI loop and gesture queue, PTY reader replay, one-pixel dragging, 600-move coalescing, clipped history, stable pixel anchors, bottom following, commands, clear, keyboard navigation and harmless blank taps\n";}
}
} // namespace TerminalFlow
