#pragma once

// Private preparation/publication support. A candidate pin is validation input,
// never production activation. No environment variable supplies approval.
#include "uct_public_bf16.hpp"
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <set>
#include <utility>
#include <cstddef>
#ifdef Q_OS_WIN
#include <Windows.h>
#include <winternl.h>
#endif

namespace okuflow::uct::public_bf16 {
enum class CatalogUse { Production, CandidateValidation };
struct Request {
    QString modelRoot, corpusRoot, cacheRoot;
    CatalogUse use{CatalogUse::Production};
    bool enabled{};
    bool preferDetector{true};
};
struct BaselineCachePolicy {QString root;bool allowPreparation{true};};
inline BaselineCachePolicy BaselinePolicy(const Request& request) {
    if(!request.enabled || request.use!=CatalogUse::CandidateValidation)return {};
    if(!runtime_precision::LocalAbsolute(request.cacheRoot))
        throw Refusal(Reason::Model,"candidate validation requires an explicit isolated FP32/BF16 cache root");
    return {request.cacheRoot,false}; // detector AND every FP32 fallback stay cache-only
}
inline std::span<const CorpusPin> CandidateCatalog() {
    static const std::array<CorpusPin,3> pins{{{"okuflow-owned-typeset-gray8-v1",
        "26432369ed6831431737072f857620b19c81fc9acd8c21579d50ee7bc0e9ec33"},
        {"okuflow-owned-typeset-gray8-small-v1","9a0b187a047e8d6aa9189adfe490df6f652d65d5b13979c857ac852ba9cdfe86"},
        {"okuflow-owned-typeset-gray8-tiny-v1","b8969dcb73d6baffea011e3953a2730bb9740a7dbd98a8f7ed47cb460c090fe1"}}};
    return pins;
}
inline std::span<const CorpusPin> Catalog(const Request& request) {
    return request.use==CatalogUse::CandidateValidation ? CandidateCatalog() : ProductionCorpusCatalog();
}
inline QByteArray ReadBounded(const QString& root,const QString& relative,qint64 maximum) {
    if(!runtime_precision::LocalAbsolute(root))throw Refusal(Reason::Model,"public BF16 requires explicit local roots");
    const auto path=QDir(root).filePath(relative);
    runtime_precision::CanonicalWithin(root,path);
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly) || file.size()<1 || file.size()>maximum)
        throw Refusal(Reason::Model,"public BF16 file missing/unreadable/out of bounds");
    const auto bytes=file.read(maximum+1);
    if(file.error()!=QFileDevice::NoError || !file.atEnd() || bytes.size()>maximum)
        throw Refusal(Reason::Model,"public BF16 file changed/exceeds read bound");
    return bytes;
}
inline Corpus LoadCorpus(const Request& request,ModelTier tier=ModelTier::Medium) {
    if(Catalog(request).empty())throw Refusal(Reason::CorpusUnavailable,"public BF16 production corpus is unavailable");
    auto corpus=ReadCorpus(ReadBounded(request.corpusRoot,"manifest.json",64*1024),Catalog(request),
        [&](const QString& relative){return ReadBounded(request.corpusRoot,relative,48*1024);});
    if(corpus.ModelIdentity()!=ModelIdentity(tier))throw Refusal(Reason::Corpus,"golden corpus does not match selected recognizer");
    return corpus;
}

