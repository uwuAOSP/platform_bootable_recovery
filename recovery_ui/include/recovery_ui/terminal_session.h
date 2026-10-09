/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <string>

namespace recovery_ui {
// A real PTY keeps shell state (cd/export), stdin and job-control signals intact.
// Owned by the terminal's event loop; only Read() runs on its output thread.
class TerminalSession {
 public:
  ~TerminalSession() {Close();}
  TerminalSession()=default;
  TerminalSession(const TerminalSession&)=delete;
  TerminalSession& operator=(const TerminalSession&)=delete;
  bool Open(int rows,int columns,const char* shell="/system/bin/sh") {
    Close();
    master_=posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC|O_NONBLOCK);
    if(master_<0)return false;
    if(grantpt(master_) || unlockpt(master_)) {Close();return false;}
    char name[128];if(ptsname_r(master_,name,sizeof(name))) {Close();return false;}
    int slave=open(name,O_RDWR|O_NOCTTY|O_CLOEXEC);
    if(slave<0) {Close();return false;}
    winsize size{};size.ws_row=std::clamp(rows,1,65535);size.ws_col=std::clamp(columns,1,65535);
    ioctl(slave,TIOCSWINSZ,&size);
    long max_fd=sysconf(_SC_OPEN_MAX);if(max_fd<0)max_fd=1024;
    pid_=fork();
    if(pid_==0) {
      if(setsid()<0 || ioctl(slave,TIOCSCTTY,0)<0)_exit(126);
      for(int fd=0;fd<3;++fd)if(dup2(slave,fd)<0)_exit(126);
      for(int fd=3;fd<max_fd;++fd)close(fd);
      struct sigaction action{};action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);
      for(int sig:{SIGINT,SIGQUIT,SIGTERM,SIGHUP,SIGPIPE})sigaction(sig,&action,nullptr);
      sigset_t mask;sigemptyset(&mask);sigprocmask(SIG_SETMASK,&mask,nullptr);
      char path[]="PATH=/system/bin:/system/xbin:/sbin:/vendor/bin:/bin:/usr/bin";
      char home[]="HOME=/",term[]="TERM=dumb",prompt[]="PS1=# ";
      char* env[]={path,home,term,prompt,nullptr};
      char* args[]={const_cast<char*>(shell),const_cast<char*>("-i"),nullptr};
      execve(shell,args,env);_exit(127);
    }
    close(slave);
    if(pid_<0) {pid_=-1;Close();return false;}
    return true;
  }
  // Poll is bounded so closing the screen can join the output reader promptly.
  ssize_t Read(char* bytes,size_t count) {
    pollfd fd{master_,POLLIN,0};int result;
    do {result=poll(&fd,1,100);}while(result<0 && errno==EINTR);
    if(result==0)return 0;
    if(result<0)return -1;
    ssize_t n=read(master_,bytes,count);
    if(n<0 && (errno==EAGAIN || errno==EINTR))return 0;
    return n>0?n:-1;
  }
  bool Send(const std::string& input) {
    size_t offset=0;
    while(offset<input.size()) {
      ssize_t n=write(master_,input.data()+offset,input.size()-offset);
      if(n>0)offset+=n;
      else if(n<0 && errno==EINTR)continue;
      else return false;
    }
    return true;
  }
  void Close() {
    if(pid_>0) {
      // The PTY's foreground group may be a running job, separate from the shell.
      pid_t job=master_>=0?tcgetpgrp(master_):-1;
      if(job>0 && job!=getpgrp())kill(-job,SIGKILL);
      kill(-pid_,SIGKILL);kill(pid_,SIGKILL);
      while(waitpid(pid_,nullptr,0)<0 && errno==EINTR){}
      pid_=-1;
    }
    if(master_>=0)close(master_);
    master_=-1;
  }
 private:
  int master_=-1;pid_t pid_=-1;
};
} // namespace recovery_ui
