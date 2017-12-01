#include "GlobalIndex.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/Errc.h"

namespace clang {
namespace clangd {

namespace {
// FIXME: Use your path.
constexpr char GlobalIndexSource[] = "/path/to/index-source-no-occurrences.yaml";
constexpr int LimitSize= 100;

// Check if |SubStr| is a subsequence of |SuperStr|.
bool IsSubSequence(llvm::StringRef SubStr, llvm::StringRef SuperStr) {
  auto It = SuperStr.begin();
  for (char C : SubStr) {
    do {
      if (It == SuperStr.end()) return false;
    } while (*It++ != C);
  }
  return true;
}

std::pair<StringRef, StringRef> Split(StringRef S, StringRef Separator) {
  size_t Idx = S.rfind(Separator);
  if (Idx == StringRef::npos)
    return std::make_pair(S, llvm::StringRef());
  return std::make_pair(S.slice(0, Idx), S.slice(Idx+1, StringRef::npos));

}

} // namespace

GlobalSymbolIndex::GlobalSymbolIndex() {
  auto Buffer = llvm::MemoryBuffer::getFile(GlobalIndexSource);
  if (!Buffer) {
    llvm::errs() << "Can't open " << GlobalIndexSource << "\n";
    return;
  }
  GlobalSymbols = ReadFromYAML(Buffer.get()->getBuffer());
}

llvm::Expected<std::string>
GlobalSymbolIndex::getSymbolInfo(llvm::StringRef UID) const {
  for (auto& S : GlobalSymbols) {
    if (S.Sym.Identifier == UID)
      return S.Sym.QualifiedName;
  }
  return llvm::make_error<llvm::StringError>("No symbol found",
                                             llvm::errc::invalid_argument);
}

llvm::Expected<std::vector<std::string>>
GlobalSymbolIndex::getAllOccurrences(llvm::StringRef UID) const {
  return llvm::make_error<llvm::StringError>("Not implemented yet",
                                             llvm::errc::invalid_argument);
}

llvm::Expected<CompletionResult>
GlobalSymbolIndex::complete(const CompletionRequest &Req) const {
  CompletionResult Result;
  llvm::errs() << "Search request: " << Req.Query << "\n";
  llvm::errs() << "Total number of size: " << GlobalSymbols.size() << "\n";
  for (auto& S : GlobalSymbols) {
    if (Result.Symbols.size() > LimitSize)
      break;
    // upstream:  llvm::StringRef(S.Sym.QualifiedName).rsplit("::");
    auto Tokens = Split(S.Sym.QualifiedName, "::");
    if (IsSubSequence(Req.Filter, Tokens.second)) {
      if (Req.Query == Tokens.first) {
        Result.Symbols.push_back(S.Sym.QualifiedName);
      }
    }
  }
  return Result;
}

} // namespace clangd
} // namespace clang
