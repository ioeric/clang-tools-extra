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
std::string escapeSnippet(const llvm::StringRef Text) {
  std::string Result;
  Result.reserve(Text.size()); // Assume '$', '}' and '\\' are rare.
  for (const auto Character : Text) {
    if (Character == '$' || Character == '}' || Character == '\\')
      Result.push_back('\\');
    Result.push_back(Character);
  }
  return Result;
}

void ProcessChunks(const CodeCompletionString &CCS,
                          SymbolCompletionInfo *Info) {
  unsigned ArgCount = 0;
  for (const auto &Chunk : CCS) {
    // Informative qualifier chunks only clutter completion results, skip
    // them.
    if (Chunk.Kind == CodeCompletionString::CK_Informative &&
        StringRef(Chunk.Text).endswith("::"))
      continue;

    switch (Chunk.Kind) {
    case CodeCompletionString::CK_TypedText:
      // The piece of text that the user is expected to type to match
      // the code-completion string, typically a keyword or the name of
      // a declarator or macro.
      // Ignore here.
      LLVM_FALLTHROUGH;
    case CodeCompletionString::CK_Text:
      // A piece of text that should be placed in the buffer,
      //Info->Label += Chunk.Text;
      break;
    case CodeCompletionString::CK_Optional:
      // A code completion string that is entirely optional.
      // For example, an optional code completion string that
      // describes the default arguments in a function call.

      // FIXME: Maybe add an option to allow presenting the optional chunks?
      break;
    case CodeCompletionString::CK_Placeholder:
      // A string that acts as a placeholder for, e.g., a function call
      // argument.
      ++ArgCount;
      // FIXME(ioeric): get more information from NamedDecl to decide whether
      // this is actually parameter.

      //Info->InsertText += "${" + std::to_string(ArgCount) + ':' +
      //                    escapeSnippet(Chunk.Text) + '}';
      Info->Params.push_back(Chunk.Text);
      //Info->Label += Chunk.Text;
      break;
    case CodeCompletionString::CK_Informative:
      // A piece of text that describes something about the result
      // but should not be inserted into the buffer.
      // For example, the word "const" for a const method, or the name of
      // the base class for methods that are part of the base class.
      //Info->Label += Chunk.Text;
      Info->Informative += Chunk.Text;
      // Don't put the informative chunks in the insertText.
      break;
    case CodeCompletionString::CK_ResultType:
      // A piece of text that describes the type of an entity or,
      // for functions and methods, the return type.
      assert(Info->Detail.empty() && "Unexpected extraneous CK_ResultType");
      Info->Detail = Chunk.Text;
      break;
    case CodeCompletionString::CK_CurrentParameter:
      // A piece of text that describes the parameter that corresponds to
      // the code-completion location within a function call, message send,
      // macro invocation, etc.
      //
      // This should never be present while collecting completion items,
      // only while collecting overload candidates.
      llvm_unreachable("Unexpected CK_CurrentParameter while collecting "
                       "CompletionItems");
      break;
    case CodeCompletionString::CK_LeftParen:
      // A left parenthesis ('(').
    case CodeCompletionString::CK_RightParen:
      // A right parenthesis (')').
    case CodeCompletionString::CK_LeftBracket:
      // A left bracket ('[').
    case CodeCompletionString::CK_RightBracket:
      // A right bracket (']').
    case CodeCompletionString::CK_LeftBrace:
      // A left brace ('{').
    case CodeCompletionString::CK_RightBrace:
      // A right brace ('}').
    case CodeCompletionString::CK_LeftAngle:
      // A left angle bracket ('<').
    case CodeCompletionString::CK_RightAngle:
      // A right angle bracket ('>').
    case CodeCompletionString::CK_Comma:
      // A comma separator (',').
    case CodeCompletionString::CK_Colon:
      // A colon (':').
    case CodeCompletionString::CK_SemiColon:
      // A semicolon (';').
    case CodeCompletionString::CK_Equal:
      // An '=' sign.
    case CodeCompletionString::CK_HorizontalSpace:
      // Horizontal whitespace (' ').
      // Info->InsertText += Chunk.Text;
      //Info->Label += Chunk.Text;
      break;
    case CodeCompletionString::CK_VerticalSpace:
      // Vertical whitespace ('\n' or '\r\n', depending on the
      // platform).
      //Info->InsertText += Chunk.Text;
      // Don't even add a space to the label.
      break;
    }
  }
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
  return Info;
}

} // namespace clangd
} // namespace clang
