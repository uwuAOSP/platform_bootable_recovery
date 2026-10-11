/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <sqlite3.h>
#include <memory>
#include <string>
#include "secure_bytes.h"
namespace recovery_crypto::android17 {
bool ApplyCommittedWal(Bytes* database, View wal);
class Snapshot final {
 public:
  ~Snapshot();
  bool Open(const std::string& path);
  sqlite3* get() const {
    return db_;
  }

 private:
  Bytes image_;
  sqlite3* db_ = nullptr;
};
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement Query(sqlite3* db, const char* sql);
bool ReadProtectorId(uint32_t user, uint64_t* id);
bool ReadPasswordQuality(uint32_t user, int64_t* quality);
// Only an absent setting defaults to 3. Malformed/unsupported settings fail.
bool ReadPatternSizeFromDatabase(sqlite3* db, uint32_t user, uint32_t* size);
bool ReadPatternSize(uint32_t user, uint32_t* size);
// Returns only live, unencrypted locksettings keys from the reviewed keystore2 schema.
// Super-encrypted, boot-level-bound, ambiguous or non-TEE keys fail closed.
bool ReadProtectorKey(uint64_t protector, Bytes* key, int* security_level);
}  // namespace recovery_crypto::android17
