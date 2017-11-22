#include "ClangdIndexStoreIndexer.h"

#include "../../ClangdFileUtils.h"
#include "../../GlobalCompilationDatabase.h"

#include "clang/Basic/TargetInfo.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/PCHContainerOperations.h"
#include "clang/Frontend/Utils.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "indexstore/IndexStoreCXX.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Timer.h"

#include <unordered_set>
#include <vector>

namespace clang {
namespace clangd {


namespace {

bool isTimeStampChanged(indexstore::IndexUnitReader &UnitReader, const llvm::sys::fs::directory_entry& Entry) {
  auto Status = Entry.status();
  if (!Status)
    return true;

  auto ModifiedTime = Status->getLastModificationTime();
  auto SecDuration = std::chrono::duration_cast<std::chrono::seconds>(
      ModifiedTime.time_since_epoch());

  auto NsDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
      (ModifiedTime - SecDuration).time_since_epoch());

  timespec FileTs;
  FileTs.tv_sec = SecDuration.count();
  FileTs.tv_nsec = NsDuration.count();

  timespec UnitTime = UnitReader.getModificationTime();
  return UnitTime.tv_sec == FileTs.tv_sec ? UnitTime.tv_nsec < FileTs.tv_nsec : UnitTime.tv_sec < FileTs.tv_sec;
}

class TranslationUnitToBeIndexedCollector {
  bool CheckModified;
  indexstore::IndexStore &DataStore;
  ClangdIndexStoreIndexer &Indexer;
  std::vector<std::string> PathsToBeIndexed;
  std::unordered_set<std::string> PathsToBeIndexedSet;

public:
  TranslationUnitToBeIndexedCollector(bool CheckModified,
      indexstore::IndexStore &DataStore, ClangdIndexStoreIndexer &Indexer) :
      CheckModified(CheckModified), DataStore(DataStore), Indexer(Indexer) {
  }

  void visitPath(StringRef Path) {
    llvm::sys::fs::directory_entry Entry(Path);
    auto Status = Entry.status();
    if (!Status) {
      llvm::errs() << " Cannot stat file. Nothing to do. " << "\n";
      return;
    }
    llvm::sys::fs::file_type Type = Status->type();
    if (Type == llvm::sys::fs::file_type::directory_file)
      visitFolder(Path);
    else
      visitFile(Entry, CheckModified);
  }

  std::vector<std::string> takePaths() {
    return std::move(PathsToBeIndexed);
  }

private:

  void addFile(StringRef File) {
    if (PathsToBeIndexedSet.find(File) == PathsToBeIndexedSet.end()) {
      PathsToBeIndexedSet.insert(File);
      PathsToBeIndexed.push_back(File);
    }
  }

