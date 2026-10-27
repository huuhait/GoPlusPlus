add_gopp_library(AST ASTDumper.cpp
  LLVM_COMPONENTS support
  LINK_LIBS goppBasic goppLex)
