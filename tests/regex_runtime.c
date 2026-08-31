#include "regex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
  if (!(condition)) { \
    fprintf(stderr, "regex runtime check failed at line %d: %s\n", \
            __LINE__, #condition); \
    exit(1); \
  } \
} while (0)

static LanRegex* compile(const char* source) {
  LanRegex* result = NULL;
  char error[512];
  CHECK(lan_regex_compile(source, strlen(source), &result, error,
                          sizeof(error)) == LAN_REGEX_OK);
  CHECK(result != NULL && error[0] == '\0');
  return result;
}

int main(void) {
  char error[512];
  uint64_t begin = 99, end = 99;
  LanRegex* digits = compile("[0-9]+");
  CHECK(lan_regex_search(digits, "a12b345", 7, 0, 0, &begin, &end,
                         error, sizeof(error)) == 1);
  CHECK(begin == 1 && end == 3);
  CHECK(lan_regex_search(digits, "a12b345", 7, end, 0, &begin, &end,
                         error, sizeof(error)) == 1);
  CHECK(begin == 4 && end == 7);
  CHECK(lan_regex_search(digits, "a12", 3, 0, 1, &begin, &end,
                         error, sizeof(error)) == 0);
  CHECK(begin == 0 && end == 0);
  lan_regex_retain(digits);
  lan_regex_release(digits);
  CHECK(lan_regex_search(digits, "12", 2, 0, 1, &begin, &end,
                         error, sizeof(error)) == 1);
  CHECK(begin == 0 && end == 2);
  CHECK(lan_regex_search(digits, "12", 2, 3, 0, &begin, &end,
                         error, sizeof(error)) == LAN_REGEX_ARGUMENT);
  lan_regex_release(digits);
  lan_regex_retain(NULL);
  lan_regex_release(NULL);

  LanRegex* anchor = compile("^a");
  CHECK(lan_regex_search(anchor, "aa", 2, 1, 0, &begin, &end,
                         error, sizeof(error)) == 0);
  lan_regex_release(anchor);

  LanRegex* empty = compile("");
  CHECK(lan_regex_search(empty, NULL, 0, 0, 1, &begin, &end,
                         error, sizeof(error)) == 1);
  CHECK(begin == 0 && end == 0);
  CHECK(lan_regex_search(empty, "a", 1, 1, 0, &begin, &end,
                         error, sizeof(error)) == 1);
  CHECK(begin == 1 && end == 1);
  lan_regex_release(empty);

  LanRegex* unicode = compile("世界");
  CHECK(lan_regex_search(unicode, "你好世界", 12, 0, 0, &begin, &end,
                         error, sizeof(error)) == 1);
  CHECK(begin == 6 && end == 12);
  lan_regex_release(unicode);

  LanRegex* nul = compile("b");
  CHECK(lan_regex_search(nul, "a\0b", 3, 0, 0, &begin, &end,
                         error, sizeof(error)) == 1);
  CHECK(begin == 2 && end == 3);
  lan_regex_release(nul);

  const char* invalid[] = {"[", "(?=a)", "(a)\\1"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    LanRegex* rejected = NULL;
    CHECK(lan_regex_compile(invalid[i], strlen(invalid[i]), &rejected,
                            error, sizeof(error)) == LAN_REGEX_INVALID);
    CHECK(rejected == NULL && error[0] != '\0');
  }
  LanRegex* rejected = NULL;
  char tiny[1] = {'x'};
  CHECK(lan_regex_compile("[", 1, &rejected, tiny, sizeof(tiny)) < 0);
  CHECK(tiny[0] == '\0');
  CHECK(lan_regex_compile(NULL, 1, &rejected, error,
                          sizeof(error)) == LAN_REGEX_ARGUMENT);
  CHECK(lan_regex_compile("", 0, NULL, NULL, 0) == LAN_REGEX_ARGUMENT);
  char* large = malloc((size_t)LAN_REGEX_MAX_PATTERN_BYTES + 1);
  CHECK(large != NULL);
  memset(large, 'a', (size_t)LAN_REGEX_MAX_PATTERN_BYTES + 1);
  CHECK(lan_regex_compile(large, LAN_REGEX_MAX_PATTERN_BYTES + 1,
                          &rejected, error, sizeof(error)) == LAN_REGEX_LIMIT);
  CHECK(rejected == NULL);
  free(large);
  /* A nested-quantifier near miss must not cause backtracking explosion. */
  LanRegex* bounded = compile("(a+)+$");
  char* adversarial = malloc(20001);
  CHECK(adversarial != NULL);
  memset(adversarial, 'a', 20000);
  adversarial[20000] = 'b';
  CHECK(lan_regex_search(bounded, adversarial, 20001, 0, 0, &begin, &end,
                         error, sizeof(error)) == 0);
  free(adversarial);
  lan_regex_release(bounded);
  puts("regex runtime checks passed");
  return 0;
}
