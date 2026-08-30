#include <stdint.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utf8proc.h"

typedef struct LanRecordNode LanRecordNode;

typedef struct {
  uint64_t length;
  void* elements;
  int32_t storage_tag;
  uint8_t* presence;
  uint32_t references;
} LanList;

typedef struct {
  uint32_t references;
  uint32_t marker;
  char bytes[];
} LanTextHeader;

enum { LAN_TEXT_MARKER = 0x4c414e54 };

typedef struct {
  int32_t tag;
  double number;
  void* pointer;
} LanValue;

typedef struct {
  uint64_t hash;
  char* key;
  LanValue value;
} LanEntry;

enum { LAN_BRANCH = 1, LAN_LEAF = 2, LAN_COLLISION = 3 };
enum { LAN_MISSING = 0, LAN_NUMBER = 1, LAN_BOOLEAN = 2,
       LAN_TEXT = 3, LAN_LIST = 4, LAN_RECORD = 5 };

static const char* diagnostic_frames[128];
static uint32_t diagnostic_frame_count = 0;
static int64_t diagnostic_error_index = -1;

typedef struct LanArcEntry {
  LanValue value;
  struct LanArcEntry* next;
} LanArcEntry;

typedef struct LanArcFrame {
  LanArcEntry* values;
  struct LanArcFrame* previous;
} LanArcFrame;

static _Thread_local LanArcFrame* arc_frame = NULL;
static _Thread_local int64_t arc_live_objects = 0;

void lan_arc_note_object_allocated(void) { ++arc_live_objects; }

void lan_arc_assert_empty(void) {
  if (arc_frame || arc_live_objects != 0) {
    fprintf(stderr, "Lan ARC 错误：程序结束时仍有 %lld 个堆对象\n",
            (long long)arc_live_objects);
    abort();
  }
}

void lan_diagnostic_push(const char* function) {
  if (diagnostic_frame_count < 128)
    diagnostic_frames[diagnostic_frame_count++] = function;
}

void lan_diagnostic_pop(void) {
  if (diagnostic_frame_count) --diagnostic_frame_count;
}

int64_t lan_diagnostic_error_index(void) {
  return diagnostic_error_index;
}

_Noreturn void lan_runtime_fail(const char* message, const char* source,
                                int32_t line, int32_t column,
                                int64_t list_index, int64_t table_row,
                                const char* field) {
  printf("Lan 运行错误：%s\n", message);
  if (source && line > 0)
    printf("位置：%s:%d:%d\n", source, line, column > 0 ? column : 1);
  if (table_row >= 0) printf("Table 行号：%lld\n", (long long)table_row);
  else if (list_index >= 0)
    printf("List 元素索引：%lld\n", (long long)list_index);
  if (field && *field) printf("字段路径：%s\n", field);
  if (diagnostic_frame_count) {
    printf("调用链：");
    for (uint32_t i = 0; i < diagnostic_frame_count; ++i)
      printf("%s%s", i ? " -> " : "", diagnostic_frames[i]);
    putchar('\n');
  }
  exit(1);
}

struct LanRecordNode {
  uint32_t references;
  uint32_t kind;
  uint32_t bitmap;
  uint32_t count;
  uint64_t hash;
  char* key;
  LanValue value;
  LanRecordNode** children;
  LanEntry* entries;
};

static LanRecordNode empty_record = {UINT32_MAX, LAN_BRANCH, 0, 0, 0,
                                     NULL, {0, 0, NULL}, NULL, NULL};

static uint64_t hash_text(const char* text) {
  uint64_t hash = UINT64_C(1469598103934665603);
  while (*text) {
    hash ^= (unsigned char)*text++;
    hash *= UINT64_C(1099511628211);
  }
  return hash;
}

static uint32_t child_index(uint32_t bitmap, uint32_t bit) {
  return (uint32_t)__builtin_popcount(bitmap & (bit - 1));
}

void lan_record_retain(void* pointer) {
  LanRecordNode* node = pointer;
  if (node && node->references != UINT32_MAX) {
    if (node->references == UINT32_MAX - 1) abort();
    ++node->references;
  }
}

static LanTextHeader* text_header(const char* text) {
  return text ? (LanTextHeader*)((char*)text - offsetof(LanTextHeader, bytes))
              : NULL;
}

void lan_text_retain(void* pointer) {
  LanTextHeader* text = text_header(pointer);
  if (text) {
    if (text->marker != LAN_TEXT_MARKER || text->references == UINT32_MAX)
      abort();
    ++text->references;
  }
}

void lan_text_release(void* pointer) {
  LanTextHeader* text = text_header(pointer);
  if (text) {
    if (text->marker != LAN_TEXT_MARKER || text->references == 0) abort();
  }
  if (text && --text->references == 0) {
    free(text);
    --arc_live_objects;
  }
}

void lan_list_retain(void* pointer) {
  LanList* list = pointer;
  if (list) {
    if (list->references == UINT32_MAX) abort();
    ++list->references;
  }
}

static void lan_list_release(void* pointer);

static void retain_value(LanValue value) {
  if (value.tag == LAN_TEXT) lan_text_retain(value.pointer);
  else if (value.tag == LAN_LIST) lan_list_retain(value.pointer);
  else if (value.tag == LAN_RECORD) lan_record_retain(value.pointer);
}

static void release_value(LanValue value);

void lan_arc_push(void) {
  LanArcFrame* frame = calloc(1, sizeof(*frame));
  if (!frame) abort();
  frame->previous = arc_frame;
  arc_frame = frame;
}

void lan_arc_adopt(int32_t tag, double number, void* pointer) {
  if (!arc_frame || !pointer ||
      (tag != LAN_TEXT && tag != LAN_LIST && tag != LAN_RECORD)) return;
  LanArcEntry* entry = malloc(sizeof(*entry));
  if (!entry) abort();
  entry->value = (LanValue){tag, number, pointer};
  entry->next = arc_frame->values;
  arc_frame->values = entry;
}

void lan_arc_pop(int32_t return_tag, double return_number,
                 void* return_pointer) {
  LanArcFrame* frame = arc_frame;
  if (!frame) abort();
  LanValue returned = {return_tag, return_number, return_pointer};
  retain_value(returned);
  LanArcEntry* entry = frame->values;
  while (entry) {
    LanArcEntry* next = entry->next;
    release_value(entry->value);
    free(entry);
    entry = next;
  }
  arc_frame = frame->previous;
  free(frame);
}

void lan_record_release(void* pointer) {
  LanRecordNode* node = pointer;
  if (!node || node->references == UINT32_MAX) return;
  if (node->references == 0) abort();
  if (--node->references != 0) return;
  if (node->kind == LAN_BRANCH) {
    for (uint32_t i = 0; i < node->count; ++i) lan_record_release(node->children[i]);
    free(node->children);
  } else if (node->kind == LAN_LEAF) {
    free(node->key);
    release_value(node->value);
  } else {
    for (uint32_t i = 0; i < node->count; ++i) {
      free(node->entries[i].key);
      release_value(node->entries[i].value);
    }
    free(node->entries);
  }
  free(node);
  --arc_live_objects;
}

static void release_value(LanValue value) {
  if (value.tag == LAN_TEXT) lan_text_release(value.pointer);
  else if (value.tag == LAN_LIST) lan_list_release(value.pointer);
  else if (value.tag == LAN_RECORD) lan_record_release(value.pointer);
}

static LanRecordNode* allocate_node(uint32_t kind) {
  LanRecordNode* node = calloc(1, sizeof(*node));
  if (!node) abort();
  node->references = 1;
  ++arc_live_objects;
  node->kind = kind;
  return node;
}

static LanRecordNode* make_leaf(uint64_t hash, const char* key, LanValue value) {
  LanRecordNode* node = allocate_node(LAN_LEAF);
  node->hash = hash;
  node->key = strdup(key);
  if (!node->key) abort();
  node->value = value;
  retain_value(value);
  return node;
}

static LanRecordNode* merge_leaf(LanRecordNode* old, LanRecordNode* added,
                                 unsigned shift) {
  if (old->hash == added->hash || shift >= 60) {
    LanRecordNode* collision = allocate_node(LAN_COLLISION);
    collision->hash = old->hash;
    collision->count = 2;
    collision->entries = calloc(2, sizeof(LanEntry));
    if (!collision->entries) abort();
    collision->entries[0] = (LanEntry){old->hash, strdup(old->key), old->value};
    collision->entries[1] = (LanEntry){added->hash, strdup(added->key), added->value};
    if (!collision->entries[0].key || !collision->entries[1].key) abort();
    retain_value(old->value);
    retain_value(added->value);
    lan_record_release(added);
    return collision;
  }
  uint32_t old_slot = (uint32_t)((old->hash >> shift) & 31);
  uint32_t new_slot = (uint32_t)((added->hash >> shift) & 31);
  LanRecordNode* branch = allocate_node(LAN_BRANCH);
  if (old_slot == new_slot) {
    branch->bitmap = UINT32_C(1) << old_slot;
    branch->count = 1;
    branch->children = malloc(sizeof(LanRecordNode*));
    if (!branch->children) abort();
    branch->children[0] = merge_leaf(old, added, shift + 5);
  } else {
    branch->bitmap = (UINT32_C(1) << old_slot) | (UINT32_C(1) << new_slot);
    branch->count = 2;
    branch->children = malloc(2 * sizeof(LanRecordNode*));
    if (!branch->children) abort();
    if (old_slot < new_slot) {
      lan_record_retain(old);
      branch->children[0] = old;
      branch->children[1] = added;
    } else {
      branch->children[0] = added;
      lan_record_retain(old);
      branch->children[1] = old;
    }
  }
  return branch;
}

