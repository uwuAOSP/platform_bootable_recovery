/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
// A tiny preference file only. Mounting is owned by Recovery's existing volume layer.
#pragma once
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
namespace recovery_m3e {
inline constexpr const char* kLocaleDirectory = "/metadata/recovery";
inline constexpr const char* kLocaleFilename = "m3e_locale";
inline bool ValidSavedLocale(const std::string& value) {return value=="en-US" || value=="zh-CN";}
inline std::string ReadLocaleAt(const std::string& folder) {
  int directory=open(folder.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if(directory<0) return {};
  int fd=openat(directory,kLocaleFilename,O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_NOFOLLOW);
  close(directory);
  if(fd<0) return {};
  struct stat st{};char buffer[17]{};
  bool regular=fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_size>0 && st.st_size<=16;
  ssize_t length=-1;
  if(regular) do {length=read(fd,buffer,16);} while(length<0 && errno==EINTR);
  close(fd);
  if(length<=0) return {};
  std::string value(buffer,length);
  while(!value.empty() && (value.back()=='\n' || value.back()=='\r')) value.pop_back();
  return ValidSavedLocale(value)?value:std::string();
}
inline bool SaveLocaleAt(const std::string& folder,const std::string& value) {
  if(!ValidSavedLocale(value)) return false;
  if(ReadLocaleAt(folder)==value) return true;
  if(mkdir(folder.c_str(),0700)!=0 && errno!=EEXIST) return false;
  int directory=open(folder.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
  if(directory<0) return false;
  struct stat st{};
  int status=fstatat(directory,kLocaleFilename,&st,AT_SYMLINK_NOFOLLOW);
  if((status==0 && !S_ISREG(st.st_mode)) || (status!=0 && errno!=ENOENT)) {close(directory);return false;}
  auto nonce=std::chrono::steady_clock::now().time_since_epoch().count();
  std::string temporary=".m3e-locale-"+std::to_string(getpid())+"-"+std::to_string(nonce);
  int fd=openat(directory,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
  if(fd<0) {close(directory);return false;}
  size_t written=0;bool ok=true;
  while(written<value.size()) {
    ssize_t size=write(fd,value.data()+written,value.size()-written);
    if(size<0 && errno==EINTR) continue;
    if(size<=0) {ok=false;break;}
    written+=size;
  }
  if(ok) ok=fsync(fd)==0;
  if(close(fd)!=0) ok=false;
  if(ok) ok=renameat(directory,temporary.c_str(),directory,kLocaleFilename)==0;
  if(ok) ok=fsync(directory)==0;
  unlinkat(directory,temporary.c_str(),0);
  close(directory);
  return ok;
}
}
