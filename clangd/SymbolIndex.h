//===--- CompletionIndex.h - Index for code completion -----------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_COMPLETIONINDEX_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_COMPLETIONINDEX_H

#include "index-source-builder/SymbolCompletionInfo.h"
#include "llvm/Support/Error.h"

namespace clang {
namespace clangd {

struct CompletionRequest {
  std::string Query;
  std::string Filter;
  std::vector<std::string> FixedPrefixes;
};

struct ScoreSignals {
  float fuzzy_score;
};

struct CompletionSymbol {
  ScoreSignals Signals;

  std::string UID;
  index::SymbolKind Kind;
  std::string QualifiedName;

  SymbolCompletionInfo CompletionInfo;
};

struct CompletionResult {
  std::vector<CompletionSymbol> Symbols;
  //std::vector<std::string> Symbols;
  bool all_matched;
};

class SymbolIndex {
public:
  virtual ~SymbolIndex() = default;

  virtual llvm::Expected<CompletionResult>
  complete(const CompletionRequest &Req) const = 0;

  virtual llvm::Expected<std::string>
  getSymbolInfo(llvm::StringRef USR) const = 0;

  virtual llvm::Expected<std::vector<std::string>>
  getAllOccurrences(llvm::StringRef USR) const = 0;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_COMPLETIONINDEX_H
