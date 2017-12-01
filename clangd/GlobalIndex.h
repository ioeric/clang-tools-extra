//===--- ASTIndex.h - Indexes for AST symbols. -----------*- C++-*-===========//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_GLOBALINDEX_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_GLOBALINDEX_H

#include "Path.h"
#include "index-source-builder/IndexSource.h"
#include "SymbolIndex.h"
#include "llvm/ADT/StringMap.h"
#include <mutex>

namespace clang {
namespace clangd {

class GlobalSymbolIndex : public SymbolIndex {
public:
  GlobalSymbolIndex();

  llvm::Expected<CompletionResult>
  complete(const CompletionRequest &Req) const override;

  llvm::Expected<std::string>
  getSymbolInfo(llvm::StringRef UID) const override;

  llvm::Expected<std::vector<std::string>>
  getAllOccurrences(llvm::StringRef UID) const override;

private:
  std::vector<SymbolAndOccurrences> GlobalSymbols;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_GLOBAL_H
