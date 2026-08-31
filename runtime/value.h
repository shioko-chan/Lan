#ifndef LAN_RUNTIME_VALUE_H
#define LAN_RUNTIME_VALUE_H

#include <stdint.h>

/* Trivial C ABI data, never an owning C++ object. Copying it does not retain. */
typedef struct {
  int32_t tag;
  double number;
  void* pointer;
} LanValue;

enum { LAN_MISSING = 0, LAN_NUMBER = 1, LAN_BOOLEAN = 2,
       LAN_TEXT = 3, LAN_LIST = 4, LAN_RECORD = 5, LAN_REGEX = 6,
       LAN_UNIT = 7, LAN_IO = 8 };

#ifdef __cplusplus
extern "C" {
#endif
void lan_value_retain(int32_t tag, double number, void* pointer);
void lan_value_release(int32_t tag, double number, void* pointer);
#ifdef __cplusplus
}
#endif
#endif
