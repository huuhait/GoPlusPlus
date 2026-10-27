# LLVM and LLD API use lives here (D0.2).
add_gopp_library(CodeGen Target.cpp Linker.cpp
  LLVM_COMPONENTS support targetparser
  PCH <llvm/IR/IRBuilder.h> <llvm/IR/Module.h> <llvm/IR/LLVMContext.h>
  LLD_LIBS lldCommon)
