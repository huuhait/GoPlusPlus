add_gopp_library(Basic Version.cpp SourceManager.cpp Diagnostics.cpp DiagnosticExplain.cpp Unicode.cpp
  LLVM_COMPONENTS support)
target_compile_definitions(goppBasic PRIVATE GOPP_VERSION_STRING="${PROJECT_VERSION}")
