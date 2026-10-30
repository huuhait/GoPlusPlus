# GIR (P04.01): types, the IR, builder, verifier, printer, bitstream form. No
# LLVM IR headers here (check-includes); only ADT, Support and Bitstream.
add_gopp_library(GIR
  Builder.cpp
  GIR.cpp
  Lower.cpp
  Print.cpp
  Serialize.cpp
  Type.cpp
  Verify.cpp
  LLVM_COMPONENTS support bitstreamreader
  LINK_LIBS goppSema goppAST goppParse goppLex goppBasic)
