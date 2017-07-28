//===--- ClangdServer.cpp - Main clangd server code --------------*- C++-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===-------------------------------------------------------------------===//

#include "ClangdServer.h"
#include <index/ClangdIndexDataConsumer.h>
#include "clang/Format/Format.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Index/IndexDataConsumer.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <llvm/Support/Timer.h>
#include <future>
#include <functional>
#include <unordered_set>

using namespace clang;
using namespace clang::clangd;

StringRef SourceExtensions[] = {".cpp", ".c", ".cc", ".cxx",
                                ".c++", ".m", ".mm"};
StringRef HeaderExtensions[] = {".h", ".hh", ".hpp", ".hxx", ".inc"};

namespace {

class FulfillPromiseGuard {
public:
  FulfillPromiseGuard(std::promise<void> &Promise) : Promise(Promise) {}

  ~FulfillPromiseGuard() { Promise.set_value(); }

private:
  std::promise<void> &Promise;
};

std::vector<tooling::Replacement> formatCode(StringRef Code, StringRef Filename,
                                             ArrayRef<tooling::Range> Ranges) {
  // Call clang-format.
  // FIXME: Don't ignore style.
  format::FormatStyle Style = format::getLLVMStyle();
  auto Result = format::reformat(Style, Code, Ranges, Filename);

  return std::vector<tooling::Replacement>(Result.begin(), Result.end());
}

std::string getStandardResourceDir() {
  static int Dummy; // Just an address in this process.
  return CompilerInvocation::GetResourcesPath("clangd", (void *)&Dummy);
}

} // namespace

size_t clangd::positionToOffset(StringRef Code, Position P) {
  size_t Offset = 0;
  for (int I = 0; I != P.line; ++I) {
    // FIXME: \r\n
    // FIXME: UTF-8
    size_t F = Code.find('\n', Offset);
    if (F == StringRef::npos)
      return 0; // FIXME: Is this reasonable?
    Offset = F + 1;
  }
  return (Offset == 0 ? 0 : (Offset - 1)) + P.character;
}

/// Turn an offset in Code into a [line, column] pair.
Position clangd::offsetToPosition(StringRef Code, size_t Offset) {
  StringRef JustBefore = Code.substr(0, Offset);
  // FIXME: \r\n
  // FIXME: UTF-8
  int Lines = JustBefore.count('\n');
  int Cols = JustBefore.size() - JustBefore.rfind('\n') - 1;
  return {Lines, Cols};
}

Tagged<IntrusiveRefCntPtr<vfs::FileSystem>>
RealFileSystemProvider::getTaggedFileSystem(PathRef File) {
  return make_tagged(vfs::getRealFileSystem(), VFSTag());
}

unsigned clangd::getDefaultAsyncThreadsCount() {
  unsigned HardwareConcurrency = std::thread::hardware_concurrency();
  // C++ standard says that hardware_concurrency()
  // may return 0, fallback to 1 worker thread in
  // that case.
  if (HardwareConcurrency == 0)
    return 1;
  return HardwareConcurrency;
}

ClangdScheduler::ClangdScheduler(unsigned AsyncThreadsCount)
    : RunSynchronously(AsyncThreadsCount == 0) {
  if (RunSynchronously) {
    // Don't start the worker thread if we're running synchronously
    return;
  }

  Workers.reserve(AsyncThreadsCount);
  for (unsigned I = 0; I < AsyncThreadsCount; ++I) {
    Workers.push_back(std::thread([this]() {
      while (true) {
        UniqueFunction<void()> Request;

        // Pick request from the queue
        {
          std::unique_lock<std::mutex> Lock(Mutex);
          // Wait for more requests.
          RequestCV.wait(Lock,
                         [this] { return !RequestQueue.empty() || Done; });
          if (Done)
            return;

          assert(!RequestQueue.empty() && "RequestQueue was empty");

          // We process requests starting from the front of the queue. Users of
          // ClangdScheduler have a way to prioritise their requests by putting
          // them to the either side of the queue (using either addToEnd or
          // addToFront).
          Request = std::move(RequestQueue.front());
          RequestQueue.pop_front();
        } // unlock Mutex

        Request();
      }
    }));
  }
}