  void visitFile(llvm::sys::fs::directory_entry FileEntry, bool CheckModified) {
    StringRef File = FileEntry.path();
    assert(!File.empty());
    llvm::errs() << llvm::formatv("Visiting {0}\n", File.str());

    bool IsSourceFilePath = isSourceFilePath(File);
    bool IsHeaderFilePath = false;
    if (!IsSourceFilePath)
      IsHeaderFilePath = isHeaderFilePath(File);

    if (!IsSourceFilePath && !IsHeaderFilePath) {
      llvm::errs() << " Not a C-family file. Nothing to do. " << "\n";
      return;
    }

    std::unique_ptr<ClangdIndexStoreIndexer::IndexStoreFile> ExistingFile = Indexer.getFile(File);
    if (ExistingFile) {

      if (CheckModified) {
        if (IsHeaderFilePath) {
          // Check all units if their timestamps are more recent than the header
          bool HeaderChanged = false;
          ExistingFile->visitDependentFiles([this, &File, &HeaderChanged, &FileEntry](ClangdIndexStoreIndexer::IndexStoreFile &IndexFile) {
            StringRef Path = IndexFile.getPath();
            if (!isSourceFilePath(Path))
              return true;

            std::string Error;
            SmallString<256> NameBuf;
            //FIXME: This assumes the output file will just have ".o" appended which might not be true
            DataStore.getUnitNameFromOutputPath(Path.str() + ".o", NameBuf);
            indexstore::IndexUnitReader UnitReader(DataStore, NameBuf, Error);
            if (UnitReader && UnitReader.hasMainFile()) {
              StringRef MainFile = UnitReader.getMainFilePath();
              indexstore::IndexRecordReader RecordReader(DataStore, MainFile, Error);
              if (!isTimeStampChanged(UnitReader, FileEntry)) {
                llvm::errs() << " Timestamp the same for " << Path.str() << "\n";
                return true;
              }
              HeaderChanged = true;
              return false;
            }

            return true;
          });
          if (!HeaderChanged)
            return;
          llvm::errs() << " Timestamp *modified* for " << File.str() << "\n";
          ExistingFile->visitDependentFiles([this](ClangdIndexStoreIndexer::IndexStoreFile &IndexFile) {
            StringRef Path = IndexFile.getPath();
            llvm::errs() << " Considering dependent : " << Path << "\n";
            llvm::sys::fs::directory_entry Entry(Path);
            auto Status = Entry.status();
            if (!Status) {
              llvm::errs() << " Cannot stat file. Nothing to do. " << "\n";
              return true;
            }
            visitFile(Entry, false);
            return true;
          });

        } else {
          std::string Error;
          SmallString<256> NameBuf;
          //FIXME: This assumes the output file will just have ".o" appended which might not be true
          DataStore.getUnitNameFromOutputPath(File.str() + ".o", NameBuf);
          indexstore::IndexUnitReader UnitReader(DataStore, NameBuf, Error);
          if (UnitReader && UnitReader.hasMainFile()) {
            StringRef MainFile = UnitReader.getMainFilePath();
            indexstore::IndexRecordReader RecordReader(DataStore, MainFile, Error);
            if (!isTimeStampChanged(UnitReader, FileEntry)) {
              // TODO: Check content hash
              llvm::errs() << " Timestamp the same for " << File.str() << "\n";
              return;
            }
            llvm::errs() << " Timestamp *modified* for " << File.str() << "\n";
            ExistingFile->visitDependentFiles([this](ClangdIndexStoreIndexer::IndexStoreFile &IndexFile) {
              StringRef Path = IndexFile.getPath();
              llvm::errs() << " Considering dependent : " << Path << "\n";
              llvm::sys::fs::directory_entry Entry(Path);
              auto Status = Entry.status();
              if (!Status) {
                llvm::errs() << " Cannot stat file. Nothing to do. " << "\n";
                return true;
              }
              visitFile(Entry, false);
              return true;
            });
          }
        }
      }

      Indexer.getMappingsStorage().startWrite();
      ExistingFile->onChange();
      Indexer.getMappingsStorage().endWrite();
    }


    // Header content is indexed when a source file that includes it is being
    // indexed.
    if (IsSourceFilePath)
      addFile(File);
  }

