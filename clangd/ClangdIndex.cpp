//===--- ClangdIndex.cpp - Symbol indexes for clangd. ------------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===-------------------------------------------------------------------===//

#include "ClangdIndex.h"

namespace clang {
namespace clangd {

void CombinedSymbolIndex::addSymbolIndex(
    llvm::StringRef Label, CombinedSymbolIndex::WeightedIndex Index) {
  Indexes.insert({Label, std::move(Index)});
}

llvm::Expected<CompletionResult>
CombinedSymbolIndex::complete(const CompletionRequest &Req) const {
  CompletionResult Result;
  // FIXME(ioeric): Consider ranking signals and index weights.
  for (auto &Entry : Indexes) {
    const WeightedIndex &Index = Entry.second;
    auto Res = Index.Index->complete(Req);
    if (!Res) {
      llvm::errs() << "Failed to complete request " << Req.Query << " in index "
                   << Entry.first() << "\n";
      continue;
    }
    // FIXME(ioeric): figure out what to do with `all_matched`.

    // FIXME(ioeric): for now, we simply merge results from all indexes.
    Result.Symbols.insert(Result.Symbols.end(), Res->Symbols.begin(),
                          Res->Symbols.end());
  }
  return Result;
}

} // namespace clangd
} // namespace clang
