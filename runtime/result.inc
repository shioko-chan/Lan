// Included by the value runtime: a Result owns exactly one tagged payload.
// Construction borrows and retains; payload access borrows from the Result.
struct LanResult {
  uint32_t references;
  bool success;
  LanValue payload;
};

static void result_retain(void* pointer) {
  auto* result = static_cast<LanResult*>(pointer);
  if (!result) return;
  if (result->references == UINT32_MAX) abort();
  ++result->references;
}

static void result_release(void* pointer) {
  auto* result = static_cast<LanResult*>(pointer);
  if (!result) return;
  if (!result->references) abort();
  if (--result->references) return;
  lan_value_release(result->payload.tag, result->payload.number, result->payload.pointer);
  free(result);
  --arc_live_objects;
}

extern "C" void* lan_result_new(int32_t success, int32_t tag, double number, void* pointer) {
  auto* result = static_cast<LanResult*>(malloc(sizeof(LanResult)));
  if (!result) abort();
  result->references = 1;
  result->success = success != 0;
  result->payload = LanValue{tag, number, pointer};
  lan_value_retain(tag, number, pointer);
  ++arc_live_objects;
  return result;
}

extern "C" int32_t lan_result_is_ok(void* pointer) {
  // Pattern testing an absent Option<Result> never dereferences null.
  return pointer && static_cast<LanResult*>(pointer)->success;
}

extern "C" void lan_result_payload(void* pointer, LanValue* output) {
  if (!pointer) abort();
  *output = static_cast<LanResult*>(pointer)->payload;
}
