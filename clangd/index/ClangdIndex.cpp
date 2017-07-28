#include "ClangdIndex.h"
#include "ClangdIndexString.h"

#include <unordered_map>

namespace clang {
namespace clangd {

namespace {
template <class T, class... Args>
std::unique_ptr<T> getPtrOrNull(ClangdIndex &Index, RecordPointer Offset,
    Args &&... args) {
  RecordPointer Rec = Index.getStorage().getRecPtr(Offset);
  if (Rec == 0) {
    return {};
  }
  return llvm::make_unique<T>(Index, Rec, std::forward<Args>(args)...);
}
}

ClangdIndexSymbol::ClangdIndexSymbol(ClangdIndex &Index, USR Usr) :
    Index(Index), Storage(Index.getStorage()) {
  Record = Storage.mallocRecord(RECORD_SIZE);
  ClangdIndexString Str(Storage, Usr.c_str());
  Storage.putRecPtr(Record + USR_OFFSET, Str.getRecord());
  assert(!getFirstOccurrence());
}

ClangdIndexSymbol::ClangdIndexSymbol(ClangdIndex &Index, RecordPointer Record) :
    Record(Record), Index(Index), Storage(Index.getStorage()) {
  assert (Record >= ClangdIndexDataStorage::DATA_AREA);
}

std::string ClangdIndexSymbol::getUsr() {
  return ClangdIndexString(Storage, Storage.getRecPtr(Record + USR_OFFSET)).getString();
}

std::unique_ptr<ClangdIndexOccurrence> ClangdIndexSymbol::getFirstOccurrence() {
  return getPtrOrNull<ClangdIndexOccurrence>(Index, Record + FIRST_OCCURRENCE);
}

void ClangdIndexSymbol::setFirstOccurrence(ClangdIndexOccurrence &Occurrence) {
  Storage.putRecPtr(Record + FIRST_OCCURRENCE, Occurrence.getRecord());
}

void ClangdIndexSymbol::clearFirstOccurrence() {
  Storage.putRecPtr(Record + FIRST_OCCURRENCE, 0);
}

void ClangdIndexSymbol::addOccurrence(ClangdIndexOccurrence &Occurrence) {
  auto FirstOccurrence = getFirstOccurrence();
  setFirstOccurrence(Occurrence);
  if (FirstOccurrence) {
    Occurrence.setNextOccurrence(*FirstOccurrence);
  }
}

void ClangdIndexSymbol::removeOccurrences(std::set<RecordPointer> ToBeRemoved) {
  std::unique_ptr<ClangdIndexOccurrence> Prev;
  auto Occurrence = getFirstOccurrence();
  while (Occurrence) {
    auto NextOccurrence = Occurrence->getNextOccurrence();
    RecordPointer CurRec = Occurrence->getRecord();
    if (ToBeRemoved.find(CurRec) != ToBeRemoved.end()) {
      // We have to delete something!
      assert(getRecord() == Occurrence->getSymbol()->getRecord());
      // Handle when it is the first one. Find a new first if any.
      if (getFirstOccurrence()->getRecord() == CurRec) {
        if (NextOccurrence) {
          setFirstOccurrence(*NextOccurrence);
        } else {
          clearFirstOccurrence();
        }
      } else {
        assert(Prev);
        if (NextOccurrence) {
          Prev->setNextOccurrence(*NextOccurrence);
        } else {
          Prev->clearNextOccurrence();
        }
      }

      Occurrence->free();
    } else {
      Prev = std::move(Occurrence);
    }
    Occurrence = std::move(NextOccurrence);
  }

  if (!getFirstOccurrence()) {
    // No more occurrences? No reason to live anymore.
    free();
  }
}

void ClangdIndexSymbol::free() {
  assert(!Index.getSymbols(USR(getUsr())).empty());
  // FIXME: We should assert here, not in debug.
  Index.getSymbolBTree().remove(getRecord());
  // Free the string we allocated ourselves
  Storage.freeRecord(Storage.getRecPtr(Record + USR_OFFSET));
  Storage.freeRecord(Record);
}

ClangdIndexOccurrence::ClangdIndexOccurrence(ClangdIndex& Index, const ClangdIndexFile& File, ClangdIndexSymbol &Symbol,
    IndexSourceLocation LocStart, IndexSourceLocation LocEnd, index::SymbolRoleSet Roles) : Index(Index), Storage(Index.getStorage()) {
  Record = Storage.mallocRecord(RECORD_SIZE);
  Storage.putRecPtr(Record + SYMBOL_OFFSET, Symbol.getRecord());
  Storage.putRecPtr(Record + FILE_OFFSET, File.getRecord());
  Storage.putInt32(Record + LOC_START_OFFSET, LocStart);
  Storage.putInt32(Record + LOC_END_OFFSET, LocEnd);
  static_assert(index::SymbolRoleBitNum <= sizeof(uint32_t) * 8, "SymbolRoles cannot fit in uint32_t");
  Storage.putInt32(Record + ROLES_OFFSET, static_cast<uint32_t>(Roles));
}

ClangdIndexOccurrence::ClangdIndexOccurrence(ClangdIndex &Index, RecordPointer Record) :
    Record(Record), Index(Index), Storage(Index.getStorage()) {
  assert (Record >= ClangdIndexDataStorage::DATA_AREA);
}

std::unique_ptr<ClangdIndexSymbol> ClangdIndexOccurrence::getSymbol() {
  return getPtrOrNull<ClangdIndexSymbol>(Index, Record + SYMBOL_OFFSET);
}

std::string ClangdIndexOccurrence::getPath() {
  RecordPointer Rec = Storage.getRecPtr(Record + FILE_OFFSET);
  if (Rec == 0) {
    return {};
  }
  return ClangdIndexFile(Index, Rec).getPath();
}

std::unique_ptr<ClangdIndexOccurrence> ClangdIndexOccurrence::getNextInFile() {
  return getPtrOrNull<ClangdIndexOccurrence>(Index, Record + FILE_NEXT_OFFSET);
}

void ClangdIndexOccurrence::setNextInFile (ClangdIndexOccurrence &Occurrence) {
  Storage.putRecPtr(Record + FILE_NEXT_OFFSET, Occurrence.getRecord());
}

std::unique_ptr<ClangdIndexOccurrence> ClangdIndexOccurrence::getNextOccurrence() {
  return getPtrOrNull<ClangdIndexOccurrence>(Index, Record + SYMBOL_NEXT_OCCURENCE);
}

void ClangdIndexOccurrence::setNextOccurrence(ClangdIndexOccurrence &Occurrence) {
  Storage.putRecPtr(Record + SYMBOL_NEXT_OCCURENCE, Occurrence.getRecord());
}

void ClangdIndexOccurrence::clearNextOccurrence() {
  Storage.putRecPtr(Record + SYMBOL_NEXT_OCCURENCE, 0);
}

void ClangdIndexOccurrence::free() {
  Storage.freeRecord(Record);
}

ClangdIndexHeaderInclusion::ClangdIndexHeaderInclusion(ClangdIndex &Index,
    const ClangdIndexFile& IncludedByFile, const ClangdIndexFile& IncludedFile) :
    Index(Index), Storage(Index.getStorage()) {
  Record = Storage.mallocRecord(RECORD_SIZE);
  Storage.putRecPtr(Record + INCLUDED_BY_FILE, IncludedByFile.getRecord());
  Storage.putRecPtr(Record + INCLUDED_FILE, IncludedFile.getRecord());
}

ClangdIndexHeaderInclusion::ClangdIndexHeaderInclusion(ClangdIndex &Index,
    RecordPointer Record) :
    Record(Record), Index(Index), Storage(Index.getStorage()) {
}

std::unique_ptr<ClangdIndexFile> ClangdIndexHeaderInclusion::getIncluded () {
  return getPtrOrNull<ClangdIndexFile>(Index, Record + INCLUDED_FILE);
}

std::unique_ptr<ClangdIndexFile> ClangdIndexHeaderInclusion::getIncludedBy () {
  return getPtrOrNull<ClangdIndexFile>(Index, Record + INCLUDED_BY_FILE);
}

std::unique_ptr<ClangdIndexHeaderInclusion> ClangdIndexHeaderInclusion::getPrevIncludeBy() {
  return getPtrOrNull<ClangdIndexHeaderInclusion>(Index, Record + PREV_INCLUDED_BY);
}

std::unique_ptr<ClangdIndexHeaderInclusion> ClangdIndexHeaderInclusion::getNextIncludeBy() {
  return getPtrOrNull<ClangdIndexHeaderInclusion>(Index, Record + NEXT_INCLUDED_BY);
}

std::unique_ptr<ClangdIndexHeaderInclusion> ClangdIndexHeaderInclusion::getPrevInclusion() {
  return getPtrOrNull<ClangdIndexHeaderInclusion>(Index, Record + PREV_INCLUDES);
}

std::unique_ptr<ClangdIndexHeaderInclusion> ClangdIndexHeaderInclusion::getNextInclusion() {
  return getPtrOrNull<ClangdIndexHeaderInclusion>(Index, Record + NEXT_INCLUDES);
}

ClangdIndexFile::ClangdIndexFile(ClangdIndex &Index, std::string Path) :
    Path(Path), Index(Index), Storage(Index.getStorage()) {
  Record = Storage.mallocRecord(RECORD_SIZE);
  ClangdIndexString Str(Storage, Path);
  Storage.putRecPtr(Record + PATH, Str.getRecord());
}

ClangdIndexFile::ClangdIndexFile(ClangdIndex &Index, RecordPointer Record) :
    Record(Record), Index(Index), Storage(Index.getStorage()) {
  assert (Record >= ClangdIndexDataStorage::DATA_AREA);
}

const std::string& ClangdIndexFile::getPath() {
  if (Path.empty()) {
    std::string Str = ClangdIndexString(Storage,
        Storage.getRecPtr(Record + PATH)).getString();
    Path = Str;
  }
  return Path;
}

std::unique_ptr<ClangdIndexOccurrence> ClangdIndexFile::getFirstOccurrence() {
  return getPtrOrNull<ClangdIndexOccurrence>(Index, Record + FIRST_OCCURRENCE);
}

std::unique_ptr<ClangdIndexHeaderInclusion> ClangdIndexFile::getFirstIncludedBy() {
  return getPtrOrNull<ClangdIndexHeaderInclusion>(Index, Record + FIRST_INCLUDED_BY);
}

std::unique_ptr<ClangdIndexHeaderInclusion> ClangdIndexFile::getFirstInclusion() {
  return getPtrOrNull<ClangdIndexHeaderInclusion>(Index, Record + FIRST_INCLUSION);
}

void ClangdIndexFile::addOccurrence(ClangdIndexOccurrence &Occurrence) {
  auto FirstOccurrence = getFirstOccurrence();
  setFirstOccurrence(Occurrence.getRecord());
  if (FirstOccurrence) {
    Occurrence.setNextInFile(*FirstOccurrence);
  }
}

void ClangdIndexFile::setLastIndexingTime(
    std::chrono::nanoseconds LastIndexingTime) {
  auto Count = LastIndexingTime.count();
  RecordPointer Rec = Storage.getRecPtr(Record + LAST_INDEXING_TIME);
  if (Rec) {
    Storage.freeRecord(Rec);
  }
  // Note: The size of std::chrono::nanoseconds::rep might vary between systems
  Rec = Storage.mallocRecord(sizeof(std::chrono::nanoseconds::rep));
  Storage.putRecPtr(Record + LAST_INDEXING_TIME, Rec);
  Storage.putData(Rec, &Count, sizeof(std::chrono::nanoseconds::rep));
}

std::chrono::nanoseconds ClangdIndexFile::getLastIndexingTime() {
  RecordPointer Rec = Storage.getRecPtr(Record + LAST_INDEXING_TIME);
  if (!Rec) {
    return {};
  }
  std::chrono::nanoseconds::rep Count;
  Storage.getData(Rec, &Count, sizeof(std::chrono::nanoseconds::rep));
  return std::chrono::nanoseconds(Count);
}

void ClangdIndexFile::clearOccurrences() {
  std::unordered_map<RecordPointer, std::set<RecordPointer>> SymbolsToDeletedOccurrences;
  auto Occurrence = getFirstOccurrence();
  while (Occurrence) {
    auto NextOccurrence = Occurrence->getNextInFile();
    auto Symbol = Occurrence->getSymbol();
    if (Symbol) {
      SymbolsToDeletedOccurrences[Symbol->getRecord()].insert(Occurrence->getRecord());
    } else {
      llvm::errs() << "Warning: Deleting orphaned occurrence: " << Occurrence->getRecord() << "\n";
    }
    Occurrence = std::move(NextOccurrence);
  }
  for (auto &I : SymbolsToDeletedOccurrences) {
    ClangdIndexSymbol Sym(Index, I.first);
    auto USR1 = Sym.getUsr();
    assert(!USR1.empty());
    assert(!Index.getSymbols(USR(USR1)).empty());
  }
  for (auto &I : SymbolsToDeletedOccurrences) {
    ClangdIndexSymbol Sym(Index, I.first);
    Sym.removeOccurrences(I.second);
  }

  setFirstOccurrence(0);
}

void ClangdIndexFile::clearInclusions() {
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

void ClangdIndexFile::clearIncludedBys() {
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

void ClangdIndexFile::visitDependentFiles(std::function<bool(ClangdIndexFile&)> Visitor,
    ClangdIndexFile &File, std::set<RecordPointer> &VisitedFiles) {
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

void ClangdIndexFile::visitDependentFiles(std::function<bool(ClangdIndexFile&)> Visitor) {
  // Prevent infinite recursion with this set.
  std::set<RecordPointer> VisitedFiles;
  visitDependentFiles(Visitor, *this, VisitedFiles);
}

void ClangdIndexFile::visitInclusions(std::function<bool(ClangdIndexFile&)> Visitor,
    ClangdIndexFile &File, std::set<RecordPointer> &VisitedFiles) {
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

void ClangdIndexFile::visitInclusions(std::function<bool(ClangdIndexFile&)> Visitor) {
  //Prevent infinite recursion with this set.
  std::set<RecordPointer> VisitedFiles;
  visitInclusions(Visitor, *this, VisitedFiles);
}

void ClangdIndexFile::onChange() {
  clearOccurrences();
  clearInclusions();
  // Don't clear includedBys, those relation ships depend on the content of
  // other files, not this one. If we removed them, we would have to re-index
  // all the included-by files.
}

void ClangdIndexFile::free() {
  clearOccurrences();
  clearInclusions();
  clearIncludedBys();

  assert(Index.getFilesBTree().remove(getRecord()));
  Storage.freeRecord(Storage.getRecPtr(Record + PATH));
  Storage.freeRecord(Record);
}

namespace {
class SymbolUSRVisitor: public BTreeVisitor {
  USR Usr;
  ClangdIndex &Index;
  llvm::SmallVector<std::unique_ptr<ClangdIndexSymbol>, 1> Result;

public:
  SymbolUSRVisitor(USR Usr, ClangdIndex &Index) :
      Usr(Usr), Index(Index) {
  }

  int compare(RecordPointer Record) override {
    ClangdIndexSymbol Current(Index, Record);
    std::string CurrentUsr = Current.getUsr();
    return CurrentUsr.compare(Usr.c_str());
  }

  void visit(RecordPointer Record) override {
    std::unique_ptr<ClangdIndexSymbol> Current = llvm::make_unique<
        ClangdIndexSymbol>(Index, Record);
    Result.push_back(std::move(Current));
  }

  llvm::SmallVector<std::unique_ptr<ClangdIndexSymbol>, 1> getResult() {
    return std::move(Result);
  }
};
}

void ClangdIndex::addSymbol(ClangdIndexSymbol &Symbol) {
  SymbolBTree.insert(Symbol.getRecord());
}

llvm::SmallVector<std::unique_ptr<ClangdIndexSymbol>, 1> ClangdIndex::getSymbols(
    const USR& Buf) {
  SymbolUSRVisitor Visitor(Buf, *this);
  SymbolBTree.accept(Visitor);
  return Visitor.getResult();
}

llvm::SmallVector<std::unique_ptr<ClangdIndexOccurrence>, 1> ClangdIndex::getDefinitions(
      const USR& Buf) {
  return getOccurrences(Buf, static_cast<index::SymbolRoleSet>(index::SymbolRole::Definition));
}

llvm::SmallVector<std::unique_ptr<ClangdIndexOccurrence>, 1> ClangdIndex::getReferences(
      const USR& Buf) {
  return getOccurrences(Buf,
      static_cast<index::SymbolRoleSet>(index::SymbolRole::Reference)
          | static_cast<index::SymbolRoleSet>(index::SymbolRole::Declaration)
          | static_cast<index::SymbolRoleSet>(index::SymbolRole::Definition));
}

llvm::SmallVector<std::unique_ptr<ClangdIndexOccurrence>, 1> ClangdIndex::getOccurrences(const USR& Buf, index::SymbolRoleSet Roles) {
  auto Symbols = getSymbols(Buf);
  if (Symbols.empty()) {
    return {};
  }

  //FIXME: multiple symbols?
  auto Symbol = *Symbols.front();
  llvm::SmallVector<std::unique_ptr<ClangdIndexOccurrence>, 1> Result;
  auto Occurrence = Symbol.getFirstOccurrence();
  while (Occurrence) {
    auto NextOccurence = Occurrence->getNextOccurrence();
    if (Occurrence->getRoles() & Roles) {
      Result.push_back(std::move(Occurrence));
    }
    Occurrence = std::move(NextOccurence);
  }

  return Result;
}

namespace {
class FileVisitor: public BTreeVisitor {

  std::string FilePath;
  ClangdIndex &Index;
  std::unique_ptr<ClangdIndexFile> Result;

public:
  FileVisitor(std::string FilePath, ClangdIndex &Index) :
      FilePath(FilePath), Index(Index) {
  }

  int compare(RecordPointer Record) override {
    ClangdIndexFile Current(Index, Record);
    return Current.getPath().compare(FilePath);
  }

  void visit(RecordPointer Record) override {
    std::unique_ptr<ClangdIndexFile> Current = llvm::make_unique<
        ClangdIndexFile>(Index, Record);
    Result = std::move(Current);
  }

  std::unique_ptr<ClangdIndexFile> getResult() {
    return std::move(Result);
  }
};
}

ClangdIndex::ClangdIndex(std::string File) : File(File),
    Storage(File, VERSION), SymbolsUSRComparator(*this), SymbolBTree(Storage,
        SYMBOLS_TREE_OFFSET, SymbolsUSRComparator), FilesComparator(*this), FilesBTree(
        Storage, FILES_TREE_OFFSET, FilesComparator) {
}

std::unique_ptr<ClangdIndexFile> ClangdIndex::getFile(
    const std::string& FilePath) {
  assert(!FilePath.empty());
  FileVisitor FV(FilePath, *this);
  FilesBTree.accept(FV);
  return FV.getResult();
}

void ClangdIndex::dumpSymbolsTree() {
  getSymbolBTree().dump([this](RecordPointer Rec, llvm::raw_ostream &OS) {
      OS << ClangdIndexSymbol(*this, Rec).getUsr();
    }, llvm::errs());
}

void ClangdIndex::dumpFilesTree() {
  getFilesBTree().dump([this](RecordPointer Rec, llvm::raw_ostream &OS) {
      OS << ClangdIndexFile(*this, Rec).getPath();
    }, llvm::errs());
}

} // namespace clangd
} // namespace clang
