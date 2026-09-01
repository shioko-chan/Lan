#include "../runtime/ownership.hpp"
#include <cstdio>
#include <cstring>

extern "C" {
void* lan_result_new(int32_t, int32_t, double, void*);
int32_t lan_result_is_ok(void*);
void lan_result_payload(void*, LanValue*);
void* lan_text_from_cstr(const char*);
char* lan_result_error_text(void*);
int32_t lan_value_same(int32_t, double, void*, int32_t, double, void*);
void lan_arc_assert_empty(void);
}

#define CHECK(test) do { if (!(test)) { \
  std::fprintf(stderr, "Result check failed at %d: %s\n", __LINE__, #test); \
  std::exit(1); } } while (0)

static OwnedValue wrap(bool success, LanValue payload) {
  return OwnedValue::adopt({LAN_RESULT, 0,
      lan_result_new(success, payload.tag, payload.number, payload.pointer)});
}

int main() {
  CHECK(!lan_result_is_ok(nullptr));
  for (int iteration = 0; iteration < 1000; ++iteration) {
    auto error = [] {
      auto text = OwnedValue::adopt({LAN_TEXT, 0, lan_text_from_cstr("message")});
      return wrap(false, text.borrow());  // Local owner dies; Result retains it.
    }();
    CHECK(!lan_result_is_ok(error.get()));
    LanValue payload{};
    lan_result_payload(error.get(), &payload);
    CHECK(payload.tag == LAN_TEXT && std::strcmp(static_cast<char*>(payload.pointer), "message") == 0);
    auto retainedPayload = OwnedValue::share(payload);
    auto message = OwnedValue::adopt({LAN_TEXT, 0, lan_result_error_text(error.get())});
    CHECK(std::strcmp(message.get<char>(), "require 收到 err(\"message\")") == 0);
    auto nested = wrap(true, error.borrow());
    auto shared = OwnedValue::share(nested.borrow());
    CHECK(lan_result_is_ok(shared.get()));
    auto missing = wrap(true, {LAN_MISSING, 0, nullptr});
    CHECK(lan_result_is_ok(missing.get()));
    lan_result_payload(missing.get(), &payload);
    CHECK(payload.tag == LAN_MISSING);
    auto unit = wrap(true, {LAN_UNIT, 0, nullptr});
    CHECK(!lan_value_same(LAN_RESULT, 0, unit.get(), LAN_RESULT, 0, missing.get()));
  }
  lan_arc_assert_empty();
}
