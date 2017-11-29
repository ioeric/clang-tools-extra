//===--- ClangdIndex.h - Symbol indexes for clangd.---------------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGDINDEX_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGDINDEX_H

#include "SymbolIndex.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"

namespace clang {
namespace clangd {

class CombinedSymbolIndex : public SymbolIndex {
public:
  CombinedSymbolIndex() = default;

  struct WeightedIndex {
    explicit WeightedIndex(std::unique_ptr<SymbolIndex> Index)
        : Index(std::move(Index)) {}

    std::unique_ptr<SymbolIndex> Index;
    double OverallWeight;
  };

  void addSymbolIndex(llvm::StringRef Label, WeightedIndex Index);

  llvm::Expected<CompletionResult>
  complete(const CompletionRequest &Req) const override;

  llvm::Expected<std::string>
  getSymbolInfo(llvm::StringRef UID) const override {
    // FIXME(ioeric): implement this.
    return "";
  }

  llvm::Expected<std::vector<std::string>>
  getAllOccurrences(llvm::StringRef UID) const override {
    // FIXME(ioeric): implement this.
    return std::vector<std::string>();
  }

private:
  llvm::StringMap<WeightedIndex> Indexes;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGDINDEX_H
