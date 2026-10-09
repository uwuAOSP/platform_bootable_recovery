/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <IMtpDatabase.h>
#include <mtp.h>
#include <android-base/unique_fd.h>
#include <sys/stat.h>
#include <string>
#include <vector>

namespace recovery_mtp {
using namespace android;
constexpr MtpStorageID kStorageId = 0x00010001;

// Index of a single, already unlocked media directory. Never exposes
// symlinks, device nodes, other filesystems or host-supplied paths. All relative
// paths start at the pinned media root; no separate key directory is indexed.
class Database final : public IMtpDatabase {
 public:
  explicit Database(const std::string& root, size_t limit = 100000);
  ~Database() override;
  bool valid() const { return root_.get() >= 0; }
  void EnableWrites(bool enable);
  bool recoveryStorageWritable() override { return writable_; }
  MtpResponseCode recoveryBeginUpload(const char* name, MtpObjectFormat format,
      MtpObjectHandle parent, MtpStorageID storage, uint64_t size, MtpObjectHandle* handle) override;
  int recoveryUploadFd(MtpObjectHandle handle) override;
  MtpResponseCode recoveryFinishUpload(MtpObjectHandle handle, bool success) override;
  MtpResponseCode recoveryDelete(MtpObjectHandle handle) override;
  MtpObjectHandle beginSendObject(const char*, MtpObjectFormat, MtpObjectHandle, MtpStorageID) override { return kInvalidObjectHandle; }
  void endSendObject(MtpObjectHandle, bool) override {}
  void rescanFile(const char*, MtpObjectHandle, MtpObjectFormat) override {}
  MtpObjectHandleList* getObjectList(MtpStorageID, MtpObjectFormat, MtpObjectHandle) override;
  int getNumObjects(MtpStorageID, MtpObjectFormat, MtpObjectHandle) override;
  MtpObjectFormatList* getSupportedPlaybackFormats() override;
  MtpObjectFormatList* getSupportedCaptureFormats() override;
  MtpObjectPropertyList* getSupportedObjectProperties(MtpObjectFormat) override;
  MtpDevicePropertyList* getSupportedDeviceProperties() override;
  MtpResponseCode getObjectPropertyValue(MtpObjectHandle, MtpObjectProperty, MtpDataPacket&) override;
  MtpResponseCode setObjectPropertyValue(MtpObjectHandle, MtpObjectProperty, MtpDataPacket&) override;
  MtpResponseCode getDevicePropertyValue(MtpDeviceProperty, MtpDataPacket&) override;
  MtpResponseCode setDevicePropertyValue(MtpDeviceProperty, MtpDataPacket&) override;
  MtpResponseCode resetDeviceProperty(MtpDeviceProperty) override;
  MtpResponseCode getObjectPropertyList(MtpObjectHandle, uint32_t, uint32_t, int, int, MtpDataPacket&) override;
  MtpResponseCode getObjectInfo(MtpObjectHandle, MtpObjectInfo&) override;
  void* getThumbnail(MtpObjectHandle, size_t& size) override { size = 0; return nullptr; }
  MtpResponseCode getObjectFilePath(MtpObjectHandle, MtpStringBuffer&, int64_t&, MtpObjectFormat&) override;
  int openFilePath(const char*, bool) override;
  MtpResponseCode beginDeleteObject(MtpObjectHandle) override { return MTP_RESPONSE_STORE_READ_ONLY; }
  void endDeleteObject(MtpObjectHandle, bool) override {}
  MtpObjectHandleList* getObjectReferences(MtpObjectHandle) override;
  MtpResponseCode setObjectReferences(MtpObjectHandle, MtpObjectHandleList*) override { return MTP_RESPONSE_STORE_READ_ONLY; }
  MtpProperty* getObjectPropertyDesc(MtpObjectProperty, MtpObjectFormat) override;
  MtpProperty* getDevicePropertyDesc(MtpDeviceProperty) override;
  MtpResponseCode beginMoveObject(MtpObjectHandle, MtpObjectHandle, MtpStorageID) override { return MTP_RESPONSE_STORE_READ_ONLY; }
  void endMoveObject(MtpObjectHandle, MtpObjectHandle, MtpStorageID, MtpStorageID, MtpObjectHandle, bool) override {}
  MtpResponseCode beginCopyObject(MtpObjectHandle, MtpObjectHandle, MtpStorageID) override { return MTP_RESPONSE_STORE_READ_ONLY; }
  void endCopyObject(MtpObjectHandle, bool) override {}
 private:
  struct Entry {
    std::string path, name;
    struct stat stat{};
    MtpObjectHandle parent = 0;
    bool scanned = false;
    bool alive = true, pending = false;
  };
  android::base::unique_fd Open(const std::string& path) const;
  android::base::unique_fd OpenEntry(MtpObjectHandle handle) const;
  bool Scan(MtpObjectHandle parent);
  bool ScanAll();
  const Entry* Get(MtpObjectHandle handle) const;
  MtpObjectFormat Format(const Entry& entry) const;
  MtpDataType PropertyType(MtpObjectProperty property) const;
  bool ValidName(const std::string& name) const;
  void CancelUpload();
  bool DeleteEntry(MtpObjectHandle handle);
  android::base::unique_fd root_, transfer_;
  android::base::unique_fd upload_, upload_parent_;
  MtpObjectHandle upload_handle_ = kInvalidObjectHandle;
  uint64_t upload_size_ = 0;
  bool writable_ = false;
  std::string transfer_path_;
  std::vector<Entry> entries_;
  size_t limit_;
};
}  // namespace recovery_mtp
