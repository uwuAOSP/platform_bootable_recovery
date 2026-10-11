/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "recovery_crypto/session.h"
#include "protocol.h"
#include "secret_memory.h"

#include <android-base/unique_fd.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace recovery_crypto {
namespace {
std::mutex reap_mutex;
std::vector<pid_t> pending_reap;
void ReapPending() {
  std::lock_guard<std::mutex> lock(reap_mutex);
  for (auto it = pending_reap.begin(); it != pending_reap.end();) {
    int status;
    auto result = waitpid(*it, &status, WNOHANG);
    if (result == *it || (result < 0 && errno == ECHILD)) it = pending_reap.erase(it);
    else ++it;
  }
}
}  // namespace
Credential::Credential() {
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) return;
  void* p = mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return;
  if (mlock(p, page) != 0 || madvise(p, page, MADV_DONTDUMP) != 0) {
    munlock(p, page); munmap(p, page); return;
  }
  bytes_ = static_cast<uint8_t*>(p);
  locked_ = true;
}
Credential::~Credential() {
  Clear();
  if (locked_) {
    const auto page = sysconf(_SC_PAGESIZE);
    munlock(bytes_, page); munmap(bytes_, page);
  }
}
bool Credential::Append(uint8_t byte) {
  if (!locked_ || length_ == RECOVERY_CRYPTO_MAX_CREDENTIAL) return false;
  bytes_[length_++] = byte;
  return true;
}
void Credential::EraseLast() { if (length_) bytes_[--length_] = 0; }
void Credential::Clear() { if (bytes_) Wipe(bytes_, RECOVERY_CRYPTO_MAX_CREDENTIAL); length_ = 0; }
bool Credential::Contains(uint8_t byte) const {
  for (size_t i = 0; i < length_; ++i) if (bytes_[i] == byte) return true;
  return false;
}
bool BackendInstalled() { return TrustedFile(kBackendPath) && TrustedFile(kWorkerPath); }

