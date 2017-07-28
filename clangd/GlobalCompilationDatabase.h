//===--- GlobalCompilationDatabase.h ----------------------------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_GLOBALCOMPILATIONDATABASE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_GLOBALCOMPILATIONDATABASE_H

#include "Path.h"
#include "index/ClangdIndex.h"

#include "llvm/ADT/StringMap.h"

#include <memory>
#include <mutex>
#include <vector>

namespace clang {

namespace tooling {
class CompilationDatabase;
struct CompileCommand;
} // namespace tooling

namespace clangd {

class Logger;

/// Returns a default compile command to use for \p File.
tooling::CompileCommand getDefaultCompileCommand(PathRef File);

/// Provides compilation arguments used for parsing C and C++ files.
class GlobalCompilationDatabase {
public:
  virtual ~GlobalCompilationDatabase() = default;

  virtual void setIndex(std::weak_ptr<ClangdIndex> Index) {
  }

  virtual std::vector<tooling::CompileCommand>
  getCompileCommands(PathRef File) = 0;

  /// FIXME(ibiryukov): add facilities to track changes to compilation flags of
  /// existing targets.
};

/// Gets compile args from tooling::CompilationDatabases built for parent
/// directories.
class DirectoryBasedGlobalCompilationDatabase
    : public GlobalCompilationDatabase {
public:
  DirectoryBasedGlobalCompilationDatabase(
      clangd::Logger &Logger, llvm::Optional<Path> CompileCommandsDir);

  std::vector<tooling::CompileCommand>
  getCompileCommands(PathRef File) override;

  void setExtraFlagsForFile(PathRef File, std::vector<std::string> ExtraFlags);
  void setIndex(std::weak_ptr<ClangdIndex> Index) override {
    this->Index = Index;
  }

private:
  tooling::CompilationDatabase *getCompilationDatabase(PathRef File);
  tooling::CompilationDatabase *tryLoadDatabaseFromPath(PathRef File);
  std::vector<tooling::CompileCommand>
  getCompileCommandsUsingIndex(std::unique_ptr<ClangdIndexFile> IndexFile);

  std::mutex Mutex;
  /// Caches compilation databases loaded from directories(keys are
  /// directories).
  llvm::StringMap<std::unique_ptr<clang::tooling::CompilationDatabase>>
      CompilationDatabases;

  /// Stores extra flags per file.
  llvm::StringMap<std::vector<std::string>> ExtraFlagsForFile;
  /// Used for logging.
  clangd::Logger &Logger;
  /// Used for command argument pointing to folder where compile_commands.json
  /// is located.
  llvm::Optional<Path> CompileCommandsDir;
  // The index can assist in which source file to lookup in the database,
  // when requesting the database for a header for example.
  //FIXME: I don't think setting the index after the fact here after
  // construction is good.
  std::weak_ptr<ClangdIndex> Index;
};
} // namespace clangd
} // namespace clang

#endif
