add_gopp_library(Format Printer.cpp TabWriter.cpp Format.cpp Diff.cpp
  LLVM_COMPONENTS support
  LINK_LIBS goppParse goppAST goppLex goppBasic)
