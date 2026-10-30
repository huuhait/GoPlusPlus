# Clang-based importer (P07/P08) and the C ABI classifier (P04.11, CABI.cpp).
# Clang API use lives here (D0.2).
add_gopp_library(Import ClangInfo.cpp CABI.cpp
  LLVM_COMPONENTS support core
  CLANG_LIBS clangBasic clangAST clangCodeGen clangFrontend clangLex
  LINK_LIBS goppGIR
  PCH <clang/AST/ASTContext.h> <clang/Frontend/CompilerInstance.h>)