ClangdScheduler::~ClangdScheduler() {
  if (RunSynchronously)
    return; // no worker thread is running in that case

  {
    std::lock_guard<std::mutex> Lock(Mutex);
    // Wake up the worker thread
    Done = true;
  } // unlock Mutex
  RequestCV.notify_all();

  for (auto &Worker : Workers)
    Worker.join();
}

ClangdServer::ClangdServer(GlobalCompilationDatabase &CDB,
                           DiagnosticsConsumer &DiagConsumer,
                           FileSystemProvider &FSProvider,
                           unsigned AsyncThreadsCount,
                           clangd::CodeCompleteOptions CodeCompleteOpts,
                           clangd::Logger &Logger,
                           llvm::Optional<StringRef> ResourceDir)
    : Logger(Logger), CDB(CDB), DiagConsumer(DiagConsumer),
      FSProvider(FSProvider),
      ResourceDir(ResourceDir ? ResourceDir->str() : getStandardResourceDir()),
      PCHs(std::make_shared<PCHContainerOperations>()),
      CodeCompleteOpts(CodeCompleteOpts), WorkScheduler(AsyncThreadsCount) {}

void ClangdServer::setRootPath(PathRef RootPath) {
  std::string NewRootPath = llvm::sys::path::convert_to_slash(
      RootPath, llvm::sys::path::Style::posix);
  if (llvm::sys::fs::is_directory(NewRootPath))
    this->RootPath = NewRootPath;

  if (!this->RootPath)
    return;

  SmallString<32> Filename(*this->RootPath);
  llvm::sys::path::append(Filename, "clangd.index");
  assert (!Index);
  // TODO, check version, etc
  bool FromScratch = !llvm::sys::fs::exists(Filename);
  Index = std::make_shared<ClangdIndex>(Filename.str());
  CDB.setIndex(Index);
  indexRoot(/*checkModified=*/!FromScratch);
}

std::future<void> ClangdServer::addDocument(PathRef File, StringRef Contents) {
  DocVersion Version = DraftMgr.updateDraft(File, Contents);

  auto TaggedFS = FSProvider.getTaggedFileSystem(File);
  std::shared_ptr<CppFile> Resources =
      Units.getOrCreateFile(File, ResourceDir, CDB, PCHs, Logger);
  return scheduleReparseAndDiags(File, VersionedDraft{Version, Contents.str()},
                                 std::move(Resources), std::move(TaggedFS));
}

std::future<void> ClangdServer::removeDocument(PathRef File) {
  DraftMgr.removeDraft(File);
  std::shared_ptr<CppFile> Resources = Units.removeIfPresent(File);
  return scheduleCancelRebuild(std::move(Resources));
}

std::future<void> ClangdServer::forceReparse(PathRef File) {
  auto FileContents = DraftMgr.getDraft(File);
  assert(FileContents.Draft &&
         "forceReparse() was called for non-added document");

  auto TaggedFS = FSProvider.getTaggedFileSystem(File);
  auto Recreated = Units.recreateFileIfCompileCommandChanged(File, ResourceDir,
                                                             CDB, PCHs, Logger);

  // Note that std::future from this cleanup action is ignored.
  scheduleCancelRebuild(std::move(Recreated.RemovedFile));
  // Schedule a reparse.
  return scheduleReparseAndDiags(File, std::move(FileContents),
                                 std::move(Recreated.FileInCollection),
                                 std::move(TaggedFS));
}

std::future<Tagged<std::vector<CompletionItem>>>
ClangdServer::codeComplete(PathRef File, Position Pos,
                           llvm::Optional<StringRef> OverridenContents,
                           IntrusiveRefCntPtr<vfs::FileSystem> *UsedFS) {
  using ResultType = Tagged<std::vector<CompletionItem>>;

  std::promise<ResultType> ResultPromise;

  auto Callback = [](std::promise<ResultType> ResultPromise,
                     ResultType Result) -> void {
    ResultPromise.set_value(std::move(Result));
  };

  std::future<ResultType> ResultFuture = ResultPromise.get_future();
  codeComplete(BindWithForward(Callback, std::move(ResultPromise)), File, Pos,
               OverridenContents, UsedFS);
  return ResultFuture;
}

