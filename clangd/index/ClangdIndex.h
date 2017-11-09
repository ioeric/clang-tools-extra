#ifndef CLANGD_INDEX_H_
#define CLANGD_INDEX_H_

#include "BTree.h"

#include "clang/Index/IndexSymbol.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Chrono.h"

#include <set>

namespace clang {
namespace clangd {

using USR = llvm::SmallString<256>;

using IndexSourceLocation = uint32_t;

class ClangdIndexFile;
class ClangdIndex;
class ClangdIndexOccurrence;

class ClangdIndexSymbol {

  const static OffsetInRecord USR_OFFSET = 0;
  const static OffsetInRecord FIRST_OCCURRENCE = USR_OFFSET + ClangdIndexDataStorage::PTR_SIZE;
  const static OffsetInRecord RECORD_SIZE  = FIRST_OCCURRENCE + ClangdIndexDataStorage::PTR_SIZE;

  RecordPointer Record;
  ClangdIndex &Index;
  ClangdIndexDataStorage &Storage;
public:
  ClangdIndexSymbol(ClangdIndexDataStorage &Storage, USR Usr, ClangdIndex& Index);
  ClangdIndexSymbol(ClangdIndexDataStorage &Storage, RecordPointer Record, ClangdIndex &Index);
  std::string getUsr();

  RecordPointer getRecord() const {
    return Record;
  }

  void addOccurrence(ClangdIndexOccurrence &Occurrence);
  void removeOccurrences(std::set<RecordPointer> ToBeRemoved);

  std::unique_ptr<ClangdIndexOccurrence> getFirstOccurrence();

  void free();

private:
  void setFirstOccurrence(ClangdIndexOccurrence &Occurrence);
  void clearFirstOccurrence();
};

class ClangdIndexOccurrence {
  // Which symbol this occurrence corresponds to
  const static OffsetInRecord SYMBOL_OFFSET = 0;

  /// Location
  const static OffsetInRecord FILE_OFFSET = SYMBOL_OFFSET + ClangdIndexDataStorage::PTR_SIZE;
  const static OffsetInRecord LOC_START_OFFSET = FILE_OFFSET + ClangdIndexDataStorage::PTR_SIZE;
  const static OffsetInRecord LOC_END_OFFSET = LOC_START_OFFSET + ClangdIndexDataStorage::INT32_SIZE;

  const static OffsetInRecord ROLES_OFFSET = LOC_END_OFFSET + ClangdIndexDataStorage::INT32_SIZE;

  // The next occurrence of the symbol in the same file
  const static OffsetInRecord FILE_NEXT_OFFSET = ROLES_OFFSET + ClangdIndexDataStorage::INT32_SIZE;
  // The next occurrence of the symbol, across any files
  const static OffsetInRecord SYMBOL_NEXT_OCCURENCE = FILE_NEXT_OFFSET + ClangdIndexDataStorage::PTR_SIZE;
  const static OffsetInRecord RECORD_SIZE  = SYMBOL_NEXT_OCCURENCE + ClangdIndexDataStorage::PTR_SIZE;

  RecordPointer Record;
  ClangdIndex &Index;
  ClangdIndexDataStorage &Storage;
public:
  ClangdIndexOccurrence(ClangdIndexDataStorage &Storage, ClangdIndex& Index, const ClangdIndexFile& File, ClangdIndexSymbol &Symbol,
      IndexSourceLocation LocStart, IndexSourceLocation LocEnd, index::SymbolRoleSet Roles);
  ClangdIndexOccurrence(ClangdIndexDataStorage &Storage, RecordPointer Record, ClangdIndex &Index);

  std::unique_ptr<ClangdIndexSymbol> getSymbol();
  std::string getPath();

  IndexSourceLocation getLocEnd() const {
    return Storage.getInt32(Record + LOC_END_OFFSET);
  }

  IndexSourceLocation getLocStart() const {
    return Storage.getInt32(Record + LOC_START_OFFSET);
  }

  index::SymbolRoleSet getRoles() {
    return static_cast<index::SymbolRoleSet>(Storage.getInt32(
        Record + ROLES_OFFSET));
  }

  //We don't really use this to look-up symbols but rather to delete them when
  //a file is deleted.
  std::unique_ptr<ClangdIndexOccurrence> getNextInFile();
  void setNextInFile (ClangdIndexOccurrence &Occurrence);
  std::unique_ptr<ClangdIndexOccurrence> getNextOccurrence();
  void setNextOccurrence(ClangdIndexOccurrence &Occurrence);

