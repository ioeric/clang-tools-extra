set(LLVM_LINK_COMPONENTS
  Support
  )

add_clang_library(clangDaemon
  ClangdFileUtils.cpp
  ClangdLSPServer.cpp
  ClangdServer.cpp
  ClangdUnit.cpp
  ClangdUnitStore.cpp
  DraftStore.cpp
  GlobalCompilationDatabase.cpp
  JSONRPCDispatcher.cpp
  Logger.cpp
  Protocol.cpp
  ProtocolHandlers.cpp
  index/BTree.cpp
  index/ClangdIndexerImpl.cpp
  index/ClangdIndex.cpp
  index/ClangdIndexDataPiece.cpp
  index/ClangdIndexDataStorage.cpp
  index/ClangdIndexString.cpp
  index/indexstore/ClangdIndexStoreIndexer.cpp

  LINK_LIBS
  clangAST
  clangBasic
  clangFormat
  clangFrontend
  clangIndex
  clangLex
  clangSema
  clangSerialization
  clangTooling
  clangToolingCore
  IndexStore
  ${LLVM_PTHREAD_LIB}
  )

if( LLVM_LIB_FUZZING_ENGINE OR LLVM_USE_SANITIZE_COVERAGE )
  add_subdirectory(fuzzer)
endif()
add_subdirectory(tool)
