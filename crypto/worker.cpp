/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "protocol.h"
#include "secret_memory.h"

#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <new>

namespace recovery_crypto {
namespace {
bool Send(Stage stage, Status status, uint32_t credential = RC_CREDENTIAL_NONE,
          uint32_t retry = 0, uint32_t kind = kResult,
          uint32_t pattern_size = RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE) {
  Reply reply{kProtocol, kind, static_cast<uint32_t>(status), static_cast<uint32_t>(stage),
              credential, retry, pattern_size};
  return send(kWorkerFd, &reply, sizeof(reply), MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(reply));
}
bool StartStage(Stage stage) { return Send(stage, Status::Ready, 0, 0, kProgress); }
void CloseInheritedFds() {
  DIR* directory = opendir("/proc/self/fd");
  if (!directory) _exit(1);
  const int scanner = dirfd(directory);
  while (auto entry = readdir(directory)) {
    char* end = nullptr;
    long fd = strtol(entry->d_name, &end, 10);
    if (end && *end == '\0' && fd > kWorkerFd && fd != scanner) close(static_cast<int>(fd));
  }
  closedir(directory);
}
bool ValidBackend(const recovery_crypto_backend_v1* b) {
  return b && b->abi_version == RECOVERY_CRYPTO_BACKEND_ABI && b->struct_size == sizeof(*b) &&
      b->prepare_services && b->mount_metadata && b->load_de_keys && b->get_credential_type &&
      b->unlock_ce && b->finish;
}
}  // namespace

int RunWorker() {
  int socket_type = 0;
  socklen_t size = sizeof(socket_type);
  if (getsockopt(kWorkerFd, SOL_SOCKET, SO_TYPE, &socket_type, &size) != 0 ||
      socket_type != SOCK_SEQPACKET) return 1;
  struct rlimit limit{0, 0};
  if (prctl(PR_SET_DUMPABLE, 0) != 0 || setrlimit(RLIMIT_CORE, &limit) != 0) return 1;
  CloseInheritedFds();
  // The backend is never injected through an environment variable or path
  // supplied by a ZIP/credential. TrustedFile rejects writable/symlink files.
  unsetenv("LD_PRELOAD");
  unsetenv("LD_LIBRARY_PATH");
  if (!TrustedFile(kBackendPath)) {
    Send(Stage::Services, Status::Unsupported); return 0;
  }
  void* library = dlopen(kBackendPath, RTLD_NOW | RTLD_LOCAL);
  if (!library) { Send(Stage::Services, Status::InvalidBackend); return 0; }
  auto get = reinterpret_cast<recovery_crypto_get_backend_fn>(
      dlsym(library, "recovery_crypto_get_backend_v1"));
  const auto backend = get ? get() : nullptr;
  auto get_pattern_size = reinterpret_cast<recovery_crypto_get_pattern_size_fn>(
      dlsym(library, "recovery_crypto_get_pattern_size_v1"));
  if (!ValidBackend(backend)) {
    Send(Stage::Services, Status::InvalidBackend); dlclose(library); return 0;
  }
  SecretMemory memory;
  if (!memory.data()) {
    Send(Stage::Services, Status::IoError); dlclose(library); return 0;
  }
  auto request = new (memory.data()) Request{};
  bool prepared = false;
  bool attempted = false;
  uint32_t user_id = 0, credential_type = RC_CREDENTIAL_NONE;
  uint32_t pattern_size = RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE;
  for (;;) {
    Wipe(request, sizeof(*request));
    const auto count = recv(kWorkerFd, request, sizeof(*request), MSG_TRUNC);
    if (count == 0) break;
    if (count != static_cast<ssize_t>(sizeof(*request)) || !ValidRequest(*request)) {
      Send(Stage::Services, Status::InvalidBackend); break;
    }
    if (request->operation == kPrepare) {
      if (attempted) { Send(Stage::Services, Status::InvalidBackend); break; }
      attempted = true;
      user_id = request->user_id;
      const struct { Stage stage; int32_t (*call)(uint32_t); } steps[] = {
          {Stage::Services, backend->prepare_services},
          {Stage::Metadata, backend->mount_metadata},
          {Stage::DeviceKeys, backend->load_de_keys},
      };
      bool ok = true;
      for (const auto& step : steps) {
        if (!StartStage(step.stage)) { ok = false; break; }
        auto result = BackendStatus(step.call(user_id));
        if (result != Status::Ready) {
          Send(step.stage, result == Status::Throttled ? Status::InvalidBackend : result);
          ok = false;
          break;
        }
      }
      if (!ok) break;
      if (!StartStage(Stage::Credential)) break;
      auto result = BackendStatus(backend->get_credential_type(user_id, &credential_type));
      if (result != Status::Ready || credential_type > RC_CREDENTIAL_PATTERN) {
        Send(Stage::Credential, result == Status::Ready || result == Status::Throttled ?
            Status::InvalidBackend : result);
        break;
      }
      if (credential_type == RC_CREDENTIAL_PATTERN && get_pattern_size) {
        result = BackendStatus(get_pattern_size(user_id, &pattern_size));
        if (result != Status::Ready || pattern_size < RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE ||
            pattern_size > RECOVERY_CRYPTO_MAX_PATTERN_SIZE) {
          Send(Stage::Credential, result == Status::Ready || result == Status::Throttled ?
               Status::InvalidBackend : result);
          break;
        }
      }
      prepared = true;
      if (credential_type != RC_CREDENTIAL_NONE) {
        if (!Send(Stage::Credential, Status::CredentialRequired, credential_type, 0, kResult,
                  pattern_size)) break;
        continue;
      }
    } else if (!prepared || request->user_id != user_id ||
               request->credential_type != credential_type || request->pattern_size != pattern_size) {
      Send(Stage::CredentialKeys, Status::InvalidBackend); break;
    }
    if (!StartStage(Stage::CredentialKeys)) break;
    uint32_t retry_seconds = 0;
    auto result = BackendStatus(backend->unlock_ce(user_id, credential_type,
        request->length ? request->credential : nullptr, request->length, &retry_seconds));
    Wipe(request, sizeof(*request));
    if (result == Status::Throttled && retry_seconds == 0) result = Status::InvalidBackend;
    if (!Send(Stage::CredentialKeys, result, credential_type, retry_seconds, kResult, pattern_size))
      break;
    if (result != Status::WrongCredential && result != Status::Throttled) break;
    // No automatic authentication retries. Each new request is initiated by
    // the user; the backend must also enforce Gatekeeper/Weaver's own limits.
  }
  Wipe(request, sizeof(*request));
  backend->finish();
  dlclose(library);
  return 0;
}
}  // namespace recovery_crypto

int main() { return recovery_crypto::RunWorker(); }
