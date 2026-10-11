/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "database.h"
#include <MtpDataPacket.h>
#include <MtpObjectInfo.h>
#include <MtpProperty.h>
#include <MtpStringBuffer.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <cstdio>

namespace recovery_mtp {
using android::base::unique_fd;
Database::~Database() { CancelUpload(); }
void Database::EnableWrites(bool enable) {
  struct statvfs fs{};
  writable_ = enable && valid() && fstatvfs(root_.get(), &fs) == 0 && !(fs.f_flag & ST_RDONLY);
}
bool Database::ValidName(const std::string& name) const {
  return !name.empty() && name.size() <= NAME_MAX && name != "." && name != ".." &&
         !name.starts_with(".recovery-mtp-") &&
         name.find_first_of("/\\:") == std::string::npos &&
         std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
void Database::CancelUpload() {
  if (upload_handle_ < entries_.size()) entries_[upload_handle_].alive = false;
  upload_.reset(); upload_parent_.reset();
  upload_handle_ = kInvalidObjectHandle;
}
MtpResponseCode Database::recoveryBeginUpload(const char* raw, MtpObjectFormat format,
    MtpObjectHandle parent, MtpStorageID storage, uint64_t size, MtpObjectHandle* handle) {
  if (!writable_) return MTP_RESPONSE_STORE_READ_ONLY;
  if (!handle || storage != kStorageId) return MTP_RESPONSE_INVALID_STORAGE_ID;
  if (parent == MTP_PARENT_ROOT) parent = 0;
  if (!raw || !ValidName(raw)) return MTP_RESPONSE_INVALID_PARAMETER;
  CancelUpload();
  if (!Scan(parent)) return MTP_RESPONSE_INVALID_PARENT_OBJECT;
  if (entries_.size() - 1 >= limit_) return MTP_RESPONSE_STORAGE_FULL;
  auto directory = OpenEntry(parent);
  if (directory.get() < 0) return MTP_RESPONSE_INVALID_PARENT_OBJECT;
  struct statvfs available{};
  if (fstatvfs(directory.get(), &available)) return MTP_RESPONSE_GENERAL_ERROR;
  if (size != UINT32_MAX && size / std::max<uint64_t>(1, available.f_frsize) > available.f_bavail)
    return MTP_RESPONSE_STORAGE_FULL;
  struct stat collision{};
  if (fstatat(directory.get(), raw, &collision, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
    return MTP_RESPONSE_ACCESS_DENIED;
  Entry entry;
  entry.name = raw; entry.parent = parent;
  entry.path = entries_[parent].path.empty() ? raw : entries_[parent].path + "/" + raw;
  if (entry.path.size() >= PATH_MAX || std::count(entry.path.begin(), entry.path.end(), '/') >= 64)
    return MTP_RESPONSE_INVALID_PARAMETER;
  if (format == MTP_FORMAT_ASSOCIATION) {
    if (mkdirat(directory.get(), raw, 0770)) return MTP_RESPONSE_ACCESS_DENIED;
    unique_fd created(openat(directory.get(), raw, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (created.get() < 0 || fstat(created.get(), &entry.stat)) return MTP_RESPONSE_GENERAL_ERROR;
    if (fchmod(created.get(), 0770) || (geteuid() == 0 && fchown(created.get(), 1023, 1023))) {
      unlinkat(directory.get(), raw, AT_REMOVEDIR);
      return MTP_RESPONSE_GENERAL_ERROR;
    }
    entries_.push_back(std::move(entry));
    *handle = entries_.size() - 1;
    return fsync(directory.get()) == 0 ? MTP_RESPONSE_OK : MTP_RESPONSE_GENERAL_ERROR;
  }
  // An unnamed inode cannot appear as a half-written ZIP, even if init kills
  // the daemon mid-transfer. Publishing with linkat also refuses overwrites.
  upload_.reset(openat(directory.get(), ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0660));
  if (upload_.get() < 0) return errno == ENOSPC ? MTP_RESPONSE_STORAGE_FULL :
      errno == EROFS ? MTP_RESPONSE_STORE_READ_ONLY : MTP_RESPONSE_ACCESS_DENIED;
  if (fstat(upload_.get(), &entry.stat) || fchmod(upload_.get(), 0660) ||
      (geteuid() == 0 && fchown(upload_.get(), 1023, 1023))) {
    CancelUpload(); return MTP_RESPONSE_GENERAL_ERROR;
  }
  entry.pending = true;
  entries_.push_back(std::move(entry));
  upload_handle_ = entries_.size() - 1; upload_size_ = size;
  upload_parent_ = std::move(directory);
  *handle = upload_handle_;
  return MTP_RESPONSE_OK;
}
int Database::recoveryUploadFd(MtpObjectHandle handle) {
  return writable_ && handle == upload_handle_ && upload_.get() >= 0
      ? fcntl(upload_.get(), F_DUPFD_CLOEXEC, 0) : -1;
}
MtpResponseCode Database::recoveryFinishUpload(MtpObjectHandle handle, bool success) {
  if (handle != upload_handle_ || upload_.get() < 0) return MTP_RESPONSE_NO_VALID_OBJECT_INFO;
  auto result = MTP_RESPONSE_GENERAL_ERROR;
  struct stat st{};
  if (success && fstat(upload_.get(), &st) == 0 && st.st_size >= 0 &&
      (upload_size_ == UINT32_MAX || uint64_t(st.st_size) == upload_size_) && fsync(upload_.get()) == 0) {
    // procfd refers to our pinned unnamed inode; never to a host-supplied path.
    const auto proc = "/proc/self/fd/" + std::to_string(upload_.get());
    auto parent = OpenEntry(entries_[handle].parent);
    struct stat before{}, now{};
    if (parent.get() >= 0 && fstat(parent.get(), &now) == 0 &&
        fstat(upload_parent_.get(), &before) == 0 && now.st_dev == before.st_dev && now.st_ino == before.st_ino &&
        linkat(AT_FDCWD, proc.c_str(), parent.get(), entries_[handle].name.c_str(), AT_SYMLINK_FOLLOW) == 0) {
      entries_[handle].stat = st; entries_[handle].pending = false;
      result = fsync(parent.get()) == 0 ? MTP_RESPONSE_OK : MTP_RESPONSE_GENERAL_ERROR;
      // Keep the committed entry visible even on a directory flush failure.
      upload_handle_ = kInvalidObjectHandle;
    }
  }
  CancelUpload();
  return result;
}
bool Database::DeleteEntry(MtpObjectHandle handle) {
  const auto* entry = Get(handle);
  if (!entry) return false;
  if (S_ISDIR(entry->stat.st_mode)) {
    if (!Scan(handle)) return false;
    for (size_t i = 1; i < entries_.size(); ++i)
      if (entries_[i].alive && entries_[i].parent == handle && !DeleteEntry(i)) return false;
  }
  entry = Get(handle);  // Scan may have grown the vector.
  auto pinned = OpenEntry(handle), parent = OpenEntry(entry->parent);
  if (pinned.get() < 0 || parent.get() < 0) return false;
  struct stat st{};
  if (fstatat(parent.get(), entry->name.c_str(), &st, AT_SYMLINK_NOFOLLOW) ||
      st.st_ino != entry->stat.st_ino || st.st_dev != entry->stat.st_dev) return false;
  if (unlinkat(parent.get(), entry->name.c_str(), S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0)) return false;
  entries_[handle].alive = false;
  return fsync(parent.get()) == 0;
}
MtpResponseCode Database::recoveryDelete(MtpObjectHandle handle) {
  if (!writable_) return MTP_RESPONSE_STORE_READ_ONLY;
  if (!Get(handle)) return MTP_RESPONSE_INVALID_OBJECT_HANDLE;
  if (upload_handle_ != kInvalidObjectHandle) return MTP_RESPONSE_DEVICE_BUSY;
  return DeleteEntry(handle) ? MTP_RESPONSE_OK : MTP_RESPONSE_PARTIAL_DELETION;
}
static const MtpObjectProperty kProperties[] = {
  MTP_PROPERTY_STORAGE_ID, MTP_PROPERTY_OBJECT_FORMAT, MTP_PROPERTY_PROTECTION_STATUS,
  MTP_PROPERTY_OBJECT_SIZE, MTP_PROPERTY_OBJECT_FILE_NAME, MTP_PROPERTY_DATE_MODIFIED,
  MTP_PROPERTY_PARENT_OBJECT, MTP_PROPERTY_PERSISTENT_UID, MTP_PROPERTY_NAME,
};
Database::Database(const std::string& root, size_t limit) : limit_(std::min(limit, size_t{100000})) {
  if (root.empty() || root.front() != '/') return;
  unique_fd fd(open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  size_t offset = 1;
  while (offset < root.size()) {
    auto end = root.find('/', offset);
    auto part = root.substr(offset, end == std::string::npos ? end : end - offset);
    if (part.empty() || part == "." || part == "..") return;
    unique_fd next(openat(fd.get(), part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (next.get() < 0) return;
    fd = std::move(next);
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  Entry entry;
  if (fd.get() < 0 || fstat(fd.get(), &entry.stat) != 0) return;
  entries_.push_back(entry);
  root_ = std::move(fd);
}
unique_fd Database::Open(const std::string& path) const {
  if (!valid()) return {};
  unique_fd fd(fcntl(root_.get(), F_DUPFD_CLOEXEC, 0));
  size_t offset = 0;
  while (offset < path.size()) {
    auto end = path.find('/', offset);
    auto part = path.substr(offset, end == std::string::npos ? end : end - offset);
    if (part.empty() || part == "." || part == "..") return {};
    unique_fd next(openat(fd.get(), part.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                         (end == std::string::npos ? 0 : O_DIRECTORY)));
    struct stat st{};
    if (next.get() < 0 || fstat(next.get(), &st) != 0 || st.st_dev != entries_[0].stat.st_dev ||
        (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) return {};
    fd = std::move(next);
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return fd;
}
const Database::Entry* Database::Get(MtpObjectHandle handle) const {
  return valid() && handle > 0 && handle < entries_.size() && entries_[handle].alive &&
         !entries_[handle].pending ? &entries_[handle] : nullptr;
}
unique_fd Database::OpenEntry(MtpObjectHandle handle) const {
  if (!valid() || handle >= entries_.size() || !entries_[handle].alive || entries_[handle].pending) return {};
  const auto& entry = entries_[handle];
  auto fd = Open(entry.path);
  struct stat st{};
  if (fd.get() < 0 || fstat(fd.get(), &st) != 0 || st.st_dev != entry.stat.st_dev ||
      st.st_ino != entry.stat.st_ino || (st.st_mode & S_IFMT) != (entry.stat.st_mode & S_IFMT))
    return {};
  return fd;
}
bool Database::Scan(MtpObjectHandle parent) {
  if (!valid() || parent >= entries_.size() || !S_ISDIR(entries_[parent].stat.st_mode)) return false;
  if (entries_[parent].scanned) return true;
  auto fd = OpenEntry(parent);
  if (fd.get() < 0) return false;
  const int directory_fd = fd.release();
  DIR* raw = fdopendir(directory_fd);
  if (!raw) {
    close(directory_fd);
    return false;
  }
  std::unique_ptr<DIR, decltype(&closedir)> directory(raw, closedir);
  const auto prefix = entries_[parent].path;
  std::vector<Entry> children;
  for (;;) {
    errno = 0;
    auto item = readdir(raw);
    if (!item) { if (errno) return false; break; }
    std::string name(item->d_name);
    if (name == "." || name == ".." || name.starts_with(".recovery-mtp-")) continue;
    Entry entry;
    if (fstatat(dirfd(raw), name.c_str(), &entry.stat, AT_SYMLINK_NOFOLLOW) != 0 ||
        entry.stat.st_dev != entries_[0].stat.st_dev || entry.stat.st_size < 0 ||
        (!S_ISREG(entry.stat.st_mode) && !S_ISDIR(entry.stat.st_mode))) continue;
    if (entries_.size() - 1 + children.size() >= limit_) return false;
    entry.name = name;
    entry.path = prefix.empty() ? name : prefix + "/" + name;
    if (entry.path.size() >= PATH_MAX || std::count(entry.path.begin(), entry.path.end(), '/') >= 64)
      return false;
    entry.parent = parent;
    children.push_back(std::move(entry));
  }
  std::sort(children.begin(), children.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
  for (auto& entry : children) entries_.push_back(std::move(entry));
  entries_[parent].scanned = true;
  return true;
}
bool Database::ScanAll() {
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i].alive && !entries_[i].pending && S_ISDIR(entries_[i].stat.st_mode) && !Scan(i)) return false;
  return true;
}
MtpObjectFormat Database::Format(const Entry& entry) const {
  return S_ISDIR(entry.stat.st_mode) ? MTP_FORMAT_ASSOCIATION : MTP_FORMAT_UNDEFINED;
}
MtpObjectHandleList* Database::getObjectList(MtpStorageID storage, MtpObjectFormat format, MtpObjectHandle parent) {
  if (!valid() || (storage != 0 && storage != UINT32_MAX && storage != kStorageId)) return nullptr;
  bool all = parent == 0;
  if (!all && parent == MTP_PARENT_ROOT) parent = 0;
  if (all ? !ScanAll() : !Scan(parent)) return nullptr;
  auto result = std::make_unique<MtpObjectHandleList>();
  for (size_t i = 1; i < entries_.size(); ++i)
    if (entries_[i].alive && !entries_[i].pending && (all || entries_[i].parent == parent) && (!format || Format(entries_[i]) == format))
      result->push_back(i);
  return result.release();
}
int Database::getNumObjects(MtpStorageID storage, MtpObjectFormat format, MtpObjectHandle parent) {
  std::unique_ptr<MtpObjectHandleList> list(getObjectList(storage, format, parent));
  return list ? static_cast<int>(list->size()) : -1;
}
MtpObjectFormatList* Database::getSupportedPlaybackFormats() { return new MtpObjectFormatList{MTP_FORMAT_UNDEFINED, MTP_FORMAT_ASSOCIATION}; }
MtpObjectFormatList* Database::getSupportedCaptureFormats() { return writable_ ? new MtpObjectFormatList{MTP_FORMAT_UNDEFINED, MTP_FORMAT_ASSOCIATION} : new MtpObjectFormatList; }
MtpObjectPropertyList* Database::getSupportedObjectProperties(MtpObjectFormat) { return new MtpObjectPropertyList(std::begin(kProperties), std::end(kProperties)); }
MtpDevicePropertyList* Database::getSupportedDeviceProperties() { return new MtpDevicePropertyList{MTP_DEVICE_PROPERTY_DEVICE_FRIENDLY_NAME}; }
MtpDataType Database::PropertyType(MtpObjectProperty property) const {
  switch (property) {
    case MTP_PROPERTY_STORAGE_ID: case MTP_PROPERTY_PARENT_OBJECT: return MTP_TYPE_UINT32;
    case MTP_PROPERTY_OBJECT_FORMAT: case MTP_PROPERTY_PROTECTION_STATUS: return MTP_TYPE_UINT16;
    case MTP_PROPERTY_OBJECT_SIZE: return MTP_TYPE_UINT64;
    case MTP_PROPERTY_PERSISTENT_UID: return MTP_TYPE_UINT128;
    case MTP_PROPERTY_OBJECT_FILE_NAME: case MTP_PROPERTY_NAME: case MTP_PROPERTY_DATE_MODIFIED: return MTP_TYPE_STR;
    default: return 0;
  }
}
MtpResponseCode Database::getObjectPropertyValue(MtpObjectHandle handle, MtpObjectProperty property, MtpDataPacket& packet) {
  const auto* entry = Get(handle);
  auto fd = OpenEntry(handle);
  struct stat st{};
  if (!entry || fd.get() < 0 || fstat(fd.get(), &st) != 0) return MTP_RESPONSE_INVALID_OBJECT_HANDLE;
  switch (property) {
    case MTP_PROPERTY_STORAGE_ID: packet.putUInt32(kStorageId); break;
    case MTP_PROPERTY_PARENT_OBJECT: packet.putUInt32(entry->parent); break;
    case MTP_PROPERTY_OBJECT_FORMAT: packet.putUInt16(Format(*entry)); break;
    case MTP_PROPERTY_PROTECTION_STATUS: packet.putUInt16(writable_ ? 0 : 1); break;
    case MTP_PROPERTY_OBJECT_SIZE: packet.putUInt64(S_ISDIR(st.st_mode) ? 0 : st.st_size); break;
    case MTP_PROPERTY_PERSISTENT_UID: {
      uint128_t uid = {handle, kStorageId, 0, 0};
      packet.putUInt128(uid); break;
    }
    case MTP_PROPERTY_OBJECT_FILE_NAME: case MTP_PROPERTY_NAME: packet.putString(entry->name.c_str()); break;
    case MTP_PROPERTY_DATE_MODIFIED: {
      struct tm value{}; char text[32]{};
      if (gmtime_r(&st.st_mtime, &value)) strftime(text, sizeof(text), "%Y%m%dT%H%M%SZ", &value);
      packet.putString(text); break;
    }
    default: return MTP_RESPONSE_OBJECT_PROP_NOT_SUPPORTED;
  }
  return MTP_RESPONSE_OK;
}
MtpResponseCode Database::setObjectPropertyValue(MtpObjectHandle handle, MtpObjectProperty property, MtpDataPacket& packet) {
  if (!writable_) return MTP_RESPONSE_STORE_READ_ONLY;
  if (property != MTP_PROPERTY_OBJECT_FILE_NAME) return MTP_RESPONSE_OBJECT_PROP_NOT_SUPPORTED;
  auto* entry = Get(handle);
  if (!entry) return MTP_RESPONSE_INVALID_OBJECT_HANDLE;
  MtpStringBuffer buffer;
  if (!packet.getString(buffer)) return MTP_RESPONSE_INVALID_OBJECT_PROP_VALUE;
  const std::string name(static_cast<const char*>(buffer));
  if (!ValidName(name)) return MTP_RESPONSE_INVALID_OBJECT_PROP_VALUE;
  if (upload_handle_ != kInvalidObjectHandle) return MTP_RESPONSE_DEVICE_BUSY;
  auto fd = OpenEntry(handle), parent = OpenEntry(entry->parent);
  if (fd.get() < 0 || parent.get() < 0) return MTP_RESPONSE_INVALID_OBJECT_HANDLE;
  if (name == entry->name) return MTP_RESPONSE_OK;
  const auto old = entry->path;
  const auto prefix = entries_[entry->parent].path;
  const auto path = prefix.empty() ? name : prefix + "/" + name;
  // Check every descendant before an atomic, no-overwrite directory rename.
  for (const auto& child : entries_) if (child.alive &&
      (child.path == old || child.path.starts_with(old + "/")) &&
      path.size() + child.path.size() - old.size() >= PATH_MAX)
    return MTP_RESPONSE_INVALID_OBJECT_PROP_VALUE;
  if (syscall(SYS_renameat2, parent.get(), entry->name.c_str(), parent.get(), name.c_str(),
              RENAME_NOREPLACE) != 0) return MTP_RESPONSE_ACCESS_DENIED;
  entries_[handle].name = name;
  for (auto& child : entries_) if (child.alive &&
      (child.path == old || child.path.starts_with(old + "/")))
    child.path = path + child.path.substr(old.size());
  return fsync(parent.get()) == 0 ? MTP_RESPONSE_OK : MTP_RESPONSE_GENERAL_ERROR;
}
MtpResponseCode Database::getDevicePropertyValue(MtpDeviceProperty property, MtpDataPacket& packet) {
  if (property != MTP_DEVICE_PROPERTY_DEVICE_FRIENDLY_NAME) return MTP_RESPONSE_DEVICE_PROP_NOT_SUPPORTED;
  packet.putString("Recovery"); return MTP_RESPONSE_OK;
}
MtpResponseCode Database::setDevicePropertyValue(MtpDeviceProperty, MtpDataPacket&) { return MTP_RESPONSE_DEVICE_PROP_NOT_SUPPORTED; }
MtpResponseCode Database::resetDeviceProperty(MtpDeviceProperty) { return MTP_RESPONSE_DEVICE_PROP_NOT_SUPPORTED; }
MtpResponseCode Database::getObjectPropertyList(MtpObjectHandle handle, uint32_t format, uint32_t property, int group, int depth, MtpDataPacket& packet) {
  if (group) return MTP_RESPONSE_SPECIFICATION_BY_GROUP_UNSUPPORTED;
  if (!property) return MTP_RESPONSE_PARAMETER_NOT_SUPPORTED;
  if (format > UINT16_MAX) return MTP_RESPONSE_INVALID_OBJECT_FORMAT_CODE;
  if (depth != 0 && depth != 1 && depth != -1) return MTP_RESPONSE_SPECIFICATION_BY_DEPTH_UNSUPPORTED;
  MtpObjectHandleList handles;
  if (handle == UINT32_MAX || handle == 0) {
    // Match Android's root selectors: 0 means direct root children for depth
    // 0/1, while UINT32_MAX at depth 0 (or either selector at -1) means all.
    const bool shallow = depth == 1 || (handle == 0 && depth == 0);
    std::unique_ptr<MtpObjectHandleList> all(getObjectList(kStorageId, format, shallow ? MTP_PARENT_ROOT : 0));
    if (!all) return MTP_RESPONSE_GENERAL_ERROR;
    handles = *all;
  } else {
    auto* entry = Get(handle);
    if (!entry) return MTP_RESPONSE_INVALID_OBJECT_HANDLE;
    if (!format || Format(*entry) == format) handles.push_back(handle);
    if (depth && S_ISDIR(entry->stat.st_mode)) {
      MtpObjectHandleList parents{handle};
      for (size_t i = 0; i < parents.size(); ++i) {
        std::unique_ptr<MtpObjectHandleList> children(getObjectList(kStorageId, 0, parents[i]));
        if (!children) return MTP_RESPONSE_GENERAL_ERROR;
        for (auto child : *children) {
          if (!format || Format(entries_[child]) == format) handles.push_back(child);
          if (depth == -1 && S_ISDIR(entries_[child].stat.st_mode)) parents.push_back(child);
        }
      }
    }
  }
  MtpObjectPropertyList properties;
  if (property == UINT32_MAX) properties.assign(std::begin(kProperties), std::end(kProperties));
  else if (property <= UINT16_MAX && PropertyType(property)) properties.push_back(property);
  else return MTP_RESPONSE_OBJECT_PROP_NOT_SUPPORTED;
  packet.putUInt32(handles.size() * properties.size());
  for (auto object : handles) for (auto prop : properties) {
    packet.putUInt32(object); packet.putUInt16(prop); packet.putUInt16(PropertyType(prop));
    auto result = getObjectPropertyValue(object, prop, packet);
    if (result != MTP_RESPONSE_OK) { packet.reset(); return result; }
  }
  return MTP_RESPONSE_OK;
}
MtpResponseCode Database::getObjectInfo(MtpObjectHandle handle, MtpObjectInfo& info) {
  auto* entry = Get(handle);
  auto fd = OpenEntry(handle);
  struct stat st{};
  if (!entry || fd.get() < 0 || fstat(fd.get(), &st) != 0) return MTP_RESPONSE_INVALID_OBJECT_HANDLE;
  info.mStorageID = kStorageId; info.mFormat = Format(*entry); info.mProtectionStatus = writable_ ? 0 : 1;
  info.mCompressedSize = S_ISDIR(st.st_mode) ? 0 : std::min<uint64_t>(st.st_size, UINT32_MAX);
  info.mParent = entry->parent; info.mAssociationType = S_ISDIR(st.st_mode) ? 1 : 0;
  info.mDateCreated = st.st_mtime; info.mDateModified = st.st_mtime;
  free(info.mName); info.mName = strdup(entry->name.c_str());
  return info.mName ? MTP_RESPONSE_OK : MTP_RESPONSE_GENERAL_ERROR;
}
MtpResponseCode Database::getObjectFilePath(MtpObjectHandle handle, MtpStringBuffer& path, int64_t& length, MtpObjectFormat& format) {
  auto* entry = Get(handle);
  transfer_ = OpenEntry(handle);
  struct stat st{};
  if (!entry || transfer_.get() < 0 || fstat(transfer_.get(), &st) != 0 || !S_ISREG(st.st_mode))
    return MTP_RESPONSE_INVALID_OBJECT_HANDLE;
  // AOSP opens this path itself. Pin a checked O_RDONLY inode rather than hand
  // it a pathname that can race with a symlink/directory replacement.
  transfer_path_ = "/proc/self/fd/" + std::to_string(transfer_.get());
  path.set(transfer_path_.c_str()); length = st.st_size; format = Format(*entry);
  return MTP_RESPONSE_OK;
}
int Database::openFilePath(const char* path, bool transcode) {
  if (!path || transcode || transfer_.get() < 0 || path != transfer_path_) return -1;
  return fcntl(transfer_.get(), F_DUPFD_CLOEXEC, 0);
}
MtpObjectHandleList* Database::getObjectReferences(MtpObjectHandle handle) { return Get(handle) ? new MtpObjectHandleList : nullptr; }
MtpProperty* Database::getObjectPropertyDesc(MtpObjectProperty property, MtpObjectFormat) {
  auto type = PropertyType(property);
  return type ? new MtpProperty(property, type, writable_ && property == MTP_PROPERTY_OBJECT_FILE_NAME) : nullptr;
}
MtpProperty* Database::getDevicePropertyDesc(MtpDeviceProperty property) {
  if (property != MTP_DEVICE_PROPERTY_DEVICE_FRIENDLY_NAME) return nullptr;
  auto* result = new MtpProperty(property, MTP_TYPE_STR, false);
  const uint16_t name[] = {'R','e','c','o','v','e','r','y',0};
  result->setDefaultValue(name); result->setCurrentValue(name); return result;
}
}  // namespace recovery_mtp
