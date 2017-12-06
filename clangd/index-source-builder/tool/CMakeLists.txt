include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

add_clang_tool(llvm-indexer
  IndexSourceBuilderMain.cpp
  )

set(LLVM_LINK_COMPONENTS
  support
  )

target_link_libraries(llvm-indexer
  PRIVATE
  clangAST
  clangIndex
  clangBasic
  clangFrontend
  clangTooling
  clangToolingRefactor
  indexSource
  )
