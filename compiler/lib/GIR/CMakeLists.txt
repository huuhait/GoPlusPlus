# GIR (P04.01): types, the IR, builder, verifier, printer, bitstream form. No
# LLVM IR headers here (check-includes); only ADT, Support and Bitstream.
add_gopp_library(GIR
  Builder.cpp
  CType.cpp
  GIR.cpp
  Lower.cpp
  Glue.cpp
  LowerExpr.cpp
  LowerStmt.cpp
  Runtime.cpp
  Print.cpp
  Serialize.cpp
  Symbols.cpp
  Type.cpp
  Verify.cpp
  LLVM_COMPONENTS support bitstreamreader
  LINK_LIBS goppMangle goppSema goppAST goppParse goppLex goppBasic)
