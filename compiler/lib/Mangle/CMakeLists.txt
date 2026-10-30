# Symbol names (D4.13, P04.17): the mangler and demangler, and the bridge from
# the checker's types. Above Sema, below GIR and CodeGen, which name what they
# emit through it.
add_gopp_library(Mangle SymbolName.cpp SemaSymbols.cpp
  LLVM_COMPONENTS support
  LINK_LIBS goppSema)
