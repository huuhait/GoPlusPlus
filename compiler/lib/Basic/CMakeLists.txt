add_gopp_library(Basic Version.cpp LLVM_COMPONENTS support)
target_compile_definitions(goppBasic PRIVATE GOPP_VERSION_STRING="${PROJECT_VERSION}")
