#include "ASTIndex.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Index/IndexingAction.h"

namespace clang {
namespace clangd {

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
  }
  const auto& IDToSymbols = Collector->getSymbols();
  std::vector<SymbolAndOccurrences> Symbols;
  for (auto &Pair : IDToSymbols) {
    Symbols.push_back(std::move(Pair.second));
  }
  llvm::errs() << "[ASTIndexSourcer] symbols updated. " << Path << ": "
               << Symbols.size() << " symbols indexed.\n";
  unsigned i = 0;
  for (const auto &Sym : Symbols) {
    if (i++ > 10)
      break;
    llvm::errs() << " oooo [" << Sym.Sym.Identifier << " : "
                 << Sym.Sym.QualifiedName << ", "
   //              << Sym.Sym.CompletionInfo.Label << ","
                 << Sym.Sym.CompletionInfo.Documentation << "], \n";
  }
  llvm::errs() << "\n";
  FileToSymbols[Path.str()] = std::move(Symbols);
}

CompletionResult ASTIndexSourcer::complete(llvm::StringRef Query) const {
  CompletionResult Result;
  // FIXME (ioeric): Do something about `all_matched`.
  Result.all_matched = true;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    for (const auto &Pair : FileToSymbols) {
      for (const auto &Symbol : Pair.second) {
        if (llvm::StringRef(Symbol.Sym.QualifiedName).contains(Query)) {
          CompletionSymbol CS;
          CS.QualifiedName = Symbol.Sym.QualifiedName;
          CS.UID = Symbol.Sym.Identifier;
          CS.CompletionInfo = Symbol.Sym.CompletionInfo;
          CS.Kind = Symbol.Sym.Kind;
          Result.Symbols.push_back(std::move(CS));
        }
      }
    }
  }
  return Result;
}

ASTSymbolIndex::ASTSymbolIndex(ASTIndexSourcer *Sourcer)
    : Sourcer(Sourcer) {}

llvm::Expected<CompletionResult>
ASTSymbolIndex::complete(const CompletionRequest &Req) const {
  return Sourcer->complete(Req.Query);
}

} // namespace clangd
} // namespace clang