Session::~Session() { Stop(); }
void Session::Stop() {
  ReapPending();
  if (fd_ >= 0) { shutdown(fd_, SHUT_RDWR); close(fd_); fd_ = -1; }
  if (pid_ > 0) {
    // The worker gets a bounded chance to clear transient state. A backend's
    // finish callback cannot block or crash the UI indefinitely.
    int status;
    for (int i = 0; i < 10; ++i) {
      auto waited = waitpid(pid_, &status, WNOHANG);
      if (waited == pid_ || (waited < 0 && errno == ECHILD)) { pid_ = -1; break; }
      usleep(10000);
    }
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      auto waited = waitpid(pid_, &status, WNOHANG);
      if (waited == 0 || (waited < 0 && errno == EINTR)) {
        // A backend stuck in kernel uninterruptible I/O might not exit even
        // after SIGKILL. Do not block the Recovery UI in waitpid(..., 0).
        std::lock_guard<std::mutex> lock(reap_mutex);
        pending_reap.push_back(pid_);
      }
      pid_ = -1;
    }
  }
  prepared_ = false;
}
Result Session::Prepare(uint32_t user_id, const std::function<void(Stage)>& progress) {
  Stop();
  retry_after_ = {};
  if (user_id > 99999 || !BackendInstalled()) return {};
  user_id_ = user_id;
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) {
    return {Status::IoError};
  }
  android::base::unique_fd parent(pair[0]), child(pair[1]);
  android::base::unique_fd source(fcntl(child.get(), F_DUPFD_CLOEXEC, kWorkerFd + 1));
  if (source.get() < 0) return {Status::IoError};
  // fd 3 is the only inherited IPC endpoint; FD_CLOEXEC remains set on the
  // parent's endpoint. If parent already equals 3 it is replaced in the child.
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) return {Status::IoError};
  bool actions_ok = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0) == 0 &&
      posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0) == 0 &&
      posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0) == 0 &&
      posix_spawn_file_actions_adddup2(&actions, source.get(), kWorkerFd) == 0;
  char* argv[] = {const_cast<char*>(kWorkerPath), nullptr};
  char* environment[] = {const_cast<char*>("PATH=/system/bin:/sbin"),
                         const_cast<char*>("ANDROID_ROOT=/system"),
                         const_cast<char*>("ANDROID_DATA=/data"), nullptr};
  pid_t worker = -1;
  const int error = actions_ok ? posix_spawn(&worker, kWorkerPath, &actions, nullptr, argv, environment)
                               : EIO;
  posix_spawn_file_actions_destroy(&actions);
  if (error != 0) return {Status::WorkerFailed};
  pid_ = worker;
  fd_ = parent.release();
  child.reset();
  source.reset();
  return Exchange(kPrepare, nullptr, progress);
}
Result Session::Unlock(const Credential& credential, const std::function<void(Stage)>& progress) {
  if (!prepared_ || fd_ < 0 || !credential.secure()) return {Status::InvalidBackend};
  const auto now = std::chrono::steady_clock::now();
  if (now < retry_after_) {
    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(retry_after_ - now).count() + 1;
    return {Status::Throttled, Stage::CredentialKeys, credential_type_,
            static_cast<uint32_t>(seconds), pattern_size_};
  }
  return Exchange(kUnlock, &credential, progress);
}
Result Session::Exchange(uint32_t operation, const Credential* credential,
                         const std::function<void(Stage)>& progress) {
  SecretMemory memory;
  if (!memory.data()) { Stop(); return {Status::IoError}; }
  auto request = new (memory.data()) Request{};
  request->operation = operation;
  request->user_id = user_id_;
  request->credential_type = operation == kPrepare ? RC_CREDENTIAL_NONE : credential_type_;
  request->pattern_size = operation == kPrepare ? RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE : pattern_size_;
  if (credential) {
    request->length = static_cast<uint32_t>(credential->size());
    if (request->length) memcpy(request->credential, credential->data(), request->length);
  }
  if (!ValidRequest(*request))
    return {Status::WrongCredential, Stage::Credential, credential_type_, 0, pattern_size_};
  if (send(fd_, request, sizeof(*request), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(*request))) {
    Stop(); return {Status::WorkerFailed};
  }
  Wipe(request, sizeof(*request));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kDeadlineSeconds);
  for (;;) {
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) { Stop(); return {Status::Timeout}; }
    pollfd pfd{fd_, POLLIN, 0};
    const int ready = poll(&pfd, 1, static_cast<int>(remaining));
    if (ready < 0 && errno == EINTR) continue;
    if (ready == 0) { Stop(); return {Status::Timeout}; }
    if (ready < 0) { Stop(); return {Status::WorkerFailed}; }
    Reply reply{};
    const auto count = recv(fd_, &reply, sizeof(reply), MSG_TRUNC | MSG_DONTWAIT);
    if (count <= 0) { Stop(); return {Status::WorkerFailed}; }
    if (count != static_cast<ssize_t>(sizeof(reply)) || !ValidReply(reply)) {
      Stop(); return {Status::InvalidBackend};
    }
    if (reply.kind == kProgress) {
      if (progress) progress(static_cast<Stage>(reply.stage));
      continue;
    }
    if (operation == kUnlock &&
        (reply.credential_type != credential_type_ || reply.pattern_size != pattern_size_)) {
      Stop(); return {Status::InvalidBackend};
    }
    Result result{static_cast<Status>(reply.status), static_cast<Stage>(reply.stage),
                  reply.credential_type, reply.retry_seconds, reply.pattern_size};
    if (result.status == Status::CredentialRequired) {
      if (operation != kPrepare || result.credential_type == RC_CREDENTIAL_NONE) {
        Stop(); return {Status::InvalidBackend};
      }
      prepared_ = true;
      credential_type_ = result.credential_type;
      pattern_size_ = result.pattern_size;
    } else if (result.status == Status::Throttled) {
      retry_after_ = std::chrono::steady_clock::now() + std::chrono::seconds(result.retry_seconds);
    } else if (result.status == Status::Ready) {
      // Never take an adapter's success code as proof of usable CE storage.
      if (progress) progress(Stage::Verify);
      if (!VerifyUserStorage(user_id_)) result = {Status::StorageLocked, Stage::Verify};
      Stop();
    } else if (result.status != Status::WrongCredential) {
      Stop();
    }
    return result;
  }
}
const char* StageMessage(Stage stage) {
  switch (stage) {
    case Stage::Services: return "Starting storage security services";
    case Stage::Metadata: return "Unlocking metadata encryption";
    case Stage::DeviceKeys: return "Loading device-encrypted keys";
    case Stage::Credential: return "Checking credential type";
    case Stage::CredentialKeys: return "Unlocking credential-encrypted storage";
    case Stage::Verify: return "Checking internal storage access";
  }
  return "Unlocking internal storage";
}
const char* StatusMessage(Status status) {
  switch (status) {
    case Status::Ready: return "Internal storage is unlocked";
    case Status::CredentialRequired: return "Enter the Android lock-screen credential";
    case Status::Unsupported: return "Storage decryption is not configured for this device";
    case Status::ServicesUnavailable: return "Storage security services are unavailable";
    case Status::MissingKey: return "An existing storage key is missing";
    case Status::UpgradeRequired: return "A storage key requires a platform upgrade";
    case Status::WrongCredential: return "The credential was not accepted";
    case Status::Throttled: return "Too many attempts; wait before trying again";
    case Status::IoError: return "Storage decryption could not complete";
    case Status::InvalidBackend: return "The storage decryption backend is incompatible";
    case Status::Timeout: return "Storage decryption timed out";
    case Status::WorkerFailed: return "The storage decryption worker stopped";
    case Status::StorageLocked: return "Internal storage is still locked";
  }
  return "Storage decryption could not complete";
}
}  // namespace recovery_crypto