  void visitFolder(StringRef Folder) {
    assert(!Folder.empty());
    std::error_code EC;
    for (llvm::sys::fs::directory_iterator I(Folder.str(), EC), E; I != E;
        I.increment(EC)) {
      if (EC)
        continue;
      const llvm::sys::fs::directory_entry& Entry = *I;
      auto Status = Entry.status();
      if (!Status)
        continue;
      llvm::sys::fs::file_type Type = Status->type();
      if (Type == llvm::sys::fs::file_type::directory_file)
        visitFolder(Entry.path());
      else
        visitFile(Entry, CheckModified);
    }
  }

};
}

std::unique_ptr<ClangdIndexStoreIndexer::IndexStoreSymbol> ClangdIndexStoreIndexer::getOrCreateSymbols(const std::string& Usr) {
  auto Symbol = getSymbol(Usr);
  if (Symbol)
    return Symbol;
  Symbol = llvm::make_unique<IndexStoreSymbol>(*MappingsDataStorage, Usr);
  Symbols->insert(Symbol->getRecord());
  return Symbol;
}

std::unique_ptr<ClangdIndexStoreIndexer::IndexStoreRecord> ClangdIndexStoreIndexer::getOrCreateIndexStoreRecord(StringRef RecordName) {
  auto Record = getRecord(RecordName);
  if (Record)
    return Record;
  Record = llvm::make_unique<IndexStoreRecord>(*MappingsDataStorage, RecordName);
  Records->insert(Record->getRecord());
  return Record;
}

std::unique_ptr<ClangdIndexStoreIndexer::IndexStoreFile> ClangdIndexStoreIndexer::getOrCreateIndexStoreFile(StringRef FilePath) {
  auto File = getFile(FilePath);
  if (File)
    return File;
  File = llvm::make_unique<IndexStoreFile>(*MappingsDataStorage, FilePath, *this);
  Files->insert(File->getRecord());
  return File;
}

void ClangdIndexStoreIndexer::collectOccurrences(StringRef File) {
  std::string Error;
  SmallString<256> NameBuf;
  //FIXME: This assumes the output file will just have ".o" appended which might not be true
  DataStore->getUnitNameFromOutputPath(File.str() + ".o", NameBuf);
  auto UnitReader = indexstore::IndexUnitReader(*DataStore, NameBuf, Error);
  if (!UnitReader) {
    llvm::errs() << "error opening unit for file '" << File << "': " << Error << '\n';
    return;
  }

  llvm::errs() << "Processing unit: " << NameBuf << "\n";
  using RelevanceInfo = std::unordered_map<std::string, index::SymbolRoleSet>;
  using UsrToSymbolRelevance = std::unordered_map<std::string, RelevanceInfo>;
  UsrToSymbolRelevance Cache;
  UnitReader.foreachDependency([&](indexstore::IndexUnitDependency Dep) -> bool {
    if (Dep.getKind() != indexstore::IndexUnitDependency::DependencyKind::Record)
      return true;

    StringRef RecordFile = Dep.getName();
    assert(!RecordFile.empty());
    std::string Error;
    auto RecordReader = indexstore::IndexRecordReader(*DataStore, RecordFile, Error);
    if (!RecordReader) {
      llvm::errs() << "failed reading record file: " << RecordFile << '\n';
      return true;
    }

    MappingsDataStorage->startWrite();
    auto Record = getOrCreateIndexStoreRecord(RecordFile);
    if (Record->getFullPath().empty()) {
      StringRef absPath;
      SmallString<128> absPathBuf;
      StringRef Path = Dep.getFilePath();
      auto workDir = UnitReader.getWorkingDirectory();
      if (llvm::sys::path::is_absolute(Path) || workDir.empty()) {
        absPath = Path;
      } else {
        absPathBuf = workDir;
        llvm::sys::path::append(absPathBuf, Path);
        absPath = absPathBuf.str();
      }

      SmallString<256> Result = absPath;
      llvm::sys::path::remove_dots(Result, true);
      Record->setFullPath(Result.str());
    }
    MappingsDataStorage->endWrite();

    RecordReader.foreachOccurrence([&](indexstore::IndexRecordOccurrence idxOccur) -> bool {
      assert(!idxOccur.getSymbol().getUSR().empty());
      Cache[idxOccur.getSymbol().getUSR()][RecordFile] |= idxOccur.getRoles();
      return true;
    });
    return true;
  });
  MappingsDataStorage->startWrite();
  for (auto &Item : Cache) {
    auto Symbol = getOrCreateSymbols(Item.first);
    auto & RelevanceInfos = Item.second;
    for (auto &RelevanceInfo : RelevanceInfos) {
      auto IndexStoreInfo = llvm::make_unique<IndexStoreSymbolRelevanceInfo>(*MappingsDataStorage, RelevanceInfo.first, RelevanceInfo.second);
      Symbol->addSymbolRelevanceInfo(*IndexStoreInfo);
    }
  }
  MappingsDataStorage->endWrite();
}

std::unique_ptr<ClangdIndexStoreIndexer::IndexStoreSymbol> ClangdIndexStoreIndexer::getSymbol(
    StringRef Usr) {
  SymbolUSRVisitor Visitor(Usr, *MappingsDataStorage);
  Symbols->accept(Visitor);
  return Visitor.getResult();
}

std::unique_ptr<ClangdIndexStoreIndexer::IndexStoreRecord> ClangdIndexStoreIndexer::getRecord(
    StringRef RecordName) {
  SymbolRecordNameVisitor Visitor(RecordName, *MappingsDataStorage);
  Records->accept(Visitor);
  return Visitor.getResult();
}

namespace {
class FileVisitor: public BTreeVisitor {

  std::string FilePath;
  ClangdIndexStoreIndexer &Indexer;
  std::unique_ptr<ClangdIndexStoreIndexer::IndexStoreFile> Result;

public:
  FileVisitor(std::string FilePath, ClangdIndexStoreIndexer &Indexer) :
      FilePath(FilePath), Indexer(Indexer) {
  }

  int compare(RecordPointer Record) override {
    ClangdIndexStoreIndexer::IndexStoreFile Current(Indexer.getMappingsStorage(), Record, Indexer);
    return Current.getPath().compare(FilePath);
  }