  RecordPointer getRecord() const {
    return Record;
  }

  void free();
  void clearNextOccurrence();
};

class ClangdIndexHeaderInclusion {
  const static int INCLUDED_BY_FILE = 0;
  const static int INCLUDED_FILE = INCLUDED_BY_FILE + ClangdIndexDataStorage::PTR_SIZE;
  const static int PREV_INCLUDED_BY = INCLUDED_FILE + ClangdIndexDataStorage::PTR_SIZE;
  const static int NEXT_INCLUDED_BY = PREV_INCLUDED_BY + ClangdIndexDataStorage::PTR_SIZE;
  const static int PREV_INCLUDES = NEXT_INCLUDED_BY + ClangdIndexDataStorage::PTR_SIZE;
  const static int NEXT_INCLUDES = PREV_INCLUDES + ClangdIndexDataStorage::PTR_SIZE;
  const static int RECORD_SIZE = NEXT_INCLUDES + ClangdIndexDataStorage::PTR_SIZE;

  RecordPointer Record;
  ClangdIndex &Index;
  ClangdIndexDataStorage &Storage;

public:
  ClangdIndexHeaderInclusion(ClangdIndexDataStorage &Storage,
      const ClangdIndexFile& IncludedByFile,
      const ClangdIndexFile& IncludedFile,
      ClangdIndex &Index);
  ClangdIndexHeaderInclusion(ClangdIndexDataStorage &Storage,
      RecordPointer Record, ClangdIndex &Index);

  void setPrevIncludedBy(RecordPointer Rec) {
    Storage.putRecPtr(Record + PREV_INCLUDED_BY, Rec);
  }
  void setNextIncludedBy(RecordPointer Rec) {
    Storage.putRecPtr(Record + NEXT_INCLUDED_BY, Rec);
  }
  void setPrevInclusion(RecordPointer Rec) {
    Storage.putRecPtr(Record + PREV_INCLUDES, Rec);
  }
  void setNextInclusion(RecordPointer Rec) {
    Storage.putRecPtr(Record + NEXT_INCLUDES, Rec);
  }
  std::unique_ptr<ClangdIndexHeaderInclusion> getPrevIncludeBy();
  std::unique_ptr<ClangdIndexHeaderInclusion> getNextIncludeBy();

  std::unique_ptr<ClangdIndexHeaderInclusion> getPrevInclusion();
  std::unique_ptr<ClangdIndexHeaderInclusion> getNextInclusion();

  std::unique_ptr<ClangdIndexFile> getIncluded();
  std::unique_ptr<ClangdIndexFile> getIncludedBy();
  RecordPointer getRecord() const {
    return Record;
  }
  void free() {
    Storage.freeRecord(Record);
  }
};

/**
 * Represents both headers and source files. There should be one
 * ClangdIndexFile per file on the file system.
 */
class ClangdIndexFile {

  const static int PATH = 0;
  const static int FIRST_OCCURRENCE  = PATH + ClangdIndexDataStorage::PTR_SIZE;
  const static int FIRST_INCLUDED_BY = FIRST_OCCURRENCE + ClangdIndexDataStorage::PTR_SIZE;
  const static int FIRST_INCLUSION = FIRST_INCLUDED_BY + ClangdIndexDataStorage::PTR_SIZE;
  const static int LAST_INDEXING_TIME = FIRST_INCLUSION + ClangdIndexDataStorage::PTR_SIZE;
  const static int RECORD_SIZE  = LAST_INDEXING_TIME + ClangdIndexDataStorage::PTR_SIZE;

  std::string Path;
  RecordPointer Record;
  ClangdIndex &Index;
  ClangdIndexDataStorage &Storage;
public:
  ClangdIndexFile(ClangdIndexDataStorage &Storage, std::string Path, ClangdIndex &Index);
  ClangdIndexFile(ClangdIndexDataStorage &Storage, RecordPointer Record, ClangdIndex &Index);

  const std::string& getPath();

  RecordPointer getRecord() const {
    return Record;
  }

  void addOccurrence(ClangdIndexOccurrence &Occurrence);

  void setFirstIncludedBy(RecordPointer Rec) {
    Storage.putRecPtr(Record + FIRST_INCLUDED_BY, Rec);
  }
  void setFirstInclusion(RecordPointer Rec) {
    Storage.putRecPtr(Record + FIRST_INCLUSION, Rec);
  }

