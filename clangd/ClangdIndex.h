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

#include "Path.h"
#include "SymbolIndex.h"
#include "index-source-builder/IndexSource.h"
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

class SimpleIndexSourcer {
  public:
   virtual ~SimpleIndexSourcer() = default;

   virtual bool safeToReferenceIndex() const = 0;

   virtual llvm::StringMap<SymbolAndOccurrences> symbolIndex() const = 0;

   virtual const llvm::StringMap<SymbolAndOccurrences> &
   symbolIndexReference() const = 0;
};

class ASTIndexSourcer : public SimpleIndexSourcer {
 public:
   ASTIndexSourcer() = default;

   void remove(PathRef Path);

   void update(PathRef Path, ASTContext &Context,
               std::shared_ptr<Preprocessor> PP,
               llvm::ArrayRef<const Decl *> TopLevelDecls);

   bool safeToReferenceIndex() const override { return false; }

   llvm::StringMap<SymbolAndOccurrences> symbolIndex() const override;

   const llvm::StringMap<SymbolAndOccurrences> &
   symbolIndexReference() const override {
     llvm_unreachable("ASTIndexSourcer does not support getting reference to "
                      "the symbol index.");
   }

 private:
   llvm::StringMap<std::vector<SymbolAndOccurrences>> FileToSymbols;
   mutable std::mutex Mutex;
};

class SimpleSymbolIndex : public SymbolIndex {
public:
  SimpleSymbolIndex(SimpleIndexSourcer *Sourcer) : Sourcer(Sourcer) {}

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
  SimpleIndexSourcer *Sourcer;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGDINDEX_H
