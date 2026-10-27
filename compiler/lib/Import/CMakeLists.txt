# Clang-based importer (P07/P08). Clang API use lives here (D0.2).
add_gopp_library(Import ClangInfo.cpp
  LLVM_COMPONENTS support
  CLANG_LIBS clangBasic
  PCH <clang/AST/ASTContext.h> <clang/Frontend/CompilerInstance.h>)