void ClangdServer::codeComplete(
    UniqueFunction<void(Tagged<std::vector<CompletionItem>>)> Callback,
    PathRef File, Position Pos, llvm::Optional<StringRef> OverridenContents,
    IntrusiveRefCntPtr<vfs::FileSystem> *UsedFS) {
  using CallbackType =
      UniqueFunction<void(Tagged<std::vector<CompletionItem>>)>;

  std::string Contents;
  if (OverridenContents) {
    Contents = *OverridenContents;
  } else {
    auto FileContents = DraftMgr.getDraft(File);
    assert(FileContents.Draft &&
           "codeComplete is called for non-added document");

    Contents = std::move(*FileContents.Draft);
  }

  auto TaggedFS = FSProvider.getTaggedFileSystem(File);
  if (UsedFS)
    *UsedFS = TaggedFS.Value;

  std::shared_ptr<CppFile> Resources = Units.getFile(File);
  assert(Resources && "Calling completion on non-added file");

  // Remember the current Preamble and use it when async task starts executing.
  // At the point when async task starts executing, we may have a different
  // Preamble in Resources. However, we assume the Preamble that we obtain here
  // is reusable in completion more often.
  std::shared_ptr<const PreambleData> Preamble =
      Resources->getPossiblyStalePreamble();
  // A task that will be run asynchronously.
  auto Task =
      // 'mutable' to reassign Preamble variable.
      [=](CallbackType Callback) mutable {
        if (!Preamble) {
          // Maybe we built some preamble before processing this request.
          Preamble = Resources->getPossiblyStalePreamble();
        }
        // FIXME(ibiryukov): even if Preamble is non-null, we may want to check
        // both the old and the new version in case only one of them matches.

        std::vector<CompletionItem> Result = clangd::codeComplete(
            File, Resources->getCompileCommand(),
            Preamble ? &Preamble->Preamble : nullptr, Contents, Pos,
            TaggedFS.Value, PCHs, CodeCompleteOpts, Logger);

        Callback(make_tagged(std::move(Result), std::move(TaggedFS.Tag)));
      };

  WorkScheduler.addToFront(std::move(Task), std::move(Callback));
}

llvm::Expected<Tagged<SignatureHelp>>
ClangdServer::signatureHelp(PathRef File, Position Pos,
                            llvm::Optional<StringRef> OverridenContents,
                            IntrusiveRefCntPtr<vfs::FileSystem> *UsedFS) {
  std::string DraftStorage;
  if (!OverridenContents) {
    auto FileContents = DraftMgr.getDraft(File);
    if (!FileContents.Draft)
      return llvm::make_error<llvm::StringError>(
          "signatureHelp is called for non-added document",
          llvm::errc::invalid_argument);

    DraftStorage = std::move(*FileContents.Draft);
    OverridenContents = DraftStorage;
  }

  auto TaggedFS = FSProvider.getTaggedFileSystem(File);
  if (UsedFS)
    *UsedFS = TaggedFS.Value;

  std::shared_ptr<CppFile> Resources = Units.getFile(File);
  if (!Resources)
    return llvm::make_error<llvm::StringError>(
        "signatureHelp is called for non-added document",
        llvm::errc::invalid_argument);

  auto Preamble = Resources->getPossiblyStalePreamble();
  auto Result = clangd::signatureHelp(File, Resources->getCompileCommand(),
                                      Preamble ? &Preamble->Preamble : nullptr,
                                      *OverridenContents, Pos, TaggedFS.Value,
                                      PCHs, Logger);
  return make_tagged(std::move(Result), TaggedFS.Tag);
}

