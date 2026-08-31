#ifndef LAN_REGEX_RUNTIME_H
#define LAN_REGEX_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* This boundary owns no Lan Text/List values. Results are byte ranges into
 * borrowed input; the Lan runtime is responsible for materializing them. */
typedef struct LanRegex LanRegex;

enum {
  LAN_REGEX_OK = 0,
  LAN_REGEX_INVALID = -1,
  LAN_REGEX_LIMIT = -2,
  LAN_REGEX_FAILURE = -3,
  LAN_REGEX_ARGUMENT = -4
};

#define LAN_REGEX_MAX_PATTERN_BYTES UINT64_C(65536)
#define LAN_REGEX_MAX_MEMORY_BYTES INT64_C(8388608)

/* A caller-owned error buffer receives a NUL-terminated diagnostic (possibly
 * truncated). No C++ exception crosses this ABI. *output is NULL on failure.
 * RE2 reports an error fragment, not an exact source offset. */
int32_t lan_regex_compile(const char* pattern, uint64_t length,
                          LanRegex** output, char* error, size_t error_capacity);

/* compile returns one owned reference. Matching borrows both inputs.
 * References are non-atomic, like the rest of the single-threaded Lan runtime.
 * NULL retain/release are no-ops for Option<Regex>'s null-pointer niche. */
void lan_regex_retain(LanRegex* pattern);
void lan_regex_release(LanRegex* pattern);

/* Search the original input starting at start, preserving ^/$ anchor context.
 * full != 0 requires the entire input to match and start == 0.
 * Return 1 for a match, 0 for no match, or a negative status above.
 * [begin, end) is a byte range; both are reset to zero on no match/error.
 * Embedded NUL bytes are accepted because input is length-delimited. */
int32_t lan_regex_search(const LanRegex* pattern, const char* text,
                         uint64_t length, uint64_t start, int32_t full,
                         uint64_t* begin, uint64_t* end,
                         char* error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
#endif
