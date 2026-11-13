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
