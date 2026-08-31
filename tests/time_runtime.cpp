#include "../runtime/ownership.hpp"
#include "absl/time/time.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C" {
int32_t lan_parse_time(const char*, void**);
char* lan_format_time(void*);
int32_t lan_time_from_unix(double, void**);
double lan_unix_seconds(void*);
int32_t lan_add_seconds(void*, double, void**);
double lan_seconds_between(void*, void*);
int32_t lan_instant_compare(void*, void*);
void lan_arc_assert_empty(void);
}

#define CHECK(test) do { if (!(test)) { \
  fprintf(stderr, "time check failed at %d: %s\n", __LINE__, #test); \
  std::exit(1); } } while (0)

static OwnedValue parse(const char* text) {
  void* value = nullptr;
  CHECK(lan_parse_time(text, &value) == 1);
  return OwnedValue::adopt({LAN_INSTANT, 0, value});
}

static void expect_format(void* value, const char* expected) {
  auto text = OwnedValue::adopt({LAN_TEXT, 0, lan_format_time(value)});
  CHECK(std::strcmp(text.get<char>(), expected) == 0);
}

static void check_calendar() {
  // Differential validation against the project's pinned Abseil dependency.
  // Every month of every supported year, including the Gregorian century rules.
  for (int year = 0; year <= 9999; ++year) {
    for (int month = 1; month <= 12; ++month) {
      char text[40];
      std::snprintf(text, sizeof(text), "%04d-%02d-01T00:00:00Z", year, month);
      auto value = parse(text);
      auto reference = absl::FromCivil(absl::CivilSecond(year, month, 1),
                                        absl::UTCTimeZone());
      CHECK(lan_unix_seconds(value.get()) == (double)absl::ToUnixSeconds(reference));
      expect_format(value.get(), text);
    }
  }
  const char* invalid[] = {"", "2024", "2024-00-01T00:00:00Z",
      "2024-13-01T00:00:00Z", "2024-01-00T00:00:00Z",
      "1900-02-29T00:00:00Z", "2023-02-29T00:00:00Z",
      "2024-04-31T00:00:00Z", "2024-01-01T24:00:00Z",
      "2024-01-01T00:60:00Z", "2024-01-01T00:00:60Z",
      "2024-01-01T00:00:00", "2024-01-01 00:00:00Z",
      "2024-01-01T00:00:00.Z", "2024-01-01T00:00:00.1234567890Z",
      "2024-01-01T00:00:00+24:00", "2024-01-01T00:00:00+00:60",
      "2024-01-01T00:00:00+0000", "2024-01-01T00:00:00Z ",
      "0000-01-01T00:00:00+00:01", "9999-12-31T23:59:59-00:01"};
  for (const char* text : invalid) {
    void* value = reinterpret_cast<void*>(1);
    CHECK(lan_parse_time(text, &value) == 0 && value == nullptr);
  }
  auto lower = parse("2000-02-29t01:02:03.001z");
  expect_format(lower.get(), "2000-02-29T01:02:03.001Z");
  auto unknown_local = parse("2000-02-29T01:02:03-00:00");
  expect_format(unknown_local.get(), "2000-02-29T01:02:03Z");
}

static void check_arithmetic() {
  auto low = parse("0000-01-01T00:00:00.5Z");
  auto high = parse("9999-12-31T23:59:59.999999999Z");
  auto epoch = parse("1970-01-01T00:00:00Z");
  void* output = nullptr;
  CHECK(lan_add_seconds(low.get(), -0.25, &output) == 0);
  auto shifted = OwnedValue::adopt({LAN_INSTANT, 0, output});
  expect_format(shifted.get(), "0000-01-01T00:00:00.25Z");
  CHECK(lan_add_seconds(high.get(), 0, &output) == 0);
  auto unchanged = OwnedValue::adopt({LAN_INSTANT, 0, output});
  CHECK(lan_instant_compare(high.get(), unchanged.get()) == 0);
  CHECK(lan_add_seconds(high.get(), 1e-9, &output) != 0);
  CHECK(lan_add_seconds(low.get(), -1, &output) != 0);
  const double invalid[] = {std::numeric_limits<double>::infinity(),
      -std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::quiet_NaN(), 1e100, -1e100};
  for (double number : invalid) {
    CHECK(lan_time_from_unix(number, &output) != 0);
    CHECK(lan_add_seconds(epoch.get(), number, &output) != 0);
  }
  for (double number : {-1e-9, 1e-9, -0.25, 0.25, -1.5, 1.5}) {
    CHECK(lan_time_from_unix(number, &output) == 0);
    auto value = OwnedValue::adopt({LAN_INSTANT, 0, output});
    CHECK(lan_unix_seconds(value.get()) == number);
    CHECK(lan_seconds_between(epoch.get(), value.get()) == number);
    CHECK(lan_seconds_between(value.get(), epoch.get()) == -number);
  }
  CHECK(lan_time_from_unix(-0.0000000005, &output) == 0);
  auto rounded = OwnedValue::adopt({LAN_INSTANT, 0, output});
  expect_format(rounded.get(), "1969-12-31T23:59:59.999999999Z");
}

int main() {
  check_calendar();
  check_arithmetic();
  lan_arc_assert_empty();
}