  void visit(RecordPointer Record) override {
    std::unique_ptr<ClangdIndexStoreIndexer::IndexStoreFile> Current = llvm::make_unique<
        ClangdIndexStoreIndexer::IndexStoreFile>(Indexer.getMappingsStorage(), Record, Indexer);
    Result = std::move(Current);
  }

  std::unique_ptr<ClangdIndexStoreIndexer::IndexStoreFile> getResult() {
    return std::move(Result);
  }
};
}

std::unique_ptr<ClangdIndexStoreIndexer::IndexStoreFile> ClangdIndexStoreIndexer::getFile(
    StringRef FilePath) {
  assert(!FilePath.empty());
  FileVisitor FV(FilePath, *this);
  Files->accept(FV);
  return FV.getResult();
}

void ClangdIndexStoreIndexer::IndexStoreFile::clearInclusions() {
  auto Inclusion = getFirstInclusion();
  while (Inclusion) {
    auto NextInclusion = Inclusion->getNextInclusion();
    auto Prev = Inclusion->getPrevIncludeBy();
    auto Next = Inclusion->getNextIncludeBy();
    RecordPointer NextRec = Next ? Next->getRecord() : 0;
    if (Prev) {
      Prev->setNextIncludedBy(NextRec);
      if (Next) {
        Next->setPrevIncludedBy(Prev->getRecord());
      }
    } else {
      Inclusion->getIncluded()->setFirstIncludedBy(NextRec);
      if (Next) {
        Next->setPrevIncludedBy(0);
      }
    }
    Inclusion->free();
    Inclusion = std::move(NextInclusion);
  }
  setFirstInclusion(0);
}

void ClangdIndexStoreIndexer::IndexStoreFile::clearIncludedBys() {
  auto IncludedBy = getFirstIncludedBy();
  while (IncludedBy) {
    auto NextIncludedBy = IncludedBy->getNextIncludeBy();
    auto Prev = IncludedBy->getPrevInclusion();
    auto Next = IncludedBy->getNextInclusion();
    RecordPointer NextRec = Next ? Next->getRecord() : 0;
    if (Prev) {
      Prev->setNextInclusion(NextRec);
      if (Next) {
        Next->setPrevInclusion(Prev->getRecord());
      }
    } else {
      IncludedBy->getIncludedBy()->setFirstInclusion(NextRec);
      if (Next) {
        Next->setPrevInclusion(0);
      }
    }
    IncludedBy->free();
    IncludedBy = std::move(NextIncludedBy);
  }
  setFirstIncludedBy(0);
}

void ClangdIndexStoreIndexer::IndexStoreFile::visitDependentFiles(std::function<bool(IndexStoreFile&)> Visitor,
    IndexStoreFile &File, std::set<RecordPointer> &VisitedFiles) {
  auto IncludedBy = File.getFirstIncludedBy();
  if (!IncludedBy) {
    return;
  }

  while (IncludedBy) {
    auto IncludedByFile = IncludedBy->getIncludedBy();
    assert(IncludedByFile && "inclusion pointing to non-existent file");

    if (VisitedFiles.find(IncludedByFile->getRecord()) != VisitedFiles.end())
      return;
    VisitedFiles.insert(IncludedByFile->getRecord());

    if (!Visitor(*IncludedByFile)) {
      return;
    }
    visitDependentFiles(Visitor, *IncludedByFile, VisitedFiles);
    IncludedBy = IncludedBy->getNextIncludeBy();
  }
}

void ClangdIndexStoreIndexer::IndexStoreFile::visitDependentFiles(std::function<bool(IndexStoreFile&)> Visitor) {
  // Prevent infinite recursion with this set.
  std::set<RecordPointer> VisitedFiles;
  visitDependentFiles(Visitor, *this, VisitedFiles);
}

void ClangdIndexStoreIndexer::IndexStoreFile::visitInclusions(std::function<bool(IndexStoreFile&)> Visitor,
    IndexStoreFile &File, std::set<RecordPointer> &VisitedFiles) {
  auto Inclusion = File.getFirstInclusion();
  if (!Inclusion) {
    return;
  }

  while (Inclusion) {
    auto IncludedFile = Inclusion->getIncluded();
    assert(IncludedFile && "inclusion pointing to non-existent file");

    if (VisitedFiles.find(IncludedFile->getRecord()) != VisitedFiles.end())
      return;
    VisitedFiles.insert(IncludedFile->getRecord());

    if (!Visitor(*IncludedFile)) {
      return;
    }

    visitInclusions(Visitor, *IncludedFile, VisitedFiles);
    Inclusion = Inclusion->getNextInclusion();
  }
}

void ClangdIndexStoreIndexer::IndexStoreFile::visitInclusions(std::function<bool(IndexStoreFile&)> Visitor) {
  //Prevent infinite recursion with this set.
  std::set<RecordPointer> VisitedFiles;
  visitInclusions(Visitor, *this, VisitedFiles);
}

void ClangdIndexStoreIndexer::IndexStoreFile::onChange() {
  clearInclusions();
  // Don't clear includedBys, those relation ships depend on the content of
  // other files, not this one. If we removed them, we would have to re-index
  // all the included-by files.
}

void ClangdIndexStoreIndexer::IndexStoreFile::free() {
  clearInclusions();
  clearIncludedBys();

  Indexer.removeFile(*this);
  Storage.freeRecord(Storage.getRecPtr(Record + PATH));
  Storage.freeRecord(Record);
}

class RecordDepencencyAction : public ASTFrontendAction {
  ClangdIndexStoreIndexer &Indexer;

