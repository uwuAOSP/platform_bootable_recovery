/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
// Complete real-shell session flow. Run on Linux; no substituted process/PTY APIs.
#include "recovery_ui/terminal_session.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

std::string Receive(recovery_ui::TerminalSession& session,const std::string& expected) {
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  std::string output;char bytes[2048];
  while(std::chrono::steady_clock::now()<deadline) {
    ssize_t n=session.Read(bytes,sizeof(bytes));
    if(n<0)break;
    if(n>0)output.append(bytes,n);
    if(output.find(expected)!=std::string::npos)return output;
  }
  std::cerr<<"Expected "<<expected<<" in shell output:\n"<<output;std::abort();
}
int main() {
  recovery_ui::TerminalSession session;
  assert(session.Open(24,80,"/bin/sh"));
  assert(session.Send("printf '\\nSTARTED\\n'\n"));Receive(session,"\r\nSTARTED\r\n");
  assert(session.Send("cd /tmp; export M3E_TEST=session_value\n"));
  assert(session.Send("printf '\\nCWD=%s ENV=%s\\n' \"$PWD\" \"$M3E_TEST\"\n"));
  Receive(session,"\r\nCWD=/tmp ENV=session_value\r\n");
  // Commands receive real stdin, including interactive read prompts.
  assert(session.Send("read answer; printf '\\nANSWER=%s\\n' \"$answer\"\n"));
  assert(session.Send("typed_input\n"));Receive(session,"\r\nANSWER=typed_input\r\n");
  assert(session.Send("printf '\\nRUNNING\\n'; sleep 30\n"));Receive(session,"\r\nRUNNING\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  assert(session.Send(std::string(1,3)));
  assert(session.Send("printf '\\nAFTER_INTERRUPT\\n'\n"));Receive(session,"\r\nAFTER_INTERRUPT\r\n");
  assert(session.Send("exit\n"));
  bool exited=false;
  for(int i=0;i<50;++i) {char bytes[256];if(session.Read(bytes,sizeof(bytes))<0) {exited=true;break;}}
  assert(exited);session.Close();
  // Closing a page also terminates its active foreground command.
  assert(session.Open(24,80,"/bin/sh"));
  assert(session.Send("printf '\\nCLOSING\\n'; sleep 30\n"));Receive(session,"\r\nCLOSING\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  auto start=std::chrono::steady_clock::now();session.Close();
  assert(std::chrono::steady_clock::now()-start<std::chrono::seconds(2));
  assert(session.Open(24,80,"/bin/sh"));
  assert(session.Send("printf '\\nREOPENED\\n'\n"));Receive(session,"\r\nREOPENED\r\n");session.Close();
  std::cout<<"PASS: real PTY shell, state, stdin, Ctrl+C, exit, cancellation and reopening\n";
}
