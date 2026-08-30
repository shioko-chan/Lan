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

typedef struct { uint64_t length; LanValue* elements; } LanList;

static int32_t same_value(LanValue left, LanValue right);

static int32_t same_list(LanList* left, LanList* right) {
  if (left == right) return 1;
  if (!left || !right || left->length != right->length) return 0;
  for (uint64_t i = 0; i < left->length; ++i)
    if (!same_value(left->elements[i], right->elements[i])) return 0;
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

static LanList* make_list(uint64_t length) {
  LanList* list = malloc(sizeof(*list));
  if (!list) abort();
  list->length = length;
  list->elements = length ? calloc(length, sizeof(*list->elements)) : NULL;
  if (length && !list->elements) abort();
  return list;
}

static void store_list_value(LanList* list, uint64_t index, LanValue value) {
  list->elements[index] = value;
  retain_value(value);
}

int32_t lan_list_lookup(void* list_pointer, double index, LanValue* output) {
  LanList* list = list_pointer;
  if (!list || index < 0 || index > 9007199254740992.0) return 0;
  uint64_t integer = (uint64_t)index;
  if ((double)integer != index || integer >= list->length) return 0;
  *output = list->elements[integer];
  return 1;
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
    LanValue value = column->elements[row];
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
  *output = list->elements[row];
  return 1;
}

void* lan_table_from_rows(void* rows_pointer, int32_t* status) {
  LanList* rows = rows_pointer;
  *status = 0;
  if (!rows) { *status = -1; return NULL; }
  if (rows->length == 0) return lan_record_empty();
  if (rows->elements[0].tag != LAN_RECORD || !rows->elements[0].pointer) {
    *status = -2; return NULL;
  }

  LanRecordNode* first = rows->elements[0].pointer;
  uint32_t count = entry_count(first);
  LanEntry* schema = count ? malloc(count * sizeof(*schema)) : NULL;
  uint32_t schema_index = 0;
  if (count) collect_entries(first, schema, &schema_index);
  void* result = lan_record_empty();

  for (uint32_t field = 0; field < count; ++field) {
    LanList* column = make_list(rows->length);
    int32_t observed_tag = LAN_MISSING;
    for (uint64_t row = 0; row < rows->length; ++row) {
      LanValue row_value = rows->elements[row];
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

/* operation: 0 sum, 1 average, 2 minimum, 3 maximum, 4 present count.
   Returns -3 for a non-number numeric aggregate, 0 when no number exists,
   and 1 on success. */
static int32_t aggregate_list(LanList* list, int32_t operation, double* output) {
  double result = 0.0;
  uint64_t present = 0;
  for (uint64_t i = 0; i < list->length; ++i) {
    LanValue value = list->elements[i];
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
  if (operation <= 3) {
    if (operation == 0 && a.tag == LAN_TEXT && b.tag == LAN_TEXT) {
      size_t left = strlen(a.pointer);
      size_t right = strlen(b.pointer);
      char* joined = malloc(left + right + 1);
      if (!joined) abort();
      memcpy(joined, a.pointer, left);
      memcpy(joined + left, b.pointer, right + 1);
      return (LanValue){LAN_TEXT, 0, joined};
    }
    if (a.tag != LAN_NUMBER || b.tag != LAN_NUMBER) {
      *status = -3;
      return (LanValue){LAN_MISSING, 0, NULL};
    }
    value.number = operation == 0 ? a.number + b.number
                 : operation == 1 ? a.number - b.number
                 : operation == 2 ? a.number * b.number : a.number / b.number;
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
    LanList* result = make_list(source->length);
    for (uint64_t i = 0; i < source->length; ++i)
      store_list_value(result, i, recursive_binary(
          source->elements[i], a_rank - 1, b, b_rank, operation, status));
    return (LanValue){LAN_LIST, 0, result};
  }
  if (b_rank > a_rank) {
    if (b.tag == LAN_MISSING) return b;
    if (b.tag != LAN_LIST || !b.pointer) {
      *status = -3; return (LanValue){LAN_MISSING, 0, NULL};
    }
    LanList* source = b.pointer;
    LanList* result = make_list(source->length);
    for (uint64_t i = 0; i < source->length; ++i)
      store_list_value(result, i, recursive_binary(
          a, a_rank, source->elements[i], b_rank - 1, operation, status));
    return (LanValue){LAN_LIST, 0, result};
  }

  /* Equal non-zero ranks. A missing sub-list adopts the other operand's
     shape, except for ?? which was handled above. */
  if (a.tag == LAN_MISSING && b.tag != LAN_MISSING) {
    if (b.tag != LAN_LIST || !b.pointer) {
      *status = -3; return (LanValue){LAN_MISSING, 0, NULL};
    }
    LanList* source = b.pointer;
    LanList* result = make_list(source->length);
    for (uint64_t i = 0; i < source->length; ++i)
      store_list_value(result, i, recursive_binary(
          a, a_rank - 1, source->elements[i], b_rank - 1, operation, status));
    return (LanValue){LAN_LIST, 0, result};
  }
  if (b.tag == LAN_MISSING && a.tag != LAN_MISSING) {
    if (a.tag != LAN_LIST || !a.pointer) {
      *status = -3; return (LanValue){LAN_MISSING, 0, NULL};
    }
    LanList* source = a.pointer;
    LanList* result = make_list(source->length);
    for (uint64_t i = 0; i < source->length; ++i)
      store_list_value(result, i, recursive_binary(
          source->elements[i], a_rank - 1, b, b_rank - 1, operation, status));
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
  LanList* result = make_list(length);
  for (uint64_t i = 0; i < length; ++i)
    store_list_value(result, i, recursive_binary(
        left->elements[i], a_rank - 1, right->elements[i], b_rank - 1,
        operation, status));
  return (LanValue){LAN_LIST, 0, result};
}

void* lan_list_binary(int32_t a_tag, double a_number, void* a_pointer,
                      uint32_t a_rank, int32_t b_tag, double b_number,
                      void* b_pointer, uint32_t b_rank,
                      int32_t operation, int32_t* status) {
  *status = 0;
  LanValue result = recursive_binary(
      (LanValue){a_tag, a_number, a_pointer}, a_rank,
      (LanValue){b_tag, b_number, b_pointer}, b_rank, operation, status);
  if (*status || result.tag != LAN_LIST) return NULL;
  return result.pointer;
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
  LanList* result = make_list(source->length);
  for (uint64_t i = 0; i < source->length; ++i)
    store_list_value(result, i, recursive_unary(
        source->elements[i], rank - 1, operation, status));
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
  LanList* result = make_list(list->length);
  int32_t observed = tag;
  for (uint64_t i = 0; i < list->length; ++i) {
    LanValue value = list->elements[i].tag == LAN_MISSING
        ? replacement : list->elements[i];
    if (value.tag != LAN_MISSING && observed != value.tag) {
      *status = -2; return NULL;
    }
    store_list_value(result, i, value);
  }
  return result;
}

static LanList* slice_list(LanList* source, uint64_t start, uint64_t length) {
  LanList* result = make_list(length);
  for (uint64_t i = 0; i < length; ++i)
    store_list_value(result, i, source->elements[start + i]);
  return result;
}

static LanList* filter_list(LanList* source, LanList* mask, uint64_t kept) {
  LanList* result = make_list(kept);
  uint64_t output = 0;
  for (uint64_t i = 0; i < source->length; ++i)
    if (mask->elements[i].tag == LAN_BOOLEAN && mask->elements[i].number != 0)
      store_list_value(result, output++, source->elements[i]);
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
    LanValue value = mask->elements[i];
    if (value.tag != LAN_MISSING && value.tag != LAN_BOOLEAN) {
      *status = -2; return NULL;
    }
    if (value.tag == LAN_BOOLEAN && value.number != 0) ++kept;
  }
  return filter_list(source, mask, kept);
}

static LanList* permute_list(LanList* source, const uint64_t* order) {
  LanList* result = make_list(source->length);
  for (uint64_t i = 0; i < source->length; ++i)
    store_list_value(result, i, source->elements[order[i]]);
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
    LanValue value = mask->elements[i];
    if (value.tag != LAN_MISSING && value.tag != LAN_BOOLEAN) {
      *status = -2; return NULL;
    }
    if (value.tag == LAN_BOOLEAN && value.number != 0) ++kept;
  }
  return table_map_lists(record, 0, 0, mask, kept, NULL);
}

void* lan_table_update_column(void* record, const char* key, void* list_pointer,
                              int32_t replace, int32_t* status) {
  LanList* list = list_pointer;
  uint64_t rows = 0;
  int32_t has_column = 0;
  LanValue existing;
  *status = 0;
  if (!list || !validate_table_node(record, &rows, &has_column) ||
      (has_column && list->length != rows)) {
    *status = -1; return NULL;
  }
  int32_t exists = lan_record_lookup(record, key, &existing);
  if ((!replace && exists) || (replace && !exists)) {
    *status = -2; return NULL;
  }
  return lan_record_assoc(record, key, LAN_LIST, 0, list);
}

void* lan_table_select_columns(void* record, void* names_pointer, int32_t* status) {
  LanList* names = names_pointer;
  *status = 0;
  if (!names || names->length == 0) { *status = -1; return NULL; }
  void* result = lan_record_empty();
  for (uint64_t i = 0; i < names->length; ++i) {
    LanValue name = names->elements[i];
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
    LanValue value = list->elements[i];
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
      print_value(list->elements[i]);
    }
    printf("]");
  }
}

void lan_value_print(int32_t tag, double number, void* pointer) {
  print_value((LanValue){tag, number, pointer});
}