  void addInclusion(ClangdIndexStoreIndexer::IndexStoreHeaderInclusion& Inclusion, ClangdIndexStoreIndexer::IndexStoreFile& IncludedByFile) {
    auto FirstInclusion = IncludedByFile.getFirstInclusion();
    IncludedByFile.setFirstInclusion(Inclusion.getRecord());
    if (FirstInclusion) {
      Inclusion.setNextInclusion(FirstInclusion->getRecord());
      FirstInclusion->setPrevInclusion(Inclusion.getRecord());
    }
  }

  bool includedByExists(ClangdIndexStoreIndexer::IndexStoreFile &IncludedFile, ClangdIndexStoreIndexer::IndexStoreFile &IncludedByFile) {
    auto IncludedBySearch = IncludedFile.getFirstIncludedBy();
    while (IncludedBySearch) {
      auto IncludedByFileSearch = IncludedBySearch->getIncludedBy();
      if (IncludedByFile.getPath().compare(IncludedByFileSearch->getPath()) == 0)
        return true;

      IncludedBySearch = IncludedBySearch->getNextIncludeBy();
    }
    return false;
  }

protected:
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                         StringRef InFile) override {
    class NullASTConsumer : public ASTConsumer {
    };
    return llvm::make_unique<NullASTConsumer>();
  }

  void EndSourceFileAction() override {
    Indexer.getMappingsStorage().startWrite();
    SourceManager &SM = getCompilerInstance().getSourceManager();
    SmallVector<SourceLocation, 10> InclusionStack;
    const unsigned n =  SM.local_sloc_entry_size();
    for (unsigned i = 0 ; i < n ; ++i) {
      bool Invalid = false;
      const SrcMgr::SLocEntry &SL = SM.getLocalSLocEntry(i, &Invalid);

      if (!SL.isFile() || Invalid)
        continue;

      const SrcMgr::FileInfo &FI = SL.getFile();
      if (FI.getIncludeLoc().isInvalid() || !FI.getContentCache()->OrigEntry)
        continue;

      StringRef FilePath = FI.getContentCache()->OrigEntry->tryGetRealPathName();
      //Probably the included file doesn't exist?
      if (FilePath.empty())
        continue;

      SourceLocation L = FI.getIncludeLoc();
      // Build the inclusion stack.
      InclusionStack.clear();
      while (L.isValid()) {
        PresumedLoc PLoc = SM.getPresumedLoc(L);
        InclusionStack.push_back(L);
        L = PLoc.isValid()? PLoc.getIncludeLoc() : SourceLocation();
      }

//      const bool HEADER_LOGGING = true;
//      if (HEADER_LOGGING) {
//        for (size_t i =0; i < InclusionStack.size(); i++) {
//          llvm::errs() << " ";
//        }
//        llvm::errs() << "at " << SM.getFilename(InclusionStack.front()).str() << ":" << SM.getSpellingLineNumber(InclusionStack.front(), &Invalid) << "\n";
//        for (size_t i =0; i < InclusionStack.size(); i++) {
//          llvm::errs() << " ";
//        }
//        llvm::errs() << "include: " << FilePath << " stack: " << InclusionStack.size() << "\n";
//      }

      auto IncludedByEntry = SM.getFileEntryForID(SM.getFileID(InclusionStack.front()));
      if (!IncludedByEntry) {
        llvm::errs() << llvm::formatv("File including \"{0}\" does not exist.\n", FilePath);
        continue;
      }
      StringRef IncludedByName = IncludedByEntry->tryGetRealPathName();
      if (IncludedByName.empty()) {
        llvm::errs() << llvm::formatv("File including \"{0}\" does not exist.\n", FilePath);
        continue;
      }

      auto IncludedByFile = Indexer.getOrCreateIndexStoreFile(IncludedByName);

      // Store the included file -> included by relationship
      auto IncludedFile = Indexer.getOrCreateIndexStoreFile(FilePath);
      if (!includedByExists(*IncludedFile, *IncludedByFile)) {
        auto Inclusion = llvm::make_unique<ClangdIndexStoreIndexer::IndexStoreHeaderInclusion>(Indexer.getMappingsStorage(), *IncludedByFile, *IncludedFile, Indexer);
        auto FirstInclusion = IncludedFile->getFirstIncludedBy();
        IncludedFile->setFirstIncludedBy(Inclusion->getRecord());
        if (FirstInclusion) {
          Inclusion->setNextIncludedBy(FirstInclusion->getRecord());
          FirstInclusion->setPrevIncludedBy(Inclusion->getRecord());
        }
        addInclusion(*Inclusion, *IncludedByFile);
      }
    }
    Indexer.getMappingsStorage().endWrite();
  }
public:
  RecordDepencencyAction(ClangdIndexStoreIndexer &Indexer) : Indexer(Indexer) {
  }
};