std::vector<tooling::Replacement> ClangdServer::formatRange(PathRef File,
                                                            Range Rng) {
  std::string Code = getDocument(File);

  size_t Begin = positionToOffset(Code, Rng.start);
  size_t Len = positionToOffset(Code, Rng.end) - Begin;
  return formatCode(Code, File, {tooling::Range(Begin, Len)});
}

std::vector<tooling::Replacement> ClangdServer::formatFile(PathRef File) {
  // Format everything.
  std::string Code = getDocument(File);
  return formatCode(Code, File, {tooling::Range(0, Code.size())});
}

std::vector<tooling::Replacement> ClangdServer::formatOnType(PathRef File,
                                                             Position Pos) {
  // Look for the previous opening brace from the character position and
  // format starting from there.
  std::string Code = getDocument(File);
  size_t CursorPos = positionToOffset(Code, Pos);
  size_t PreviousLBracePos = StringRef(Code).find_last_of('{', CursorPos);
  if (PreviousLBracePos == StringRef::npos)
    PreviousLBracePos = CursorPos;
  size_t Len = 1 + CursorPos - PreviousLBracePos;

  return formatCode(Code, File, {tooling::Range(PreviousLBracePos, Len)});
}

std::string ClangdServer::getDocument(PathRef File) {
  auto draft = DraftMgr.getDraft(File);
  assert(draft.Draft && "File is not tracked, cannot get contents");
  return *draft.Draft;
}

std::string ClangdServer::dumpAST(PathRef File) {
  std::shared_ptr<CppFile> Resources = Units.getFile(File);
  assert(Resources && "dumpAST is called for non-added document");

  std::string Result;
  Resources->getAST().get()->runUnderLock([&Result](ParsedAST *AST) {
    llvm::raw_string_ostream ResultOS(Result);
    if (AST) {
      clangd::dumpAST(*AST, ResultOS);
    } else {
      ResultOS << "<no-ast>";
    }
    ResultOS.flush();
  });
  return Result;
}

llvm::Expected<Tagged<std::vector<Location>>>
ClangdServer::findDefinitions(PathRef File, Position Pos) {
  auto TaggedFS = FSProvider.getTaggedFileSystem(File);

  std::shared_ptr<CppFile> Resources = Units.getFile(File);
  if (!Resources)
    return llvm::make_error<llvm::StringError>(
        "findDefinitions called on non-added file",
        llvm::errc::invalid_argument);

  std::lock_guard<std::recursive_mutex> Lock(IndexMutex);
  std::vector<Location> Result;
  Resources->getAST().get()->runUnderLock([Pos, &Result, this](ParsedAST *AST) {
    if (!AST)
      return;
    Result = clangd::findDefinitions(*AST, Pos, *Index, Logger);
  });
  return make_tagged(std::move(Result), TaggedFS.Tag);
}

bool isSourceFilePath(StringRef Path) {
  StringRef PathExt = llvm::sys::path::extension(Path);

  auto SourceIter =
      std::find_if(std::begin(SourceExtensions), std::end(SourceExtensions),
                   [&PathExt](PathRef SourceExt) {
                     return SourceExt.equals_lower(PathExt);
                   });
  return SourceIter != std::end(SourceExtensions);
}

bool isHeaderFilePath(StringRef Path) {
  StringRef PathExt = llvm::sys::path::extension(Path);

  auto HeaderIter =
      std::find_if(std::begin(HeaderExtensions), std::end(HeaderExtensions),
                   [&PathExt](PathRef HeaderExt) {
                     return HeaderExt.equals_lower(PathExt);
                   });

  return HeaderIter != std::end(HeaderExtensions);
}

