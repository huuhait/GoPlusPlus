// Function values (CLO-1 to CLO-9): two words, the code and the
// environment that the code is given. A function without captured
// variables has no environment: the word is null. An environment starts
// with a count of the function values that share it and has the cells
// of the variables that the function captured; the last value that
// goes lets the cells go and frees it. An environment in the frame of
// a function is never counted or freed.
//
// A captured variable lives in a cell: a count of what shares it, the
// function around it and every environment that has it, and then the
// variable. The last that lets it go ends the variable and frees it.

#include "internal.h"

void free(void *room);

typedef struct gopp_func {
  uint8_t *code;
  gopp_env *env;
} gopp_func;

// The function value at the address becomes a copy of its own: one more
// value shares the environment.
void _gopp_func_clone(uint8_t *value) {
  gopp_env *env = ((gopp_func *)value)->env;
  if (env != 0 && env->count != GOPP_ENV_IN_FRAME) {
    __atomic_fetch_add(&env->count, 1, __ATOMIC_RELAXED);
  }
}

// The function value at the address ends: the last one that shares the
// environment ends what it holds and frees it.
void _gopp_func_drop(uint8_t *value) {
  gopp_env *env = ((gopp_func *)value)->env;
  if (env == 0 || env->count == GOPP_ENV_IN_FRAME) {
    return;
  }
  if (__atomic_fetch_sub(&env->count, 1, __ATOMIC_ACQ_REL) == 1) {
    for (uint64_t at = 0; at < env->cells; at++) {
      _gopp_cell_release(env->all[at].cell, env->all[at].drop);
    }
    free(env);
  }
}

// An environment for so many cells, which one function value has.
uint8_t *_gopp_env_new(uint64_t cells) {
  uint64_t size = sizeof(gopp_env) + cells * sizeof(gopp_captured);
  gopp_env *env = _gopp_alloc(size);
  __builtin_memset(env, 0, size);
  env->count = 1;
  env->cells = cells;
  return (uint8_t *)env;
}

// A cell of `size` bytes with its count, all zero but the count: the
// function that declares the variable has it.
uint8_t *_gopp_cell_new(uint64_t size) {
  uint64_t *cell = _gopp_alloc(size);
  __builtin_memset(cell, 0, size);
  *cell = 1;
  return (uint8_t *)cell;
}

// One more environment has the cell.
void _gopp_cell_retain(uint8_t *cell) {
  __atomic_fetch_add((uint64_t *)cell, 1, __ATOMIC_RELAXED);
}

// What the list of the variables that `defer` statements borrowed
// keeps of one of them, in its cell after the variable (PAN-3): the
// variable that was declared before it, the number of the variable
// among those that were declared, and the cell with what ends the
// variable, once a statement borrowed it.
typedef struct gopp_extended {
  struct gopp_extended *next;
  uint64_t number;
  uint8_t *cell;
  void (*drop)(uint8_t *value);
} gopp_extended;

// A `defer` statement that runs borrows the variable of the cell: the
// list has the cell too from here on, once, so the variable lives until
// the list ends. The list has the variable that was declared last
// first.
void _gopp_extend(uint8_t *list, uint8_t *kept, uint8_t *cell, void (*drop)(uint8_t *value)) {
  gopp_extended *extended = (gopp_extended *)kept;
  if (extended->cell != 0) {
    return;
  }
  extended->cell = cell;
  extended->drop = drop;
  _gopp_cell_retain(cell);
  gopp_extended **at = (gopp_extended **)list;
  while (*at != 0 && (*at)->number > extended->number) {
    at = &(*at)->next;
  }
  extended->next = *at;
  *at = extended;
}

// The function is left, and its deferred calls are made: the list lets
// every variable go, the one that was declared last first (PAN-4).
void _gopp_extended_end(uint8_t *list) {
  gopp_extended **first = (gopp_extended **)list;
  while (*first != 0) {
    gopp_extended *extended = *first;
    *first = extended->next;
    _gopp_cell_release(extended->cell, extended->drop);
  }
}

// One less has the cell: the last ends the variable by `drop`, if its
// bytes are not all there is to it, and frees the cell. A cell that was
// never made is null.
void _gopp_cell_release(uint8_t *cell, void (*drop)(uint8_t *value)) {
  if (cell != 0 && __atomic_fetch_sub((uint64_t *)cell, 1, __ATOMIC_ACQ_REL) == 1) {
    if (drop != 0) {
      drop(cell + sizeof(uint64_t));
    }
    free(cell);
  }
}

// A range over a function (RNG-6a). The cell of the loop has what the
// body of the loop is given: the state of the loop, and the frame of
// the function with the loop, which is read only while the loop runs.
typedef struct gopp_ranging {
  uint64_t count;
  uint64_t state;
  uint8_t *frame;
} gopp_ranging;

// The function that walks may call the body.
#define RANGE_READY 0
// The body runs; it stays so if the body ends in a panic.
#define RANGE_BODY 1
// The body said that the loop is over.
#define RANGE_DONE 2
// The loop was left.
#define RANGE_LEFT 3

// The body of the loop is called: only a loop that is ready runs it,
// as in Go.
void _gopp_range_enter(uint8_t *loop) {
  gopp_ranging *ranging = (gopp_ranging *)loop;
  switch (ranging->state) {
  case RANGE_READY:
    ranging->state = RANGE_BODY;
    return;
  case RANGE_BODY:
    _gopp_fail("runtime error: range function continued iteration after loop body panic");
  case RANGE_DONE:
    _gopp_fail("runtime error: range function continued iteration after function for loop body "
               "returned false");
  default:
    _gopp_fail("runtime error: range function continued iteration after whole loop exit");
  }
}

// The function that walks returned.
void _gopp_range_exit(uint8_t *loop) {
  gopp_ranging *ranging = (gopp_ranging *)loop;
  if (ranging->state == RANGE_BODY) {
    _gopp_fail("runtime error: range function recovered a loop body panic and did not resume "
               "panicking");
  }
  ranging->state = RANGE_LEFT;
}

// The loop is left, by any way: `slot` has its cell, which the function
// lets go. A function value of the body that is still kept has it too,
// and is told so when it is called.
void _gopp_range_end(uint8_t *slot) {
  gopp_ranging *ranging = *(gopp_ranging **)slot;
  ranging->state = RANGE_LEFT;
  _gopp_cell_release((uint8_t *)ranging, 0);
}
