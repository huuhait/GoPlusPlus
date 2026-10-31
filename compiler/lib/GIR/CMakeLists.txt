# GIR (P04.01): types, the IR, builder, verifier, printer, bitstream form. No
# LLVM IR headers here (check-includes); only ADT, Support and Bitstream.
add_gopp_library(GIR
  Builder.cpp
  CType.cpp
  GIR.cpp
  Escape.cpp
  Lower.cpp
  Glue.cpp
  Inline.cpp
  LowerExpr.cpp
  LowerFunc.cpp
  LowerIface.cpp
  LowerBang.cpp
  LowerStmt.cpp
  LowerStr.cpp
  LowerTest.cpp
  Runtime.cpp
  RuntimeMap.cpp
  RuntimeIface.cpp
  RuntimeStr.cpp
  Print.cpp
  Serialize.cpp
  Symbols.cpp
  Type.cpp
  Verify.cpp
  LLVM_COMPONENTS support bitstreamreader
  LINK_LIBS goppMangle goppSema goppAST goppParse goppLex goppBasic)
