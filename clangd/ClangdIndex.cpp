//===--- ClangdIndex.cpp - Symbol indexes for clangd. ------------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===-------------------------------------------------------------------===//

#include "ClangdIndex.h"
#include "SymbolIndex.h"

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

    // FIXME(ioeric): deduplicate and rank symbols from different indexes.
    Result.Symbols.insert(Result.Symbols.end(), Res->Symbols.begin(),
                          Res->Symbols.end());
  }
  return Result;
}

void ASTIndexSourcer::remove(PathRef Path) {
  std::lock_guard<std::mutex> Lock(Mutex);
  FileToSymbols.erase(Path);
}

void ASTIndexSourcer::update(PathRef Path, ASTContext &Ctx,
                             std::shared_ptr<Preprocessor> PP,
                             llvm::ArrayRef<const Decl *> TopLevelDecls) {
  auto Collector = std::make_shared<SymbolCollector>();
  Collector->setPreprocessor(std::move(PP));
  index::IndexingOptions IndexOpts;
  IndexOpts.SystemSymbolFilter =
      index::IndexingOptions::SystemSymbolFilterKind::All;
  IndexOpts.IndexFunctionLocals = false;

  {
    std::lock_guard<std::mutex> Lock(Mutex);

    index::indexTopLevelDecls(Ctx, TopLevelDecls, Collector, IndexOpts);
    const auto &IDToSymbols = Collector->getSymbols();
    std::vector<SymbolAndOccurrences> Symbols;
    for (auto &Pair : IDToSymbols) {
      Symbols.push_back(std::move(Pair.second));
    }
    FileToSymbols[Path.str()] = std::move(Symbols);
  }
}
llvm::StringMap<SymbolAndOccurrences> ASTIndexSourcer::symbolIndex() const {
  llvm::StringMap<SymbolAndOccurrences> IDToSymbols;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    for (const auto &Pair : FileToSymbols)
      for (const auto &Symbol : Pair.second)
        IDToSymbols[Symbol.Sym.Identifier] = Symbol;
  }
  return IDToSymbols;
}

llvm::Expected<CompletionResult>
SimpleSymbolIndex::complete(const CompletionRequest &Req) const {
  CompletionResult Result;
  // FIXME (ioeric): Do something about `all_matched`.
  Result.all_matched = true;
  llvm::StringMap<SymbolAndOccurrences> CopiedSymbols;
  if (!Sourcer->safeToReferenceIndex())
    CopiedSymbols = Sourcer->symbolIndex();
  const llvm::StringMap<SymbolAndOccurrences> &SymbolsRef =
      Sourcer->safeToReferenceIndex() ? Sourcer->symbolIndexReference()
                                      : CopiedSymbols;

  std::string FullQuery = Req.Query;
  if (!Req.Filter.empty()) {
    if (!FullQuery.empty())
      FullQuery += "::";
    FullQuery += Req.Filter;
  }
  FullQuery = StringRef(FullQuery).lower();
  llvm::errs() << "Full query: [" << FullQuery << "]\n";
  for (const auto &IDAndSymbol : SymbolsRef) {
    const auto &Symbol = IDAndSymbol.second;
    if (StringRef(StringRef(Symbol.Sym.QualifiedName).lower())
            .contains(FullQuery)) {
      CompletionSymbol CS;
      CS.QualifiedName = Symbol.Sym.QualifiedName;
      CS.UID = Symbol.Sym.Identifier;
      CS.CompletionInfo = Symbol.Sym.CompletionInfo;
      CS.Kind = Symbol.Sym.Kind;
      Result.Symbols.push_back(std::move(CS));
    }
  }
  return Result;
}

} // namespace clangd
} // namespace clang
