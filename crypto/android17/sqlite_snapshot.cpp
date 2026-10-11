/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sqlite_snapshot.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include "files.h"
#include "recovery_crypto/backend.h"
namespace recovery_crypto::android17 {
constexpr size_t kMaxDatabase = 64 * 1024 * 1024;
static uint32_t Be(View v, size_t p) {
  return (uint32_t(v[p]) << 24) | (uint32_t(v[p + 1]) << 16) | (uint32_t(v[p + 2]) << 8) | v[p + 3];
}
static bool Checksum(View v, bool big, uint32_t* s1, uint32_t* s2) {
  if (v.size() % 8) return false;
  for (size_t i = 0; i < v.size(); i += 8) {
    auto word = [&](size_t p) {
      return big ? Be(v, p)
                 : uint32_t(v[p]) | (uint32_t(v[p + 1]) << 8) | (uint32_t(v[p + 2]) << 16) |
                       (uint32_t(v[p + 3]) << 24);
    };
    *s1 += word(i) + *s2;
    *s2 += word(i + 4) + *s1;
  }
  return true;
}
bool ApplyCommittedWal(Bytes* database, View wal) {
  if (database->size() < 100 || memcmp(database->data(), "SQLite format 3\0", 16) != 0)
    return false;
  uint32_t page = (uint32_t((*database)[16]) << 8) | (*database)[17];
  if (page == 1) page = 65536;
  if (page < 512 || page > 65536 || (page & (page - 1)) || database->size() % page) return false;
  if (wal.empty()) return true;
  if (wal.size() < 32 || (Be(wal, 0) != 0x377f0682 && Be(wal, 0) != 0x377f0683) ||
      Be(wal, 4) != 3007000 || Be(wal, 8) != page)
    return false;
  bool big = Be(wal, 0) == 0x377f0683;
  uint32_t s1 = 0, s2 = 0;
  Checksum(wal.first(24), big, &s1, &s2);
  if (s1 != Be(wal, 24) || s2 != Be(wal, 28)) return false;
  size_t committed_end = 32;
  uint32_t committed_pages = 0;
  for (size_t p = 32; p + 24 + page <= wal.size(); p += 24 + page) {
    // Reused WAL files may contain trailing frames from an older salt generation.
    if (Be(wal, p + 8) != Be(wal, 16) || Be(wal, p + 12) != Be(wal, 20)) break;
    uint32_t number = Be(wal, p), count = Be(wal, p + 4);
    if (!number || uint64_t(number) * page > kMaxDatabase || uint64_t(count) * page > kMaxDatabase)
      return false;
    Checksum(wal.subspan(p, 8), big, &s1, &s2);
    Checksum(wal.subspan(p + 24, page), big, &s1, &s2);
    if (s1 != Be(wal, p + 16) || s2 != Be(wal, p + 20)) return false;
    if (count) {
      committed_end = p + 24 + page;
      committed_pages = count;
    }
  }
  if (!committed_pages) return true;
  database->resize(size_t(committed_pages) * page);
  for (size_t p = 32; p < committed_end; p += 24 + page) {
    uint32_t number = Be(wal, p);
    if (number <= committed_pages)
      memcpy(database->data() + size_t(number - 1) * page, wal.data() + p + 24, page);
  }
  return true;
}
Snapshot::~Snapshot() {
  if (db_) sqlite3_close(db_);
}
bool Snapshot::Open(const std::string& path) {
  if (db_) return false;
  Bytes wal, journal;
  bool missing = false;
  if (ReadFile(path + "-journal", &journal, kMaxDatabase, &missing)) {
    // A hot rollback journal requires recovery, which must not run on the original database.
    if (!journal.empty() &&
        std::any_of(journal.begin(), journal.end(), [](uint8_t n) { return n != 0; }))
      return false;
  } else if (!missing)
    return false;
  if (!ReadFile(path, &image_, kMaxDatabase)) return false;
  if (!ReadFile(path + "-wal", &wal, kMaxDatabase, &missing) && !missing) return false;
  if (!ApplyCommittedWal(&image_, wal)) return false;
  // This is a private, consolidated image. Do not ask SQLite to open WAL/SHM sidecars.
  image_[18] = 1;
  image_[19] = 1;
  if (sqlite3_open_v2(":memory:", &db_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                      nullptr) != SQLITE_OK)
    return false;
  if (sqlite3_deserialize(db_, "main", image_.data(), image_.size(), image_.size(),
                          SQLITE_DESERIALIZE_READONLY) != SQLITE_OK)
    return false;
  sqlite3_db_config(db_, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
  sqlite3_db_config(db_, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
  sqlite3_limit(db_, SQLITE_LIMIT_LENGTH, kMaxDatabase);
  return sqlite3_exec(db_, "PRAGMA query_only=ON;", nullptr, nullptr, nullptr) == SQLITE_OK;
}
Statement Query(sqlite3* db, const char* sql) {
  sqlite3_stmt* s = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
    if (s) sqlite3_finalize(s);
    s = nullptr;
  }
  return Statement(s, sqlite3_finalize);
}
bool ReadProtectorId(uint32_t user, uint64_t* id) {
  Snapshot db;
  if (!db.Open("/data/system/locksettings.db")) return false;
  auto q = Query(db.get(), "SELECT value FROM locksettings WHERE name='sp-handle' AND user=?");
  if (!q || sqlite3_bind_int64(q.get(), 1, user) != SQLITE_OK ||
      sqlite3_step(q.get()) != SQLITE_ROW)
    return false;
  const char* value = reinterpret_cast<const char*>(sqlite3_column_text(q.get(), 0));
  int length = sqlite3_column_bytes(q.get(), 0);
  int64_t signed_id = 0;
  if (!value || length < 1 || length > 20) return false;
  auto [end, error] = std::from_chars(value, value + length, signed_id);
  if (error != std::errc{} || end != value + length || signed_id == 0 ||
      sqlite3_step(q.get()) != SQLITE_DONE)
    return false;
  *id = static_cast<uint64_t>(signed_id);
  return true;
}
bool ReadPasswordQuality(uint32_t user, int64_t* quality) {
  Snapshot db;
  if (!db.Open("/data/system/locksettings.db")) return false;
  auto q = Query(db.get(),
                 "SELECT value FROM locksettings WHERE name='lockscreen.password_type' AND user=?");
  if (!q || sqlite3_bind_int64(q.get(), 1, user) != SQLITE_OK ||
      sqlite3_step(q.get()) != SQLITE_ROW)
    return false;
  const char* value = reinterpret_cast<const char*>(sqlite3_column_text(q.get(), 0));
  int length = sqlite3_column_bytes(q.get(), 0);
  if (!value || length < 1 || length > 10) return false;
  auto [end, error] = std::from_chars(value, value + length, *quality);
  return error == std::errc{} && end == value + length && *quality >= 0 &&
         sqlite3_step(q.get()) == SQLITE_DONE;
}
bool ReadPatternSizeFromDatabase(sqlite3* db, uint32_t user, uint32_t* size) {
  if (!db || !size) return false;
  auto q = Query(db, "SELECT value FROM locksettings WHERE name='lock_pattern_size' AND user=?");
  if (!q || sqlite3_bind_int64(q.get(), 1, user) != SQLITE_OK) return false;
  const int step = sqlite3_step(q.get());
  if (step == SQLITE_DONE) {
    // AOSP/older installs without this custom setting use the standard grid.
    *size = RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE;
    return true;
  }
  if (step != SQLITE_ROW) return false;
  const char* value = reinterpret_cast<const char*>(sqlite3_column_text(q.get(), 0));
  int length = sqlite3_column_bytes(q.get(), 0);
  uint32_t parsed = 0;
  if (!value || length < 1 || length > 3) return false;
  auto [end, error] = std::from_chars(value, value + length, parsed);
  if (error != std::errc{} || end != value + length ||
      parsed < RECOVERY_CRYPTO_DEFAULT_PATTERN_SIZE || parsed > RECOVERY_CRYPTO_MAX_PATTERN_SIZE ||
      sqlite3_step(q.get()) != SQLITE_DONE)
    return false;
  *size = parsed;
  return true;
}
bool ReadPatternSize(uint32_t user, uint32_t* size) {
  Snapshot db;
  return db.Open("/data/system/locksettings.db") && ReadPatternSizeFromDatabase(db.get(), user, size);
}
bool ReadProtectorKey(uint64_t protector, Bytes* key, int* security_level) {
  Clear(*key);
  Snapshot db;
  if (!db.Open("/data/misc/keystore/persistent.sqlite")) return false;
  char alias[48];
  snprintf(alias, sizeof(alias), "synthetic_password_%llx",
           static_cast<unsigned long long>(protector));
  auto q = Query(
      db.get(),
      "SELECT k.id,k.km_uuid,b.id,b.blob FROM keyentry k JOIN blobentry b ON b.keyentryid=k.id "
      "WHERE k.key_type=0 AND k.domain=2 AND k.namespace=103 AND k.state=1 AND CAST(k.alias AS "
      "TEXT)=? "
      "AND b.subcomponent_type=0 AND b.state=0");
  if (!q || sqlite3_bind_text(q.get(), 1, alias, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_step(q.get()) != SQLITE_ROW)
    return false;
  int64_t blob_id = sqlite3_column_int64(q.get(), 2);
  View uuid(static_cast<const uint8_t*>(sqlite3_column_blob(q.get(), 1)),
            sqlite3_column_bytes(q.get(), 1));
  if (uuid.size() != 16 ||
      std::any_of(uuid.begin(), uuid.end() - 1, [](uint8_t n) { return n != 0; }) ||
      (uuid.back() != 1 && uuid.back() != 2))
    return false;
  *security_level = uuid.back();
  const auto* raw = static_cast<const uint8_t*>(sqlite3_column_blob(q.get(), 3));
  int length = sqlite3_column_bytes(q.get(), 3);
  if (!raw || length <= 0 || length > 65536) return false;
  key->assign(raw, raw + length);
  if (sqlite3_step(q.get()) != SQLITE_DONE) {
    Clear(*key);
    return false;
  }
  // Reviewed BlobMetaData ordinals: EncryptedBy=0, Salt=1, IV=2, AEAD=3, KmUuid=4,
  // PublicKey=5, MaxBootLevel=6. Never interpret a super-encrypted blob as a KeyMint blob.
  auto metadata = Query(db.get(), "SELECT tag,data FROM blobmetadata WHERE blobentryid=?");
  if (!metadata || sqlite3_bind_int64(metadata.get(), 1, blob_id) != SQLITE_OK) return false;
  int step;
  while ((step = sqlite3_step(metadata.get())) == SQLITE_ROW) {
    if (sqlite3_column_int(metadata.get(), 0) != 4) {
      Clear(*key);
      return false;
    }
    const auto* data = static_cast<const uint8_t*>(sqlite3_column_blob(metadata.get(), 1));
    if (!data || sqlite3_column_bytes(metadata.get(), 1) != 16 ||
        std::any_of(data, data + 15, [](uint8_t n) { return n != 0; }) ||
        data[15] != *security_level) {
      Clear(*key);
      return false;
    }
  }
  return step == SQLITE_DONE;
}
}  // namespace recovery_crypto::android17
