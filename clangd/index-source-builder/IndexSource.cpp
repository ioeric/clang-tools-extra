#include "IndexSource.h"

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Index/IndexSymbol.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Execution.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using llvm::yaml::MappingTraits;
using llvm::yaml::IO;
using llvm::yaml::Input;
using clang::clangd::Occurrence;

LLVM_YAML_IS_DOCUMENT_LIST_VECTOR(clang::clangd::SymbolAndOccurrences)
LLVM_YAML_IS_SEQUENCE_VECTOR(clang::clangd::Occurrence)

namespace llvm {
namespace yaml {

struct NPOccurrenceSet {
  NPOccurrenceSet(IO &) {}
  NPOccurrenceSet(IO &, std::set<clang::clangd::Occurrence> &OccurrenceSet)
      : Occurrences(OccurrenceSet.begin(), OccurrenceSet.end()) {}
  std::set<Occurrence> denormalize(IO &) {
    return {Occurrences.begin(), Occurrences.end()};
  }
  std::vector<Occurrence> Occurrences;
};

template <> struct MappingTraits<clang::clangd::SymbolLocation> {
  static void mapping(IO &io, clang::clangd::SymbolLocation &value) {
    io.mapRequired("Offset", value.StartOffset);
    io.mapRequired("FilePath", value.FilePath);
  }
};

struct NPSymbolRoleSet {
  NPSymbolRoleSet(IO &) {}
  NPSymbolRoleSet(IO&, clang::index::SymbolRoleSet & S) {
    R = static_cast<clang::index::SymbolRole>(S);
  }
  clang::index::SymbolRoleSet denormalize(IO&) {
    return static_cast<clang::index::SymbolRoleSet>(R);
  }
  clang::index::SymbolRole R;
};

//template<>
//struct ScalarBitSetTraits<clang::index::SymbolRole> {
  //static void bitset(IO &io, clang::index::SymbolRole& value) {
    //io.bitSetCase(value, "Declaration", clang::index::SymbolRole::Declaration);
    //io.bitSetCase(value, "Definition", clang::index::SymbolRole::Definition);
  //}
//};


template <> struct MappingTraits<clang::clangd::Occurrence> {
  static void mapping(IO &io, clang::clangd::Occurrence &value) {
    //MappingNormalization<NPSymbolRoleSet, clang::index::SymbolRoleSet> NR(
        //io, value.Roles);
    io.mapRequired("Location", value.Location);
    io.mapRequired("Roles", value.Roles);
  }
};

template<> struct MappingTraits<clang::clangd::SymbolAndOccurrences> {
  static void mapping(IO &IO, clang::clangd::SymbolAndOccurrences &Symbol) {
    IO.mapRequired("ID", Symbol.Sym.Identifier);
    IO.mapRequired("QualifiedName", Symbol.Sym.QualifiedName);
    IO.mapRequired("Language", Symbol.Sym.Language);
    IO.mapRequired("SymbolKind", Symbol.Sym.Kind);
    IO.mapRequired("PreferredLocation", Symbol.Sym.PreferredLocation);
    MappingNormalization<NPOccurrenceSet, std::set<Occurrence>> NPOccurrenceSet(
        IO, Symbol.Occurrences);
    IO.mapOptional("Occurrences", NPOccurrenceSet->Occurrences);
  }
};

template <> struct ScalarEnumerationTraits<clang::index::SymbolLanguage> {
  static void enumeration(IO &io, clang::index::SymbolLanguage &value) {
    io.enumCase(value, "C", clang::index::SymbolLanguage::C);
    io.enumCase(value, "Cpp", clang::index::SymbolLanguage::CXX);
    io.enumCase(value, "ObjC", clang::index::SymbolLanguage::ObjC);
    io.enumCase(value, "Swift", clang::index::SymbolLanguage::Swift);
  }
};

template <> struct ScalarEnumerationTraits<clang::index::SymbolKind> {
  static void enumeration(IO &io, clang::index::SymbolKind &value) {
#define DEFINE_ENUM(name) \
    io.enumCase(value, #name, clang::index::SymbolKind::name)

    DEFINE_ENUM(Unknown);
    DEFINE_ENUM(Function);
    DEFINE_ENUM(Module);
    DEFINE_ENUM(Namespace);
    DEFINE_ENUM(NamespaceAlias);
    DEFINE_ENUM(Macro);
    DEFINE_ENUM(Enum);
    DEFINE_ENUM(Struct);
    DEFINE_ENUM(Class);
    DEFINE_ENUM(Protocol);
    DEFINE_ENUM(Extension);
    DEFINE_ENUM(Union);
    DEFINE_ENUM(TypeAlias);
    DEFINE_ENUM(Function);
    DEFINE_ENUM(Variable);
    DEFINE_ENUM(Field);
    DEFINE_ENUM(EnumConstant);
    DEFINE_ENUM(InstanceMethod);
    DEFINE_ENUM(ClassMethod);
    DEFINE_ENUM(StaticMethod);
    DEFINE_ENUM(InstanceProperty);
    DEFINE_ENUM(ClassProperty);
    DEFINE_ENUM(StaticProperty);
    DEFINE_ENUM(Constructor);
    DEFINE_ENUM(Destructor);
    DEFINE_ENUM(ConversionFunction);
    DEFINE_ENUM(Parameter);
    DEFINE_ENUM(Using);
#undef DEFINE_ENUM
  }
};

} // namespace yaml
} // namespace llvm

namespace clang {
namespace clangd {

std::string ToYAML(
    const std::vector<clang::clangd::SymbolAndOccurrences> &Symbols) {
  std::string Str;
  llvm::raw_string_ostream OS(Str);
  llvm::yaml::Output yout(OS);
  for (auto Symbol : Symbols) {
    yout << Symbol;
  }
  return OS.str();
}

std::string ToYAML(
    const std::map<std::string, clang::clangd::SymbolAndOccurrences> &Symbols) {
  std::string Str;
  llvm::raw_string_ostream OS(Str);
  llvm::yaml::Output yout(OS);
  for (auto Symbol : Symbols) {
    yout << Symbol.second;
  }
  return OS.str();
}

std::vector<SymbolAndOccurrences> ReadFromYAML(llvm::StringRef YAML) {
  std::vector<SymbolAndOccurrences> S;
  llvm::yaml::Input Yin(YAML);
  Yin >> S;
  return S;
}

void SymbolCollector::initialize(ASTContext &Ctx) {
  auto FID = Ctx.getSourceManager().getMainFileID();
  const auto *Entry = Ctx.getSourceManager().getFileEntryForID(FID);
  Filename = Entry->tryGetRealPathName();
}

bool SymbolCollector::handleDeclOccurence(
    const Decl *D, index::SymbolRoleSet Roles,
    ArrayRef<index::SymbolRelation> Relations, FileID FID, unsigned Offset,
    index::IndexDataConsumer::ASTNodeInfo ASTNode) {
  if (const NamedDecl *ND = llvm::dyn_cast<NamedDecl>(D)) {
    auto &SM = ND->getASTContext().getSourceManager();
    SourceLocation Loc = SM.getLocForStartOfFile(FID).getLocWithOffset(Offset);
    StringRef FilePath = SM.getFilename(Loc);

    llvm::SmallVector<char, 128> Buff;
    if (index::generateUSRForDecl(ND, Buff))
      return false;
    std::string ID(Buff.data(), Buff.size());
    auto it = Symbols.find(ID);
    if (it == Symbols.end()) {
      auto SymbolInfo = index::getSymbolInfo(D);

      SymbolAndOccurrences NewSymbol;
      NewSymbol.Sym.Kind = SymbolInfo.Kind;
      NewSymbol.Sym.Language = SymbolInfo.Lang;
      NewSymbol.Sym.Identifier = ID;
      NewSymbol.Sym.PreferredLocation.StartOffset =
          SM.getFileOffset(D->getLocation());
      NewSymbol.Sym.PreferredLocation.FilePath =
          SM.getFilename(D->getLocation());
      NewSymbol.Sym.QualifiedName = ND->getQualifiedNameAsString();
      Symbols[ID].Sym = std::move(NewSymbol.Sym);
      it = Symbols.find(ID);
    }
    Occurrence Occ;
    Occ.Roles = Roles;
    Occ.Location.StartOffset = Offset;
    Occ.Location.FilePath = FilePath.str();
    it->second.Occurrences.insert(std::move(Occ));
    return true;
  }
  return false;
}

void SymbolCollector::finish() {
  if (Context)
    Context->reportResult(Filename, ToYAML(Symbols));
}


}  // clangd
}  // clang
