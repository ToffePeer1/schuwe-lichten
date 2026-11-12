// Unit tests for the onboard status LED.

#include <unity.h>

#include "status_led.h"

using namespace laser;

void setUp() {}
void tearDown() {}

void test_flashes_while_nobody_is_on_the_wifi() {
  TEST_ASSERT_TRUE(isStatusLedOn(true, 0, 0));
  TEST_ASSERT_TRUE(isStatusLedOn(true, 0, STATUS_LED_ON_MS - 1));
  TEST_ASSERT_FALSE(isStatusLedOn(true, 0, STATUS_LED_ON_MS));
  TEST_ASSERT_FALSE(isStatusLedOn(true, 0, STATUS_LED_PERIOD_MS - 1));
  TEST_ASSERT_TRUE(isStatusLedOn(true, 0, 5 * STATUS_LED_PERIOD_MS + 10));
}

void test_off_once_a_phone_joins() {
  for (uint32_t t = 0; t < 2 * STATUS_LED_PERIOD_MS; t += 25) {
    TEST_ASSERT_FALSE(isStatusLedOn(true, 1, t));
    TEST_ASSERT_FALSE(isStatusLedOn(true, 8, t));
  }
}

void test_off_when_the_wifi_failed_to_start() {
  TEST_ASSERT_FALSE(isStatusLedOn(false, 0, 0));
  TEST_ASSERT_FALSE(isStatusLedOn(false, 0, 50));
}

void test_keeps_flashing_across_the_millis_wrap() {
  TEST_ASSERT_FALSE(isStatusLedOn(true, 0, UINT32_MAX));  // 4294967295 % 1000 = 295
  TEST_ASSERT_TRUE(isStatusLedOn(true, 0, 0));
}

int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(test_flashes_while_nobody_is_on_the_wifi);
  RUN_TEST(test_off_once_a_phone_joins);
  RUN_TEST(test_off_when_the_wifi_failed_to_start);
  RUN_TEST(test_keeps_flashing_across_the_millis_wrap);
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
