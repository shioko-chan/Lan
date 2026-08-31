/* UTC Instant support. The runtime stores a normalized second plus a
 * nanosecond in the supported RFC 3339 range, years 0000 through 9999.
 * Gregorian conversion uses March-based 400-year eras (civil calendar
 * algorithms: https://howardhinnant.github.io/date_algorithms.html).
 * Keeping this bounded UTC parser in bitcode avoids locale/time-zone state
 * and the native dependency bundle. Tests cross-check against pinned Abseil. */

static int64_t time_days_from_civil(int32_t year, uint32_t month,
                                    uint32_t day) {
  year -= month <= 2;
  int32_t era = (year >= 0 ? year : year - 399) / 400;
  uint32_t year_of_era = (uint32_t)(year - era * 400);
  uint32_t day_of_year =
      (153 * (month + (month > 2 ? (uint32_t)-3 : 9)) + 2) / 5 + day - 1;
  uint32_t day_of_era = year_of_era * 365 + year_of_era / 4 -
      year_of_era / 100 + day_of_year;
  return (int64_t)era * 146097 + day_of_era - 719468;
}

static void time_civil_from_days(int64_t days, int32_t* year,
                                 uint32_t* month, uint32_t* day) {
  days += 719468;
  int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  uint32_t day_of_era = (uint32_t)(days - era * 146097);
  uint32_t year_of_era = (day_of_era - day_of_era / 1460 +
      day_of_era / 36524 - day_of_era / 146096) / 365;
  int32_t y = (int32_t)year_of_era + (int32_t)era * 400;
  uint32_t day_of_year = day_of_era -
      (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  uint32_t month_prime = (5 * day_of_year + 2) / 153;
  *day = day_of_year - (153 * month_prime + 2) / 5 + 1;
  *month = month_prime + (month_prime < 10 ? 3 : (uint32_t)-9);
  *year = y + (*month <= 2);
}

static int32_t time_is_leap_year(int32_t year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static uint32_t time_days_in_month(int32_t year, uint32_t month) {
  static const uint8_t lengths[] =
      {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && time_is_leap_year(year) ? 29 : lengths[month - 1];
}

static int64_t time_minimum_second() {
  return time_days_from_civil(0, 1, 1) * INT64_C(86400);
}

static int64_t time_maximum_second() {
  return time_days_from_civil(10000, 1, 1) * INT64_C(86400) - 1;
}

static LanInstant* time_make_instant(int64_t seconds, uint32_t nanoseconds) {
  LanInstant* instant = static_cast<LanInstant*>(malloc(sizeof(*instant)));
  if (!instant) abort();
  instant->references = 1;
  instant->seconds = seconds;
  instant->nanoseconds = nanoseconds;
  ++arc_live_objects;
  return instant;
}

static int32_t time_digit(const char* text, size_t offset) {
  unsigned char value = (unsigned char)text[offset];
  return value >= '0' && value <= '9' ? value - '0' : -1;
}

static int32_t time_digits(const char* text, size_t offset, size_t count,
                           int32_t* output) {
  int32_t value = 0;
  for (size_t i = 0; i < count; ++i) {
    int32_t digit = time_digit(text, offset + i);
    if (digit < 0) return 0;
    value = value * 10 + digit;
  }
  *output = value;
  return 1;
}

static int32_t time_parse_rfc3339(const char* text, int64_t* seconds,
                                  uint32_t* nanoseconds) {
  size_t length = strlen(text);
  if (length < 20 || text[4] != '-' || text[7] != '-' ||
      (text[10] != 'T' && text[10] != 't') ||
      text[13] != ':' || text[16] != ':') return 0;
  int32_t year, month, day, hour, minute, second;
  if (!time_digits(text, 0, 4, &year) ||
      !time_digits(text, 5, 2, &month) ||
      !time_digits(text, 8, 2, &day) ||
      !time_digits(text, 11, 2, &hour) ||
      !time_digits(text, 14, 2, &minute) ||
      !time_digits(text, 17, 2, &second)) return 0;
  if (month < 1 || month > 12 || day < 1 ||
      (uint32_t)day > time_days_in_month(year, (uint32_t)month) ||
      hour > 23 || minute > 59 || second > 59) return 0;

  size_t offset = 19;
  uint32_t nanos = 0;
  if (offset < length && text[offset] == '.') {
    ++offset;
    size_t first = offset;
    while (offset < length && time_digit(text, offset) >= 0) {
      if (offset - first >= 9) return 0;
      nanos = nanos * 10 + (uint32_t)time_digit(text, offset++);
    }
    size_t digits = offset - first;
    if (!digits) return 0;
    while (digits++ < 9) nanos *= 10;
  }

  int32_t zone_seconds = 0;
  if (offset < length && (text[offset] == 'Z' || text[offset] == 'z')) {
    if (++offset != length) return 0;
  } else {
    if (length - offset != 6 ||
        (text[offset] != '+' && text[offset] != '-') ||
        text[offset + 3] != ':') return 0;
    int32_t zone_hour, zone_minute;
    if (!time_digits(text, offset + 1, 2, &zone_hour) ||
        !time_digits(text, offset + 4, 2, &zone_minute) ||
        zone_hour > 23 || zone_minute > 59) return 0;
    zone_seconds = (zone_hour * 60 + zone_minute) * 60;
    if (text[offset] == '-') zone_seconds = -zone_seconds;
  }

  int64_t result = time_days_from_civil(year, (uint32_t)month, (uint32_t)day) *
      INT64_C(86400) + hour * 3600 + minute * 60 + second;
  result -= zone_seconds;
  if (result < time_minimum_second() || result > time_maximum_second()) return 0;
  *seconds = result;
  *nanoseconds = nanos;
  return 1;
}

extern "C" int32_t lan_instant_compare(void* left, void* right) {
  LanInstant* a = static_cast<LanInstant*>(left);
  LanInstant* b = static_cast<LanInstant*>(right);
  if (!a || !b) abort();
  if (a->seconds != b->seconds) return a->seconds < b->seconds ? -1 : 1;
  return a->nanoseconds < b->nanoseconds ? -1
       : a->nanoseconds > b->nanoseconds ? 1 : 0;
}

extern "C" int32_t lan_parse_time(const char* text, void** output) {
  int64_t seconds;
  uint32_t nanoseconds;
  if (!time_parse_rfc3339(text, &seconds, &nanoseconds)) {
    *output = nullptr;
    return 0;
  }
  *output = time_make_instant(seconds, nanoseconds);
  return 1;
}

extern "C" char* lan_format_time(void* pointer) {
  LanInstant* instant = static_cast<LanInstant*>(pointer);
  if (!instant) abort();
  int64_t days = instant->seconds / 86400;
  int64_t day_seconds = instant->seconds % 86400;
  if (day_seconds < 0) { day_seconds += 86400; --days; }
  int32_t year;
  uint32_t month, day;
  time_civil_from_days(days, &year, &month, &day);
  uint32_t hour = (uint32_t)(day_seconds / 3600);
  uint32_t minute = (uint32_t)(day_seconds % 3600 / 60);
  uint32_t second = (uint32_t)(day_seconds % 60);
  char text[40];
  int length = snprintf(text, sizeof(text), "%04d-%02u-%02uT%02u:%02u:%02u",
                        year, month, day, hour, minute, second);
  if (length != 19) abort();
  if (instant->nanoseconds) {
    int fraction = snprintf(text + length, sizeof(text) - (size_t)length,
                            ".%09u", instant->nanoseconds);
    if (fraction != 10) abort();
    length += fraction;
    while (text[length - 1] == '0') --length;
  }
  text[length++] = 'Z';
  text[length] = '\0';
  return lan_text_from_cstr(text);
}

extern "C" double lan_unix_seconds(void* pointer) {
  LanInstant* instant = static_cast<LanInstant*>(pointer);
  if (!instant) abort();
  // Avoid cancellation for instants just before the epoch.
  if (instant->seconds < 0 && instant->nanoseconds)
    return (double)(instant->seconds + 1) -
        (1000000000 - instant->nanoseconds) / 1000000000.0;
  return (double)instant->seconds + instant->nanoseconds / 1000000000.0;
}

extern "C" int32_t lan_add_seconds(void* pointer, double added, void** output) {
  LanInstant* instant = static_cast<LanInstant*>(pointer);
  if (!instant || !isfinite(added)) return -1;
  double whole = trunc(added);
  double minimum_added = (double)(time_minimum_second() - instant->seconds);
  double maximum_added = (double)(time_maximum_second() - instant->seconds);
  // Allow one second of headroom; fractional carry/borrow is checked below.
  if (whole < minimum_added - 1 || whole > maximum_added + 1) return -1;
  int64_t seconds = instant->seconds + (int64_t)whole;
  int64_t nanoseconds = (int64_t)instant->nanoseconds +
      llround((added - whole) * 1000000000.0);
  if (nanoseconds >= 1000000000) {
    ++seconds;
    nanoseconds -= 1000000000;
  }
  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += 1000000000;
  }
  if (seconds < time_minimum_second() || seconds > time_maximum_second())
    return -1;
  *output = time_make_instant(seconds, (uint32_t)nanoseconds);
  return 0;
}

extern "C" int32_t lan_time_from_unix(double value, void** output) {
  LanInstant epoch = {1, 0, 0};
  return lan_add_seconds(&epoch, value, output);
}

extern "C" double lan_seconds_between(void* start_pointer, void* end_pointer) {
  LanInstant* start = static_cast<LanInstant*>(start_pointer);
  LanInstant* end = static_cast<LanInstant*>(end_pointer);
  if (!start || !end) abort();
  int64_t seconds = end->seconds - start->seconds;
  int64_t nanos = (int64_t)end->nanoseconds - start->nanoseconds;
  if (seconds > 0 && nanos < 0) { --seconds; nanos += 1000000000; }
  if (seconds < 0 && nanos > 0) { ++seconds; nanos -= 1000000000; }
  return (double)seconds + nanos / 1000000000.0;
}

static int32_t fixed_time_enabled;
static int32_t fixed_time_invalid;
static int64_t fixed_time_seconds;
static uint32_t fixed_time_nanoseconds;

extern "C" void lan_time_configure(const char* text) {
  fixed_time_enabled = text != nullptr;
  fixed_time_invalid = 0;
  fixed_time_seconds = 0;
  fixed_time_nanoseconds = 0;
  if (text && !time_parse_rfc3339(text, &fixed_time_seconds,
                                  &fixed_time_nanoseconds))
    fixed_time_invalid = 1;
}

extern "C" int32_t lan_io_now(LanValue* output, char* error,
                               uint64_t capacity) {
  int64_t seconds;
  uint32_t nanoseconds;
  if (fixed_time_invalid)
    return io_error(error, capacity, "now", "运行器提供了无效固定时间", 0);
  if (fixed_time_enabled) {
    seconds = fixed_time_seconds;
    nanoseconds = fixed_time_nanoseconds;
  } else {
    struct timespec current;
    if (clock_gettime(CLOCK_REALTIME, &current) != 0)
      return io_error(error, capacity, "now", "无法读取系统实时时钟",
                      errno ? errno : EIO);
    seconds = current.tv_sec;
    nanoseconds = (uint32_t)current.tv_nsec;
    if (seconds < time_minimum_second() || seconds > time_maximum_second())
      return io_error(error, capacity, "now", "系统时间超出 Instant 范围", 0);
  }
  *output = LanValue{LAN_INSTANT, 0, time_make_instant(seconds, nanoseconds)};
  return 0;
}
