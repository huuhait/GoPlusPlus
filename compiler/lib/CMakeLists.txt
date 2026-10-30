# One library per directory (D0.5). Dependencies go down the pipeline:
# Basic <- Lex <- Parse <- AST/Sema <- Mangle <- GIR <- CodeGen/Build ...
foreach(lib Basic Lex Parse AST Format Sema Mangle GIR CodeGen Import Macro Build Mod CDep)
  add_subdirectory(${lib})
endforeach()
