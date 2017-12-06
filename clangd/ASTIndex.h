//===--- ASTIndex.h - Indexes for AST symbols. -----------*- C++-*-===========//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_ASTINDEX_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_ASTINDEX_H

#include "Path.h"
#include "SymbolIndex.h"
#include "index-source-builder/IndexSource.h"
#include "clang/AST/ASTContext.h"
#include "llvm/ADT/StringMap.h"
#include <mutex>

namespace clang {
namespace clangd {

class ASTIndexSourcer {
 public:
   ASTIndexSourcer() = default;

   void remove(PathRef Path);

   void update(PathRef Path, ASTContext &Context,
               std::shared_ptr<Preprocessor> PP,
               llvm::ArrayRef<const Decl *> TopLevelDecls);

   CompletionResult complete(llvm::StringRef Query) const;

 private:
   llvm::StringMap<std::vector<SymbolAndOccurrences>> FileToSymbols;
   mutable std::mutex Mutex;
};

class ASTSymbolIndex : public SymbolIndex {
public:
  ASTSymbolIndex(ASTIndexSourcer *Sourcer);

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
  ASTIndexSourcer *Sourcer;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_ASTINDEX_H