void ClangdIndexStoreIndexer::indexFile(StringRef File) {
  llvm::errs() << " Indexing " << File.str() << "\n";
  assert(!isHeaderFilePath(File) && "Only source files should be indexed.");

  std::vector<tooling::CompileCommand> Commands;
  Commands = CDB.getCompileCommands(File);
  // chdir. This is thread hostile.
  if (!Commands.empty())
    llvm::sys::fs::set_current_path(Commands.front().Directory);

  if (Commands.empty()) {
    // Add a fake command line if we know nothing.
    Commands.push_back(tooling::CompileCommand(
        llvm::sys::path::parent_path(File), llvm::sys::path::filename(File),
        {"clang", "-fsyntax-only", File.str()}, ""));
  }

  // Inject the resource dir.
  // FIXME: Don't overwrite it if it's already there.
  static int Dummy; // Just an address in this process.
  std::string ResourceDir =
      CompilerInvocation::GetResourcesPath("clangd", (void *)&Dummy);
  Commands.front().CommandLine.push_back("-resource-dir=" + ResourceDir);
  std::vector<const char *> ArgStrs;
  for (const auto &S : Commands.front().CommandLine)
    ArgStrs.push_back(S.c_str());

  auto CInvok = createInvocationFromCommandLine(ArgStrs, nullptr);
  if (!CInvok)
    return;

  CInvok->getFrontendOpts().IndexStorePath = RootPath;
  auto DependencyAction = llvm::make_unique<RecordDepencencyAction>(*this);
  auto IndexAction = index::createIndexDataRecordingAction(CInvok->getFrontendOpts(), /*WrappedAction=*/std::move(DependencyAction));

  auto PCHContainerOps = std::make_shared<PCHContainerOperations>();
  auto Clang = llvm::make_unique<CompilerInstance>(PCHContainerOps);
  Clang->setInvocation(std::move(CInvok));
  Clang->createDiagnostics();
  Clang->setTarget(TargetInfo::CreateTargetInfo(
      Clang->getDiagnostics(), Clang->getInvocation().TargetOpts));
  if (!Clang->hasTarget())
    return;
  Clang->ExecuteAction(*IndexAction);

  collectOccurrences(File);
}

const int ClangdIndexStoreIndexer::SYMBOLS_TREE_OFFSET = ClangdIndexDataStorage::DATA_AREA;
const int ClangdIndexStoreIndexer::RECORDS_TREE_OFFSET = ClangdIndexStoreIndexer::SYMBOLS_TREE_OFFSET + ClangdIndexDataStorage::PTR_SIZE;
const int ClangdIndexStoreIndexer::FILES_TREE_OFFSET = ClangdIndexStoreIndexer::RECORDS_TREE_OFFSET + ClangdIndexDataStorage::PTR_SIZE;

