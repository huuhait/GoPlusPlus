// The C side of field_tables.gopp: it reads the descriptor of the value
// that an interface value holds and the table of its fields, as the
// header of the runtime has them, and each field of the value where the
// table says that it is. It writes what it finds as text.
#include <stdbool.h>
#include <stdint.h>

typedef struct text {
  const uint8_t *bytes;
  uint64_t len;
  const uint8_t *backing;
} text;

typedef struct type {
  const struct type *type;
  uint64_t size;
  uint64_t flags;
  void *clone;
  void *drop;
  void *eq;
  void *hash;
  text name;
  const void *methods;
  uint64_t kind;
  const struct type *elem;
  const struct type *key;
  uint64_t count;
  uint64_t fields;
} type;

typedef struct field {
  text name;
  text tag;
  const type *type;
  uint64_t offset;
  uint64_t flags;
} field;

typedef struct any {
  const type *type;
  const uint8_t *data;
} any;

typedef struct out {
  uint8_t *at;
  int64_t left;
  int64_t wrote;
} out;

static void put(out *to, const uint8_t *bytes, uint64_t len) {
  for (uint64_t i = 0; i < len && to->left > 0; i++) {
    *to->at++ = bytes[i];
    to->left--;
    to->wrote++;
  }
}

static void say(out *to, const char *words) {
  uint64_t len = 0;
  while (words[len] != 0) {
    len++;
  }
  put(to, (const uint8_t *)words, len);
}

static void number(out *to, int64_t value) {
  uint8_t digits[24];
  int at = 24;
  uint64_t rest = value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
  do {
    digits[--at] = (uint8_t)('0' + rest % 10);
    rest /= 10;
  } while (rest != 0);
  if (value < 0) {
    digits[--at] = '-';
  }
  put(to, digits + at, (uint64_t)(24 - at));
}

static void name_of(out *to, const type *of) {
  if (of == 0) {
    say(to, "?");
    return;
  }
  put(to, of->name.bytes, of->name.len);
}

// What a type is made of, by its kind.
static void made_of(out *to, const type *of) {
  switch (of->kind) {
  case 6:
    say(to, "a struct with ");
    number(to, (int64_t)of->fields);
    say(to, " fields");
    break;
  case 7:
    say(to, "a slice of ");
    name_of(to, of->elem);
    break;
  case 8:
    say(to, "an array of ");
    number(to, (int64_t)of->count);
    say(to, " ");
    name_of(to, of->elem);
    break;
  case 9:
    say(to, "a map of ");
    name_of(to, of->key);
    say(to, " to ");
    name_of(to, of->elem);
    break;
  case 11:
    say(to, "an interface");
    break;
  case 12:
    say(to, "a function");
    break;
  case 13:
    say(to, "an option of ");
    name_of(to, of->elem);
    break;
  case 14:
    say(to, "a unique ");
    name_of(to, of->elem);
    break;
  default:
    say(to, "kind ");
    number(to, (int64_t)of->kind);
  }
}

// The value at the address, for the kinds whose bytes say it.
static void value_at(out *to, const type *of, const uint8_t *at) {
  switch (of->kind) {
  case 1:
    say(to, *(const bool *)at ? "true" : "false");
    break;
  case 2:
    switch (of->size) {
    case 1:
      number(to, *(const int8_t *)at);
      break;
    case 2:
      number(to, *(const int16_t *)at);
      break;
    case 4:
      number(to, *(const int32_t *)at);
      break;
    default:
      number(to, *(const int64_t *)at);
    }
    break;
  case 3:
    switch (of->size) {
    case 1:
      number(to, *(const uint8_t *)at);
      break;
    case 2:
      number(to, *(const uint16_t *)at);
      break;
    case 4:
      number(to, *(const uint32_t *)at);
      break;
    default:
      number(to, (int64_t) * (const uint64_t *)at);
    }
    break;
  case 4:
    // In hundredths.
    number(to, of->size == 4 ? (int64_t)(*(const float *)at * 100)
                              : (int64_t)(*(const double *)at * 100));
    say(to, "/100");
    break;
  case 5: {
    const text *held = (const text *)at;
    say(to, "\"");
    put(to, held->bytes, held->len);
    say(to, "\"");
    break;
  }
  // A slice says how many elements it has in its second word.
  case 7:
    made_of(to, of);
    say(to, ", ");
    number(to, (int64_t)((const uint64_t *)at)[1]);
    say(to, " long");
    break;
  default:
    made_of(to, of);
  }
}

int64_t c_describe(const any *value, uint8_t *room, int64_t size) {
  out to = {room, size, 0};
  const type *of = value->type;
  // A value of no more than a word that owns nothing is the word.
  const uint8_t *data = (of->flags & 1) != 0 ? (const uint8_t *)&value->data : value->data;
  name_of(&to, of);
  say(&to, ": ");
  number(&to, (int64_t)of->size);
  say(&to, " bytes, ");
  if (of->kind != 6) {
    value_at(&to, of, data);
    say(&to, "\n");
    return to.wrote;
  }
  made_of(&to, of);
  say(&to, "\n");
  // The table follows the descriptor.
  const field *fields = (const field *)(of + 1);
  for (uint64_t at = 0; at < of->fields; at++) {
    const field *one = &fields[at];
    say(&to, "  ");
    put(&to, one->name.bytes, one->name.len);
    say(&to, " ");
    name_of(&to, one->type);
    say(&to, " at ");
    number(&to, (int64_t)one->offset);
    if (one->type != 0) {
      say(&to, " = ");
      value_at(&to, one->type, data + one->offset);
    }
    if (one->tag.len != 0) {
      say(&to, " `");
      put(&to, one->tag.bytes, one->tag.len);
      say(&to, "`");
    }
    if ((one->flags & 1) != 0) {
      say(&to, " required");
    }
    say(&to, "\n");
  }
  return to.wrote;
}
