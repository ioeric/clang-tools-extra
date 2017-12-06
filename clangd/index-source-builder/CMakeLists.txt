set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(indexSource
  SymbolCompletionInfo.cpp
  IndexSource.cpp

  LINK_LIBS
  clangAST
  clangIndex
  clangBasic
  clangFrontend
  clangSema
  clangTooling
  clangToolingRefactor
  )

add_subdirectory(tool)
