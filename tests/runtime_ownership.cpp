#include "ownership.hpp"

#include <cstdlib>

static int retains;
static int releases;

static void check(bool condition) {
  if (!condition) std::abort();
}

extern "C" void lan_value_retain(int32_t, double, void*) { ++retains; }
extern "C" void lan_value_release(int32_t tag, double, void* pointer) {
  if (tag != LAN_MISSING && pointer) {
    ++releases;
    std::free(pointer);
  }
}

static LanValue allocated() {
  return {LAN_TEXT, 0, std::malloc(1)};
}

int main() {
  {
    auto value = OwnedValue::adopt(allocated());
    check(retains == 0 && releases == 0);
  }
  check(releases == 1);

  {
    LanValue borrowed = allocated();
    auto value = OwnedValue::share(borrowed);
    check(retains == 1);
    (void)value.take();
    std::free(borrowed.pointer);
  }
  check(releases == 1);

  {
    auto first = OwnedValue::adopt(allocated());
    auto moved = std::move(first);
    auto replacement = OwnedValue::adopt(allocated());
    moved = std::move(replacement);
    check(releases == 2);
  }
  check(releases == 3);
}
