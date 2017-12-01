set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(indexSource
  IndexSource.cpp

  LINK_LIBS
  clangAST
  clangIndex
  clangBasic
  clangFrontend
  clangTooling
  clangToolingRefactor
  )

add_subdirectory(tool)