ClangdIndexStoreIndexer::ClangdIndexStoreIndexer(std::string RootPath,
    GlobalCompilationDatabase& CDB) :
    RootPath(RootPath), CDB(CDB) {
  std::string Error;
  DataStore = llvm::make_unique<indexstore::IndexStore>(RootPath, Error);
  SmallString<32> Filename(RootPath);
  llvm::sys::path::append(Filename, "indexstore.index");
  MappingsDataStorage = llvm::make_unique<ClangdIndexDataStorage>(Filename.str(), 1);
  SymbolsUSRComparator = llvm::make_unique<SymbolUSRComparator>(*MappingsDataStorage);
  RecordNamesComparator = llvm::make_unique<RecordNameComparator>(*MappingsDataStorage);
  FilesComparator = llvm::make_unique<FileComparator>(*this);
  Symbols = llvm::make_unique<BTree>(*MappingsDataStorage, SYMBOLS_TREE_OFFSET, *SymbolsUSRComparator);
  Records = llvm::make_unique<BTree>(*MappingsDataStorage, RECORDS_TREE_OFFSET, *RecordNamesComparator);
  Files = llvm::make_unique<BTree>(*MappingsDataStorage, FILES_TREE_OFFSET, *FilesComparator);
}

void ClangdIndexStoreIndexer::indexRoot() {
  assert(DataStore);
  auto IndexTotalTimer = llvm::Timer("index time", "Indexing Total Time");
  IndexTotalTimer.startTimer();
  TranslationUnitToBeIndexedCollector C(true, *DataStore, *this);
  C.visitPath(RootPath);
  auto FilesToIndex = C.takePaths();
  llvm::errs() << llvm::format("Indexing %u source files.", FilesToIndex.size()) << "\n";
  unsigned I = 0;
  for (auto TU : FilesToIndex) {
    llvm::errs() << llvm::format("(%u/%u) ", I, FilesToIndex.size());
    indexFile(TU);
    I++;
  }
  IndexTotalTimer.stopTimer();
}

bool deleteUnit(indexstore::IndexStore &DataStore, StringRef UnitName) {
  std::string Error;
  SmallString<256> NameBuf;
  //FIXME: This assumes the output file will just have ".o" appended which might not be true
  DataStore.getUnitNameFromOutputPath(UnitName.str() + ".o", NameBuf);
  auto UnitReader = indexstore::IndexUnitReader(DataStore, NameBuf, Error);
  if (!UnitReader) {
    llvm::errs() << "error opening unit file '" << UnitName << "': " << Error << '\n';
    return false;
  }

  llvm::errs() << "Deleting unit: " << UnitName << "\n";

  bool Succ = UnitReader.foreachDependency([&](indexstore::IndexUnitDependency dep) -> bool {
        switch (dep.getKind()) {
          case indexstore::IndexUnitDependency::DependencyKind::Unit: {
            break;
          }
          case indexstore::IndexUnitDependency::DependencyKind::Record: {
            StringRef RecordFile = dep.getName();
            DataStore.discardRecord(RecordFile);
            break;
          }
          case indexstore::IndexUnitDependency::DependencyKind::File: {
            break;
          }
        }
        return true;
      });
  if (!Succ)
    return false;

  DataStore.discardUnit(UnitName);
  return true;
}

void ClangdIndexStoreIndexer::reindex() {
  bool Succ = DataStore->foreachUnit(/*sorted=*/false,
      [&](StringRef UnitName) -> bool {
        return deleteUnit(*DataStore, UnitName);
      });
  if (!Succ)
    llvm::errs() << "Failed to delete index store.\n";

  indexRoot();
}

void ClangdIndexStoreIndexer::onFileEvent(FileEvent Event) {
  assert(DataStore);
  switch (Event.type) {
  case FileChangeType::Created:
  case FileChangeType::Changed:
  case FileChangeType::Deleted:
    TranslationUnitToBeIndexedCollector C(true, *DataStore, *this);
    C.visitPath(Event.Path);
    auto FilesToIndex = C.takePaths();

    if (Event.type == FileChangeType::Deleted) {
      deleteUnit(*DataStore, Event.Path);
      auto FileInIndex = getFile(Event.Path);
      if (!FileInIndex) {
        llvm::errs() << " Unknown to index. Nothing to do. " << "\n";
        return;
      }
      MappingsDataStorage->startWrite();
      FileInIndex->free();
      MappingsDataStorage->endWrite();

      // Don't index the file that just got deleted but re-index its dependents.
      std::remove(std::begin(FilesToIndex), std::end(FilesToIndex), Event.Path);
    }

    llvm::errs() << llvm::format("Indexing %u source files.\n", FilesToIndex.size());
    for (auto TU : FilesToIndex) {
      indexFile(TU);
    }
  }
}

