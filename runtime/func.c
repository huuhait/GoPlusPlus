// Function values (CLO-1 to CLO-9): two words, the code and the
// environment that the code is given. A function without captured
// variables has no environment: the word is null. An environment starts
// with a count of the function values that share it and the function
// that ends what it holds; the last value that goes frees it. An
// environment in the frame of a function is never counted or freed.

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
    if (env->drop != 0) {
      env->drop((uint8_t *)env);
    }
    free(env);
  }
}
