/*
 * SPDX-FileCopyrightText: The uwuAOSP Project
 * SPDX-License-Identifier: Apache-2.0
 */
// Pure format/crypto fixtures only. No test calls a HAL, the backend or real key paths.
#include <gtest/gtest.h>
#include <cerrno>
#include <climits>
#include "diagnostic.h"
#include "dm_ioctl_compat.h"
#include "primitives.h"
#include "sqlite_snapshot.h"
#include "synthetic_password.h"
#include "vectors.h"
using namespace recovery_crypto::android17;
TEST(Android17RecoveryCrypto, GatekeeperBoundsAcceptLegacyVersionZeroWithoutAuthentication) {
  // Synthetic handle with the public packed password_handle_t size; no SID,
  // salt/signature from a phone and no HAL lookup or credential attempt.
  Bytes handle(58, 0);
  EXPECT_TRUE(ValidGatekeeperInput(100000, handle));
  for (uint8_t version : {0, 1, 2, 3}) {
    handle[0] = version;
    EXPECT_TRUE(ValidGatekeeperInput(100000, handle));
  }
  handle[0] = 4;
  EXPECT_FALSE(ValidGatekeeperInput(100000, handle));
  handle[0] = 0;
  EXPECT_FALSE(ValidGatekeeperInput(UINT32_MAX, handle));
  EXPECT_FALSE(ValidGatekeeperInput(100000, {}));
  handle.resize(8);
  EXPECT_FALSE(ValidGatekeeperInput(100000, handle));
  handle.resize(4097);
  EXPECT_FALSE(ValidGatekeeperInput(100000, handle));
}
TEST(Android17RecoveryCrypto, DiagnosticFormatIsBoundedAndContainsOnlyTypedCodes) {
  EXPECT_EQ(FormatDiagnostic(RC_IO_ERROR, Checkpoint::KeymintBegin, CodeSource::Hal, -26),
            "[recovery_crypto] point=keymint_begin result=7 source=hal code=-26\n");
  EXPECT_EQ(FormatDiagnostic(RC_IO_ERROR, Checkpoint::FscryptAddKey, CodeSource::Errno, EACCES),
            "[recovery_crypto] point=fscrypt_add_key result=7 source=errno code=" +
                std::to_string(EACCES) + "\n");
  EXPECT_EQ(FormatDiagnostic(RC_OK, Checkpoint::SpUnlock),
            "[recovery_crypto] point=sp_unlock result=0 source=none code=0\n");
  EXPECT_TRUE(FormatDiagnostic(-1, Checkpoint::SpUnlock).empty());
  EXPECT_TRUE(FormatDiagnostic(RC_IO_ERROR + 1, Checkpoint::SpUnlock).empty());
  EXPECT_TRUE(FormatDiagnostic(RC_IO_ERROR, static_cast<Checkpoint>(-1)).empty());
  EXPECT_TRUE(FormatDiagnostic(RC_IO_ERROR, Checkpoint::SpUnlock,
                               static_cast<CodeSource>(-1)).empty());
  EXPECT_TRUE(FormatDiagnostic(RC_IO_ERROR, Checkpoint::SpUnlock, CodeSource::None, 1).empty());
  for (int32_t code : {INT32_MIN, INT32_MAX}) {
    auto line = FormatDiagnostic(RC_IO_ERROR, Checkpoint::KeymintFinish, CodeSource::Hal, code);
    EXPECT_FALSE(line.empty());
    EXPECT_LT(line.size(), 160u);
    EXPECT_EQ(line.find('\n'), line.size() - 1);
  }
}
TEST(Android17RecoveryCrypto, PatternGridUsesUserSettingAndDefaultsOnlyWhenAbsent) {
  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open(":memory:", &raw), SQLITE_OK);
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
  ASSERT_EQ(sqlite3_exec(raw, "CREATE TABLE locksettings(name TEXT,user INTEGER,value TEXT);",
                        nullptr, nullptr, nullptr), SQLITE_OK);
  uint32_t size = 0;
  ASSERT_TRUE(ReadPatternSizeFromDatabase(raw, 0, &size));
  EXPECT_EQ(size, 3u);
  ASSERT_EQ(sqlite3_exec(raw, "INSERT INTO locksettings VALUES('lock_pattern_size',10,'6');",
                        nullptr, nullptr, nullptr), SQLITE_OK);
  ASSERT_TRUE(ReadPatternSizeFromDatabase(raw, 0, &size));
  EXPECT_EQ(size, 3u);
  ASSERT_TRUE(ReadPatternSizeFromDatabase(raw, 10, &size));
  EXPECT_EQ(size, 6u);
  for (uint32_t n = 3; n <= 6; ++n) {
    auto sql = "DELETE FROM locksettings WHERE user=0; INSERT INTO locksettings VALUES"
               "('lock_pattern_size',0,'" + std::to_string(n) + "');";
    ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_TRUE(ReadPatternSizeFromDatabase(raw, 0, &size));
    EXPECT_EQ(size, n);
  }
  for (const char* value : {"", "0", "2", "7", "128", "-1", "4x", " 4", "4.0"}) {
    auto sql = std::string("UPDATE locksettings SET value='") + value + "' WHERE user=0;";
    ASSERT_EQ(sqlite3_exec(raw, sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_FALSE(ReadPatternSizeFromDatabase(raw, 0, &size));
  }
  ASSERT_EQ(sqlite3_exec(raw, "UPDATE locksettings SET value='4' WHERE user=0;"
                        "INSERT INTO locksettings VALUES('lock_pattern_size',0,'5');",
                        nullptr, nullptr, nullptr), SQLITE_OK);
  EXPECT_FALSE(ReadPatternSizeFromDatabase(raw, 0, &size));
}
TEST(Android17RecoveryCrypto, LargerPatternsUseSingleBytesRatherThanDecimalDigits) {
  for (uint32_t size = 3; size <= 6; ++size) {
    Bytes cells, encoded;
    for (uint32_t i = 0; i < size * size; ++i) cells.push_back(i);
    ASSERT_TRUE(EncodePatternCredential(cells, size, &encoded));
    ASSERT_EQ(encoded.size(), size * size);
    for (uint32_t i = 0; i < size * size; ++i) EXPECT_EQ(encoded[i], '1' + i);
    cells.back() = cells.front();
    EXPECT_FALSE(EncodePatternCredential(cells, size, &encoded));
    EXPECT_TRUE(encoded.empty());
    cells.back() = size * size;
    EXPECT_FALSE(EncodePatternCredential(cells, size, &encoded));
    cells.resize(3);
    EXPECT_FALSE(EncodePatternCredential(cells, size, &encoded));
    EXPECT_FALSE(EncodePatternCredential(cells, 7, &encoded));
  }
}
TEST(Android17RecoveryCrypto, DeviceMapperRequestsSupportOlderKernelMinor) {
  struct {
    dm_ioctl io;
    dm_target_spec target;
  } request{};
  InitializeDmIo(&request.io, sizeof(request));
  // diting's kernel accepts 4.44 while Android 17 build headers declare 4.50.
  // Other devices use the same backward-compatible libdm request convention.
  EXPECT_EQ(request.io.version[0], 4u);
  EXPECT_EQ(request.io.version[1], 0u);
  EXPECT_EQ(request.io.version[2], 0u);
  EXPECT_EQ(request.io.data_size, sizeof(request));
  EXPECT_EQ(request.io.data_start, sizeof(dm_ioctl));
  EXPECT_LE(request.io.version[1], 44u);
  // ioctl writes back the running kernel's version. Reset it for activation,
  // and clear the old table/flags rather than reusing a load response.
  request.io.version[1] = 44;
  request.io.flags = DM_READONLY_FLAG | DM_SECURE_DATA_FLAG;
  InitializeDmIo(&request.io, sizeof(dm_ioctl));
  EXPECT_EQ(request.io.version[1], 0u);
  EXPECT_EQ(request.io.flags, 0u);
  EXPECT_EQ(request.io.data_size, sizeof(dm_ioctl));
}
static Bytes Hex(const char* value) {
  Bytes out;
  auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; value[i]; i += 2) out.push_back((digit(value[i]) << 4) | digit(value[i + 1]));
  return out;
}
TEST(Android17RecoveryCrypto, SyntheticPasswordVersionedSubkeys) {
  auto sp = AsBytes(kSyntheticPassword);
  EXPECT_EQ(SpSubkey(1, sp, "fbe-key"), Hex(kFbeV2));
  EXPECT_EQ(SpSubkey(2, sp, "fbe-key"), Hex(kFbeV2));
  EXPECT_EQ(SpSubkey(3, sp, "fbe-key"), Hex(kFbeV3));
  EXPECT_TRUE(SpSubkey(4, sp, "fbe-key").empty());
  EXPECT_NE(SpSubkey(3, sp, "fbe-key"), SpSubkey(3, sp, "sp-gk-authentication"));
}
TEST(Android17RecoveryCrypto, ScryptUsesStoredParameters) {
  Bytes out;
  ASSERT_TRUE(Stretch(AsBytes("123456"), Hex("000102030405060708090a0b0c0d0e0f"), 9, 3, 1, &out));
  EXPECT_EQ(out, Hex(kStretchedPin));
  EXPECT_FALSE(Stretch(AsBytes("123456"), Hex("00"), 255, 3, 1, &out));
  EXPECT_FALSE(Stretch(AsBytes("123456"), Hex("00"), 20, 8, 8, &out));
}
TEST(Android17RecoveryCrypto, AesGcmAuthenticatesBeforePublishingPlaintext) {
  // BoringSSL aes_256_gcm_tests.txt, first vector: empty plaintext, no AAD.
  auto key = Hex("e5ac4a32c67e425ac4b143c83c6f161312a97d88d634afdf9f4da5bd35223f01");
  auto blob = Hex("5bf11a0951f0bfc7ea5c9e58d7cba289d6d19a5af45dc13857016bac");
  Bytes out{ 1, 2, 3 };
  ASSERT_TRUE(GcmDecrypt(key, blob, &out));
  EXPECT_TRUE(out.empty());
  blob.back() ^= 1;
  out = { 1, 2, 3 };
  EXPECT_FALSE(GcmDecrypt(key, blob, &out));
  EXPECT_TRUE(out.empty());
  blob.resize(12);
  EXPECT_FALSE(GcmDecrypt(key, blob, &out));
}
TEST(Android17RecoveryCrypto, PasswordDataTruncationAndBetaFormat) {
  auto valid = Hex(kPasswordData);
  PasswordData data;
  ASSERT_TRUE(ParsePasswordData(valid, &data));
  EXPECT_EQ(data.type, 3);
  EXPECT_EQ(data.salt.size(), 16u);
  EXPECT_EQ(data.handle.size(), 9u);
  for (size_t length = 0; length < valid.size() - 4; ++length)
    EXPECT_FALSE(ParsePasswordData(View(valid).first(length), &data));
  auto beta = valid;
  beta[0] = 0;
  beta[1] = 2;
  EXPECT_TRUE(ParsePasswordData(beta, &data));
  EXPECT_EQ(data.type, 3);
  valid[7] = 0xff;
  EXPECT_FALSE(ParsePasswordData(valid, &data));
}
TEST(Android17RecoveryCrypto, WalCommitsAndChecksums) {
  auto database = Hex(kDatabase), wal = Hex(kWal), expected = Hex(kCommittedDatabase);
  ASSERT_TRUE(ApplyCommittedWal(&database, wal));
  EXPECT_EQ(database, expected);
  auto damaged = wal;
  damaged[damaged.size() - 1] ^= 1;
  database = Hex(kDatabase);
  EXPECT_FALSE(ApplyCommittedWal(&database, damaged));
  database = Hex(kDatabase);
  EXPECT_TRUE(ApplyCommittedWal(&database, View(wal).first(wal.size() - 1)));
  EXPECT_EQ(database, Hex(kDatabase));  // Incomplete final commit frame must not be applied.
}