static LanRecordNode* assoc(LanRecordNode* node, uint64_t hash, const char* key,
                            LanValue value, unsigned shift) {
  if (!node || (node == &empty_record && node->count == 0))
    return make_leaf(hash, key, value);
  if (node->kind == LAN_LEAF) {
    if (node->hash == hash && strcmp(node->key, key) == 0)
      return make_leaf(hash, key, value);
    return merge_leaf(node, make_leaf(hash, key, value), shift);
  }
  if (node->kind == LAN_COLLISION) {
    LanRecordNode* result = allocate_node(LAN_COLLISION);
    result->hash = node->hash;
    uint32_t replace = node->count;
    for (uint32_t i = 0; i < node->count; ++i)
      if (node->entries[i].hash == hash && strcmp(node->entries[i].key, key) == 0)
        replace = i;
    result->count = node->count + (replace == node->count);
    result->entries = calloc(result->count, sizeof(LanEntry));
    if (!result->entries) abort();
    for (uint32_t i = 0; i < result->count; ++i) {
      const LanEntry* source = i < node->count ? &node->entries[i] : NULL;
      if (i == replace || !source) {
        result->entries[i] = (LanEntry){hash, strdup(key), value};
      } else {
        result->entries[i] = (LanEntry){source->hash, strdup(source->key), source->value};
      }
      if (!result->entries[i].key) abort();
      retain_value(result->entries[i].value);
    }
    return result;
  }

  uint32_t slot = (uint32_t)((hash >> shift) & 31);
  uint32_t bit = UINT32_C(1) << slot;
  uint32_t index = child_index(node->bitmap, bit);
  LanRecordNode* result = allocate_node(LAN_BRANCH);
  result->bitmap = node->bitmap | bit;
  result->count = node->count + ((node->bitmap & bit) == 0);
  result->children = malloc(result->count * sizeof(LanRecordNode*));
  if (!result->children) abort();
  uint32_t old_index = 0;
  for (uint32_t i = 0; i < result->count; ++i) {
    if (i == index) {
      result->children[i] = (node->bitmap & bit)
          ? assoc(node->children[old_index++], hash, key, value, shift + 5)
          : make_leaf(hash, key, value);
    } else {
      result->children[i] = node->children[old_index++];
      lan_record_retain(result->children[i]);
    }
  }
  return result;
}

static LanRecordNode* dissoc(LanRecordNode* node, uint64_t hash,
                             const char* key, unsigned shift) {
  if (!node || node == &empty_record) return &empty_record;
  if (node->kind == LAN_LEAF) {
    if (node->hash == hash && strcmp(node->key, key) == 0)
      return &empty_record;
    lan_record_retain(node);
    return node;
  }
  if (node->kind == LAN_COLLISION) {
    uint32_t removed = node->count;
    for (uint32_t i = 0; i < node->count; ++i)
      if (node->entries[i].hash == hash &&
          strcmp(node->entries[i].key, key) == 0) {
        removed = i;
        break;
      }
    if (removed == node->count) {
      lan_record_retain(node);
      return node;
    }
    if (node->count == 2) {
      const LanEntry* kept = &node->entries[removed == 0 ? 1 : 0];
      return make_leaf(kept->hash, kept->key, kept->value);
    }
    LanRecordNode* result = allocate_node(LAN_COLLISION);
    result->hash = node->hash;
    result->count = node->count - 1;
    result->entries = calloc(result->count, sizeof(LanEntry));
    if (!result->entries) abort();
    uint32_t output = 0;
    for (uint32_t i = 0; i < node->count; ++i) {
      if (i == removed) continue;
      const LanEntry* source = &node->entries[i];
      result->entries[output] =
          (LanEntry){source->hash, strdup(source->key), source->value};
      if (!result->entries[output].key) abort();
      retain_value(result->entries[output].value);
      ++output;
    }
    return result;
  }

  uint32_t slot = (uint32_t)((hash >> shift) & 31);
  uint32_t bit = UINT32_C(1) << slot;
  if ((node->bitmap & bit) == 0) {
    lan_record_retain(node);
    return node;
  }
  uint32_t index = child_index(node->bitmap, bit);
  LanRecordNode* child = dissoc(node->children[index], hash, key, shift + 5);
  if (child == node->children[index]) {
    /* dissoc returned an owned reference to the unchanged subtree. */
    lan_record_release(child);
    lan_record_retain(node);
    return node;
  }
  if (child == &empty_record && node->count == 1) return &empty_record;
  if (child == &empty_record && node->count == 2) {
    LanRecordNode* kept = node->children[index == 0 ? 1 : 0];
    if (kept->kind != LAN_BRANCH) {
      lan_record_retain(kept);
      return kept;
    }
  }

  LanRecordNode* result = allocate_node(LAN_BRANCH);
  int removed = child == &empty_record;
  result->bitmap = removed ? node->bitmap & ~bit : node->bitmap;
  result->count = node->count - (uint32_t)removed;
  result->children = malloc(result->count * sizeof(LanRecordNode*));
  if (!result->children) abort();
  uint32_t output = 0;
  for (uint32_t i = 0; i < node->count; ++i) {
    if (i == index) {
      if (!removed) result->children[output++] = child;
      continue;
    }
    result->children[output] = node->children[i];
    lan_record_retain(result->children[output]);
    ++output;
  }
  return result;
}

void* lan_record_empty(void) { return &empty_record; }

void* lan_record_assoc(void* record, const char* key, int32_t tag,
                       double number, void* pointer) {
  LanValue value = {tag, number, pointer};
  return assoc(record, hash_text(key), key, value, 0);
}

void* lan_record_remove_field(void* record, const char* key) {
  return dissoc(record, hash_text(key), key, 0);
}

int32_t lan_record_lookup(void* record, const char* key, LanValue* output) {
  LanRecordNode* node = record;
  if (!node) return 0;
  uint64_t hash = hash_text(key);
  unsigned shift = 0;
  while (node) {
    if (node->kind == LAN_LEAF) {
      if (node->hash == hash && strcmp(node->key, key) == 0) {
        *output = node->value;
        return 1;
      }
      return 0;
    }
    if (node->kind == LAN_COLLISION) {
      for (uint32_t i = 0; i < node->count; ++i)
        if (node->entries[i].hash == hash && strcmp(node->entries[i].key, key) == 0) {
          *output = node->entries[i].value;
          return 1;
        }
      return 0;
    }
    uint32_t slot = (uint32_t)((hash >> shift) & 31);
    uint32_t bit = UINT32_C(1) << slot;
    if ((node->bitmap & bit) == 0) return 0;
    node = node->children[child_index(node->bitmap, bit)];
    shift += 5;
  }
  return 0;
}

static uint32_t entry_count(LanRecordNode* node) {
  if (!node) return 0;
  if (node->kind == LAN_LEAF) return 1;
  if (node->kind == LAN_COLLISION) return node->count;
  uint32_t total = 0;
  for (uint32_t i = 0; i < node->count; ++i) total += entry_count(node->children[i]);
  return total;
}

static void collect_entries(LanRecordNode* node, LanEntry* entries, uint32_t* index) {
  if (node->kind == LAN_LEAF) {
    entries[(*index)++] = (LanEntry){node->hash, node->key, node->value};
    return;
  }
  if (node->kind == LAN_COLLISION) {
    for (uint32_t i = 0; i < node->count; ++i) entries[(*index)++] = node->entries[i];
    return;
  }
  for (uint32_t i = 0; i < node->count; ++i)
    collect_entries(node->children[i], entries, index);
}

static int compare_entries(const void* left, const void* right) {
  const LanEntry* a = left;
  const LanEntry* b = right;
  return strcmp(a->key, b->key);
}

static LanValue load_list_value(const LanList* list, uint64_t index) {
  if (list->storage_tag == LAN_NUMBER) {
    if (list->presence && !list->presence[index])
      return (LanValue){LAN_MISSING, 0, NULL};
    return (LanValue){LAN_NUMBER, ((double*)list->elements)[index], NULL};
  }
  if (list->storage_tag == LAN_BOOLEAN) {
    uint8_t encoded = ((uint8_t*)list->elements)[index];
    return encoded == 0 ? (LanValue){LAN_MISSING, 0, NULL}
                        : (LanValue){LAN_BOOLEAN, encoded == 2, NULL};
  }
  if (list->storage_tag == LAN_TEXT || list->storage_tag == LAN_LIST ||
      list->storage_tag == LAN_RECORD) {
    void* pointer = ((void**)list->elements)[index];
    return pointer ? (LanValue){list->storage_tag, 0, pointer}
                   : (LanValue){LAN_MISSING, 0, NULL};
  }
  return ((LanValue*)list->elements)[index];
}

static int32_t same_value(LanValue left, LanValue right);

static int32_t same_list(LanList* left, LanList* right) {
  if (left == right) return 1;
  if (!left || !right || left->length != right->length) return 0;
  for (uint64_t i = 0; i < left->length; ++i)
    if (!same_value(load_list_value(left, i), load_list_value(right, i))) return 0;
  return 1;
}

static int32_t same_record(LanRecordNode* left, LanRecordNode* right) {
  if (left == right) return 1;
  uint32_t count = entry_count(left);
  if (count != entry_count(right)) return 0;
  LanEntry* entries = count ? malloc(count * sizeof(*entries)) : NULL;
  if (count && !entries) abort();
  uint32_t index = 0;
  if (count) collect_entries(left, entries, &index);
  for (uint32_t i = 0; i < count; ++i) {
    LanValue other;
    if (!lan_record_lookup(right, entries[i].key, &other) ||
        !same_value(entries[i].value, other)) {
      free(entries);
      return 0;
    }
  }
  free(entries);
  return 1;
}

static int32_t same_value(LanValue left, LanValue right) {
  if (left.tag != right.tag) return 0;
  if (left.tag == LAN_MISSING) return 1;
  if (left.tag == LAN_NUMBER || left.tag == LAN_BOOLEAN)
    return left.number == right.number;
  if (left.tag == LAN_TEXT)
    return left.pointer == right.pointer ||
           (left.pointer && right.pointer && strcmp(left.pointer, right.pointer) == 0);
  if (left.tag == LAN_LIST) return same_list(left.pointer, right.pointer);
  if (left.tag == LAN_RECORD) return same_record(left.pointer, right.pointer);
  return 0;
}

int32_t lan_value_same(int32_t left_tag, double left_number, void* left_pointer,
                       int32_t right_tag, double right_number, void* right_pointer) {
  return same_value((LanValue){left_tag, left_number, left_pointer},
                    (LanValue){right_tag, right_number, right_pointer});
}

