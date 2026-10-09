/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "database.h"
#include <MtpDataPacket.h>
#include <MtpObjectInfo.h>
#include <MtpStringBuffer.h>
#include <android-base/file.h>
#include <android-base/test_utils.h>
#include <gtest/gtest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <string>

using namespace android;
using recovery_mtp::Database;
using recovery_mtp::kStorageId;

TEST(RecoveryMtp, EnumeratesDirectoriesUnicodeAndExcludesLinks) {
  TemporaryDir temp;
  const std::string root(temp.path);
  ASSERT_EQ(0, mkdir((root + "/Download").c_str(), 0700));
  ASSERT_TRUE(android::base::WriteStringToFile("rom", root + "/Download/更新.zip"));
  ASSERT_EQ(0, symlink("/etc", (root + "/escape").c_str()));
  ASSERT_EQ(0, mkfifo((root + "/pipe").c_str(), 0600));
  Database db(root);
  std::unique_ptr<MtpObjectHandleList> children(db.getObjectList(kStorageId, 0, MTP_PARENT_ROOT));
  ASSERT_NE(nullptr, children);
  ASSERT_EQ(1u, children->size());
  MtpObjectInfo directory(children->at(0));
  ASSERT_EQ(MTP_RESPONSE_OK, db.getObjectInfo(directory.mHandle, directory));
  EXPECT_STREQ("Download", directory.mName);
  EXPECT_EQ(MTP_FORMAT_ASSOCIATION, directory.mFormat);
  std::unique_ptr<MtpObjectHandleList> files(db.getObjectList(kStorageId, 0, directory.mHandle));
  ASSERT_NE(nullptr, files);
  ASSERT_EQ(1u, files->size());
  MtpObjectInfo file(files->at(0));
  ASSERT_EQ(MTP_RESPONSE_OK, db.getObjectInfo(file.mHandle, file));
  EXPECT_STREQ("更新.zip", file.mName);
  EXPECT_EQ(3u, file.mCompressedSize);
  EXPECT_EQ(directory.mHandle, file.mParent);
}
TEST(RecoveryMtp, RejectsReplacedInodesAndPinsSuccessfulTransfers) {
  TemporaryDir temp;
  const std::string root(temp.path);
  ASSERT_TRUE(android::base::WriteStringToFile("original", root + "/file"));
  Database db(root);
  std::unique_ptr<MtpObjectHandleList> objects(db.getObjectList(kStorageId, 0, MTP_PARENT_ROOT));
  ASSERT_NE(nullptr, objects);
  ASSERT_EQ(1u, objects->size());
  const auto handle = objects->at(0);
  MtpStringBuffer path;
  int64_t length;
  MtpObjectFormat format;
  ASSERT_EQ(MTP_RESPONSE_OK, db.getObjectFilePath(handle, path, length, format));
  ASSERT_EQ(0, rename((root + "/file").c_str(), (root + "/old").c_str()));
  ASSERT_TRUE(android::base::WriteStringToFile("replacement", root + "/file"));
  const int raw = db.openFilePath(static_cast<const char*>(path), false);
  ASSERT_GE(raw, 0);
  android::base::unique_fd fd(raw);
  std::string content;
  ASSERT_TRUE(android::base::ReadFdToString(fd.get(), &content));
  EXPECT_EQ("original", content);
  EXPECT_EQ(-1, db.openFilePath("/etc/passwd", false));
  EXPECT_EQ(-1, db.openFilePath(static_cast<const char*>(path), true));
  EXPECT_EQ(MTP_RESPONSE_INVALID_OBJECT_HANDLE, db.getObjectFilePath(handle, path, length, format));
}
TEST(RecoveryMtp, ReportsLargeSizesWithoutTruncatingTransfer) {
  TemporaryDir temp;
  const std::string root(temp.path);
  android::base::unique_fd fd(open((root + "/large.zip").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600));
  ASSERT_GE(fd.get(), 0);
  constexpr int64_t size = (int64_t{1} << 32) + 4096;
  ASSERT_EQ(0, ftruncate(fd.get(), size));
  Database db(root);
  std::unique_ptr<MtpObjectHandleList> objects(db.getObjectList(kStorageId, 0, MTP_PARENT_ROOT));
  ASSERT_NE(nullptr, objects);
  ASSERT_EQ(1u, objects->size());
  MtpStringBuffer path;
  int64_t length;
  MtpObjectFormat format;
  ASSERT_EQ(MTP_RESPONSE_OK, db.getObjectFilePath(objects->at(0), path, length, format));
  EXPECT_EQ(size, length);
  MtpDataPacket packet;
  ASSERT_EQ(MTP_RESPONSE_OK, db.getObjectPropertyValue(objects->at(0), MTP_PROPERTY_OBJECT_SIZE, packet));
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= uint64_t(packet.getData()[i]) << (8 * i);
  EXPECT_EQ(uint64_t(size), value);
}
TEST(RecoveryMtp, RefusesMutationInvalidHandlesAndUnboundedEnumeration) {
  TemporaryDir temp;
  const std::string root(temp.path);
  ASSERT_TRUE(android::base::WriteStringToFile("1", root + "/a"));
  ASSERT_TRUE(android::base::WriteStringToFile("2", root + "/b"));
  Database bounded(root, 1);
  EXPECT_EQ(nullptr, bounded.getObjectList(kStorageId, 0, MTP_PARENT_ROOT));
  Database db(root);
  MtpDataPacket packet;
  EXPECT_EQ(MTP_RESPONSE_STORE_READ_ONLY, db.setObjectPropertyValue(1, MTP_PROPERTY_OBJECT_FILE_NAME, packet));
  EXPECT_EQ(MTP_RESPONSE_STORE_READ_ONLY, db.beginDeleteObject(1));
  EXPECT_EQ(MTP_RESPONSE_STORE_READ_ONLY, db.beginMoveObject(1, 0, kStorageId));
  EXPECT_EQ(MTP_RESPONSE_STORE_READ_ONLY, db.beginCopyObject(1, 0, kStorageId));
  EXPECT_EQ(kInvalidObjectHandle, db.beginSendObject("../../secret", MTP_FORMAT_UNDEFINED, 0, kStorageId));
  EXPECT_EQ(MTP_RESPONSE_INVALID_OBJECT_HANDLE, db.getObjectPropertyValue(UINT32_MAX, MTP_PROPERTY_OBJECT_SIZE, packet));
  Database traversal(root + "/../");
  EXPECT_FALSE(traversal.valid());
}
TEST(RecoveryMtp, RootPropertyListsPreserveShallowEnumeration) {
  TemporaryDir temp;
  const std::string root(temp.path);
  ASSERT_EQ(0, mkdir((root + "/folder").c_str(), 0700));
  ASSERT_TRUE(android::base::WriteStringToFile("child", root + "/folder/child"));
  ASSERT_TRUE(android::base::WriteStringToFile("root", root + "/file"));
  Database db(root);
  auto count = [&](MtpObjectHandle handle, int depth) {
    MtpDataPacket packet;
    const auto result = db.getObjectPropertyList(handle, 0, MTP_PROPERTY_NAME, 0, depth, packet);
    EXPECT_EQ(MTP_RESPONSE_OK, result);
    if (result != MTP_RESPONSE_OK) return UINT32_MAX;
    const auto* data = packet.getData();
    return uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
  };
  EXPECT_EQ(2u, count(0, 0));
  EXPECT_EQ(2u, count(0, 1));
  EXPECT_EQ(2u, count(UINT32_MAX, 1));
  EXPECT_EQ(3u, count(0, -1));
  EXPECT_EQ(3u, count(UINT32_MAX, 0));
}
TEST(RecoveryMtp, UploadPublishesOnlyCompleteInodeAndRefusesOverwrite) {
  TemporaryDir temp;
  const std::string root(temp.path);
  Database db(root);
  db.EnableWrites(true);
  ASSERT_TRUE(db.recoveryStorageWritable());
  MtpObjectHandle handle = kInvalidObjectHandle;
  ASSERT_EQ(MTP_RESPONSE_OK, db.recoveryBeginUpload("更新.zip", MTP_FORMAT_UNDEFINED, 0, kStorageId, 3, &handle));
  EXPECT_NE(0, access((root + "/更新.zip").c_str(), F_OK));
  android::base::unique_fd fd(db.recoveryUploadFd(handle));
  ASSERT_GE(fd.get(), 0);
  ASSERT_TRUE(android::base::WriteStringToFd("zip", fd.get()));
  ASSERT_EQ(MTP_RESPONSE_OK, db.recoveryFinishUpload(handle, true));
  std::string value;
  ASSERT_TRUE(android::base::ReadFileToString(root + "/更新.zip", &value));
  EXPECT_EQ("zip", value);
  EXPECT_EQ(MTP_RESPONSE_ACCESS_DENIED, db.recoveryBeginUpload("更新.zip", MTP_FORMAT_UNDEFINED, 0, kStorageId, 0, &handle));
  MtpStringBuffer path; int64_t length; MtpObjectFormat format;
  EXPECT_EQ(MTP_RESPONSE_OK, db.getObjectFilePath(handle, path, length, format));
  EXPECT_EQ(3, length);
}
TEST(RecoveryMtp, CancelShortUploadAndTraversalLeaveNoPublishedFile) {
  TemporaryDir temp;
  const std::string root(temp.path);
  Database db(root);
  db.EnableWrites(true);
  for (bool success : {false, true}) {
    MtpObjectHandle handle = kInvalidObjectHandle;
    ASSERT_EQ(MTP_RESPONSE_OK, db.recoveryBeginUpload("rom.zip", MTP_FORMAT_UNDEFINED, 0, kStorageId, 100, &handle));
    android::base::unique_fd fd(db.recoveryUploadFd(handle));
    ASSERT_TRUE(android::base::WriteStringToFd("short", fd.get()));
    EXPECT_NE(MTP_RESPONSE_OK, db.recoveryFinishUpload(handle, success));
    EXPECT_NE(0, access((root + "/rom.zip").c_str(), F_OK));
  }
  MtpObjectHandle handle = kInvalidObjectHandle;
  for (const char* name : {"../key", "folder/file", ".", "..", "dir\\file"})
    EXPECT_EQ(MTP_RESPONSE_INVALID_PARAMETER, db.recoveryBeginUpload(name, MTP_FORMAT_UNDEFINED, 0, kStorageId, 0, &handle));
  ASSERT_EQ(0, symlink("/data/misc/vold", (root + "/escape").c_str()));
  EXPECT_EQ(MTP_RESPONSE_ACCESS_DENIED, db.recoveryBeginUpload("escape", MTP_FORMAT_UNDEFINED, 0, kStorageId, 0, &handle));
}
TEST(RecoveryMtp, RenameDirectoryKeepsHandlesAndDeleteIsRecursive) {
  TemporaryDir temp;
  const std::string root(temp.path);
  Database db(root);
  db.EnableWrites(true);
  MtpObjectHandle parent, child;
  ASSERT_EQ(MTP_RESPONSE_OK, db.recoveryBeginUpload("folder", MTP_FORMAT_ASSOCIATION, 0, kStorageId, 0, &parent));
  ASSERT_EQ(MTP_RESPONSE_OK, db.recoveryBeginUpload("child", MTP_FORMAT_UNDEFINED, parent, kStorageId, 0, &child));
  ASSERT_EQ(MTP_RESPONSE_OK, db.recoveryFinishUpload(child, true));
  MtpDataPacket encoded, input;
  encoded.putString("renamed"); input.copyFrom(encoded);
  ASSERT_EQ(MTP_RESPONSE_OK, db.setObjectPropertyValue(parent, MTP_PROPERTY_OBJECT_FILE_NAME, input));
  MtpStringBuffer path; int64_t length; MtpObjectFormat format;
  EXPECT_EQ(MTP_RESPONSE_OK, db.getObjectFilePath(child, path, length, format));
  EXPECT_EQ(0, access((root + "/renamed/child").c_str(), F_OK));
  ASSERT_EQ(MTP_RESPONSE_OK, db.recoveryDelete(parent));
  EXPECT_NE(0, access((root + "/renamed").c_str(), F_OK));
  EXPECT_EQ(MTP_RESPONSE_INVALID_OBJECT_HANDLE, db.getObjectFilePath(child, path, length, format));
  std::unique_ptr<MtpObjectHandleList> remaining(db.getObjectList(kStorageId, 0, 0));
  ASSERT_NE(nullptr, remaining);
  EXPECT_TRUE(remaining->empty());
}
TEST(RecoveryMtp, ReadonlyProfileAndReplacedInodesCannotMutate) {
  TemporaryDir temp;
  const std::string root(temp.path);
  ASSERT_TRUE(android::base::WriteStringToFile("old", root + "/file"));
  Database db(root);
  std::unique_ptr<MtpObjectHandleList> objects(db.getObjectList(kStorageId, 0, MTP_PARENT_ROOT));
  ASSERT_NE(nullptr, objects);
  ASSERT_EQ(1u, objects->size());
  MtpObjectHandle handle = objects->at(0), uploaded;
  EXPECT_EQ(MTP_RESPONSE_STORE_READ_ONLY, db.recoveryBeginUpload("new", MTP_FORMAT_UNDEFINED, 0, kStorageId, 0, &uploaded));
  EXPECT_EQ(MTP_RESPONSE_STORE_READ_ONLY, db.recoveryDelete(handle));
  db.EnableWrites(true);
  ASSERT_EQ(0, rename((root + "/file").c_str(), (root + "/old").c_str()));
  ASSERT_TRUE(android::base::WriteStringToFile("replacement", root + "/file"));
  EXPECT_EQ(MTP_RESPONSE_PARTIAL_DELETION, db.recoveryDelete(handle));
  std::string content;
  ASSERT_TRUE(android::base::ReadFileToString(root + "/file", &content));
  EXPECT_EQ("replacement", content);
}