  void setLastIndexingTime(std::chrono::nanoseconds LastIndexingTime);
  std::chrono::nanoseconds getLastIndexingTime();

  std::unique_ptr<ClangdIndexHeaderInclusion> getFirstIncludedBy();
  std::unique_ptr<ClangdIndexHeaderInclusion> getFirstInclusion();

  std::unique_ptr<ClangdIndexOccurrence> getFirstOccurrence();

  ClangdIndexDataStorage& getStorage() {
    return Storage;
  }
  void visitDependentFiles(std::function<bool(ClangdIndexFile&)> Visitor);
  void visitInclusions(std::function<bool(ClangdIndexFile&)> Visitor);
  void onChange();

  void free();

private:
  void setFirstOccurrence(RecordPointer Rec) {
    Storage.putRecPtr(Record + FIRST_OCCURRENCE, Rec);
  }

  void visitDependentFiles(std::function<bool(ClangdIndexFile&)> Visitor,
      ClangdIndexFile &File, std::set<RecordPointer> &VisitedFiles);
  void visitInclusions(std::function<bool(ClangdIndexFile&)> Visitor,
      ClangdIndexFile &File, std::set<RecordPointer> &VisitedFiles);

  void clearOccurrences();
  void clearInclusions();
  void clearIncludedBys();
};

class ClangdIndex {

  class FileComparator: public BTreeComparator {

    ClangdIndex &Index;

  public:
    FileComparator(ClangdIndex &Index) :
        Index(Index) {
    }

    int compare(RecordPointer Record1, RecordPointer Record2) override {
      ClangdIndexFile File1(Index.getStorage(), Record1, Index);
      ClangdIndexFile File2(Index.getStorage(), Record2, Index);
      return File1.getPath().compare(File2.getPath());
    }
  };

  class SymbolUSRComparator: public BTreeComparator {
    ClangdIndex &Index;

  public:
    SymbolUSRComparator(ClangdIndex &Index) :
        Index(Index) {
    }

    int compare(RecordPointer Record1, RecordPointer Record2) override {
      ClangdIndexSymbol Symbol1(Index.getStorage(), Record1, Index);
      ClangdIndexSymbol Symbol2(Index.getStorage(), Record2, Index);
      return Symbol1.getUsr().compare(Symbol2.getUsr());
    }
  };

  std::string File;
  ClangdIndexDataStorage Storage;
  SymbolUSRComparator SymbolsUSRComparator;
  BTree SymbolBTree;
  FileComparator FilesComparator;
  BTree FilesBTree;

  const static int VERSION = 1;

  const static int SYMBOLS_TREE_OFFSET = ClangdIndexDataStorage::DATA_AREA;
  const static int FILES_TREE_OFFSET = SYMBOLS_TREE_OFFSET + ClangdIndexDataStorage::PTR_SIZE;
  const static int FIRST_FILE_OFFSET;

public:
  ClangdIndex(std::string File);
  ~ClangdIndex() {
    flush();
  }

  void addFile(ClangdIndexFile &IndexFile) {
    FilesBTree.insert(IndexFile.getRecord());
  }

  void addSymbol(ClangdIndexSymbol &Symbol);

  llvm::SmallVector<std::unique_ptr<ClangdIndexSymbol>, 1> getSymbols(
      const USR& Buf);
  llvm::SmallVector<std::unique_ptr<ClangdIndexOccurrence>, 1> getDefinitions(
      const USR& Buf);
  llvm::SmallVector<std::unique_ptr<ClangdIndexOccurrence>, 1> getReferences(
      const USR& Buf);
  llvm::SmallVector<std::unique_ptr<ClangdIndexOccurrence>, 1> getOccurrences(const USR& Buf, index::SymbolRoleSet Roles);

  std::unique_ptr<ClangdIndexFile> getFile(const std::string& FilePath);

  void flush() {
    Storage.flush();
  }

  ClangdIndexDataStorage& getStorage() {
    return Storage;
  }

  BTree& getSymbolBTree() {
    return SymbolBTree;
  }

  BTree& getFilesBTree() {
    return FilesBTree;
  }

  // For troubleshooting
  void dumpSymbolsTree();
  void dumpFilesTree();
};

} // namespace clangd
} // namespace clang

#endif
