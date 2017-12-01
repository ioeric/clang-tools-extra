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
#include "llvm/Support/Path.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#include <map>

using namespace llvm;
using clang::clangd::SymbolAndOccurrences;
using llvm::yaml::MappingTraits;
using llvm::yaml::IO;
using llvm::yaml::Input;

namespace clang {
namespace clangd {
class SymbolIndexActionFactory : public tooling::FrontendActionFactory {
 public:
  SymbolIndexActionFactory(tooling::ExecutionContext &Context)
      : Context(Context) {}
  clang::ASTFrontendAction *create() override {
    index::IndexingOptions IndexOpts;
    IndexOpts.SystemSymbolFilter =
                  index::IndexingOptions::SystemSymbolFilterKind::All;
    IndexOpts.IndexFunctionLocals = false;
    Collector = std::make_shared<SymbolCollector>(&Context);
    FrontendAction *Action =
        index::createIndexingAction(Collector, IndexOpts, nullptr).release();
    return llvm::cast<ASTFrontendAction>(Action);
  }

  tooling::ExecutionContext &Context;
  std::shared_ptr<SymbolCollector> Collector;
};
} // namespace clangd
} // namespace clang

static cl::OptionCategory IndexSourceCategory("index-source-builder options");

static cl::opt<std::string> OutputDir("output-dir", cl::desc(R"(
The output directory for saving the results.)"),
                                      cl::init("."),
                                      cl::cat(IndexSourceCategory));

static cl::opt<std::string> MergeDir("merge-dir", cl::desc(R"(
The directory for merging symbols.)"),
                                     cl::init(""),
                                     cl::cat(IndexSourceCategory));

bool WriteFile(llvm::StringRef OutputFile,
               const std::map<std::string, SymbolAndOccurrences> &Symbols) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(OutputFile, EC, llvm::sys::fs::F_None);
  if (EC) {
    llvm::errs() << "Can't open '" << OutputFile << "': " << EC.message()
                 << '\n';
    return false;
  }
  OS << clang::clangd::ToYAML(Symbols);
  return true;
}

bool Merge(llvm::StringRef MergeDir, llvm::StringRef OutputFile) {
  std::error_code EC;
  std::map<std::string, SymbolAndOccurrences> Symbols;
  std::mutex SymbolMutex;
  auto AddSymbols = [&](ArrayRef<SymbolAndOccurrences> NewSymbols) {
    // Synchronize set accesses.
    std::unique_lock<std::mutex> LockGuard(SymbolMutex);
    for (const auto &Symbol : NewSymbols) {
      //Symbols[Symbol.Symbol] += Symbol.Signals;
      auto it = Symbols.find(Symbol.Sym.Identifier);
      if (it == Symbols.end()) {
        Symbols[Symbol.Sym.Identifier] =  Symbol;
      } else {
        for (const auto& Occ : Symbol.Occurrences) {
          it->second.Occurrences.insert(Occ);
        }
      }
    }
  };

  // Load all symbol files in MergeDir.
  {
    llvm::ThreadPool Pool;
    for (llvm::sys::fs::directory_iterator Dir(MergeDir, EC), DirEnd;
         Dir != DirEnd && !EC; Dir.increment(EC)) {
      // Parse YAML files in parallel.
      Pool.async(
          [&AddSymbols](std::string Path) {
            auto Buffer = llvm::MemoryBuffer::getFile(Path);
            if (!Buffer) {
              llvm::errs() << "Can't open " << Path << "\n";
              return;
            }
            std::vector<SymbolAndOccurrences> Symbols =
                clang::clangd::ReadFromYAML(Buffer.get()->getBuffer());
            // FIXME: Merge without creating such a heavy contention point.
            AddSymbols(Symbols);
          },
          Dir->path());
    }
  }
  WriteFile("index-source.yaml", Symbols);
  for (auto iter : Symbols) {
    iter.second.Occurrences.clear();
  }
  WriteFile("index-source-no-occurrences.yaml", Symbols);
  return true;
}

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  auto Executor = clang::tooling::createExecutorFromCommandLineArgs(
      argc, argv, IndexSourceCategory);

  if (!MergeDir.empty()) {
    llvm::errs() << "merging\n";
    Merge(MergeDir, "index-source.yaml");
    return 0;
    //auto Buffer =
        //llvm::MemoryBuffer::getFile("index-source-no-occurrences.yaml");
    //std::vector<SymbolAndOccurrences> Symbols =
                //clang::clangd::ReadFromYAML(Buffer.get()->getBuffer());
    //////for (auto &S : Symbols) {
      //////S.Occurrences.clear();
    //////}
    ////llvm::errs() << Symbols.size() << "\n";

     //std::error_code EC;
     //llvm::raw_fd_ostream OS("index-source-no-occurrences2.yaml", EC,
                             //llvm::sys::fs::F_None);
     //if (EC) {
       //llvm::errs() << "Can't open '"
                    //<< "': " << EC.message() << '\n';
       //return 1;
    //}
    //OS <<  clang::clangd::ToYAML(Symbols);
    return 0;
  }

  if (!Executor) {
    llvm::errs() << llvm::toString(Executor.takeError()) << "\n";
    return 1;
  }

  std::unique_ptr<clang::tooling::FrontendActionFactory> T(
      new clang::clangd::SymbolIndexActionFactory(
          *Executor->get()->getExecutionContext()));
  auto Err = Executor->get()->execute(std::move(T));
  if (Err) {
    llvm::errs() << llvm::toString(std::move(Err)) << "\n";
  }
  Executor->get()->getToolResults()->forEachResult(
      [](llvm::StringRef Key, llvm::StringRef Value) {
        int FD;
        SmallString<128> ResultPath;
        llvm::sys::fs::createUniqueFile(
            OutputDir + "/" + llvm::sys::path::filename(Key) + "-%%%%%%.yaml",
            FD, ResultPath);
        llvm::raw_fd_ostream OS(FD, /*shouldClose=*/true);
        OS << Value;
      });

  return 0;
}