// Raw text from the SAME production compacted CTC tokens. Preserve every space
// and dictionary code point; the positioned-word decoder deliberately splits
// words and therefore cannot be used to manufacture exact golden text.
inline QString RawText(std::span<const PaddleToken> tokens,int validSteps,const QStringList& dictionary) {
    QString text;
    for(const auto& token:tokens) {
        if(token.id<=0 || token.id>=dictionary.size() || token.first<0 || token.last<token.first ||
           token.last>=validSteps || !std::isfinite(token.confidence) || token.confidence<0 || token.confidence>1.0001f)
            throw Refusal(Reason::GoldenMismatch,"golden compacted CTC token is invalid");
        text+=dictionary[token.id];
        if(text.size()>8192)throw Refusal(Reason::GoldenMismatch,"golden text exceeds bound");
    }
    return text;
}
inline QJsonObject PartialRecord(const QJsonObject& identity,std::span<const Observation> observations,
                                 const QString& completion,const QString& error={}) {
    if(observations.size()>kGoldenObservations)throw Refusal(Reason::GoldenMismatch,"partial golden evidence exceeds bound");
    QJsonArray rows;
    for(const auto& row:observations) {
        if(row.crop.size()>48 || row.text.size()>8192)throw Refusal(Reason::GoldenMismatch,"partial golden output exceeds bound");
        rows.append(QJsonObject{{"crop",row.crop},{"batch",row.batch},{"T",row.steps},{"classes",row.classes},
            {"pixelsSha256",QString::fromLatin1(row.pixelsSha256)},{"text",row.text}});
    }
    return {{"complete",false},{"passed",false},{"completion",completion},{"identity",identity},{"rows",rows},{"error",error.left(4096)}};
}

// A bounded process-session latch, including in-flight attempts. Only success
// removes an identity. Changed plan/corpus/builder/device produces a new key.
class Attempts {
public:
    bool Begin(const QByteArray& key) {
        std::lock_guard lock(mutex_);
        return keys_.size()<64 && keys_.insert(key).second;
    }
    void Passed(const QByteArray& key) {std::lock_guard lock(mutex_);keys_.erase(key);}
private:
    std::mutex mutex_;
    std::set<QByteArray> keys_;
};
inline Attempts& SessionAttempts() {static Attempts attempts;return attempts;}

