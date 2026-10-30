//===- NewStack.cpp - Deep recursion on stacks of their own
//----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/NewStack.h"

#include "llvm/Support/ProgramStack.h"

#include <atomic>
#include <cstdint>
#include <pthread.h>

using namespace gopp;

namespace {
struct StackJob {
  llvm::function_ref<void()> Fn;
  static void *entry(void *Self) {
    static_cast<StackJob *>(Self)->Fn();
    return nullptr;
  }
};
} // namespace

static std::atomic<long> Budget{-1};
static std::atomic<unsigned> SegmentSetting{0};
static std::atomic<unsigned long> Threads{0};

unsigned long gopp::newStackThreads() { return Threads.load(); }
void gopp::noteNewStackThread() { ++Threads; }

namespace {
/// The lowest usable address of this thread's stack (0: not known).
struct StackBase {
  uintptr_t Lo = 0;
  StackBase() {
    pthread_attr_t A;
    if (pthread_getattr_np(pthread_self(), &A) != 0)
      return;
    void *Addr = nullptr;
    size_t Size = 0;
    if (pthread_attr_getstack(&A, &Addr, &Size) == 0)
      Lo = reinterpret_cast<uintptr_t>(Addr);
    pthread_attr_destroy(&A);
  }
};
} // namespace

bool gopp::stackLow(size_t Reserve) {
  static thread_local StackBase Base;
  if (!Base.Lo)
    return false;
  uintptr_t Sp = llvm::getStackPointer();
  return Sp < Base.Lo + Reserve;
}

unsigned gopp::setNewStackSegment(unsigned Levels) {
  return SegmentSetting.exchange(Levels);
}

unsigned gopp::newStackSegment() {
  return SegmentSetting.load(std::memory_order_relaxed);
}

long gopp::failNewStacksAfter(long N) { return Budget.exchange(N); }

bool gopp::newStackAllowed() {
  long B = Budget.load();
  while (B >= 0) {
    if (B == 0)
      return false;
    if (Budget.compare_exchange_weak(B, B - 1))
      return true;
  }
  return true;
}

bool gopp::runOnNewStack(llvm::function_ref<void()> Fn, size_t Bytes) {
  if (!newStackAllowed())
    return false;
  StackJob Job{Fn};
  pthread_attr_t Attr;
  pthread_t Id;
  bool Made = pthread_attr_init(&Attr) == 0;
  if (Made) {
    Made = pthread_attr_setstacksize(&Attr, Bytes) == 0 &&
           pthread_create(&Id, &Attr, &StackJob::entry, &Job) == 0;
    pthread_attr_destroy(&Attr);
  }
  if (!Made)
    return false;
  ++Threads;
  pthread_join(Id, nullptr);
  return true;
}

void gopp::runOnNewStackOrHere(llvm::function_ref<void()> Fn, size_t Bytes) {
  if (!runOnNewStack(Fn, Bytes))
    Fn();
}
