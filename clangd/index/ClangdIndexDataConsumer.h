//===--- ClangdIndexDataConsumer.h - Consumes index data for Clangd index--===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//


#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGDINDEXDATACONSUMER_H_
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGDINDEXDATACONSUMER_H_

#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Index/CodegenNameGenerator.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Index/USRGeneration.h"
#include "clang/Lex/Lexer.h"
#include "index/ClangdIndex.h"

#include <unordered_map>

namespace clang {
namespace clangd{

/// This only consumes function definitions at the moment
class ClangdIndexDataConsumer : public index::IndexDataConsumer {
  std::unique_ptr<index::CodegenNameGenerator> CGNameGen;
  ClangdIndex &Index;
  ClangdIndexFile &IndexFile;
  std::string MainFilePath;
  std::set<FileID> FilesBeingIndexed;
  std::set<std::string> FilePathsBeingIndexed;
  std::chrono::nanoseconds IndexingTime;
  SourceManager *SM;

  std::unordered_map<std::string, RecordPointer> IndexFileCache;
  std::unordered_map<std::string, RecordPointer> IndexSymbolCache;

  IndexSourceLocation createIndexSourceLocation(SourceLocation Loc, const SourceManager& SourceMgr, const LangOptions& LangOpts) {
    unsigned Offset = SourceMgr.getFileOffset(SourceMgr.getSpellingLoc(Loc));
    if (Offset > UINT32_MAX) {
      //FIXME is uint32_t considered not big enough??
      llvm::errs() << "Offset too big to fit in uint32_t" << "\n";
    }
    uint32_t Offset32 = Offset;
    return IndexSourceLocation(Offset32);
  }

  std::unique_ptr<ClangdIndexFile> getOrCreateIndexFile(const SourceManager &SM, FileID FID) {
    const FileEntry *F = SM.getFileEntryForID(FID);
    if (!F)
      return {};

    StringRef FilePath = F->tryGetRealPathName();
    if (FilePath.empty()) {
      llvm::errs()
          << llvm::format("File Path empty. Indexing %s",
              IndexFile.getPath().c_str()) << "\n";
      return {};
    }

    auto FileCacheIt = IndexFileCache.find(FilePath);
    if (FileCacheIt != IndexFileCache.end()) {
      return llvm::make_unique<ClangdIndexFile>(Index.getStorage(), FileCacheIt->second, Index);
    }

    auto File = Index.getFile(FilePath);
    if (!File) {
      File = llvm::make_unique<ClangdIndexFile>(Index.getStorage(), FilePath, Index);
      Index.addFile(*File);
    }
    IndexFileCache.insert( { FilePath, File->getRecord() });
    return File;
  }

  llvm::SmallVector<std::unique_ptr<ClangdIndexSymbol>, 1> getOrCreateSymbols(const USR& Buf) {
    llvm::SmallVector<std::unique_ptr<ClangdIndexSymbol>, 1> Result;
    std::string USRStr = Buf.str();
    auto SymbolCacheIt = IndexSymbolCache.find(USRStr);
    if (SymbolCacheIt != IndexSymbolCache.end()) {
      Result.push_back(llvm::make_unique<ClangdIndexSymbol>(Index.getStorage(), SymbolCacheIt->second, Index));
      return Result;
    }

    Result = Index.getSymbols(Buf);
    if (Result.empty()) {
      Result.push_back(llvm::make_unique<ClangdIndexSymbol>(Index.getStorage(), Buf, Index));
      Index.addSymbol(*Result.front());
    }
    IndexSymbolCache.insert( { USRStr, Result.front()->getRecord() });
    return Result;
  }

public:
  ClangdIndexDataConsumer(raw_ostream &OS, ClangdIndex &Index, ClangdIndexFile &IndexFile, std::chrono::nanoseconds IndexingTime) :
      Index(Index), IndexFile(IndexFile), MainFilePath(IndexFile.getPath()), IndexingTime(IndexingTime), SM(nullptr) {
    assert(!MainFilePath.empty());
  }

  void initialize(ASTContext &Ctx) override {
    CGNameGen.reset(new index::CodegenNameGenerator(Ctx));
    SM = &Ctx.getSourceManager();
  }