static LanList* make_typed_list(uint64_t length, int32_t storage_tag) {
  LanList* list = malloc(sizeof(*list));
  if (!list) abort();
  list->length = length;
  list->storage_tag = storage_tag;
  list->references = 1;
  ++arc_live_objects;
  list->presence = NULL;
  size_t element_size = sizeof(LanValue);
  if (storage_tag == LAN_NUMBER) element_size = sizeof(double);
  else if (storage_tag == LAN_BOOLEAN) element_size = sizeof(uint8_t);
  else if (storage_tag == LAN_TEXT || storage_tag == LAN_LIST ||
           storage_tag == LAN_RECORD) element_size = sizeof(void*);
  list->elements = length ? calloc(length, element_size) : NULL;
  if (length && !list->elements) abort();
  if (storage_tag == LAN_NUMBER && length) {
    list->presence = calloc(length, sizeof(*list->presence));
    if (!list->presence) abort();
  }
  return list;
}

static void lan_list_release(void* pointer) {
  LanList* list = pointer;
  if (!list) return;
  if (list->references == 0) abort();
  if (--list->references != 0) return;
  for (uint64_t i = 0; i < list->length; ++i)
    release_value(load_list_value(list, i));
  free(list->presence);
  free(list->elements);
  free(list);
  --arc_live_objects;
}

void lan_value_retain(int32_t tag, double number, void* pointer) {
  retain_value((LanValue){tag, number, pointer});
}

void lan_value_release(int32_t tag, double number, void* pointer) {
  release_value((LanValue){tag, number, pointer});
}

static char* allocate_text(size_t length) {
  if (length > SIZE_MAX - sizeof(LanTextHeader) - 1) abort();
  LanTextHeader* text = malloc(sizeof(*text) + length + 1);
  if (!text) abort();
  text->references = 1;
  text->marker = LAN_TEXT_MARKER;
  text->bytes[length] = '\0';
  ++arc_live_objects;
  return text->bytes;
}

char* lan_text_from_cstr(const char* source) {
  size_t length = strlen(source);
  char* result = allocate_text(length);
  memcpy(result, source, length);
  return result;
}

static void store_list_value(LanList* list, uint64_t index, LanValue value) {
  if (list->storage_tag == LAN_NUMBER) {
    if (value.tag == LAN_MISSING) {
      list->presence[index] = 0;
      return;
    }
    ((double*)list->elements)[index] = value.number;
    list->presence[index] = 1;
    return;
  }
  if (list->storage_tag == LAN_BOOLEAN) {
    ((uint8_t*)list->elements)[index] =
        value.tag == LAN_MISSING ? 0 : (value.number != 0 ? 2 : 1);
    return;
  }
  if (list->storage_tag == LAN_TEXT || list->storage_tag == LAN_LIST ||
      list->storage_tag == LAN_RECORD) {
    ((void**)list->elements)[index] =
        value.tag == LAN_MISSING ? NULL : value.pointer;
    retain_value(value);
    return;
  }
  ((LanValue*)list->elements)[index] = value;
  retain_value(value);
}

int32_t lan_list_lookup(void* list_pointer, double index, LanValue* output) {
  LanList* list = list_pointer;
  if (!list || index < 0 || index > 9007199254740992.0) return 0;
  uint64_t integer = (uint64_t)index;
  if ((double)integer != index || integer >= list->length) return 0;
  *output = load_list_value(list, integer);
  return 1;
}

int32_t lan_list_lookup_at(void* list_pointer, uint64_t index,
                           LanValue* output) {
  LanList* list = list_pointer;
  if (!list || index >= list->length) return 0;
  *output = load_list_value(list, index);
  return 1;
}

static int32_t unicode_white_space(utf8proc_int32_t codepoint) {
  return (codepoint >= 0x0009 && codepoint <= 0x000d) || codepoint == 0x0020 ||
         codepoint == 0x0085 || codepoint == 0x00a0 || codepoint == 0x1680 ||
         (codepoint >= 0x2000 && codepoint <= 0x200a) || codepoint == 0x2028 ||
         codepoint == 0x2029 || codepoint == 0x202f || codepoint == 0x205f ||
         codepoint == 0x3000;
}

static char* duplicate_text_range(const char* start, size_t length) {
  char* result = allocate_text(length);
  memcpy(result, start, length);
  return result;
}

int32_t lan_text_length(const char* text, double* output) {
  const utf8proc_uint8_t* current = (const utf8proc_uint8_t*)text;
  utf8proc_ssize_t remaining = (utf8proc_ssize_t)strlen(text);
  utf8proc_int32_t previous = 0;
  utf8proc_int32_t state = 0;
  uint64_t graphemes = 0;
  int32_t first = 1;
  while (remaining > 0) {
    utf8proc_int32_t codepoint;
    utf8proc_ssize_t consumed = utf8proc_iterate(current, remaining, &codepoint);
    if (consumed < 0) return -1;
    if (first || utf8proc_grapheme_break_stateful(previous, codepoint, &state))
      ++graphemes;
    first = 0;
    previous = codepoint;
    current += consumed;
    remaining -= consumed;
  }
  *output = (double)graphemes;
  return 0;
}

char* lan_text_trim(const char* text, int32_t* status) {
  const utf8proc_uint8_t* bytes = (const utf8proc_uint8_t*)text;
  size_t length = strlen(text);
  size_t first = length;
  size_t last = 0;
  size_t offset = 0;
  int32_t saw_non_space = 0;
  *status = 0;
  while (offset < length) {
    utf8proc_int32_t codepoint;
    utf8proc_ssize_t consumed = utf8proc_iterate(
        bytes + offset, (utf8proc_ssize_t)(length - offset), &codepoint);
    if (consumed < 0) { *status = -1; return NULL; }
    if (!unicode_white_space(codepoint)) {
      if (!saw_non_space) first = offset;
      saw_non_space = 1;
      last = offset + (size_t)consumed;
    }
    offset += (size_t)consumed;
  }
  return saw_non_space ? duplicate_text_range(text + first, last - first)
                       : duplicate_text_range("", 0);
}

char* lan_text_case(const char* text, int32_t uppercase, int32_t* status) {
  size_t length = strlen(text);
  if (length > (SIZE_MAX - 1) / 4) abort();
  utf8proc_uint8_t* output = (utf8proc_uint8_t*)allocate_text(length * 4);
  const utf8proc_uint8_t* current = (const utf8proc_uint8_t*)text;
  utf8proc_ssize_t remaining = (utf8proc_ssize_t)length;
  size_t written = 0;
  *status = 0;
  while (remaining > 0) {
    utf8proc_int32_t codepoint;
    utf8proc_ssize_t consumed = utf8proc_iterate(current, remaining, &codepoint);
    if (consumed < 0) {
      lan_text_release(output);
      *status = -1;
      return NULL;
    }
    codepoint = uppercase ? utf8proc_toupper(codepoint)
                          : utf8proc_tolower(codepoint);
    utf8proc_ssize_t encoded = utf8proc_encode_char(codepoint, output + written);
    if (encoded < 0) {
      lan_text_release(output);
      *status = -1;
      return NULL;
    }
    written += (size_t)encoded;
    current += consumed;
    remaining -= consumed;
  }
  output[written] = '\0';
  return (char*)output;
}

int32_t lan_text_starts_with(const char* text, const char* prefix) {
  size_t text_length = strlen(text);
  size_t prefix_length = strlen(prefix);
  return prefix_length <= text_length &&
         memcmp(text, prefix, prefix_length) == 0;
}

int32_t lan_text_ends_with(const char* text, const char* suffix) {
  size_t text_length = strlen(text);
  size_t suffix_length = strlen(suffix);
  return suffix_length <= text_length &&
         memcmp(text + text_length - suffix_length, suffix, suffix_length) == 0;
}

int32_t lan_text_contains(const char* text, const char* part) {
  return strstr(text, part) != NULL;
}

char* lan_text_concat(const char* left, const char* right) {
  size_t left_length = strlen(left);
  size_t right_length = strlen(right);
  if (right_length > SIZE_MAX - left_length) abort();
  char* result = allocate_text(left_length + right_length);
  memcpy(result, left, left_length);
  memcpy(result + left_length, right, right_length);
  return result;
}

void* lan_text_split(const char* text, const char* separator, int32_t* status) {
  size_t separator_length = strlen(separator);
  *status = 0;
  if (separator_length == 0) { *status = -1; return NULL; }
  uint64_t pieces = 1;
  const char* current = text;
  const char* found;
  while ((found = strstr(current, separator)) != NULL) {
    ++pieces;
    current = found + separator_length;
  }
  LanList* result = make_typed_list(pieces, LAN_TEXT);
  current = text;
  for (uint64_t index = 0; index < pieces; ++index) {
    found = strstr(current, separator);
    size_t length = found ? (size_t)(found - current) : strlen(current);
    char* piece = duplicate_text_range(current, length);
    store_list_value(result, index, (LanValue){LAN_TEXT, 0, piece});
    lan_text_release(piece);
    current = found ? found + separator_length : current + length;
  }
  return result;
}

char* lan_text_join(void* list_pointer, const char* separator, int32_t* status) {
  LanList* list = list_pointer;
  size_t separator_length = strlen(separator);
  size_t total = 0;
  diagnostic_error_index = -1;
  *status = 0;
  for (uint64_t index = 0; index < list->length; ++index) {
    LanValue value = load_list_value(list, index);
    if (value.tag != LAN_TEXT) {
      diagnostic_error_index = (int64_t)index;
      *status = -1;
      return NULL;
    }
    size_t item_length = strlen(value.pointer);
    if (item_length > SIZE_MAX - total) abort();
    total += item_length;
    if (index != 0) {
      if (separator_length > SIZE_MAX - total) abort();
      total += separator_length;
    }
  }
  char* result = allocate_text(total);
  size_t written = 0;
  for (uint64_t index = 0; index < list->length; ++index) {
    if (index != 0) {
      memcpy(result + written, separator, separator_length);
      written += separator_length;
    }
    LanValue value = load_list_value(list, index);
    size_t item_length = strlen(value.pointer);
    memcpy(result + written, value.pointer, item_length);
    written += item_length;
  }
  return result;
}

