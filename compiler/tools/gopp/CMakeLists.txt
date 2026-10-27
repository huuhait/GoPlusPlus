add_gopp_tool(gopp gopp.cpp
  LLVM_COMPONENTS support
  LINK_LIBS goppBasic goppLex goppAST goppParse goppBuild goppCodeGen goppImport)
