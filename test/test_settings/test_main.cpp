// Unit tests for the persistent settings blob.

#include <unity.h>

#include <cstring>

#include "settings_blob.h"

using namespace laser;

namespace {

Settings sample() {
  Settings s = defaultSettings("13579246", 1);
  s.limits.ch7Low = 90;
  s.limits.ch7High = 12;
  s.limits.ch6Enabled = true;
  s.limits.ch6Left = 20;
  s.limits.ch6Right = 100;
  s.limits.isCalibrated = true;
  s.dmxAddress = 503;
  return s;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_crc32_known_value() {
  const uint8_t text[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32(text, sizeof(text)));
}

void test_round_trip_keeps_every_field() {
  uint8_t blob[SETTINGS_BLOB_SIZE];
  TEST_ASSERT_EQUAL(SETTINGS_BLOB_SIZE, encodeSettings(sample(), blob, sizeof(blob)));
  Settings out = defaultSettings("000000", 1);
  TEST_ASSERT_TRUE(decodeSettings(blob, sizeof(blob), &out));
  const Settings in = sample();
  TEST_ASSERT_EQUAL_UINT8(in.limits.ch7Low, out.limits.ch7Low);
  TEST_ASSERT_EQUAL_UINT8(in.limits.ch7High, out.limits.ch7High);
  TEST_ASSERT_TRUE(out.limits.ch6Enabled);
  TEST_ASSERT_EQUAL_UINT8(20, out.limits.ch6Left);
  TEST_ASSERT_EQUAL_UINT8(100, out.limits.ch6Right);
  TEST_ASSERT_TRUE(out.limits.isCalibrated);
  TEST_ASSERT_EQUAL_UINT16(503, out.dmxAddress);
  TEST_ASSERT_EQUAL_STRING("13579246", out.pin);
}

void test_any_flipped_byte_is_rejected() {
  uint8_t blob[SETTINGS_BLOB_SIZE];
  encodeSettings(sample(), blob, sizeof(blob));
  for (size_t i = 0; i < SETTINGS_BLOB_SIZE; ++i) {
    uint8_t copy[SETTINGS_BLOB_SIZE];
    std::memcpy(copy, blob, sizeof(copy));
    copy[i] ^= 0x01;
    Settings out = defaultSettings("000000", 7);
    TEST_ASSERT_FALSE(decodeSettings(copy, sizeof(copy), &out));
    TEST_ASSERT_EQUAL_UINT16(7, out.dmxAddress);  // untouched on failure
  }
}

void test_wrong_size_and_version_rejected() {
  uint8_t blob[SETTINGS_BLOB_SIZE + 1] = {};
  encodeSettings(sample(), blob, sizeof(blob));
  Settings out;
  TEST_ASSERT_FALSE(decodeSettings(blob, SETTINGS_BLOB_SIZE - 1, &out));
  TEST_ASSERT_FALSE(decodeSettings(blob, SETTINGS_BLOB_SIZE + 1, &out));
  blob[0] = SETTINGS_VERSION + 1;
  TEST_ASSERT_FALSE(decodeSettings(blob, SETTINGS_BLOB_SIZE, &out));
}

void test_out_of_range_values_rejected_even_with_valid_crc() {
  Settings bad = sample();
  bad.limits.ch7High = 200;  // would allow the sweep range
  uint8_t blob[SETTINGS_BLOB_SIZE];
  encodeSettings(bad, blob, sizeof(blob));
  Settings out;
  TEST_ASSERT_FALSE(decodeSettings(blob, sizeof(blob), &out));
  bad = sample();
  bad.dmxAddress = 504;
  encodeSettings(bad, blob, sizeof(blob));
  TEST_ASSERT_FALSE(decodeSettings(blob, sizeof(blob), &out));
}

void test_defaults_are_valid_and_uncalibrated() {
  const Settings s = defaultSettings("246810", 1);
  TEST_ASSERT_TRUE(isValidSettings(s));
  TEST_ASSERT_FALSE(s.limits.isCalibrated);
  TEST_ASSERT_TRUE(s.limits.ch6Enabled);
  TEST_ASSERT_FALSE(isValidSettings(defaultSettings("12", 1)));
}

void test_old_blob_with_auto_flag_still_loads() {
  uint8_t blob[SETTINGS_BLOB_SIZE];
  encodeSettings(sample(), blob, sizeof(blob));
  blob[3] |= 1 << 2;  // the old "allow auto" flag from an earlier firmware
  const uint32_t crc = crc32(blob, SETTINGS_BLOB_SIZE - 4);
  for (int i = 0; i < 4; ++i) {
    blob[SETTINGS_BLOB_SIZE - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
  }
  Settings out;
  TEST_ASSERT_TRUE(decodeSettings(blob, sizeof(blob), &out));
  TEST_ASSERT_TRUE(out.limits.isCalibrated);
}

void test_encode_refuses_small_buffer() {
  uint8_t small[SETTINGS_BLOB_SIZE - 1];
  TEST_ASSERT_EQUAL(0, encodeSettings(sample(), small, sizeof(small)));
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_known_value);
  RUN_TEST(test_round_trip_keeps_every_field);
  RUN_TEST(test_any_flipped_byte_is_rejected);
  RUN_TEST(test_wrong_size_and_version_rejected);
  RUN_TEST(test_out_of_range_values_rejected_even_with_valid_crc);
  RUN_TEST(test_defaults_are_valid_and_uncalibrated);
  RUN_TEST(test_old_blob_with_auto_flag_still_loads);
  RUN_TEST(test_encode_refuses_small_buffer);
  return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup() {
  delay(2000);
  runAllTests();
}
void loop() {}
#else
int main() { return runAllTests(); }
#endif