void* lan_record_fields(void* record) {
  LanRecordNode* root = record;
  uint32_t count = entry_count(root);
  LanEntry* entries = count ? malloc(count * sizeof(*entries)) : NULL;
  uint32_t index = 0;
  if (count) collect_entries(root, entries, &index);
  qsort(entries, count, sizeof(*entries), compare_entries);
  LanList* result = make_typed_list(count, LAN_TEXT);
  for (uint32_t field = 0; field < count; ++field)
  {
    char* name = lan_text_from_cstr(entries[field].key);
    store_list_value(result, field, (LanValue){LAN_TEXT, 0, name});
    lan_text_release(name);
  }
  free(entries);
  return result;
}

int32_t lan_record_has_field(void* record, const char* key) {
  LanValue ignored;
  return lan_record_lookup(record, key, &ignored);
}

static int32_t validate_table_node(LanRecordNode* node, uint64_t* rows,
                                   int32_t* has_column) {
  if (!node) return 1;
  if (node->kind == LAN_BRANCH) {
    for (uint32_t i = 0; i < node->count; ++i)
      if (!validate_table_node(node->children[i], rows, has_column)) return 0;
    return 1;
  }
  uint32_t count = node->kind == LAN_LEAF ? 1 : node->count;
  for (uint32_t i = 0; i < count; ++i) {
    LanValue value = node->kind == LAN_LEAF ? node->value : node->entries[i].value;
    if (value.tag != LAN_LIST || !value.pointer) return 0;
    uint64_t length = ((LanList*)value.pointer)->length;
    if (*has_column && length != *rows) return 0;
    *rows = length;
    *has_column = 1;
  }
  return 1;
}

int32_t lan_table_validate(void* record, uint64_t* rows) {
  int32_t has_column = 0;
  *rows = 0;
  return validate_table_node(record, rows, &has_column);
}

uint64_t lan_table_row_count(void* record) {
  uint64_t rows = 0;
  int32_t ignored = 0;
  validate_table_node(record, &rows, &ignored);
  return rows;
}

void* lan_table_row(void* record, uint64_t row, int32_t* status) {
  *status = 0;
  uint64_t rows = lan_table_row_count(record);
  if (row >= rows) { *status = -1; return NULL; }
  LanRecordNode* root = record;
  uint32_t count = entry_count(root);
  LanEntry* entries = count ? malloc(count * sizeof(*entries)) : NULL;
  uint32_t index = 0;
  if (count) collect_entries(root, entries, &index);
  void* result = lan_record_empty();
  for (uint32_t i = 0; i < count; ++i) {
    if (entries[i].value.tag != LAN_LIST || !entries[i].value.pointer) {
      *status = -2; lan_record_release(result); free(entries); return NULL;
    }
    LanList* column = entries[i].value.pointer;
    if (row >= column->length) {
      *status = -2; lan_record_release(result); free(entries); return NULL;
    }
    LanValue value = load_list_value(column, row);
    void* updated = lan_record_assoc(result, entries[i].key,
                                     value.tag, value.number, value.pointer);
    lan_record_release(result);
    result = updated;
  }
  free(entries);
  return result;
}

int32_t lan_table_cell(void* record, const char* key, uint64_t row,
                       LanValue* output) {
  LanValue column;
  if (!lan_record_lookup(record, key, &column) ||
      column.tag != LAN_LIST || !column.pointer)
    return 0;
  LanList* list = column.pointer;
  if (row >= list->length) return 0;
  *output = load_list_value(list, row);
  return 1;
}

void* lan_table_from_rows(void* rows_pointer, int32_t* status) {
  LanList* rows = rows_pointer;
  *status = 0;
  if (!rows) { *status = -1; return NULL; }
  if (rows->length == 0) return lan_record_empty();
  LanValue first_row = load_list_value(rows, 0);
  if (first_row.tag != LAN_RECORD || !first_row.pointer) {
    *status = -2; return NULL;
  }

  LanRecordNode* first = first_row.pointer;
  uint32_t count = entry_count(first);
  LanEntry* schema = count ? malloc(count * sizeof(*schema)) : NULL;
  uint32_t schema_index = 0;
  if (count) collect_entries(first, schema, &schema_index);
  void* result = lan_record_empty();

  for (uint32_t field = 0; field < count; ++field) {
    int32_t observed_tag = LAN_MISSING;
    for (uint64_t row = 0; row < rows->length; ++row) {
      LanValue row_value = load_list_value(rows, row);
      if (row_value.tag != LAN_RECORD || !row_value.pointer ||
          entry_count(row_value.pointer) != count) {
        *status = -3; free(schema); return NULL;
      }
      LanValue cell;
      if (!lan_record_lookup(row_value.pointer, schema[field].key, &cell)) {
        *status = -3; free(schema); return NULL;
      }
      if (cell.tag != LAN_MISSING) {
        if (observed_tag != LAN_MISSING && observed_tag != cell.tag) {
          *status = -4; free(schema); return NULL;
        }
        observed_tag = cell.tag;
      }
    }
    LanList* column = make_typed_list(rows->length, observed_tag);
    for (uint64_t row = 0; row < rows->length; ++row) {
      LanValue row_value = load_list_value(rows, row);
      LanValue cell;
      lan_record_lookup(row_value.pointer, schema[field].key, &cell);
      store_list_value(column, row, cell);
    }
    void* updated = lan_record_assoc(result, schema[field].key,
                                     LAN_LIST, 0, column);
    lan_record_release(result);
    result = updated;
  }
  free(schema);
  return result;
}

void* lan_record_merge_checked(void* base_pointer, void* changes_pointer,
                               int32_t replace, int32_t* status) {
  LanRecordNode* base = base_pointer;
  LanRecordNode* changes = changes_pointer;
  *status = 0;
  uint32_t count = entry_count(changes);
  LanEntry* entries = count ? malloc(count * sizeof(*entries)) : NULL;
  uint32_t index = 0;
  if (count) collect_entries(changes, entries, &index);
  void* result = base;
  lan_record_retain(result);
  for (uint32_t i = 0; i < count; ++i) {
    LanValue ignored;
    int exists = lan_record_lookup(base, entries[i].key, &ignored);
    if ((!replace && exists) || (replace && !exists)) {
      *status = replace ? -2 : -1;
      lan_record_release(result);
      free(entries);
      return NULL;
    }
    LanValue value = entries[i].value;
    void* updated = lan_record_assoc(result, entries[i].key,
                                     value.tag, value.number, value.pointer);
    lan_record_release(result);
    result = updated;
  }
  free(entries);
  return result;
}

void* lan_record_merge(void* left_pointer, void* right_pointer) {
  LanRecordNode* right = right_pointer;
  uint32_t count = entry_count(right);
  LanEntry* entries = count ? malloc(count * sizeof(*entries)) : NULL;
  uint32_t index = 0;
  if (count) collect_entries(right, entries, &index);
  void* result = left_pointer;
  lan_record_retain(result);
  for (uint32_t i = 0; i < count; ++i) {
    LanValue value = entries[i].value;
    void* updated = lan_record_assoc(result, entries[i].key,
                                     value.tag, value.number, value.pointer);
    lan_record_release(result);
    result = updated;
  }
  free(entries);
  return result;
}

/* operation: 0 sum, 1 average, 2 minimum, 3 maximum, 4 present count.
   Returns -3 for a non-number numeric aggregate, 0 when no number exists,
   and 1 on success. */
static int32_t aggregate_list(LanList* list, int32_t operation, double* output) {
  double result = 0.0;
  uint64_t present = 0;
  for (uint64_t i = 0; i < list->length; ++i) {
    LanValue value = load_list_value(list, i);
    if (value.tag == LAN_MISSING) continue;
    if (operation == 4) {
      ++present;
      continue;
    }
    if (value.tag != LAN_NUMBER) return -3;
    if (present == 0) result = value.number;
    else if (operation == 0 || operation == 1) result += value.number;
    else if (operation == 2 && value.number < result) result = value.number;
    else if (operation == 3 && value.number > result) result = value.number;
    ++present;
  }
  if (operation == 4) {
    *output = (double)present;
    return 1;
  }
  if (present == 0) {
    *output = 0.0;
    return operation == 0 ? 1 : 0;
  }
  if (operation == 1) result /= (double)present;
  *output = result;
  return 1;
}

static double floor_modulo(double dividend, double divisor) {
  double result = fmod(dividend, divisor);
  if (result != 0.0 && ((result < 0.0) != (divisor < 0.0))) result += divisor;
  return result == 0.0 ? 0.0 : result;
}

int32_t lan_number_unary(double number, int32_t operation, double* output) {
  if (!isfinite(number)) return -1;
  double result;
  switch (operation) {
    case 0: result = fabs(number); break;
    case 1: result = floor(number); break;
    case 2: result = ceil(number); break;
    case 3: result = number < 0.0 ? -1.0 : number > 0.0 ? 1.0 : 0.0; break;
    case 4:
      if (number < 0.0) return -1;
      result = sqrt(number);
      break;
    case 5: result = exp(number); break;
    case 6: result = sin(number); break;
    case 7: result = cos(number); break;
    case 8: result = tan(number); break;
    case 9:
      if (number < -1.0 || number > 1.0) return -1;
      result = asin(number);
      break;
    case 10:
      if (number < -1.0 || number > 1.0) return -1;
      result = acos(number);
      break;
    case 11: result = atan(number); break;
    case 12: result = number * (0x1.921fb54442d18p+1 / 180.0); break;
    case 13: result = number * (180.0 / 0x1.921fb54442d18p+1); break;
    default: return -1;
  }
  if (!isfinite(result)) return -1;
  *output = result == 0.0 ? 0.0 : result;
  return 0;
}

int32_t lan_number_binary(double left, double right, int32_t operation,
                          double* output) {
  if (!isfinite(left) || !isfinite(right)) return -1;
  double result;
  switch (operation) {
    case 0:
      if ((left < 0.0 && right != trunc(right)) ||
          (left == 0.0 && right < 0.0))
        return -1;
      result = pow(left, right);
      break;
    case 1:
      if (left <= 0.0 || right <= 0.0 || right == 1.0) return -1;
      result = log(left) / log(right);
      break;
    case 2: result = hypot(left, right); break;
    case 3: result = atan2(left, right); break;
    default: return -1;
  }
  if (!isfinite(result)) return -1;
  *output = result == 0.0 ? 0.0 : result;
  return 0;
}