  bool handleDeclOccurence(const Decl *D, index::SymbolRoleSet Roles,
                           ArrayRef<index::SymbolRelation> Relations,
                           FileID FID, unsigned Offset,
                           index::IndexDataConsumer::ASTNodeInfo ASTNode) override {
    ASTContext &Ctx = D->getASTContext();

    SmallString<256> USRBuf;
    if (!index::generateUSRForDecl(D, USRBuf)) {
      SourceRange Range;
      //FIXME: Save full range of definitions. Need a new class in Index model?
      /*if (dyn_cast<FunctionDecl>(D)) {
        if (auto OrigD = dyn_cast<FunctionDecl>(ASTNode.OrigD)) {
          if (OrigD->isThisDeclarationADefinition()) {
            Range = OrigD->getSourceRange();
            isFunctionDefinition = true;
          }
        }
      }*/

      SourceLocation StartOfFileLoc = SM->getLocForStartOfFile(FID);
      SourceLocation StartLoc = StartOfFileLoc.getLocWithOffset(Offset);
      Range = SourceRange(StartLoc,
          Lexer::getLocForEndOfToken(StartLoc, 0, *SM, Ctx.getLangOpts()));

      if ((Roles
          & static_cast<index::SymbolRoleSet>(index::SymbolRole::Declaration))
          || (Roles
              & static_cast<index::SymbolRoleSet>(index::SymbolRole::Definition))
          || (Roles
              & static_cast<index::SymbolRoleSet>(index::SymbolRole::Reference))) {
        auto OccurrenceFile = getOrCreateIndexFile(*SM, SM->getFileID(Range.getBegin()));
        if (!OccurrenceFile) {
          return true;
        }

        //This is the logic to only index a given header once. We can only
        //start indexing a file if it has no symbol registered to the
        //file. We can only resume indexing (it already has symbols) if we
        //were already indexing it before in the same consumer.
        //A file is considered to be already indexing when it has the same
        //FileID for the same file path. Since different inclusions of the
        //same file have different FileIDs, it means we only index one
        //version of a given file.
        auto FilePath = OccurrenceFile->getPath();
        assert(!FilePath.empty());
        if (FilePathsBeingIndexed.find(FilePath) == FilePathsBeingIndexed.end()) {
          if (OccurrenceFile->getFirstOccurrence()) {
            // We should at least need to index the main file. If this trips
            // this means we starting indexing without clearing the symbols
            // first.
            assert(!SM->isInMainFile(Range.getBegin()));
            // File already has occurrences and we were not already indexing
            // it, skip it.
            return true;
          }
          // First time seeing this file, start indexing it.
          FilePathsBeingIndexed.insert(FilePath);
          assert(FilesBeingIndexed.find(FID) == FilesBeingIndexed.end());
          FilesBeingIndexed.insert(FID);
        } else if (FilesBeingIndexed.find(FID) == FilesBeingIndexed.end()) {
          // File already being indexed, included a second time.
          return true;
        }

        auto Symbols = getOrCreateSymbols(USRBuf);
        //FIXME: multiple symbols?
        auto Symbol = std::move(Symbols.front());
        IndexSourceLocation LocStart = createIndexSourceLocation(Range.getBegin(), *SM, Ctx.getLangOpts());
        IndexSourceLocation LocEnd = createIndexSourceLocation(Range.getEnd(), *SM, Ctx.getLangOpts());

        auto Occurrence =
            llvm::make_unique<ClangdIndexOccurrence>(Index.getStorage(), Index,
                *OccurrenceFile,
                *Symbol, LocStart, LocEnd,
                static_cast<index::SymbolRoleSet>(Roles));

        Symbol->addOccurrence(*Occurrence);
        OccurrenceFile->addOccurrence(*Occurrence);
      }
    }

    return true;
  }

  void finish() override {
    // Now that we've finished indexing the unit as a whole, set the indexing
    // time. If somehow things are aborted before we get to this point, the
    // unit will be re-indexed.
    for (auto FID : FilesBeingIndexed) {
      auto File = getOrCreateIndexFile(*SM, FID);
      assert(File);
      File->setLastIndexingTime(IndexingTime);
    }
  }
};

} // namespace clangd
} // namespace clang

#endif
