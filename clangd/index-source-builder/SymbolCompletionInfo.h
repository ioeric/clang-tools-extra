#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_SYMBOLCOMPLETIONINFO_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_SYMBOLCOMPLETIONINFO_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Index/IndexSymbol.h"
#include "clang/Lex/Preprocessor.h"

namespace clang {
namespace clangd {

struct SymbolCompletionInfo {
  static SymbolCompletionInfo create(ASTContext &Ctx, Preprocessor &PP,
                                     const NamedDecl *ND);

  index::SymbolKind Kind;
  std::string Label;
  std::string Documentation;
  std::string Detail;
  std::string Informative;
};

}  // clangd
}  // clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_SYMBOLCOMPLETIONINFO_H