int32_t lan_number_round(double number, double digits_value, double* output) {
  if (!isfinite(number) || !isfinite(digits_value) ||
      digits_value < -308.0 || digits_value > 308.0 ||
      digits_value != trunc(digits_value))
    return -1;
  int digits = (int)digits_value;
  double result;
  if (digits >= 16 || (digits >= 0 && fabs(number) >= 1.0e15)) {
    result = number;
  } else if (digits >= 0) {
    double scale = pow(10.0, digits);
    result = nearbyint(number * scale) / scale;
  } else {
    double scale = pow(10.0, -digits);
    result = nearbyint(number / scale) * scale;
  }
  if (!isfinite(result)) return -1;
  *output = result == 0.0 ? 0.0 : result;
  return 0;
}

int32_t lan_number_clamp(double number, double minimum, double maximum,
                         double* output) {
  if (!isfinite(number) || !isfinite(minimum) || !isfinite(maximum) ||
      minimum > maximum)
    return -1;
  *output = number < minimum ? minimum : number > maximum ? maximum : number;
  return 0;
}

static int32_t valid_number_text(const char* text) {
  const unsigned char* current = (const unsigned char*)text;
  if (*current == '+' || *current == '-') ++current;
  int32_t before = 0;
  while (*current >= '0' && *current <= '9') { before = 1; ++current; }
  int32_t after = 0;
  if (*current == '.') {
    ++current;
    while (*current >= '0' && *current <= '9') { after = 1; ++current; }
  }
  if (!before && !after) return 0;
  if (*current == 'e' || *current == 'E') {
    ++current;
    if (*current == '+' || *current == '-') ++current;
    int32_t exponent = 0;
    while (*current >= '0' && *current <= '9') { exponent = 1; ++current; }
    if (!exponent) return 0;
  }
  return *current == '\0';
}

int32_t lan_parse_number(const char* text, double* output) {
  if (!valid_number_text(text)) return 0;
  errno = 0;
  char* end = NULL;
  double value = strtod(text, &end);
  if (errno == ERANGE || !end || *end != '\0' || !isfinite(value)) return 0;
  *output = value;
  return 1;
}

int32_t lan_list_aggregate(void* list, int32_t operation, double* output) {
  if (!list) return -2;
  return aggregate_list(list, operation, output);
}

int32_t lan_table_aggregate(void* record, const char* key, int32_t operation,
                            double* output) {
  LanValue column;
  if (!lan_record_lookup(record, key, &column)) return -1;
  if (column.tag != LAN_LIST || !column.pointer) return -2;
  return aggregate_list(column.pointer, operation, output);
}

static LanValue scalar_binary(LanValue a, LanValue b, int32_t operation,
                              int32_t* status) {
  if (operation == 12) {
    if (a.tag != LAN_MISSING && b.tag != LAN_MISSING && a.tag != b.tag)
      *status = -3;
    return a.tag == LAN_MISSING ? b : a;
  }
  if (operation == 10 || operation == 11) {
    if ((a.tag != LAN_MISSING && a.tag != LAN_BOOLEAN) ||
        (b.tag != LAN_MISSING && b.tag != LAN_BOOLEAN)) {
      *status = -3;
      return (LanValue){LAN_MISSING, 0, NULL};
    }
    int a_decides = a.tag == LAN_BOOLEAN &&
        (operation == 10 ? a.number == 0 : a.number != 0);
    int b_decides = b.tag == LAN_BOOLEAN &&
        (operation == 10 ? b.number == 0 : b.number != 0);
    if (a_decides || b_decides)
      return (LanValue){LAN_BOOLEAN, operation == 10 ? 0 : 1, NULL};
    if (a.tag == LAN_MISSING || b.tag == LAN_MISSING)
      return (LanValue){LAN_MISSING, 0, NULL};
    return (LanValue){LAN_BOOLEAN, operation == 10 ? 1 : 0, NULL};
  }
  if (a.tag == LAN_MISSING || b.tag == LAN_MISSING)
    return (LanValue){LAN_MISSING, 0, NULL};
  LanValue value = {LAN_NUMBER, 0, NULL};
  if (operation <= 3 || operation == 13) {
    if (operation == 0 && a.tag == LAN_TEXT && b.tag == LAN_TEXT) {
      size_t left = strlen(a.pointer);
      size_t right = strlen(b.pointer);
      char* joined = allocate_text(left + right);
      memcpy(joined, a.pointer, left);
      memcpy(joined + left, b.pointer, right);
      return (LanValue){LAN_TEXT, 0, joined};
    }
    if (a.tag != LAN_NUMBER || b.tag != LAN_NUMBER) {
      *status = -3;
      return (LanValue){LAN_MISSING, 0, NULL};
    }
    if ((operation == 3 || operation == 13) && b.number == 0.0) {
      *status = -4;
      return (LanValue){LAN_MISSING, 0, NULL};
    }
    value.number = operation == 0 ? a.number + b.number
                 : operation == 1 ? a.number - b.number
                 : operation == 2 ? a.number * b.number
                 : operation == 3 ? a.number / b.number
                                  : floor_modulo(a.number, b.number);
    return value;
  }
  if (a.tag != b.tag) {
    *status = -3;
    return (LanValue){LAN_MISSING, 0, NULL};
  }
  int equal = 0;
  if (a.tag == LAN_NUMBER || a.tag == LAN_BOOLEAN) equal = a.number == b.number;
  else if (a.tag == LAN_TEXT) equal = strcmp(a.pointer, b.pointer) == 0;
  else {
    *status = -3;
    return (LanValue){LAN_MISSING, 0, NULL};
  }
  value.tag = LAN_BOOLEAN;
  if (operation == 4 || operation == 5)
    value.number = operation == 4 ? equal : !equal;
  else if (a.tag != LAN_NUMBER) *status = -3;
  else value.number = operation == 6 ? a.number < b.number
                    : operation == 7 ? a.number > b.number
                    : operation == 8 ? a.number <= b.number : a.number >= b.number;
  return value;
}

static int32_t binary_leaf_tag(LanValue value, uint32_t rank) {
  if (rank == 0) return value.tag;
  if (value.tag != LAN_LIST || !value.pointer) return LAN_MISSING;
  LanList* list = value.pointer;
  return rank == 1 ? list->storage_tag : LAN_LIST;
}

static int32_t binary_result_storage_tag(LanValue a, uint32_t a_rank,
                                         LanValue b, uint32_t b_rank,
                                         int32_t operation) {
  uint32_t result_rank = a_rank > b_rank ? a_rank : b_rank;
  if (result_rank > 1) return LAN_LIST;
  if (operation >= 4 && operation <= 11) return LAN_BOOLEAN;

  int32_t a_tag = binary_leaf_tag(a, a_rank);
  int32_t b_tag = binary_leaf_tag(b, b_rank);
  int32_t element_tag = a_tag == LAN_MISSING ? b_tag : a_tag;
  if (element_tag == LAN_NUMBER || element_tag == LAN_BOOLEAN ||
      element_tag == LAN_TEXT || element_tag == LAN_LIST ||
      element_tag == LAN_RECORD)
    return element_tag;
  return LAN_MISSING;
}

static LanValue recursive_binary(LanValue a, uint32_t a_rank,
                                 LanValue b, uint32_t b_rank,
                                 int32_t operation, int32_t* status);

static LanValue indexed_recursive_binary(
    LanValue a, uint32_t a_rank, LanValue b, uint32_t b_rank,
    int32_t operation, int32_t* status, uint64_t index) {
  LanValue value = recursive_binary(a, a_rank, b, b_rank, operation, status);
  if (*status && diagnostic_error_index < 0)
    diagnostic_error_index = (int64_t)index;
  return value;
}

static LanValue recursive_binary(LanValue a, uint32_t a_rank,
                                 LanValue b, uint32_t b_rank,
                                 int32_t operation, int32_t* status) {
  if (*status) return (LanValue){LAN_MISSING, 0, NULL};
  if (a_rank == 0 && b_rank == 0) return scalar_binary(a, b, operation, status);

  if (a_rank == b_rank && operation == 12) {
    if (a.tag == LAN_MISSING) return b;
    if (b.tag == LAN_MISSING) return a;
  }

  if (a_rank > b_rank) {
    if (a.tag == LAN_MISSING) return a;
    if (a.tag != LAN_LIST || !a.pointer) {
      *status = -3; return (LanValue){LAN_MISSING, 0, NULL};
    }
    LanList* source = a.pointer;
    LanList* result = make_typed_list(
        source->length,
        binary_result_storage_tag(a, a_rank, b, b_rank, operation));
    for (uint64_t i = 0; i < source->length; ++i)
      store_list_value(result, i, indexed_recursive_binary(
          load_list_value(source, i), a_rank - 1, b, b_rank, operation, status, i));
    return (LanValue){LAN_LIST, 0, result};
  }
  if (b_rank > a_rank) {
    if (b.tag == LAN_MISSING) return b;
    if (b.tag != LAN_LIST || !b.pointer) {
      *status = -3; return (LanValue){LAN_MISSING, 0, NULL};
    }
    LanList* source = b.pointer;
    LanList* result = make_typed_list(
        source->length,
        binary_result_storage_tag(a, a_rank, b, b_rank, operation));
    for (uint64_t i = 0; i < source->length; ++i)
      store_list_value(result, i, indexed_recursive_binary(
          a, a_rank, load_list_value(source, i), b_rank - 1, operation, status, i));
    return (LanValue){LAN_LIST, 0, result};
  }

  /* Equal non-zero ranks. A missing sub-list adopts the other operand's
     shape, except for ?? which was handled above. */
  if (a.tag == LAN_MISSING && b.tag != LAN_MISSING) {
    if (b.tag != LAN_LIST || !b.pointer) {
      *status = -3; return (LanValue){LAN_MISSING, 0, NULL};
    }
    LanList* source = b.pointer;
    LanList* result = make_typed_list(
        source->length,
        binary_result_storage_tag(a, a_rank, b, b_rank, operation));
    for (uint64_t i = 0; i < source->length; ++i)
      store_list_value(result, i, indexed_recursive_binary(
          a, a_rank - 1, load_list_value(source, i), b_rank - 1, operation, status, i));
    return (LanValue){LAN_LIST, 0, result};
  }
  if (b.tag == LAN_MISSING && a.tag != LAN_MISSING) {
    if (a.tag != LAN_LIST || !a.pointer) {
      *status = -3; return (LanValue){LAN_MISSING, 0, NULL};
    }
    LanList* source = a.pointer;
    LanList* result = make_typed_list(
        source->length,
        binary_result_storage_tag(a, a_rank, b, b_rank, operation));
    for (uint64_t i = 0; i < source->length; ++i)
      store_list_value(result, i, indexed_recursive_binary(
          load_list_value(source, i), a_rank - 1, b, b_rank - 1, operation, status, i));
    return (LanValue){LAN_LIST, 0, result};
  }
  if (a.tag == LAN_MISSING || b.tag == LAN_MISSING)
    return (LanValue){LAN_MISSING, 0, NULL};
  if (a.tag != LAN_LIST || b.tag != LAN_LIST || !a.pointer || !b.pointer) {
    *status = -3; return (LanValue){LAN_MISSING, 0, NULL};
  }
  LanList* left = a.pointer;
  LanList* right = b.pointer;
  uint64_t length = left->length < right->length ? left->length : right->length;
  LanList* result = make_typed_list(
      length, binary_result_storage_tag(a, a_rank, b, b_rank, operation));
  for (uint64_t i = 0; i < length; ++i)
    store_list_value(result, i, indexed_recursive_binary(
        load_list_value(left, i), a_rank - 1, load_list_value(right, i), b_rank - 1,
        operation, status, i));
  return (LanValue){LAN_LIST, 0, result};
}

