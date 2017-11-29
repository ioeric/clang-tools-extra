#include "ASTIndex.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Index/IndexingAction.h"

namespace clang {
namespace clangd {

namespace {

class IndexConsumer : public index::IndexDataConsumer {
public:
  IndexConsumer(std::set<std::string> &Symbols) : Symbols(Symbols) {}

  bool handleDeclOccurence(const Decl *D, index::SymbolRoleSet Roles,
                           ArrayRef<index::SymbolRelation> Relations,
                           FileID FID, unsigned Offset,
                           ASTNodeInfo ASTNode) override {
    if (!(Roles & (unsigned)index::SymbolRole::Declaration ||
          Roles & (unsigned)index::SymbolRole::Definition)) {
      return true;
    }
    if (const auto *ND = llvm::dyn_cast<NamedDecl>(D)) {
      Symbols.insert(ND->getQualifiedNameAsString());
    }
    return true;
  }

private:
  std::set<std::string> &Symbols;
};

} // namespace

void ASTIndexSourcer::remove(PathRef Path) {
  std::lock_guard<std::mutex> Lock(Mutex);
  Symbols.erase(Path);
}

void ASTIndexSourcer::update(PathRef Path, ASTContext &Ctx,
                             llvm::ArrayRef<const Decl *> TopLevelDecls) {
  std::set<std::string> TUSymbols;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    auto Consumer = std::make_shared<IndexConsumer>(TUSymbols);
    index::IndexingOptions IndexOpts;
    IndexOpts.SystemSymbolFilter =
        index::IndexingOptions::SystemSymbolFilterKind::All;

    index::indexTopLevelDecls(Ctx, TopLevelDecls, Consumer, IndexOpts);
  }
  llvm::errs() << "[ASTIndexSourcer] symbols updated. " << Path << ": "
               << TUSymbols.size() << " symbols indexed.\n";
  unsigned i = 0;
  for (llvm::StringRef Symbol : TUSymbols) {
    if (i++ > 10)
      break;
    llvm::errs() << " --- " << Symbol;
  }
  llvm::errs() << "\n";
  Symbols[Path.str()] = std::move(TUSymbols);
}

CompletionResult ASTIndexSourcer::complete(llvm::StringRef Query) const {
  CompletionResult Result;
  // FIXME (ioeric): Do something about `all_matched`.
  Result.all_matched = true;
  {
    std::lock_guard<std::mutex> Lock(Mutex);
    llvm::errs() << Symbols.size() << " symbols in the index.\n";
    for (const auto &Pair : Symbols) {
      for (llvm::StringRef Symbol : Pair.second) {
        if (Symbol.contains(Query))
          Result.Symbols.push_back(Symbol);
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
