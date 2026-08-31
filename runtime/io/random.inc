/* Versioned PCG XSL RR 128/64 random stream: lan-pcg64-v1. */
using LanRandomWord = unsigned __int128;

static LanRandomWord random_state;
static int32_t random_ready;
static int32_t random_seed_invalid;
static uint64_t random_explicit_seed;
static int32_t random_has_explicit_seed;

static uint64_t random_rotate_right(uint64_t value, unsigned rotation) {
  return (value >> rotation) | (value << ((0u - rotation) & 63u));
}

static uint64_t random_next_word() {
  constexpr LanRandomWord multiplier =
      ((LanRandomWord)UINT64_C(0x2360ed051fc65da4) << 64) |
      UINT64_C(0x4385df649fccf645);
  constexpr LanRandomWord increment =
      ((LanRandomWord)UINT64_C(0x5851f42d4c957f2d) << 64) |
      UINT64_C(0x14057b7ef767814f);
  LanRandomWord previous = random_state;
  random_state = previous * multiplier + increment;
  uint64_t mixed = (uint64_t)(previous >> 64) ^ (uint64_t)previous;
  return random_rotate_right(mixed, (unsigned)(previous >> 122));
}

static int32_t random_initialize(char* error, uint64_t capacity) {
  if (random_ready) return 0;
  if (random_seed_invalid)
    return io_error(error, capacity, "random", "运行器提供了无效随机种子", 0);
  uint64_t seed = random_explicit_seed;
  if (!random_has_explicit_seed && getentropy(&seed, sizeof(seed)) != 0)
    return io_error(error, capacity, "random", "无法从操作系统取得随机种子",
                    errno ? errno : EIO);
  random_state = 0;
  (void)random_next_word();
  random_state += seed;
  (void)random_next_word();
  random_ready = 1;
  return 0;
}

static uint64_t random_bounded(uint64_t bound) {
  if (!bound) abort();
  uint64_t threshold = (uint64_t)(0 - bound) % bound;
  for (;;) {
    uint64_t value = random_next_word();
    if (value >= threshold) return value % bound;
  }
}

static double random_unit() {
  return (double)(random_next_word() >> 11) * 0x1.0p-53;
}

extern "C" void lan_random_configure(const char* seed) {
  random_ready = 0;
  random_seed_invalid = 0;
  random_has_explicit_seed = seed != nullptr;
  random_explicit_seed = 0;
  if (!seed) return;
  if (!*seed) { random_seed_invalid = 1; return; }
  uint64_t value = 0;
  for (const char* digit = seed; *digit; ++digit) {
    if (*digit < '0' || *digit > '9') { random_seed_invalid = 1; return; }
    unsigned next = (unsigned)(*digit - '0');
    if (value > (UINT64_C(9007199254740991) - next) / 10) {
      random_seed_invalid = 1;
      return;
    }
    value = value * 10 + next;
  }
  random_explicit_seed = value;
}

extern "C" int32_t lan_io_random(LanValue* output, char* error, uint64_t capacity) {
  if (random_initialize(error, capacity)) return -1;
  *output = LanValue{LAN_NUMBER, random_unit(), nullptr};
  return 0;
}

extern "C" int32_t lan_io_random_between(double minimum, double maximum,
    LanValue* output, char* error, uint64_t capacity) {
  if (!isfinite(minimum) || !isfinite(maximum) || minimum >= maximum)
    return io_error(error, capacity, "random_between",
                    "边界必须是有限 Number，且 minimum < maximum", 0);
  if (random_initialize(error, capacity)) return -1;
  long double unit = random_unit();
  double result = (double)((long double)minimum * (1 - unit) +
                           (long double)maximum * unit);
  if (!isfinite(result))
    return io_error(error, capacity, "random_between", "随机结果超出 Number 范围", 0);
  if (result >= maximum) result = nextafter(maximum, minimum);
  if (result < minimum) result = minimum;
  *output = LanValue{LAN_NUMBER, result, nullptr};
  return 0;
}

extern "C" int32_t lan_io_random_integer(double minimum, double maximum,
    LanValue* output, char* error, uint64_t capacity) {
  constexpr double limit = 9007199254740991.0;
  if (!isfinite(minimum) || !isfinite(maximum) || minimum < -limit ||
      maximum > limit || minimum > maximum || minimum != trunc(minimum) ||
      maximum != trunc(maximum))
    return io_error(error, capacity, "random_integer",
                    "边界必须是安全整数，且 minimum <= maximum", 0);
  if (random_initialize(error, capacity)) return -1;
  int64_t low = (int64_t)minimum;
  int64_t high = (int64_t)maximum;
  uint64_t width = (uint64_t)(high - low) + 1;
  int64_t selected = low + (int64_t)random_bounded(width);
  *output = LanValue{LAN_NUMBER, (double)selected, nullptr};
  return 0;
}

extern "C" int32_t lan_io_choose(void* list_pointer, LanValue* output,
    char* error, uint64_t capacity) {
  LanList* list = static_cast<LanList*>(list_pointer);
  if (!list) return io_error(error, capacity, "choose", "输入必须是 List", 0);
  uint64_t present = 0;
  for (uint64_t i = 0; i < list->length; ++i)
    present += load_list_value(list, i).tag != LAN_MISSING;
  if (!present) {
    *output = LanValue{LAN_MISSING, 0, nullptr};
    return 0;
  }
  if (random_initialize(error, capacity)) return -1;
  uint64_t selected = random_bounded(present);
  for (uint64_t i = 0; i < list->length; ++i) {
    LanValue value = load_list_value(list, i);
    if (value.tag != LAN_MISSING && selected-- == 0) {
      retain_value(value);
      *output = value;
      return 0;
    }
  }
  abort();
}

extern "C" int32_t lan_io_shuffle(void* list_pointer, LanValue* output,
    char* error, uint64_t capacity) {
  LanList* source = static_cast<LanList*>(list_pointer);
  if (!source) return io_error(error, capacity, "shuffle", "输入必须是 List", 0);
  if (source->length > 1 && random_initialize(error, capacity)) return -1;
  if (source->length > SIZE_MAX / sizeof(uint64_t)) abort();
  OwnedMemory<uint64_t> order(source->length
      ? static_cast<uint64_t*>(malloc((size_t)source->length * sizeof(uint64_t)))
      : nullptr);
  if (source->length && !order) abort();
  for (uint64_t i = 0; i < source->length; ++i) order.get()[i] = i;
  for (uint64_t remaining = source->length; remaining > 1; --remaining) {
    uint64_t selected = random_bounded(remaining);
    uint64_t temporary = order.get()[remaining - 1];
    order.get()[remaining - 1] = order.get()[selected];
    order.get()[selected] = temporary;
  }
  LanList* result = make_typed_list(source->length, source->storage_tag);
  auto resultOwner = OwnedValue::adopt({LAN_LIST, 0, result});
  for (uint64_t i = 0; i < source->length; ++i)
    store_list_shared(result, i, load_list_value(source, order.get()[i]));
  *output = resultOwner.take();
  return 0;
}
