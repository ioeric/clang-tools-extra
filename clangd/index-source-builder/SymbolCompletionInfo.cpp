#include "SymbolCompletionInfo.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "llvm/Support/YAMLTraits.h"
#include <memory>

namespace clang {
namespace clangd {
namespace {

std::string getDocumentation(const CodeCompletionString &CCS) {
  // Things like __attribute__((nonnull(1,3))) and [[noreturn]]. Present this
  // information in the documentation field.
  std::string Result;
  const unsigned AnnotationCount = CCS.getAnnotationCount();
  if (AnnotationCount > 0) {
    Result += "Annotation";
    if (AnnotationCount == 1) {
      Result += ": ";
    } else /* AnnotationCount > 1 */ {
      Result += "s: ";
    }
    for (unsigned I = 0; I < AnnotationCount; ++I) {
      Result += CCS.getAnnotation(I);
      Result.push_back(I == AnnotationCount - 1 ? '\n' : ' ');
    }
  }
  // Add brief documentation (if there is any).
  if (CCS.getBriefComment() != nullptr) {
    if (!Result.empty()) {
      // This means we previously added annotations. Add an extra newline
      // character to make the annotations stand out.
      Result.push_back('\n');
    }
    Result += CCS.getBriefComment();
  }
  return Result;
}

void ProcessChunks(const CodeCompletionString &CCS,
                   SymbolCompletionInfo *Info) {
  for (const auto &Chunk : CCS) {
    // Informative qualifier chunks only clutter completion results, skip
    // them.
    if (Chunk.Kind == CodeCompletionString::CK_Informative &&
        StringRef(Chunk.Text).endswith("::"))
      continue;

    switch (Chunk.Kind) {
    case CodeCompletionString::CK_TypedText:
      // There's always exactly one CK_TypedText chunk.
      Info->Label += Chunk.Text;
      break;
    case CodeCompletionString::CK_ResultType:
      Info->Detail = Chunk.Text;
      break;
    case CodeCompletionString::CK_Optional:
      break;
    case CodeCompletionString::CK_Placeholder:
      // A string that acts as a placeholder for, e.g., a function call
      // argument.
      Info->Params.push_back(Chunk.Text);
      LLVM_FALLTHROUGH;
    default:
      Info->Label += Chunk.Text;
      break;
    }
  }
}

inline std::string
joinNamespaces(const llvm::SmallVectorImpl<StringRef> &Namespaces) {
  if (Namespaces.empty())
    return "";
  std::string Result = Namespaces.front();
  for (auto I = Namespaces.begin() + 1, E = Namespaces.end(); I != E; ++I)
    Result += ("::" + *I).str();
  return Result;
}

// Given "a::b::c", returns {"a", "b", "c"}.
llvm::SmallVector<llvm::StringRef, 4> splitSymbolName(llvm::StringRef Name) {
  llvm::SmallVector<llvm::StringRef, 4> Splitted;
  Name.split(Splitted, "::", /*MaxSplit=*/-1,
             /*KeepEmpty=*/false);
  return Splitted;
}

} // namespace

SymbolCompletionInfo SymbolCompletionInfo::create(ASTContext &Ctx,
                                                  Preprocessor &PP,
                                                  const NamedDecl *ND) {
  CodeCompletionResult SymbolCompletion(ND, 0);
  auto Allocator = std::make_shared<GlobalCodeCompletionAllocator>();
  CodeCompletionTUInfo TUInfo(Allocator);
  const auto *CCS = SymbolCompletion.CreateCodeCompletionString(
      Ctx, PP, CodeCompletionContext::CCC_Name, *Allocator, TUInfo,
      /*IncludeBriefComments*/ true);
  SymbolCompletionInfo Info;
  Info.Documentation = getDocumentation(*CCS);

  ProcessChunks(*CCS, &Info);
  // Since symbol names in CCS labels are not qualified, we prepend a namespace
  // qualfifier.
  std::string QualifiedName = ND->getQualifiedNameAsString();
  auto SplittedNames = splitSymbolName(QualifiedName);
  if (SplittedNames.size() > 1) {
    std::string LabelPrefix = joinNamespaces(SmallVector<llvm::StringRef, 4>(
        SplittedNames.begin(), SplittedNames.end() - 1));
    Info.Label = LabelPrefix + "::" + Info.Label;
  }
  return Info;
}

} // namespace clangd
} // namespace clang
