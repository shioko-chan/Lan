#ifndef LAN_REGEX_OWNERSHIP_H
#define LAN_REGEX_OWNERSHIP_H

#include "regex.h"
#include <stdlib.h>

/* Shared C/C++ layout. The bitcode runtime can own a Regex without directly
 * referencing RE2 symbols; only construction needs the native archive. */
struct LanRegex {
  uint32_t references;
  void* state;
  const char* source;
  void (*destroy)(struct LanRegex*);
};

static inline void lan_regex_handle_retain(LanRegex* value) {
  if (!value) return;
  if (value->references == UINT32_MAX) abort();
  ++value->references;
}

static inline int lan_regex_handle_release(LanRegex* value) {
  if (!value) return 0;
  if (!value->references) abort();
  if (--value->references) return 0;
  value->destroy(value);
  return 1;
}
#endif