// Completion is a capability carried by the preparation owner. A failed or
// uncertain attempt cannot publish even with a writable, plausible receipt.
class Publication {
public:
    void Queued() {confirmed_=false;}
    void Confirmed() {confirmed_=true;}
    void RequireConfirmed() const {
        if(!confirmed_)throw std::runtime_error("BF16 publication before confirmed stream completion; retire/quarantine");
    }
private:
    bool confirmed_{};
};
template<class Cleanup> void DiscardAfterCompletion(const Publication& completion,Cleanup&& cleanup) {
    completion.RequireConfirmed();
    std::forward<Cleanup>(cleanup)();
}
inline QByteArray ReceiptBytes(const LocalBuild& build,const Corpus& corpus,const GoldenResult& golden,
                              const precision_artifacts::Runtime& runtime,const Publication& completion) {
    completion.RequireConfirmed();
    return QJsonDocument(ReadyReceipt(build,corpus,golden,runtime)).toJson(QJsonDocument::Compact);
}
struct CachedPlan {QByteArray bytes,builderVersion;QString directory;};
inline constexpr char kCommitName[]="COMMITTED";
inline constexpr char kCommitBytes[]="okuflow-public-bf16-v2\n";
inline bool Nonce(const QByteArray& nonce) {
    return nonce.size()==32 && QByteArray::fromHex(nonce).toHex()==nonce;
}
inline QString GenerationName(const QByteArray& key,const QByteArray& nonce) {
    if(!runtime_precision::Hash(key) || !Nonce(nonce))throw Refusal(Reason::Model,"invalid public BF16 v2 generation identity");
    return QString::fromLatin1(key+'-'+nonce);
}
struct GenerationInventory {std::array<QString,32> names{};int count{};};
#ifdef Q_OS_WIN
inline std::wstring NativeCachePath(const QString& path) {
    if(!runtime_precision::LocalAbsolute(path))throw Refusal(Reason::Model,"public BF16 v2 native path must be local/absolute");
    // Recipe+nonce names can exceed MAX_PATH under an ordinary private temp root.
    // Do not require owner registry/environment or manifest changes for that.
    return L"\\\\?\\"+QDir::toNativeSeparators(QDir::cleanPath(path)).toStdWString();
}
#endif
inline GenerationInventory CollectGenerations(const QString& parent,const char* phase) {
#ifdef Q_OS_WIN
    GenerationInventory inventory;
    // Native iterator exposes enumeration errors; QDirIterator cannot distinguish
    // an I/O failure from normal exhaustion. Include files/hidden/system entries.
    const auto pattern=NativeCachePath(QDir(parent).filePath("*"));
    WIN32_FIND_DATAW data{};
    const auto search=FindFirstFileW(pattern.c_str(),&data);
    if(search==INVALID_HANDLE_VALUE) {
        if(GetLastError()==ERROR_FILE_NOT_FOUND)return inventory;
        throw Refusal(Reason::Model,"cannot enumerate public BF16 v2 inventory");
    }
    struct SearchOwner {HANDLE handle;~SearchOwner(){FindClose(handle);}} owner{search};
    for(;;) {
        const auto entry=QString::fromWCharArray(data.cFileName);
        if(entry!="." && entry!="..") {
            // Bound the iterator BEFORE appending or sorting; no unbounded entryList.
            if(inventory.count==int(inventory.names.size()))throw Refusal(Reason::Model,"public BF16 cache inventory exceeds bound");
            const auto bytes=entry.toLatin1();
            if(bytes.size()!=97 || bytes[64]!='-' || !runtime_precision::Hash(bytes.first(64)) || !Nonce(bytes.sliced(65))) {
                const auto context=QJsonDocument(QJsonObject{{"phase",phase},{"ordinal",inventory.count+1},
                    {"entry",entry.left(128)},{"entryChars",int(entry.size())},{"path",parent.left(512)}}).toJson(QJsonDocument::Compact);
                throw Refusal(Reason::Model,(QByteArray("unknown public BF16 cache generation: ")+context).constData());
            }
            inventory.names[inventory.count++]=entry;
        }
        if(!FindNextFileW(search,&data)) {
            if(GetLastError()!=ERROR_NO_MORE_FILES)throw Refusal(Reason::Model,"public BF16 v2 inventory enumeration failed");
            break;
        }
    }
    std::sort(inventory.names.begin(),inventory.names.begin()+inventory.count);
    return inventory;
#else
    throw Refusal(Reason::Model,"public BF16 v2 inventory requires native Windows ownership");
#endif
}
#ifdef Q_OS_WIN
// Hold actual directories with list access and no write/delete sharing. Reparse points
// below the explicit canonical root are refusals BEFORE creating descendants.
// Newly created directories return the SAME handle that created them: no adoption gap.
class DirectoryPins {
public:
    DirectoryPins()=default;
    DirectoryPins(const DirectoryPins&)=delete;
    DirectoryPins& operator=(const DirectoryPins&)=delete;
    ~DirectoryPins(){for(int i=0;i<count_;++i)CloseHandle(handles_[i]);}
    HANDLE Hold(const QString& path) {
        if(count_==int(handles_.size()))throw Refusal(Reason::Model,"public cache path exceeds directory bound");
        const auto name=NativeCachePath(path);
        // Attribute-only access did not enforce delete exclusion in root's probe.
        const auto handle=CreateFileW(name.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|FILE_TRAVERSE|SYNCHRONIZE,
            FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw Refusal(Reason::Model,"cannot pin public cache directory");
        return Retain(handle);
    }
    HANDLE Last() const {return count_?handles_[count_-1]:INVALID_HANDLE_VALUE;}
    HANDLE CreateNew(HANDLE parent,const QString& component) {
        if(count_==int(handles_.size()) || component.isEmpty() || component.size()>128)
            throw Refusal(Reason::Model,"public cache path exceeds directory bound");
        for(const auto c:component)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))
            throw Refusal(Reason::Model,"invalid public cache component");
        // Documented FILE_CREATE returns a handle to the newly created directory;
        // unlike mkdir followed by Hold it cannot adopt a replacement in between.
        const auto create=reinterpret_cast<decltype(&NtCreateFile)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCreateFile"));
        if(!create)throw Refusal(Reason::Model,"native public cache exclusive directory creation unavailable");
        auto name=component.toStdWString();
        UNICODE_STRING unicode{};unicode.Buffer=name.data();unicode.Length=USHORT(name.size()*sizeof(wchar_t));unicode.MaximumLength=unicode.Length;
        OBJECT_ATTRIBUTES attributes{};attributes.Length=sizeof(attributes);attributes.RootDirectory=parent;
        attributes.ObjectName=&unicode;attributes.Attributes=OBJ_CASE_INSENSITIVE|OBJ_DONT_REPARSE;
        IO_STATUS_BLOCK status{};HANDLE handle=INVALID_HANDLE_VALUE;
        const auto result=create(&handle,FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|FILE_TRAVERSE|SYNCHRONIZE,
            &attributes,&status,nullptr,FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ,FILE_CREATE,
            FILE_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT,nullptr,0);
        if(result!=0 || status.Information!=FILE_CREATED) {
            if(handle!=INVALID_HANDLE_VALUE && handle)CloseHandle(handle);
            throw Refusal(Reason::Model,"cannot exclusively create public cache directory; no adoption/retry");
        }
        return Retain(handle);
    }