llvm::Optional<Path> ClangdServer::switchSourceHeader(PathRef Path) {
  bool IsSource = isSourceFilePath(Path);
  bool IsHeader = isHeaderFilePath(Path);

  // We can only switch between extensions known extensions.
  if (!IsSource && !IsHeader)
    return llvm::None;

  // Array to lookup extensions for the switch. An opposite of where original
  // extension was found.
  ArrayRef<StringRef> NewExts;
  if (IsSource)
    NewExts = HeaderExtensions;
  else
    NewExts = SourceExtensions;

  // Storage for the new path.
  SmallString<128> NewPath = StringRef(Path);

  // Instance of vfs::FileSystem, used for file existence checks.
  auto FS = FSProvider.getTaggedFileSystem(Path).Value;

  // Loop through switched extension candidates.
  for (StringRef NewExt : NewExts) {
    llvm::sys::path::replace_extension(NewPath, NewExt);
    if (FS->exists(NewPath))
      return NewPath.str().str(); // First str() to convert from SmallString to
                                  // StringRef, second to convert from StringRef
                                  // to std::string

    // Also check NewExt in upper-case, just in case.
    llvm::sys::path::replace_extension(NewPath, NewExt.upper());
    if (FS->exists(NewPath))
      return NewPath.str().str();
  }

  return llvm::None;
}

std::future<void> ClangdServer::scheduleReparseAndDiags(
    PathRef File, VersionedDraft Contents, std::shared_ptr<CppFile> Resources,
    Tagged<IntrusiveRefCntPtr<vfs::FileSystem>> TaggedFS) {

  assert(Contents.Draft && "Draft must have contents");
  UniqueFunction<llvm::Optional<std::vector<DiagWithFixIts>>()>
      DeferredRebuild =
          Resources->deferRebuild(*Contents.Draft, TaggedFS.Value);
  std::promise<void> DonePromise;
  std::future<void> DoneFuture = DonePromise.get_future();

  DocVersion Version = Contents.Version;
  Path FileStr = File;
  VFSTag Tag = TaggedFS.Tag;
  auto ReparseAndPublishDiags =
      [this, FileStr, Version,
       Tag](UniqueFunction<llvm::Optional<std::vector<DiagWithFixIts>>()>
                DeferredRebuild,
            std::promise<void> DonePromise) -> void {
    FulfillPromiseGuard Guard(DonePromise);

    auto CurrentVersion = DraftMgr.getVersion(FileStr);
    if (CurrentVersion != Version)
      return; // This request is outdated

    auto Diags = DeferredRebuild();
    if (!Diags)
      return; // A new reparse was requested before this one completed.

    // We need to serialize access to resulting diagnostics to avoid calling
    // `onDiagnosticsReady` in the wrong order.
    std::lock_guard<std::mutex> DiagsLock(DiagnosticsMutex);
    DocVersion &LastReportedDiagsVersion = ReportedDiagnosticVersions[FileStr];
    // FIXME(ibiryukov): get rid of '<' comparison here. In the current
    // implementation diagnostics will not be reported after version counters'
    // overflow. This should not happen in practice, since DocVersion is a
    // 64-bit unsigned integer.
    if (Version < LastReportedDiagsVersion)
      return;
    LastReportedDiagsVersion = Version;

    DiagConsumer.onDiagnosticsReady(FileStr,
                                    make_tagged(std::move(*Diags), Tag));
  };

  WorkScheduler.addToFront(std::move(ReparseAndPublishDiags),
                           std::move(DeferredRebuild), std::move(DonePromise));
  return DoneFuture;
}

std::future<void>
ClangdServer::scheduleCancelRebuild(std::shared_ptr<CppFile> Resources) {
  std::promise<void> DonePromise;
  std::future<void> DoneFuture = DonePromise.get_future();
  if (!Resources) {
    // No need to schedule any cleanup.
    DonePromise.set_value();
    return DoneFuture;
  }

  UniqueFunction<void()> DeferredCancel = Resources->deferCancelRebuild();
  auto CancelReparses = [Resources](std::promise<void> DonePromise,
                                    UniqueFunction<void()> DeferredCancel) {
    FulfillPromiseGuard Guard(DonePromise);
    DeferredCancel();
  };
  WorkScheduler.addToFront(std::move(CancelReparses), std::move(DonePromise),
                           std::move(DeferredCancel));
  return DoneFuture;
}

