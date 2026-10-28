add_gopp_library(Sema
  Assign.cpp
  Builtins.cpp
  Call.cpp
  Check.cpp
  Constant.cpp
  Loader.cpp
  Decl.cpp
  Expr.cpp
  Foreign.cpp
  ExprString.cpp
  Index.cpp
  Infer.cpp
  InitOrder.cpp
  Lookup.cpp
  Operand.cpp
  Ownership.cpp
  Predicates.cpp
  Serialize.cpp
  Stmt.cpp
  TypeSet.cpp
  TypeString.cpp
  Typexpr.cpp
  Vet.cpp
  Types.cpp
  LLVM_COMPONENTS support
  LINK_LIBS goppParse goppAST goppLex goppBasic)