private:
    HANDLE Retain(HANDLE handle) {
        BY_HANDLE_FILE_INFORMATION information{};
        if(!GetFileInformationByHandle(handle,&information) ||
           !(information.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (information.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(handle);throw Refusal(Reason::Model,"public cache directory is not an owned ordinary directory");
        }
        handles_[count_++]=handle;
        return handle;
    }
public:
    QString Prepare(const QString& cacheRoot,const QString& parent,bool create=true) {
        auto current=QFileInfo(cacheRoot).canonicalFilePath();
        if(current.isEmpty())throw Refusal(Reason::Model,"public cache root must already exist");
        Hold(current);
        const auto relative=QDir::fromNativeSeparators(QDir(cacheRoot).relativeFilePath(parent));
        const auto parts=relative.split('/');
        if(parts.size()>8)throw Refusal(Reason::Model,"public cache path exceeds component bound");
        for(const auto& part:parts) {
            if(part.isEmpty() || part=="." || part=="..")throw Refusal(Reason::Model,"invalid public cache component");
            for(const auto c:part)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))
                throw Refusal(Reason::Model,"invalid public cache component");
            const auto next=QDir(current).filePath(part);
            // The held parent cannot be retargeted while this component is
            // examined/created. Hold rejects an existing junction, not its target.
            if(!QFileInfo::exists(next)) {
                if(!create)return {};
                CreateNew(Last(),part);current=next;continue;
            }
            Hold(next);current=next;
        }
        return current;
    }