void ClangdServer::onFileEvent(const DidChangeWatchedFilesParams &Params) {
  for (const FileEvent &FE : Params.changes) {
    llvm::errs() << FE.uri.file << " " << static_cast<int>(FE.type) << "\n";
    switch (FE.type) {
     case FileChangeType::Created:
       fileCreated(FE.uri);
       break;
     case FileChangeType::Deleted:
       fileDeleted(FE.uri);
       break;
     case FileChangeType::Changed:
       fileChanged(FE.uri);
       break;
    }
  }
}

llvm::Expected<Tagged<std::vector<Location>>>
ClangdServer::findReferences(PathRef File, Position Pos) {
  assert(Index);
  auto FileContents = DraftMgr.getDraft(File);
  assert(FileContents.Draft &&
         "findReferences is called for non-added document");

  auto TaggedFS = FSProvider.getTaggedFileSystem(File);

  std::shared_ptr<CppFile> Resources = Units.getFile(File);
  assert(Resources && "Calling findReferences on non-added file");

  std::lock_guard<std::recursive_mutex> Lock(IndexMutex);
  std::vector<Location> Result;
  Resources->getAST().get()->runUnderLock([Pos, &Result, this](ParsedAST *AST) {
    if (!AST)
      return;
    Result = clangd::findReferences(*AST, Pos, *Index, Logger);
  });
  return make_tagged(std::move(Result), TaggedFS.Tag);
}

bool isTimeStampChanged(ClangdIndexFile &File, const llvm::sys::fs::directory_entry& Entry) {
  // FIXME: Check error of status()
  llvm::sys::fs::basic_file_status Status = *Entry.status();
  auto ModifiedTime = Status.getLastModificationTime();
  auto Duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
      ModifiedTime.time_since_epoch());
  return File.getLastIndexingTime() < Duration;
}

