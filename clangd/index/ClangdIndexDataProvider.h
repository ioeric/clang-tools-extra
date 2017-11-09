#ifndef TOOLS_CLANG_TOOLS_EXTRA_CLANGD_INDEX_CLANGDINDEXDATAPROVIDER_H_
#define TOOLS_CLANG_TOOLS_EXTRA_CLANGD_INDEX_CLANGDINDEXDATAPROVIDER_H_

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"

#include "clang/Index/IndexSymbol.h"

#include <memory>

namespace clang {

class SourceManager;

namespace clangd {

using USR = llvm::SmallString<256>;

class ClangdIndexDataOccurrence {
public:
  virtual std::string getPath() = 0;
  virtual uint32_t getStartOffset(SourceManager &SM) = 0;
  virtual uint32_t getEndOffset(SourceManager &SM) = 0;
  virtual ~ClangdIndexDataOccurrence() = default;
};

class ClangdIndexDataProvider {
public:
  virtual void foreachOccurrence(const USR& Buf, index::SymbolRoleSet Roles, llvm::function_ref<bool(ClangdIndexDataOccurrence&)> Receiver) = 0;

  virtual void dumpIncludedBy(StringRef File) {}
  virtual void dumpInclusions(StringRef File) {}

  virtual ~ClangdIndexDataProvider() = default;
};

} // namespace clangd
} // namespace clang

#endif