void* lan_list_binary(int32_t a_tag, double a_number, void* a_pointer,
                      uint32_t a_rank, int32_t b_tag, double b_number,
                      void* b_pointer, uint32_t b_rank,
                      int32_t operation, int32_t* status) {
  *status = 0;
  diagnostic_error_index = -1;
  LanValue result = recursive_binary(
      (LanValue){a_tag, a_number, a_pointer}, a_rank,
      (LanValue){b_tag, b_number, b_pointer}, b_rank, operation, status);
  if (*status || result.tag != LAN_LIST) return NULL;
  return result.pointer;
}

void* lan_flat_list_binary(int32_t a_tag, double a_number, void* a_pointer,
                           uint32_t a_rank, int32_t b_tag, double b_number,
                           void* b_pointer, uint32_t b_rank,
                           int32_t operation, int32_t* status) {
  LanValue a = {a_tag, a_number, a_pointer};
  LanValue b = {b_tag, b_number, b_pointer};
  *status = 0;
  diagnostic_error_index = -1;
  if ((a_rank > 1 || b_rank > 1) || (a_rank == 0 && b_rank == 0)) {
    *status = -3;
    return NULL;
  }

  LanList* left = a_rank == 1 ? a.pointer : NULL;
  LanList* right = b_rank == 1 ? b.pointer : NULL;
  if ((left && left->storage_tag == LAN_MISSING) ||
      (right && right->storage_tag == LAN_MISSING) ||
      (a_rank == 1 && (!left || a.tag != LAN_LIST)) ||
      (b_rank == 1 && (!right || b.tag != LAN_LIST))) {
    *status = -3;
    return NULL;
  }

  uint64_t length = left ? left->length : right->length;
  if (left && right && right->length < length) length = right->length;
  int32_t storage_tag = binary_result_storage_tag(
      a, a_rank, b, b_rank, operation);
  if (storage_tag == LAN_MISSING || storage_tag == LAN_LIST ||
      storage_tag == LAN_RECORD) {
    *status = -3;
    return NULL;
  }


  if (operation == 3 || operation == 13) {
    for (uint64_t i = 0; i < length; ++i) {
      LanValue left_value = left ? load_list_value(left, i) : a;
      LanValue right_value = right ? load_list_value(right, i) : b;
      if (left_value.tag == LAN_NUMBER && right_value.tag == LAN_NUMBER &&
          right_value.number == 0.0) {
        *status = -4;
        diagnostic_error_index = (int64_t)i;
        return NULL;
      }
    }
  }

  LanList* result = make_typed_list(length, storage_tag);
  for (uint64_t i = 0; i < length; ++i) {
    LanValue left_value = left ? load_list_value(left, i) : a;
    LanValue right_value = right ? load_list_value(right, i) : b;
    LanValue value = scalar_binary(left_value, right_value, operation, status);
    if (*status) return NULL;
    store_list_value(result, i, value);
    if (operation == 0 && value.tag == LAN_TEXT) release_value(value);
  }
  return result;
}

void* lan_number_list_binary(void* left_pointer, void* right_pointer,
                             int32_t operation, int32_t* status) {
  LanList* left = left_pointer;
  LanList* right = right_pointer;
  *status = 0;
  diagnostic_error_index = -1;
  if (!left || !right || left->storage_tag != LAN_NUMBER ||
      right->storage_tag != LAN_NUMBER || operation < 0 || operation > 13 ||
      operation == 10 || operation == 11) {
    *status = -3;
    return NULL;
  }

  uint64_t length = left->length < right->length
      ? left->length : right->length;
  int32_t result_tag = operation >= 4 && operation <= 9
      ? LAN_BOOLEAN : LAN_NUMBER;
  const double* restrict left_values = left->elements;
  const double* restrict right_values = right->elements;
  const uint8_t* restrict left_presence = left->presence;
  const uint8_t* restrict right_presence = right->presence;

  if (operation == 3 || operation == 13) {
    for (uint64_t i = 0; i < length; ++i) {
      uint8_t left_present = !left_presence || left_presence[i];
      uint8_t right_present = !right_presence || right_presence[i];
      if (left_present && right_present && right_values[i] == 0.0) {
        *status = -4;
        diagnostic_error_index = (int64_t)i;
        return NULL;
      }
    }
  }

  LanList* result = make_typed_list(length, result_tag);

  if (result_tag == LAN_NUMBER) {
    double* restrict values = result->elements;
    uint8_t* restrict presence = result->presence;
    if (operation == 13) {
      for (uint64_t i = 0; i < length; ++i) {
        uint8_t left_present = !left_presence || left_presence[i];
        uint8_t right_present = !right_presence || right_presence[i];
        presence[i] = left_present && right_present;
        values[i] = presence[i]
            ? floor_modulo(left_values[i], right_values[i]) : 0.0;
      }
      return result;
    }
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i) {
      uint8_t left_present = !left_presence || left_presence[i];
      uint8_t right_present = !right_presence || right_presence[i];
      double a = left_values[i];
      double b = right_values[i];
      if (operation == 12) {
        presence[i] = left_present || right_present;
        values[i] = left_present ? a : b;
      } else {
        presence[i] = left_present && right_present;
        values[i] = operation == 0 ? a + b
                  : operation == 1 ? a - b
                  : operation == 2 ? a * b : a / b;
      }
    }
    return result;
  }

  uint8_t* restrict values = result->elements;
#pragma clang loop vectorize(enable)
  for (uint64_t i = 0; i < length; ++i) {
    uint8_t present = (!left_presence || left_presence[i]) &&
                      (!right_presence || right_presence[i]);
    double a = left_values[i];
    double b = right_values[i];
    int predicate = operation == 4 ? a == b
                  : operation == 5 ? a != b
                  : operation == 6 ? a < b
                  : operation == 7 ? a > b
                  : operation == 8 ? a <= b : a >= b;
    values[i] = present ? (predicate ? 2 : 1) : 0;
  }
  return result;
}

void* lan_boolean_list_binary(void* left_pointer, void* right_pointer,
                              int32_t operation, int32_t* status) {
  LanList* left = left_pointer;
  LanList* right = right_pointer;
  *status = 0;
  diagnostic_error_index = -1;
  if (!left || !right || left->storage_tag != LAN_BOOLEAN ||
      right->storage_tag != LAN_BOOLEAN ||
      (operation != 4 && operation != 5 && operation != 10 &&
       operation != 11 && operation != 12)) {
    *status = -3;
    return NULL;
  }

  uint64_t length = left->length < right->length
      ? left->length : right->length;
  const uint8_t* restrict a = left->elements;
  const uint8_t* restrict b = right->elements;
  LanList* result = make_typed_list(length, LAN_BOOLEAN);
  uint8_t* restrict values = result->elements;

  if (operation == 4) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i)
      values[i] = a[i] && b[i] ? (a[i] == b[i] ? 2 : 1) : 0;
    return result;
  }
  if (operation == 5) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i)
      values[i] = a[i] && b[i] ? (a[i] != b[i] ? 2 : 1) : 0;
    return result;
  }
  if (operation == 10) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i)
      values[i] = a[i] == 1 || b[i] == 1
          ? 1 : (a[i] == 0 || b[i] == 0 ? 0 : 2);
    return result;
  }
  if (operation == 11) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i)
      values[i] = a[i] == 2 || b[i] == 2
          ? 2 : (a[i] == 0 || b[i] == 0 ? 0 : 1);
    return result;
  }
#pragma clang loop vectorize(enable)
  for (uint64_t i = 0; i < length; ++i)
    values[i] = a[i] ? a[i] : b[i];
  return result;
}

void* lan_boolean_list_scalar_binary(void* list_pointer, int32_t scalar,
                                     int32_t list_on_left, int32_t operation,
                                     int32_t* status) {
  LanList* source = list_pointer;
  *status = 0;
  diagnostic_error_index = -1;
  if (!source || source->storage_tag != LAN_BOOLEAN || scalar < 0 || scalar > 2 ||
      (operation != 4 && operation != 5 && operation != 10 &&
       operation != 11 && operation != 12)) {
    *status = -3;
    return NULL;
  }

  uint64_t length = source->length;
  const uint8_t* restrict input = source->elements;
  LanList* result = make_typed_list(length, LAN_BOOLEAN);
  uint8_t* restrict values = result->elements;

  if (operation == 4) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i)
      values[i] = input[i] && scalar ? (input[i] == scalar ? 2 : 1) : 0;
    return result;
  }
  if (operation == 5) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i)
      values[i] = input[i] && scalar ? (input[i] != scalar ? 2 : 1) : 0;
    return result;
  }
  if (operation == 10) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i)
      values[i] = input[i] == 1 || scalar == 1
          ? 1 : (input[i] == 0 || scalar == 0 ? 0 : 2);
    return result;
  }
  if (operation == 11) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i)
      values[i] = input[i] == 2 || scalar == 2
          ? 2 : (input[i] == 0 || scalar == 0 ? 0 : 1);
    return result;
  }