private:
    std::array<HANDLE,10> handles_{};
    int count_{};
};
// Exact regular files, not just their directories, exclude other writers/deleters.
// Writer handles close before read pins are acquired; actual bytes are rehashed
// under the read pins before commit. Nothing is committed during that handoff.
class FilePin {
public:
    FilePin(const QString& path,bool createNew=false,bool allowMissing=false):writable_(createNew) {
        const auto name=NativeCachePath(path);
        handle_=CreateFileW(name.c_str(),GENERIC_READ|(createNew?GENERIC_WRITE:0),FILE_SHARE_READ,nullptr,
            createNew?CREATE_NEW:OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if(handle_==INVALID_HANDLE_VALUE) {
            const auto error=GetLastError();
            if(!createNew && allowMissing && error==ERROR_FILE_NOT_FOUND)return;
            throw Refusal(Reason::Model,"cannot pin public BF16 v2 file");
        }
        try {Size();}catch(...) {CloseHandle(handle_);handle_=INVALID_HANDLE_VALUE;throw;}
    }
    FilePin(const FilePin&)=delete;
    FilePin& operator=(const FilePin&)=delete;
    ~FilePin(){if(handle_!=INVALID_HANDLE_VALUE)CloseHandle(handle_);}
    bool Exists() const {return handle_!=INVALID_HANDLE_VALUE;}
    QByteArray Read(qint64 maximum) const {
        const auto size=Size();
        if(size<1 || size>maximum)throw Refusal(Reason::Model,"public BF16 v2 file exceeds read bound");
        LARGE_INTEGER start{};
        if(!SetFilePointerEx(handle_,start,nullptr,FILE_BEGIN))throw Refusal(Reason::Model,"cannot seek public BF16 v2 file");
        QByteArray bytes(qsizetype(size),Qt::Uninitialized);
        for(qint64 offset=0;offset<size;) {
            DWORD read{};const auto amount=DWORD(std::min<qint64>(size-offset,1024*1024));
            if(!ReadFile(handle_,bytes.data()+offset,amount,&read,nullptr) || read!=amount)
                throw Refusal(Reason::Model,"cannot completely read public BF16 v2 file");
            offset+=read;
        }
        if(Size()!=size)throw Refusal(Reason::Model,"public BF16 v2 file changed during pinned read");
        return bytes;
    }
    void Write(const QByteArray& bytes,qint64 maximum) const {
        if(!writable_ || bytes.isEmpty() || bytes.size()>maximum)throw Refusal(Reason::Model,"invalid public BF16 v2 new file");
        for(qsizetype offset=0;offset<bytes.size();) {
            DWORD written{};const auto amount=DWORD(std::min<qsizetype>(bytes.size()-offset,1024*1024));
            if(!WriteFile(handle_,bytes.constData()+offset,amount,&written,nullptr) || written!=amount)
                throw Refusal(Reason::Model,"cannot completely write public BF16 v2 new file");
            offset+=written;
        }
        if(Size()!=bytes.size())throw Refusal(Reason::Model,"public BF16 v2 new file size mismatch");
    }
private:
    qint64 Size() const {
        BY_HANDLE_FILE_INFORMATION information{};
        if(handle_==INVALID_HANDLE_VALUE || GetFileType(handle_)!=FILE_TYPE_DISK || !GetFileInformationByHandle(handle_,&information) ||
           (information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DEVICE)) || information.nNumberOfLinks!=1)
            throw Refusal(Reason::Model,"public BF16 v2 file must be regular/non-reparse/single-link");
        const auto size=(quint64(information.nFileSizeHigh)<<32)|information.nFileSizeLow;
        if(size>=quint64(runtime_precision::kPlanLimit))throw Refusal(Reason::Model,"public BF16 v2 file exceeds absolute bound");
        return qint64(size);
    }
    HANDLE handle_{INVALID_HANDLE_VALUE};bool writable_{};
};
inline bool HasCommit(const FilePin& marker) {
    if(!marker.Exists())return false; // ONLY absent fixed marker is incomplete.
    if(marker.Read(sizeof(kCommitBytes)-1)!=QByteArray(kCommitBytes))throw Refusal(Reason::Model,"invalid public BF16 v2 commit marker");
    return true;
}
// Cooperating publishers reserve inventory capacity across processes. Never
// wait: contention is a typed refusal, not a retry loop or a CPU/UI dependency.
class PublicationLock {
public:
    PublicationLock(const QString& root,const QString& parent) {
        const auto canonical=QFileInfo(root).canonicalFilePath();
        if(canonical.isEmpty())throw Refusal(Reason::Model,"public cache root must already exist");
        const auto resolved=QDir(canonical).filePath(QDir(root).relativeFilePath(parent));
        const auto digest=precision_artifacts::Digest(QDir::cleanPath(QDir::fromNativeSeparators(resolved)).toCaseFolded().toUtf8());
        const auto name=(QStringLiteral("Global\\OkuFlow-public-bf16-v2-")+QString::fromLatin1(digest)).toStdWString();
        handle_=CreateMutexW(nullptr,FALSE,name.c_str());
        if(!handle_)throw Refusal(Reason::Model,"cannot serialize public BF16 v2 reservation");
        const auto result=WaitForSingleObject(handle_,0);
        if(result!=WAIT_OBJECT_0 && result!=WAIT_ABANDONED) {
            CloseHandle(handle_);handle_=nullptr;
            throw Refusal(Reason::Model,"public BF16 v2 reservation busy/unavailable; no wait/retry");
        }
    }
    PublicationLock(const PublicationLock&)=delete;
    PublicationLock& operator=(const PublicationLock&)=delete;
    ~PublicationLock(){if(handle_){ReleaseMutex(handle_);CloseHandle(handle_);}}
private:
    HANDLE handle_{};
};
#endif
inline QString CacheParent(const Request& request,const LocalBuild& build,const Corpus& corpus) {
    // Parent deliberately omits builder as well as driver/runtime provenance.
    // Each v2 recipe still binds its actual builder via CacheKey; nonce is only a locator.
    auto identity=BuildIdentity(build,corpus);identity.remove("builderTensorRtVersion");
    return QDir(request.cacheRoot).filePath("public-bf16-v2/"+QString::fromLatin1(
        precision_artifacts::Digest(QJsonDocument(identity).toJson(QJsonDocument::Compact))));
}
// Shared production integrity validation. Opaque storage fixtures cannot supply
// model approval, and no receipt field is authority for expected golden text.
inline QByteArray ValidateCommitted(const QString& entry,const QByteArray& plan,const QByteArray& receiptBytes,
                                    const LocalBuild& build,const Corpus& corpus) {
    if(plan.isEmpty() || plan.size()>=runtime_precision::kPlanLimit || receiptBytes.isEmpty() || receiptBytes.size()>256*1024)
        throw Refusal(Reason::Model,"public BF16 v2 committed pair exceeds bounds");
    QJsonParseError parseError;
    const auto document=QJsonDocument::fromJson(receiptBytes,&parseError);
    if(parseError.error!=QJsonParseError::NoError || !document.isObject())throw Refusal(Reason::Model,"invalid public BF16 v2 committed receipt");
    const auto receipt=document.object();
    const auto identity=receipt["identity"].toObject();
    auto actual=build;actual.builderVersion=identity["builderTensorRtVersion"].toString().toLatin1();
    const auto expected=BuildIdentity(actual,corpus);
    actual.planFileSha256=identity["planFileSha256"].toString().toLatin1();
    auto stripped=identity;stripped.remove("planFileSha256");stripped.remove("golden");
    if(receipt["schema"]!="uct-local-bf16-ready-v2" || stripped!=expected ||
       entry.toLatin1().first(64)!=CacheKey(actual,corpus) || !runtime_precision::Hash(actual.planFileSha256) ||
       receipt["identitySha256"]!=QString::fromLatin1(precision_artifacts::Digest(QJsonDocument(identity).toJson(QJsonDocument::Compact))))
        throw Refusal(Reason::Model,"public BF16 cached receipt identity mismatch");
    if(precision_artifacts::Digest(plan)!=actual.planFileSha256)throw Refusal(Reason::Model,"public BF16 cached plan hash mismatch");
    return actual.builderVersion;
}
inline CachedPlan FindPlan(const Request& request,const LocalBuild& build,const Corpus& corpus) {
    if(!runtime_precision::LocalAbsolute(request.cacheRoot))throw Refusal(Reason::Model,"public BF16 cache root must be local/absolute");
    const auto requestedParent=CacheParent(request,build,corpus);
#ifdef Q_OS_WIN
    DirectoryPins readPins;
    const auto located=readPins.Prepare(request.cacheRoot,requestedParent,false);
    if(located.isEmpty())return {};
    const QDir parent(located);
    const auto entries=CollectGenerations(parent.absolutePath(),"FindPlan/generation-name");
    CachedPlan selected;
    for(int i=0;i<entries.count;++i) {
        const auto& entry=entries.names[i];
        const auto directory=parent.filePath(entry);
        // Ancestors stay pinned for the whole lookup; each child is pinned only
        // through its bounded reads, then closes on skip/return after bytes copy.
        DirectoryPins generationPin;
        generationPin.Hold(directory);
        runtime_precision::CanonicalWithin(request.cacheRoot,directory);
        FilePin marker(QDir(directory).filePath(kCommitName),false,true);
        if(!HasCommit(marker))continue;
        FilePin receiptPin(QDir(directory).filePath("ready.json")),planPin(QDir(directory).filePath("recognizer.plan"));
        const auto plan=planPin.Read(runtime_precision::kPlanLimit-1);
        const auto builder=ValidateCommitted(entry,plan,receiptPin.Read(256*1024),build,corpus);
        // This lookup selects the current actual builder's plan recipe. Older
        // valid generations stay untouched; runtime/ABI admission is separate.
        if(builder!=build.builderVersion)continue;
        // Receipt text/pass fields are NEVER authority. The caller must actually
        // deserialize/check the engine and run all nine golden calls again.
        if(selected.bytes.isEmpty())selected={plan,builder,directory};
        // Validate ALL committed siblings, including those after a match. Pins
        // close per generation, after any selected bytes have been copied.
    }
    return selected;
#else
    throw Refusal(Reason::Model,"public BF16 v2 lookup requires native Windows file ownership");
#endif
}
// Storage primitive, independently fixture-testable with plainly labelled
// non-model bytes. The production entry below enforces actual golden evidence.
using StorageObserver=std::function<void(const QString&,bool)>; // false=precommit, true=committed, pins still held
using StoredValidator=std::function<void(const QString&,const QByteArray&,const QByteArray&)>;
inline bool PublishFilesNew(const QString& cacheRoot,const QString& parent,const QByteArray& key,
                            const QByteArray& plan,const QByteArray& receipt,const QByteArray& fixtureNonce={},
                            const StorageObserver& observer={},const StoredValidator& validate={}) {
    if(!runtime_precision::LocalAbsolute(cacheRoot) || !runtime_precision::Hash(key) ||
       !runtime_precision::Within(cacheRoot,parent) || plan.isEmpty() || plan.size()>=runtime_precision::kPlanLimit ||
       receipt.isEmpty() || receipt.size()>256*1024)throw Refusal(Reason::Model,"invalid public cache publication path/bytes");
#ifdef Q_OS_WIN
    DirectoryPins parents,generationPin;
    QString pinnedParent,directory;
    {
        PublicationLock reservation(cacheRoot,parent);
        pinnedParent=parents.Prepare(cacheRoot,parent);
        const auto entries=CollectGenerations(pinnedParent,"PublishFilesNew/generation-name");
        bool duplicate=false;
        for(int i=0;i<entries.count;++i) {
            const auto existing=QDir(pinnedParent).filePath(entries.names[i]);
            DirectoryPins childPin;childPin.Hold(existing);
            FilePin marker(QDir(existing).filePath(kCommitName),false,true);
            if(!HasCommit(marker))continue;
            FilePin existingPlan(QDir(existing).filePath("recognizer.plan")),existingReceipt(QDir(existing).filePath("ready.json"));
            const auto oldPlan=existingPlan.Read(runtime_precision::kPlanLimit-1),oldReceipt=existingReceipt.Read(256*1024);
            if(validate)validate(entries.names[i],oldPlan,oldReceipt);
            duplicate|=entries.names[i].toLatin1().first(64)==key;
        }
        // A match never masks a later corrupt committed sibling.
        if(duplicate)return false;
        if(entries.count==int(entries.names.size()))throw Refusal(Reason::Model,"public BF16 v2 inventory full; no new generation");
        const auto name=GenerationName(key,fixtureNonce.isEmpty()?QUuid::createUuid().toRfc4122().toHex():fixtureNonce);
        generationPin.CreateNew(parents.Last(),name); // one exclusive creation, no retry/adoption
        directory=QDir(pinnedParent).filePath(name);
    }
    // Reservation releases BEFORE callbacks/writes, so overlapping unique
    // attempts can complete independently without exceeding cooperating capacity.
    const auto write=[&](const char* name,const QByteArray& bytes) {
        FilePin file(QDir(directory).filePath(name),true);file.Write(bytes,bytes.size());
    };
    write("recognizer.plan",plan);write("ready.json",receipt);
    FilePin planPin(QDir(directory).filePath("recognizer.plan")),receiptPin(QDir(directory).filePath("ready.json"));
    if(observer)observer(directory,false); // private fixture seam, never approval
    if(precision_artifacts::Digest(planPin.Read(runtime_precision::kPlanLimit-1))!=precision_artifacts::Digest(plan) ||
       precision_artifacts::Digest(receiptPin.Read(256*1024))!=precision_artifacts::Digest(receipt))
        throw Refusal(Reason::Model,"public BF16 v2 stored bytes differ before commit");
    // External additions are not frozen by pins. Any observed over-bound
    // inventory refuses; there is no cleanup, automatic retry or migration.
    CollectGenerations(pinnedParent,"PublishFilesNew/precommit-generation-name");
    {
        FilePin marker(QDir(directory).filePath(kCommitName),true);
        marker.Write(QByteArray(kCommitBytes),sizeof(kCommitBytes)-1);HasCommit(marker);
        // Verify on the exclusive creation handle BEFORE close makes it readable.
    }
    if(observer)observer(directory,true); // any throw here is already committed
    // Exact plan/receipt read pins still coexist with other readers at commit.
    // Marker-present failures are preserved and may be corrupt/indeterminate;
    // only marker-absent generations are classified as incomplete.
    return true;
#else
    throw Refusal(Reason::Model,"public BF16 v2 publication requires native Windows file ownership");
#endif
}
// Product entry shares the SAME full validator as lookup. The opaque overload
// above exists for labelled file-publication fixtures, not model admission.
inline bool PublishFilesNew(const Request& request,const LocalBuild& build,const Corpus& corpus,
                            const QByteArray& plan,const QByteArray& receipt,const QByteArray& fixtureNonce={},
                            const StorageObserver& observer={}) {
    ValidateCommitted(QString::fromLatin1(CacheKey(build,corpus)),plan,receipt,build,corpus);
    return PublishFilesNew(request.cacheRoot,CacheParent(request,build,corpus),CacheKey(build,corpus),plan,receipt,fixtureNonce,observer,
        [&](const QString& entry,const QByteArray& storedPlan,const QByteArray& storedReceipt){ValidateCommitted(entry,storedPlan,storedReceipt,build,corpus);});
}
inline bool PublishNew(const Request& request,const LocalBuild& build,const Corpus& corpus,
                       const GoldenResult& golden,const precision_artifacts::Runtime& runtime,
                       const Publication& completion,const QByteArray& plan) {
    const auto receipt=ReceiptBytes(build,corpus,golden,runtime,completion);
    if(plan.isEmpty() || plan.size()>=runtime_precision::kPlanLimit || precision_artifacts::Digest(plan)!=build.planFileSha256)
        throw Refusal(Reason::Model,"public BF16 publication plan differs from tested bytes");
    return PublishFilesNew(request,build,corpus,plan,receipt);
}
using AttestationObserver=std::function<void(const QJsonObject&)>;
} // namespace okuflow::uct::public_bf16

namespace okuflow::uct {
// Private explicit source-validation seam. No environment/catalog override and
// no default public activation. The caller supplies isolated local roots.
std::unique_ptr<Recognizer> CreatePublicBf16ValidationRecognizer(const QString& root,const QString& sdk,
    const QString& modelRoot,const QString& corpusRoot,const QString& cacheRoot,
    public_bf16::AttestationObserver observer={},TensorRtRetirementObserver retirementObserver={},
    ModelTier detector=ModelTier::Medium,ModelTier recognizer=ModelTier::Medium,bool preferDetector=false);
}
