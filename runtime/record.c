#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct LanRecordNode LanRecordNode;

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
  if (node && node->references != UINT32_MAX) ++node->references;
}

static void retain_value(LanValue value) {
  if (value.tag == LAN_RECORD) lan_record_retain(value.pointer);
}

static void release_value(LanValue value);

void lan_record_release(void* pointer) {
  LanRecordNode* node = pointer;
  if (!node || node->references == UINT32_MAX || --node->references != 0) return;
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
}

static void release_value(LanValue value) {
  if (value.tag == LAN_RECORD) lan_record_release(value.pointer);
}

static LanRecordNode* allocate_node(uint32_t kind) {
  LanRecordNode* node = calloc(1, sizeof(*node));
  if (!node) abort();
  node->references = 1;
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

void* lan_record_empty(void) { return &empty_record; }

void* lan_record_assoc(void* record, const char* key, int32_t tag,
                       double number, void* pointer) {
  LanValue value = {tag, number, pointer};
  return assoc(record, hash_text(key), key, value, 0);
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

static void print_value(LanValue value);

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

typedef struct { uint64_t length; LanValue* elements; } LanList;

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
      print_value(list->elements[i]);
    }
    printf("]");
  }
}

void lan_value_print(int32_t tag, double number, void* pointer) {
  print_value((LanValue){tag, number, pointer});
}
