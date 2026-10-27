add_gopp_library(Parse Parser.cpp
  LLVM_COMPONENTS support
  LINK_LIBS goppAST goppLex goppBasic)