#pragma clang loop vectorize(enable)
  for (uint64_t i = 0; i < length; ++i)
    values[i] = list_on_left
        ? (input[i] ? input[i] : (uint8_t)scalar)
        : (scalar ? (uint8_t)scalar : input[i]);
  return result;
}

void* lan_number_list_scalar_binary(void* list_pointer, double scalar,
                                    int32_t list_on_left, int32_t operation,
                                    int32_t* status) {
  LanList* source = list_pointer;
  *status = 0;
  diagnostic_error_index = -1;
  if (!source || source->storage_tag != LAN_NUMBER ||
      (source->length && !source->presence) || operation < 0 ||
      (operation > 3 && operation != 13)) {
    *status = -3;
    return NULL;
  }

  const double* restrict source_values = source->elements;
  const uint8_t* restrict source_presence = source->presence;
  uint64_t length = source->length;
  if (operation == 3 || operation == 13) {
    for (uint64_t i = 0; i < length; ++i) {
      uint8_t present = source_presence[i];
      double divisor = list_on_left ? scalar : source_values[i];
      if (present && divisor == 0.0) {
        *status = -4;
        diagnostic_error_index = (int64_t)i;
        return NULL;
      }
    }
  }

  LanList* result = make_typed_list(length, LAN_NUMBER);
  double* restrict values = result->elements;
  uint8_t* restrict presence = result->presence;
  if (operation == 0) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i) {
      presence[i] = source_presence[i];
      values[i] = source_values[i] + scalar;
    }
    return result;
  }
  if (operation == 1) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i) {
      presence[i] = source_presence[i];
      values[i] = list_on_left ? source_values[i] - scalar
                               : scalar - source_values[i];
    }
    return result;
  }
  if (operation == 2) {
#pragma clang loop vectorize(enable)
    for (uint64_t i = 0; i < length; ++i) {
      presence[i] = source_presence[i];
      values[i] = source_values[i] * scalar;
    }
    return result;
  }
  if (operation == 13) {
    for (uint64_t i = 0; i < length; ++i) {
      presence[i] = source_presence[i];
      values[i] = presence[i]
          ? (list_on_left ? floor_modulo(source_values[i], scalar)
                         : floor_modulo(scalar, source_values[i]))
          : 0.0;
    }
    return result;
  }
#pragma clang loop vectorize(enable)
  for (uint64_t i = 0; i < length; ++i) {
    presence[i] = source_presence[i];
    values[i] = list_on_left ? source_values[i] / scalar
                             : scalar / source_values[i];
  }
  return result;
}

static LanValue recursive_unary(LanValue value, uint32_t rank,
                                int32_t operation, int32_t* status) {
  if (value.tag == LAN_MISSING) return value;
  if (rank == 0) {
    if ((operation == 0 && value.tag != LAN_NUMBER) ||
        (operation == 1 && value.tag != LAN_BOOLEAN)) {
      *status = -2; return (LanValue){LAN_MISSING, 0, NULL};
    }
    value.number = operation == 0 ? -value.number : value.number == 0;
    return value;
  }
  if (value.tag != LAN_LIST || !value.pointer) {
    *status = -2; return (LanValue){LAN_MISSING, 0, NULL};
  }
  LanList* source = value.pointer;
  int32_t storage_tag = rank > 1 ? LAN_LIST : source->storage_tag;
  if (rank == 1 && storage_tag != LAN_NUMBER && storage_tag != LAN_BOOLEAN)
    storage_tag = LAN_MISSING;
  LanList* result = make_typed_list(source->length, storage_tag);
  for (uint64_t i = 0; i < source->length; ++i)
    store_list_value(result, i, recursive_unary(
        load_list_value(source, i), rank - 1, operation, status));
  return (LanValue){LAN_LIST, 0, result};
}

void* lan_list_unary(void* list_pointer, uint32_t rank,
                     int32_t operation, int32_t* status) {
  *status = 0;
  LanValue result = recursive_unary(
      (LanValue){LAN_LIST, 0, list_pointer}, rank, operation, status);
  return *status ? NULL : result.pointer;
}

void* lan_list_fill_missing(void* list_pointer, int32_t tag, double number,
                            void* pointer, int32_t* status) {
  LanList* list = list_pointer;
  *status = 0;
  if (!list || tag == LAN_MISSING) { *status = -1; return NULL; }
  LanValue replacement = {tag, number, pointer};
  LanList* result = make_typed_list(list->length, tag);
  int32_t observed = tag;
  for (uint64_t i = 0; i < list->length; ++i) {
    LanValue original = load_list_value(list, i);
    LanValue value = original.tag == LAN_MISSING ? replacement : original;
    if (value.tag != LAN_MISSING && observed != value.tag) {
      *status = -2; return NULL;
    }
    store_list_value(result, i, value);
  }
  return result;
}

static LanList* slice_list(LanList* source, uint64_t start, uint64_t length) {
  LanList* result = make_typed_list(length, source->storage_tag);
  for (uint64_t i = 0; i < length; ++i)
    store_list_value(result, i, load_list_value(source, start + i));
  return result;
}

void* lan_list_concat(void* left_pointer, void* right_pointer, int32_t* status) {
  LanList* left = left_pointer;
  LanList* right = right_pointer;
  diagnostic_error_index = -1;
  *status = 0;
  int32_t storage_tag = left->storage_tag == LAN_MISSING
      ? right->storage_tag : left->storage_tag;
  if (right->storage_tag != LAN_MISSING && storage_tag != right->storage_tag) {
    *status = -1;
    return NULL;
  }
  if (right->length > UINT64_MAX - left->length) abort();
  LanList* result = make_typed_list(left->length + right->length, storage_tag);
  for (uint64_t i = 0; i < left->length; ++i)
    store_list_value(result, i, load_list_value(left, i));
  for (uint64_t i = 0; i < right->length; ++i)
    store_list_value(result, left->length + i, load_list_value(right, i));
  return result;
}

void* lan_list_flatten(void* list_pointer, int32_t* status) {
  LanList* outer = list_pointer;
  diagnostic_error_index = -1;
  *status = 0;
  if (!outer || (outer->storage_tag != LAN_LIST &&
                 outer->storage_tag != LAN_MISSING)) {
    *status = -1;
    return NULL;
  }

  uint64_t length = 0;
  int32_t storage_tag = LAN_MISSING;
  for (uint64_t i = 0; i < outer->length; ++i) {
    LanValue value = load_list_value(outer, i);
    if (value.tag == LAN_MISSING) {
      diagnostic_error_index = (int64_t)i;
      *status = -2;
      return NULL;
    }
    if (value.tag != LAN_LIST) {
      diagnostic_error_index = (int64_t)i;
      *status = -1;
      return NULL;
    }
    LanList* inner = value.pointer;
    if (inner->length > UINT64_MAX - length) abort();
    length += inner->length;
    if (inner->storage_tag != LAN_MISSING) {
      if (storage_tag != LAN_MISSING && storage_tag != inner->storage_tag) {
        diagnostic_error_index = (int64_t)i;
        *status = -1;
        return NULL;
      }
      storage_tag = inner->storage_tag;
    }
  }

  LanList* result = make_typed_list(length, storage_tag);
  uint64_t output = 0;
  for (uint64_t i = 0; i < outer->length; ++i) {
    LanList* inner = load_list_value(outer, i).pointer;
    for (uint64_t j = 0; j < inner->length; ++j)
      store_list_value(result, output++, load_list_value(inner, j));
  }
  return result;
}

void* lan_list_reverse(void* list_pointer) {
  LanList* source = list_pointer;
  LanList* result = make_typed_list(source->length, source->storage_tag);
  for (uint64_t i = 0; i < source->length; ++i)
    store_list_value(result, i, load_list_value(source, source->length - i - 1));
  return result;
}

void* lan_list_slice(void* list_pointer, double amount, int32_t drop,
                     int32_t* status) {
  LanList* source = list_pointer;
  *status = 0;
  if (amount < 0 || amount > 9007199254740992.0 ||
      amount != (double)(uint64_t)amount) {
    *status = -1;
    return NULL;
  }
  uint64_t count = (uint64_t)amount;
  if (count > source->length) count = source->length;
  uint64_t start = drop ? count : 0;
  uint64_t length = drop ? source->length - count : count;
  return slice_list(source, start, length);
}

static LanList* filter_list(LanList* source, LanList* mask, uint64_t kept) {
  LanList* result = make_typed_list(kept, source->storage_tag);
  uint64_t output = 0;
  for (uint64_t i = 0; i < source->length; ++i)
    if (load_list_value(mask, i).tag == LAN_BOOLEAN &&
        load_list_value(mask, i).number != 0)
      store_list_value(result, output++, load_list_value(source, i));
  return result;
}

void* lan_list_filter(void* source_pointer, void* mask_pointer, int32_t* status) {
  LanList* source = source_pointer;
  LanList* mask = mask_pointer;
  *status = 0;
  if (!source || !mask || source->length != mask->length) {
    *status = -1; return NULL;
  }
  uint64_t kept = 0;
  for (uint64_t i = 0; i < mask->length; ++i) {
    LanValue value = load_list_value(mask, i);
    if (value.tag != LAN_MISSING && value.tag != LAN_BOOLEAN) {
      *status = -2; return NULL;
    }
    if (value.tag == LAN_BOOLEAN && value.number != 0) ++kept;
  }
  return filter_list(source, mask, kept);
}

static LanList* permute_list(LanList* source, const uint64_t* order) {
  LanList* result = make_typed_list(source->length, source->storage_tag);
  for (uint64_t i = 0; i < source->length; ++i)
    store_list_value(result, i, load_list_value(source, order[i]));
  return result;
}

