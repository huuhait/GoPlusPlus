# LLVM and LLD API use lives here (D0.2). Target components are spelled as
# LLVM names the targets (X86, AArch64: case matters, gopp_llvm_libs checks).
add_gopp_library(CodeGen Target.cpp Linker.cpp Emit.cpp LowerToLLVM.cpp CABILower.cpp
  Pipeline.cpp
  LLVM_COMPONENTS support core target mc X86 AArch64 targetparser passes analysis
  PCH <llvm/IR/IRBuilder.h> <llvm/IR/Module.h> <llvm/IR/LLVMContext.h>
  LLD_LIBS lldCommon lldELF
  LINK_LIBS goppGIR goppBuild goppImport)
