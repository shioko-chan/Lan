#include "regex.h"
#include "ownership.h"

#include <re2/re2.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>

namespace {

RE2::Options options() {
  RE2::Options result;
  result.set_log_errors(false);
  result.set_encoding(RE2::Options::EncodingUTF8);
  result.set_max_mem(LAN_REGEX_MAX_MEMORY_BYTES);
  return result;
}

void destroyRegex(LanRegex* value) {
  delete static_cast<RE2*>(value->state);
  delete value;
}

void diagnostic(char* output, size_t capacity, const char* message) noexcept {
  if (!output || !capacity) return;
  size_t count = std::min(capacity - 1, std::strlen(message));
  std::memcpy(output, message, count);
  output[count] = '\0';
}

bool validInput(const char* data, uint64_t length) noexcept {
  return (data || length == 0) &&
      length <= static_cast<uint64_t>(std::numeric_limits<ptrdiff_t>::max());
}

}  // namespace

extern "C" int32_t lan_regex_compile(
    const char* pattern, uint64_t length, LanRegex** output,
    char* error, size_t error_capacity) {
  diagnostic(error, error_capacity, "");
  if (output) *output = nullptr;
  if (!output || !validInput(pattern, length)) {
    diagnostic(error, error_capacity, "regex 的模式参数无效");
    return LAN_REGEX_ARGUMENT;
  }
  if (length > LAN_REGEX_MAX_PATTERN_BYTES) {
    diagnostic(error, error_capacity, "regex 模式超过 65536 字节限制");
    return LAN_REGEX_LIMIT;
  }
  try {
    auto compiled = std::make_unique<RE2>(
        absl::string_view(pattern ? pattern : "", static_cast<size_t>(length)), options());
    if (!compiled->ok()) {
      diagnostic(error, error_capacity, compiled->error().c_str());
      return compiled->error_code() == RE2::ErrorPatternTooLarge
          ? LAN_REGEX_LIMIT : LAN_REGEX_INVALID;
    }
    auto handle = std::make_unique<LanRegex>();
    *handle = {1, compiled.get(), compiled->pattern().c_str(), destroyRegex};
    compiled.release();
    *output = handle.release();
    return LAN_REGEX_OK;
  } catch (const std::bad_alloc&) {
    diagnostic(error, error_capacity, "regex 内存分配失败");
    return LAN_REGEX_FAILURE;
  } catch (const std::exception& exception) {
    diagnostic(error, error_capacity, exception.what());
    return LAN_REGEX_FAILURE;
  } catch (...) {
    diagnostic(error, error_capacity, "RE2 编译失败");
    return LAN_REGEX_FAILURE;
  }
}

extern "C" void lan_regex_retain(LanRegex* pattern) {
  lan_regex_handle_retain(pattern);
}

extern "C" void lan_regex_release(LanRegex* pattern) {
  lan_regex_handle_release(pattern);
}

extern "C" int32_t lan_regex_search(
    const LanRegex* pattern, const char* text, uint64_t length, uint64_t start,
    int32_t full, uint64_t* begin, uint64_t* end,
    char* error, size_t error_capacity) {
  diagnostic(error, error_capacity, "");
  if (begin) *begin = 0;
  if (end) *end = 0;
  if (!pattern || !begin || !end || !validInput(text, length) ||
      start > length || (full && start != 0)) {
    diagnostic(error, error_capacity, "regex 的匹配参数无效");
    return LAN_REGEX_ARGUMENT;
  }
  try {
    absl::string_view input(text ? text : "", static_cast<size_t>(length));
    absl::string_view match;
    if (!static_cast<const RE2*>(pattern->state)->Match(input, static_cast<size_t>(start),
                                   input.size(), full ? RE2::ANCHOR_BOTH
                                                      : RE2::UNANCHORED,
                                   &match, 1))
      return 0;
    *begin = static_cast<uint64_t>(match.data() - input.data());
    *end = *begin + match.size();
    return 1;
  } catch (const std::bad_alloc&) {
    diagnostic(error, error_capacity, "regex 内存分配失败");
    return LAN_REGEX_FAILURE;
  } catch (const std::exception& exception) {
    diagnostic(error, error_capacity, exception.what());
    return LAN_REGEX_FAILURE;
  } catch (...) {
    diagnostic(error, error_capacity, "RE2 匹配失败");
    return LAN_REGEX_FAILURE;
  }
}
