/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "files.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <sstream>
namespace recovery_crypto::android17 {
using android::base::unique_fd;
unique_fd OpenDirectory(const std::string& path) {
  if (path.empty() || path.front() != '/') return {};
  unique_fd fd(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  std::istringstream parts(path);
  std::string part;
  while (std::getline(parts, part, '/')) {
    if (part.empty()) continue;
    if (part == "." || part == "..") return {};
    unique_fd next(openat(fd.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (next.get() < 0) return {};
    fd = std::move(next);
  }
  return fd;
}
bool ReadFile(const std::string& path, Bytes* bytes, size_t limit, bool* missing) {
  Clear(*bytes);
  if (missing) *missing = false;
  auto split = path.find_last_of('/');
  if (split == std::string::npos) return false;
  auto dir = OpenDirectory(path.substr(0, split));
  if (dir.get() < 0) return false;
  unique_fd fd(
      openat(dir.get(), path.substr(split + 1).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  if (fd.get() < 0) {
    if (missing && errno == ENOENT) *missing = true;
    return false;
  }
  struct stat before{}, after{};
  if (fstat(fd.get(), &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
      static_cast<uint64_t>(before.st_size) > limit)
    return false;
  bytes->resize(before.st_size);
  size_t offset = 0;
  while (offset < bytes->size()) {
    ssize_t n = TEMP_FAILURE_RETRY(read(fd.get(), bytes->data() + offset, bytes->size() - offset));
    if (n <= 0) {
      Clear(*bytes);
      return false;
    }
    offset += n;
  }
  if (fstat(fd.get(), &after) != 0 || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
    Clear(*bytes);
    return false;
  }
  return true;
}
bool IsTrustedFile(const std::string& path) {
  auto split = path.find_last_of('/');
  if (split == std::string::npos) return false;
  auto dir = OpenDirectory(path.substr(0, split));
  if (dir.get() < 0) return false;
  struct stat st{};
  return fstatat(dir.get(), path.substr(split + 1).c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISREG(st.st_mode) && st.st_uid == 0 && !(st.st_mode & 0022);
}
bool ListDirectories(const std::string& path, std::vector<std::string>* names) {
  names->clear();
  auto fd = OpenDirectory(path);
  if (fd.get() < 0) return false;
  int descriptor = fd.release();
  DIR* d = fdopendir(descriptor);
  if (!d) {
    close(descriptor);
    return false;
  }
  bool ok = true;
  for (;;) {
    errno = 0;
    auto e = readdir(d);
    if (!e) {
      ok = errno == 0;
      break;
    }
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    struct stat st{};
    if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      ok = false;
      break;
    }
    if (S_ISDIR(st.st_mode)) names->emplace_back(e->d_name);
    if (names->size() > 128) {
      ok = false;
      break;
    }
  }
  closedir(d);
  return ok;
}
}  // namespace recovery_crypto::android17
