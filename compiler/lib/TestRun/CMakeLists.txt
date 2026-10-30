# The fast test runner (P04.19, D4.14): batch compile driver, executor pool
# and result protocol. LLJIT lives here and nowhere else.
add_gopp_library(TestRun Protocol.cpp BatchCompile.cpp ExecutorPool.cpp
  Executor.cpp Runner.cpp
  LLVM_COMPONENTS support core object target orcjit native
  LINK_LIBS goppSema goppGIR goppCodeGen goppBasic)
