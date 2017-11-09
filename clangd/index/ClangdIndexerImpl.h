#ifndef TOOLS_CLANG_TOOLS_EXTRA_CLANGD_INDEX_CLANGDINDEXERIMPL_H_
#define TOOLS_CLANG_TOOLS_EXTRA_CLANGD_INDEX_CLANGDINDEXERIMPL_H_

#include "ClangdIndexer.h"
#include "ClangdIndexDataProvider.h"

#include "../GlobalCompilationDatabase.h"

namespace clang {
namespace clangd {

class ClangdIndex;

class ClangdIndexerImpl: public ClangdIndexer, public ClangdIndexDataProvider {
  std::string RootPath;
  GlobalCompilationDatabase &CDB;
  std::shared_ptr<ClangdIndex> Index;
  bool IsFromScratch = false;
public:
  ClangdIndexerImpl(std::string RootPath, GlobalCompilationDatabase &CDB);
  void onFileEvent(FileEvent Event) override;
  void indexRoot() override;
  void reindex() override;
  void foreachOccurrence(const USR& Buf, index::SymbolRoleSet Roles,
      llvm::function_ref<bool(ClangdIndexDataOccurrence&)> Receiver) override;

  void dumpIncludedBy(StringRef File) override;
  void dumpInclusions(StringRef File) override;

private:
  void indexFile (StringRef File);
};

} /* namespace clangd */
} /* namespace clang */

#endif /* TOOLS_CLANG_TOOLS_EXTRA_CLANGD_INDEX_CLANGDINDEXERIMPL_H_ */