class TranslationUnitToBeIndexedCollector {
  bool CheckModified;
  ClangdIndex &Index;
  std::vector<std::string> PathsToBeIndexed;
  std::unordered_set<std::string> PathsToBeIndexedSet;

public:
  TranslationUnitToBeIndexedCollector(bool CheckModified, ClangdIndex &Index) : CheckModified(CheckModified), Index(Index) {
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
      visitFolder(Path, CheckModified);
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

    std::unique_ptr<ClangdIndexFile> ExistingFile = Index.getFile(File);
    if (ExistingFile) {
      if (CheckModified) {
        if (!isTimeStampChanged(*ExistingFile, FileEntry)) {
          // TODO: Check content hash
          llvm::errs() << " Timestamp the same for " << File.str() << "\n";
          return;
        }

        llvm::errs() << " Timestamp *modified* for " << File.str() << "\n";
        ExistingFile->visitDependentFiles([this](ClangdIndexFile &IndexFile) {
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

      Index.getStorage().startWrite();
      // In the case of a header, we especially need to call onChange
      // before we start indexing the including source files because
      // that's how we know we need to index the content of the header (We
      // check the presence of symbols and onChange clears them).
      ExistingFile->onChange();
      Index.getStorage().endWrite();
    }

    // Header content is indexed when a source file that includes it is being
    // indexed.
    if (IsSourceFilePath)
      addFile(File);
  }

  void visitFolder(StringRef Folder, bool CheckModified) {
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
        visitFolder(Entry.path(), CheckModified);
      else
        visitFile(Entry, CheckModified);
    }
  }

};

void ClangdServer::indexRoot(bool CheckModified) {
  assert(Index);
  assert(RootPath);
  auto IndexTotalTimer = llvm::Timer("index time", "Indexing Total Time");
  IndexTotalTimer.startTimer();

  std::lock_guard<std::recursive_mutex> Lock(IndexMutex);
  TranslationUnitToBeIndexedCollector C(CheckModified, *Index);
  C.visitPath(*RootPath);
  auto FilesToIndex = C.takePaths();
  llvm::errs() << llvm::format("Indexing %u source files.", FilesToIndex.size()) << "\n";
  for (auto TU : FilesToIndex) {
    indexFile(TU);
  }
  IndexTotalTimer.stopTimer();
  Index->flush();
}

void ClangdServer::reindex() {
  if (!RootPath) {
    return;
  }

  SmallString<32> Filename(*this->RootPath);
  llvm::sys::path::append(Filename, "clangd.index");
  if (llvm::sys::fs::exists(Filename)) {
    llvm::sys::fs::remove_directories(Filename);
  }
  Index = std::make_shared<ClangdIndex>(Filename.str());
  CDB.setIndex(Index);
  indexRoot(false);
}

void ClangdServer::fileChanged (URI File) {
  assert(Index);

  llvm::errs() << llvm::formatv(" File changed: {0}\n", File.file);
  auto FileInIndex = Index->getFile(File.file);
  if (!FileInIndex) {
    llvm::errs() << " Unknown to index. Nothing to do.\n";
    return;
  }

  std::lock_guard<std::recursive_mutex> Lock(IndexMutex);
  TranslationUnitToBeIndexedCollector C(true, *Index);
  C.visitPath(File.file);
  auto FilesToIndex = C.takePaths();
  for (auto TU : FilesToIndex) {
    indexFile(TU);
  }
}

void ClangdServer::fileCreated (URI File) {
  assert(Index);
  llvm::errs() << " File created. Indexing " << File.file << "\n";
  std::lock_guard<std::recursive_mutex> Lock(IndexMutex);
  TranslationUnitToBeIndexedCollector C(true, *Index);
  C.visitPath(File.file);
  auto FilesToIndex = C.takePaths();
  for (auto TU : FilesToIndex) {
    indexFile(TU);
  }
}

void ClangdServer::fileDeleted (URI File) {
  assert(Index);
  llvm::errs() << " File deleted. " << File.file << "\n";
  std::lock_guard<std::recursive_mutex> Lock(IndexMutex);
  auto FileInIndex = Index->getFile(File.file);
  if (!FileInIndex) {
    llvm::errs() << " Unknown to index. Nothing to do. " << "\n";
    return;
  }
  Index->getStorage().startWrite();
  FileInIndex->free();
  Index->getStorage().endWrite();
  //TODO: Reindex affected files?
}

namespace {
// Stores the "included by" -> "included file" relationship
void addInclusion(ClangdIndexHeaderInclusion& Inclusion, ClangdIndexFile& IncludedByFile) {
  auto FirstInclusion = IncludedByFile.getFirstInclusion();
  IncludedByFile.setFirstInclusion(Inclusion.getRecord());
  if (FirstInclusion) {
    Inclusion.setNextInclusion(FirstInclusion->getRecord());
    FirstInclusion->setPrevInclusion(Inclusion.getRecord());
  }
}

bool includedByExists(ClangdIndexFile &IncludedFile, ClangdIndexFile &IncludedByFile) {
  auto IncludedBySearch = IncludedFile.getFirstIncludedBy();
  while (IncludedBySearch) {
    auto IncludedByFileSearch = IncludedBySearch->getIncludedBy();
    if (IncludedByFile.getPath().compare(IncludedByFileSearch->getPath()) == 0)
      return true;

    IncludedBySearch = IncludedBySearch->getNextIncludeBy();
  }
  return false;
}
}

void handleInclusions(ASTUnit &Unit, ClangdIndexFile &IndexFile, ClangdIndex &Index) {
  // Handle includes
  SourceManager &SM = Unit.getSourceManager();
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

    auto IncludedByFileFromIndex = Index.getFile(IncludedByName);
    ClangdIndexFile &IncludedByFile = InclusionStack.size() == 1 ? IndexFile : *IncludedByFileFromIndex;

    // Store the included file -> included by relationship
    auto IncludedFile = Index.getFile(FilePath);
    if (!IncludedFile) {
      IncludedFile = llvm::make_unique<ClangdIndexFile>(Index, FilePath);
      Index.addFile(*IncludedFile);
    }

    if (!includedByExists(*IncludedFile, IncludedByFile)) {
      auto Inclusion = llvm::make_unique<ClangdIndexHeaderInclusion>(Index, IncludedByFile, *IncludedFile);
      auto FirstInclusion = IncludedFile->getFirstIncludedBy();
      IncludedFile->setFirstIncludedBy(Inclusion->getRecord());
      if (FirstInclusion) {
        Inclusion->setNextIncludedBy(FirstInclusion->getRecord());
        FirstInclusion->setPrevIncludedBy(Inclusion->getRecord());
      }
      addInclusion(*Inclusion, IncludedByFile);
    }
  }
}

void ClangdServer::indexFile (StringRef File) {
  llvm::errs() << " Indexing " << File.str() << "\n";
  assert(!isHeaderFilePath(File) && "Only source files should be indexed.");

  std::lock_guard<std::recursive_mutex> Lock(IndexMutex);
  Index->getStorage().startWrite();
  std::unique_ptr<ClangdIndexFile> IndexFile = Index->getFile(File);
  if (!IndexFile) {
    IndexFile = llvm::make_unique<ClangdIndexFile>(*Index, File.str());
    Index->addFile(*IndexFile);
  }

  llvm::sys::fs::directory_entry Entry(File);
  auto Status = Entry.status();
  if (!Status) {
    llvm::errs() << " Cannot stat file. Skipping file. " << "\n";
    return;
  }

  // Get the time so we know when we last indexed the file.
  auto IndexingTimePoint = std::chrono::system_clock::now();
  auto IndexingTimePointDurationNs = std::chrono::duration_cast<std::chrono::nanoseconds>(IndexingTimePoint.time_since_epoch());
  // We only save the time stamp after indexing, that way if indexing is
  // cancelled midway, the file will still be re-indexed later.

  auto DataConsumer = std::make_shared<ClangdIndexDataConsumer>(llvm::errs(), *Index, *IndexFile, IndexingTimePointDurationNs);
  index::IndexingOptions IndexOpts;
  IndexOpts.SystemSymbolFilter = index::IndexingOptions::SystemSymbolFilterKind::All;
  IndexOpts.IndexFunctionLocals = true;
  std::unique_ptr<FrontendAction> IndexAction;
  IndexAction = index::createIndexingAction(DataConsumer, IndexOpts,
                                     /*WrappedAction=*/nullptr);

  IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
      CompilerInstance::createDiagnostics(new DiagnosticOptions);
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

  auto CInvok = createInvocationFromCommandLine(ArgStrs, Diags);
  if (!CInvok)
    return;
  auto PCHContainerOps = std::make_shared<PCHContainerOperations>();
  std::unique_ptr<ASTUnit> Unit(ASTUnit::LoadFromCompilerInvocationAction(
      std::move(CInvok), PCHContainerOps, Diags, IndexAction.get()));

  handleInclusions(*Unit, *IndexFile, *Index);
  // FIXME: Are we setting the time twice?
  IndexFile->setLastIndexingTime(IndexingTimePointDurationNs);
  Index->getStorage().endWrite();
}

void ClangdServer::dumpIncludedBy(URI File) {
  std::lock_guard<std::recursive_mutex> Lock(IndexMutex);
  auto IndexFile = Index->getFile(File.file);
  if (!IndexFile) {
    llvm::errs() << " Unknown to index. Nothing to do. " << "\n";
    return;
  }
  IndexFile->visitDependentFiles([](ClangdIndexFile &File) {
    //FIXME: Add some nice indentation
    llvm::errs() << File.getPath() << "\n";
    return true;
  });
}

void ClangdServer::dumpInclusions(URI File) {
  auto IndexFile = Index->getFile(File.file);
  if (!IndexFile) {
    llvm::errs() << " Unknown to index. Nothing to do. " << "\n";
    return;
  }

  IndexFile->visitInclusions([](ClangdIndexFile &File) {
    //FIXME: Add some nice indentation
    llvm::errs() << File.getPath() << "\n";
    return true;
  });
}
