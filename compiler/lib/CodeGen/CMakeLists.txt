# LLVM and LLD API use lives here (D0.2).
add_gopp_library(CodeGen Target.cpp Linker.cpp Emit.cpp LowerToLLVM.cpp
  LLVM_COMPONENTS support core target mc x86 aarch64 targetparser
  PCH <llvm/IR/IRBuilder.h> <llvm/IR/Module.h> <llvm/IR/LLVMContext.h>
  LLD_LIBS lldCommon lldELF
  LINK_LIBS goppGIR goppBuild)