static void* table_map_lists(void* record, uint64_t start, uint64_t length,
                             LanList* mask, uint64_t kept,
                             const uint64_t* order) {
  LanRecordNode* root = record;
  uint32_t count = entry_count(root);
  LanEntry* entries = count ? malloc(count * sizeof(*entries)) : NULL;
  uint32_t index = 0;
  if (count) collect_entries(root, entries, &index);
  void* result = lan_record_empty();
  for (uint32_t i = 0; i < count; ++i) {
    LanList* source = entries[i].value.pointer;
    LanList* column = mask ? filter_list(source, mask, kept)
                    : order ? permute_list(source, order)
                            : slice_list(source, start, length);
    void* updated = lan_record_assoc(result, entries[i].key, LAN_LIST, 0, column);
    lan_record_release(result);
    result = updated;
  }
  free(entries);
  return result;
}

void* lan_table_where(void* record, void* mask_pointer, int32_t* status) {
  LanList* mask = mask_pointer;
  uint64_t rows = lan_table_row_count(record);
  *status = 0;
  if (!mask || mask->length != rows) { *status = -1; return NULL; }
  uint64_t kept = 0;
  for (uint64_t i = 0; i < rows; ++i) {
    LanValue value = load_list_value(mask, i);
    if (value.tag != LAN_MISSING && value.tag != LAN_BOOLEAN) {
      *status = -2; return NULL;
    }
    if (value.tag == LAN_BOOLEAN && value.number != 0) ++kept;
  }
  return table_map_lists(record, 0, 0, mask, kept, NULL);
}

void* lan_table_select_columns(void* record, void* names_pointer, int32_t* status) {
  LanList* names = names_pointer;
  *status = 0;
  if (!names || names->length == 0) { *status = -1; return NULL; }
  void* result = lan_record_empty();
  for (uint64_t i = 0; i < names->length; ++i) {
    LanValue name = load_list_value(names, i);
    LanValue column;
    if (name.tag != LAN_TEXT || !lan_record_lookup(record, name.pointer, &column)) {
      *status = -2; return NULL;
    }
    void* updated = lan_record_assoc(result, name.pointer, column.tag,
                                     column.number, column.pointer);
    lan_record_release(result);
    result = updated;
  }
  return result;
}

void* lan_table_slice(void* record, double amount, int32_t drop, int32_t* status) {
  uint64_t rows = lan_table_row_count(record);
  *status = 0;
  /* Every integer through 2^53 is represented exactly by Number. Larger row
     counts are not constructible by this runtime and are rejected explicitly. */
  if (amount < 0 || amount > 9007199254740992.0 ||
      amount != (double)(uint64_t)amount) {
    *status = -1; return NULL;
  }
  uint64_t count = (uint64_t)amount;
  if (count > rows) count = rows;
  uint64_t start = drop ? count : 0;
  uint64_t length = drop ? rows - count : count;
  return table_map_lists(record, start, length, NULL, 0, NULL);
}

typedef struct { uint64_t index; LanValue value; } LanSortItem;

static int compare_sort_items(const void* left, const void* right) {
  const LanSortItem* a = left;
  const LanSortItem* b = right;
  int compared = 0;
  if (a->value.tag == LAN_MISSING || b->value.tag == LAN_MISSING) {
    if (a->value.tag != b->value.tag) compared = a->value.tag == LAN_MISSING ? 1 : -1;
  } else if (a->value.tag == LAN_NUMBER || a->value.tag == LAN_BOOLEAN) {
    compared = a->value.number < b->value.number ? -1
             : a->value.number > b->value.number ? 1 : 0;
  } else if (a->value.tag == LAN_TEXT) {
    compared = strcmp(a->value.pointer, b->value.pointer);
  }
  if (compared) return compared;
  return a->index < b->index ? -1 : a->index > b->index;
}

void* lan_list_sort(void* list_pointer, int32_t* status) {
  LanList* list = list_pointer;
  *status = 0;
  if (list->storage_tag != LAN_NUMBER && list->storage_tag != LAN_TEXT) {
    *status = -1;
    return NULL;
  }
  LanSortItem* items = list->length ? malloc(list->length * sizeof(*items)) : NULL;
  uint64_t* order = list->length ? malloc(list->length * sizeof(*order)) : NULL;
  if (list->length && (!items || !order)) abort();
  for (uint64_t i = 0; i < list->length; ++i)
    items[i] = (LanSortItem){i, load_list_value(list, i)};
  qsort(items, list->length, sizeof(*items), compare_sort_items);
  for (uint64_t i = 0; i < list->length; ++i) order[i] = items[i].index;
  LanList* result = permute_list(list, order);
  free(items);
  free(order);
  return result;
}

void* lan_table_sort_by(void* record, const char* key, int32_t* status) {
  LanValue column;
  *status = 0;
  if (!lan_record_lookup(record, key, &column) || column.tag != LAN_LIST) {
    *status = -1; return NULL;
  }
  LanList* list = column.pointer;
  LanSortItem* items = list->length ? malloc(list->length * sizeof(*items)) : NULL;
  uint64_t* order = list->length ? malloc(list->length * sizeof(*order)) : NULL;
  if (list->length && (!items || !order)) abort();
  int32_t observed = LAN_MISSING;
  for (uint64_t i = 0; i < list->length; ++i) {
    LanValue value = load_list_value(list, i);
    if (value.tag != LAN_MISSING) {
      if (value.tag != LAN_NUMBER && value.tag != LAN_BOOLEAN && value.tag != LAN_TEXT) {
        *status = -2; free(items); free(order); return NULL;
      }
      if (observed != LAN_MISSING && observed != value.tag) {
        *status = -2; free(items); free(order); return NULL;
      }
      observed = value.tag;
    }
    items[i] = (LanSortItem){i, value};
  }
  qsort(items, list->length, sizeof(*items), compare_sort_items);
  for (uint64_t i = 0; i < list->length; ++i) order[i] = items[i].index;
  void* result = table_map_lists(record, 0, 0, NULL, 0, order);
  free(items);
  free(order);
  return result;
}

static void print_value(LanValue value);

typedef struct {
  char* data;
  size_t length;
  size_t capacity;
} LanTextBuilder;

static void text_builder_reserve(LanTextBuilder* builder, size_t added) {
  if (added > SIZE_MAX - builder->length - 1) abort();
  size_t needed = builder->length + added + 1;
  if (needed <= builder->capacity) return;
  size_t capacity = builder->capacity ? builder->capacity : 32;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
    capacity *= 2;
  }
  char* data = realloc(builder->data, capacity);
  if (!data) abort();
  builder->data = data;
  builder->capacity = capacity;
}

static void text_builder_append_bytes(LanTextBuilder* builder,
                                      const char* text, size_t length) {
  text_builder_reserve(builder, length);
  memcpy(builder->data + builder->length, text, length);
  builder->length += length;
  builder->data[builder->length] = '\0';
}

static void text_builder_append(LanTextBuilder* builder, const char* text) {
  text_builder_append_bytes(builder, text, strlen(text));
}

static void format_value(LanTextBuilder* builder, LanValue value, int32_t nested) {
  if (value.tag == LAN_MISSING) {
    text_builder_append(builder, "missing");
  } else if (value.tag == LAN_NUMBER) {
    char number[64];
    int length = snprintf(number, sizeof(number), "%.17g", value.number);
    if (length < 0 || (size_t)length >= sizeof(number)) abort();
    text_builder_append_bytes(builder, number, (size_t)length);
  } else if (value.tag == LAN_BOOLEAN) {
    text_builder_append(builder, value.number != 0 ? "true" : "false");
  } else if (value.tag == LAN_TEXT) {
    if (nested) text_builder_append(builder, "\"");
    text_builder_append(builder, value.pointer);
    if (nested) text_builder_append(builder, "\"");
  } else if (value.tag == LAN_LIST) {
    LanList* list = value.pointer;
    text_builder_append(builder, "[");
    for (uint64_t i = 0; i < list->length; ++i) {
      if (i) text_builder_append(builder, ", ");
      format_value(builder, load_list_value(list, i), 1);
    }
    text_builder_append(builder, "]");
  } else if (value.tag == LAN_RECORD) {
    LanRecordNode* record = value.pointer;
    uint32_t count = entry_count(record);
    LanEntry* entries = count ? malloc(count * sizeof(*entries)) : NULL;
    uint32_t index = 0;
    if (count) collect_entries(record, entries, &index);
    qsort(entries, count, sizeof(*entries), compare_entries);
    text_builder_append(builder, "{");
    for (uint32_t i = 0; i < count; ++i) {
      if (i) text_builder_append(builder, ", ");
      text_builder_append(builder, entries[i].key);
      text_builder_append(builder, ": ");
      format_value(builder, entries[i].value, 1);
    }
    text_builder_append(builder, "}");
    free(entries);
  }
}

char* lan_value_to_text(int32_t tag, double number, void* pointer) {
  if (tag == LAN_TEXT && pointer) return lan_text_from_cstr(pointer);
  LanTextBuilder builder = {NULL, 0, 0};
  format_value(&builder, (LanValue){tag, number, pointer}, 0);
  if (!builder.data) return lan_text_from_cstr("");
  char* result = lan_text_from_cstr(builder.data);
  free(builder.data);
  return result;
}

void lan_record_print(void* record) {
  LanRecordNode* root = record;
  uint32_t count = entry_count(root);
  LanEntry* entries = count ? malloc(count * sizeof(*entries)) : NULL;
  uint32_t index = 0;
  if (count) collect_entries(root, entries, &index);
  qsort(entries, count, sizeof(*entries), compare_entries);
  printf("{");
  for (uint32_t i = 0; i < count; ++i) {
    if (i) printf(", ");
    printf("%s: ", entries[i].key);
    print_value(entries[i].value);
  }
  printf("}");
  free(entries);
}

static void print_value(LanValue value) {
  if (value.tag == LAN_MISSING) printf("missing");
  else if (value.tag == LAN_NUMBER) printf("%g", value.number);
  else if (value.tag == LAN_BOOLEAN) printf(value.number != 0 ? "true" : "false");
  else if (value.tag == LAN_TEXT) printf("\"%s\"", (char*)value.pointer);
  else if (value.tag == LAN_RECORD) lan_record_print(value.pointer);
  else if (value.tag == LAN_LIST) {
    LanList* list = value.pointer;
    printf("[");
    for (uint64_t i = 0; i < list->length; ++i) {
      if (i) printf(", ");
      print_value(load_list_value(list, i));
    }
    printf("]");
  }
}

void lan_value_print(int32_t tag, double number, void* pointer) {
  print_value((LanValue){tag, number, pointer});
}