namespace {

class ClangdIndexDataOccurrenceImpl : public ClangdIndexDataOccurrence {
  std::string Path;
  unsigned Line;
  unsigned Column;

  uint32_t getOffsetFromLineCol(SourceManager &SM) {
    auto FileEntry = SM.getFileManager().getFile(Path);
    if (FileEntry) {
      auto FID = SM.getOrCreateFileID(FileEntry, SrcMgr::C_User);
      auto Loc = SM.translateLineCol(FID, Line, Column);;
      if (Loc.isValid()) {
        //FIXME: Check return value.
        return SM.getFileOffset(SM.getSpellingLoc(Loc));
      }
    }

    return 0;
  }

public:
  ClangdIndexDataOccurrenceImpl(std::string Path, uint32_t Line, uint32_t Column) : Path(Path), Line(Line), Column(Column) {}
  std::string getPath() { return Path; }
  uint32_t getStartOffset(SourceManager &SM) { return getOffsetFromLineCol(SM); }
  uint32_t getEndOffset(SourceManager &SM) { return getOffsetFromLineCol(SM); }
};
}

void ClangdIndexStoreIndexer::foreachOccurrence(
    const USR& Usr, index::SymbolRoleSet Roles,
    llvm::function_ref<bool(ClangdIndexDataOccurrence&)> Receiver) {

  auto Symbol = getSymbol(Usr.str());
  if (!Symbol) {
    llvm::errs() << "Could not find symbol '" << Usr << '\n';
    return;
  }
  llvm::SmallVector<std::string, 1> RecordNames;
  auto RelevanceInf = Symbol->getFirstSymbolRelevanceInfo();
  while (RelevanceInf) {
    if (RelevanceInf->getRoles() & Roles) {
      RecordNames.push_back(RelevanceInf->getUnitName());
    }
    RelevanceInf = RelevanceInf->getNextSymbolRelevanceInfo();
  }

  for (auto & RecordName : RecordNames) {
    std::string error;
    auto recordReader = indexstore::IndexRecordReader(*DataStore, RecordName, error);
    if (!recordReader) {
      llvm::errs() << "failed reading record file: " << RecordName << '\n';
      continue;
    }

    auto Record = getRecord(RecordName);
    if (!Record) {
      llvm::errs() << "failed reading record from mappings storage: " << RecordName << '\n';
      continue;
    }
    auto FullPath = Record->getFullPath();

    recordReader.foreachOccurrence([&](indexstore::IndexRecordOccurrence idxOccur) -> bool {
     if (idxOccur.getSymbol().getUSR() == Usr &&
         (static_cast<index::SymbolRoleSet>(idxOccur.getRoles()) & Roles)) {
       unsigned Line, Column;
       std::tie(Line, Column) = idxOccur.getLineCol();
       llvm::errs() << "Found definition at " << Line << ":" << Column << "\n";
       ClangdIndexDataOccurrenceImpl Occ(FullPath, Line, Column);
       Receiver(Occ);
     }
     return true;
    });
  }
}

void ClangdIndexStoreIndexer::dumpIncludedBy(StringRef File) {
  auto IndexFile = getFile(File);
  if (!IndexFile) {
    llvm::errs() << " Unknown to index. Nothing to do. " << "\n";
    return;
  }
  IndexFile->visitDependentFiles([](IndexStoreFile &File) {
    //FIXME: Add some nice indentation
    llvm::errs() << File.getPath() << "\n";
    return true;
  });
}

void ClangdIndexStoreIndexer::dumpInclusions(StringRef File) {
  auto IndexFile = getFile(File);
  if (!IndexFile) {
    llvm::errs() << " Unknown to index. Nothing to do. " << "\n";
    return;
  }

  IndexFile->visitInclusions([](IndexStoreFile &File) {
    //FIXME: Add some nice indentation
    llvm::errs() << File.getPath() << "\n";
    return true;
  });
}

} /* namespace clangd */
} /* namespace clang */
