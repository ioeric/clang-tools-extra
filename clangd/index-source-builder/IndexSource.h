#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_SOURCE_BUILDER_INDEX_SOURCE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_SOURCE_BUILDER_INDEX_SOURCE_H

#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Index/IndexSymbol.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Tooling/Execution.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#include <set>

namespace clang {
namespace clangd {

struct SymbolLocation {
  // The path of the source file where a symbol occurs.
  std::string FilePath;
  // The offset to the first character of the symbol from the beginning of the
  // source file.
  unsigned StartOffset;
  // The offset to the last character of the symbol from the beginning of the
  // source file.
  unsigned EndOffset;

  bool operator<(const SymbolLocation& RHS) const {
    return std::tie(FilePath, StartOffset, EndOffset) <
           std::tie(RHS.FilePath, RHS.StartOffset, RHS.EndOffset);
  }
};

struct Symbol {
  // The symbol identifier, using USR.
  std::string Identifier;
  // The qualified name of the symbol, e.g. Foo::bar.
  std::string QualifiedName;
  // The language, e.g. C, CPP.
  index::SymbolLanguage Language;
  // The symbol kind, e.g. Class, Function.
  index::SymbolKind Kind;

  // The "preferred" location of the symbol. Usually it is the location where
  // the symbol is declared.
  // In C++, a symbol could have multiple locations. For
  // example, class "Foo" is declared in foo.h, and its definition is defined in
  // foo.cc. The "foo.h" header location would be the preferred location.
  SymbolLocation PreferredLocation;

  enum class InfoKinds {
    // The doc-comment about the symbol.
    Documentation = 0,
    // The type information about then symbol.
    TypedText = 1,
    // ...others
  };

  static const int NumInfoKinds = 2;
  // Extra information of the symbol, used in codeCompletion.
  std::string ExtraInfo[NumInfoKinds];

  // Some extra fields for computing index scoring signals? These signals could
  // be computed when index_builder builds its index from the source.
  // The number of files in the codebase where the symbol was used (one per
  // file).
  // int NumOfUsages;
};

struct Occurrence {
  // The location of the symbol occurrence.
  SymbolLocation Location;
  // The kind of symbol occurrences, e.g. declaration, definition, references.
  index::SymbolRoleSet Roles;

  bool operator<(const Occurrence& RHS) const {
    return std::tie(Location, Roles) < std::tie(RHS.Location, RHS.Roles);
  }
};

struct SymbolAndOccurrences {
  Symbol Sym;
  // Include all occurrences of the symbol in the code base, including the
  // symbol declaration, definition, and references.
  std::set<Occurrence> Occurrences;
};

std::string ToYAML(
    const std::map<std::string, clang::clangd::SymbolAndOccurrences> &Symbols);

std::string ToYAML(
    const std::vector<clang::clangd::SymbolAndOccurrences> &Symbols);

std::vector<SymbolAndOccurrences> ReadFromYAML(llvm::StringRef YAML);

// Collect all symbol occurrences from an AST.
//
// Clients (e.g. clangd) can use SymbolCollector together with
// index::indexTopLevelDecls to retrieve all symbol occurrences when
// reparsing the source file.
class SymbolCollector : public index::IndexDataConsumer {
 public:
   SymbolCollector(tooling::ExecutionContext *Context = nullptr)
       : Context(Context) {}

   const std::map<std::string, SymbolAndOccurrences> getSymbols() const {
     return Symbols;
   }

   void initialize(ASTContext &Ctx) override;

   bool
   handleDeclOccurence(const Decl *D, index::SymbolRoleSet Roles,
                       ArrayRef<index::SymbolRelation> Relations, FileID FID,
                       unsigned Offset,
                       index::IndexDataConsumer::ASTNodeInfo ASTNode) override;

   void finish() override;

 private:
  tooling::ExecutionContext* Context;
  std::string Filename;
  std::map<std::string, SymbolAndOccurrences> Symbols;
};

}  // clangd
}  // clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_SOURCE_BUILDER_INDEX_SOURCE_H
