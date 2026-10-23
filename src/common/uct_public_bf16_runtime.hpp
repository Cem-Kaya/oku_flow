#pragma once

// Private preparation/publication support. A candidate pin is validation input,
// never production activation. No environment variable supplies approval.
#include "uct_public_bf16.hpp"
#include <QTemporaryDir>
#include <QDirIterator>
#include <cmath>
#include <mutex>
#include <set>
#include <utility>
#include <cstddef>
#ifdef Q_OS_WIN
#include <Windows.h>
#endif

namespace okuflow::uct::public_bf16 {
enum class CatalogUse { Production, CandidateValidation };
struct Request {
    QString modelRoot, corpusRoot, cacheRoot;
    CatalogUse use{CatalogUse::Production};
    bool enabled{};
};
struct BaselineCachePolicy {QString root;bool allowPreparation{true};};
inline BaselineCachePolicy BaselinePolicy(const Request& request) {
    if(!request.enabled || request.use!=CatalogUse::CandidateValidation)return {};
    if(!runtime_precision::LocalAbsolute(request.cacheRoot))
        throw Refusal(Reason::Model,"candidate validation requires an explicit isolated FP32/BF16 cache root");
    return {request.cacheRoot,false}; // detector AND every FP32 fallback stay cache-only
}
inline std::span<const CorpusPin> CandidateCatalog() {
    static const std::array<CorpusPin,1> pins{{{"okuflow-owned-typeset-gray8-v1",
        "26432369ed6831431737072f857620b19c81fc9acd8c21579d50ee7bc0e9ec33"}}};
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
inline Corpus LoadCorpus(const Request& request) {
    if(Catalog(request).empty())throw Refusal(Reason::CorpusUnavailable,"public BF16 production corpus is unavailable");
    return ReadCorpus(ReadBounded(request.corpusRoot,"manifest.json",64*1024),Catalog(request),
        [&](const QString& relative){return ReadBounded(request.corpusRoot,relative,48*1024);});
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
#ifdef Q_OS_WIN
// Hold actual directories with list access and no delete sharing. Reparse points
// below the explicit canonical root are refusals BEFORE creating descendants.
// The same handles remain held through file creation and the final native rename.
class DirectoryPins {
public:
    DirectoryPins()=default;
    DirectoryPins(const DirectoryPins&)=delete;
    DirectoryPins& operator=(const DirectoryPins&)=delete;
    ~DirectoryPins(){for(int i=0;i<count_;++i)CloseHandle(handles_[i]);}
    HANDLE Hold(const QString& path,bool forRename=false) {
        if(count_==int(handles_.size()))throw Refusal(Reason::Model,"public cache path exceeds directory bound");
        const auto name=QDir::toNativeSeparators(path).toStdWString();
        // Attribute-only access did not enforce delete exclusion in root's probe.
        const auto handle=CreateFileW(name.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES|(forRename?DELETE:0),
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE)throw Refusal(Reason::Model,"cannot pin public cache directory");
        BY_HANDLE_FILE_INFORMATION information{};
        if(!GetFileInformationByHandle(handle,&information) ||
           !(information.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (information.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(handle);throw Refusal(Reason::Model,"public cache directory is not an owned ordinary directory");
        }
        handles_[count_++]=handle;
        return handle;
    }
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
                if(!QDir().mkdir(next))throw Refusal(Reason::Model,"cannot create public cache component");
            }
            Hold(next);current=next;
        }
        return current;
    }
    bool RenameNew(HANDLE staging,const QString& destination) {
        const auto name=QDir::toNativeSeparators(destination).toStdWString();
        const auto length=name.size()*sizeof(wchar_t);
        std::vector<unsigned char> bytes(offsetof(FILE_RENAME_INFO,FileName)+length,0);
        auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(bytes.data());
        rename->ReplaceIfExists=FALSE;rename->RootDirectory=nullptr;rename->FileNameLength=DWORD(length);
        std::copy(name.begin(),name.end(),rename->FileName);
        return SetFileInformationByHandle(staging,FileRenameInfo,rename,DWORD(bytes.size()))!=FALSE;
    }
private:
    std::array<HANDLE,10> handles_{};
    int count_{};
};
#endif
inline QString CacheParent(const Request& request,const LocalBuild& build,const Corpus& corpus) {
    // Parent deliberately omits builder as well as driver/runtime provenance.
    // Each atomic child still binds its actual builder via CacheKey.
    auto identity=BuildIdentity(build,corpus);identity.remove("builderTensorRtVersion");
    return QDir(request.cacheRoot).filePath("public-bf16/"+QString::fromLatin1(
        precision_artifacts::Digest(QJsonDocument(identity).toJson(QJsonDocument::Compact))));
}
inline CachedPlan FindPlan(const Request& request,const LocalBuild& build,const Corpus& corpus) {
    if(!runtime_precision::LocalAbsolute(request.cacheRoot))throw Refusal(Reason::Model,"public BF16 cache root must be local/absolute");
    const auto requestedParent=CacheParent(request,build,corpus);
#ifdef Q_OS_WIN
    DirectoryPins readPins;
    const auto located=readPins.Prepare(request.cacheRoot,requestedParent,false);
    if(located.isEmpty())return {};
    const QDir parent(located);
#else
    const QDir parent(requestedParent);
#endif
    if(!parent.exists())return {};
    QDirIterator entries(parent.absolutePath(),QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden);
    int visited=0;
    while(entries.hasNext()) {
        if(++visited>32)throw Refusal(Reason::Model,"public BF16 cache inventory exceeds bound");
        entries.next();
        const auto entry=entries.fileName();
        // Owned incomplete staging is never a usable plan.
        if(entry.startsWith(".pending-"))continue;
        if(!runtime_precision::Hash(entry.toLatin1()))throw Refusal(Reason::Model,"unknown public BF16 cache generation");
        const auto directory=parent.filePath(entry);
#ifdef Q_OS_WIN
        readPins.Hold(directory);
#endif
        runtime_precision::CanonicalWithin(request.cacheRoot,directory);
        const auto receipt=QJsonDocument::fromJson(ReadBounded(directory,"ready.json",256*1024)).object();
        const auto identity=receipt["identity"].toObject();
        auto actual=build;actual.builderVersion=identity["builderTensorRtVersion"].toString().toLatin1();
        auto expected=BuildIdentity(actual,corpus);
        actual.planFileSha256=identity["planFileSha256"].toString().toLatin1();
        auto stripped=identity;stripped.remove("planFileSha256");stripped.remove("golden");
        if(receipt["schema"]!="uct-local-bf16-ready-v1" || stripped!=expected ||
           entry.toLatin1()!=CacheKey(actual,corpus) || !runtime_precision::Hash(actual.planFileSha256) ||
           receipt["identitySha256"]!=QString::fromLatin1(precision_artifacts::Digest(QJsonDocument(identity).toJson(QJsonDocument::Compact))))
            throw Refusal(Reason::Model,"public BF16 cached receipt identity mismatch");
        const auto plan=ReadBounded(directory,"recognizer.plan",runtime_precision::kPlanLimit-1);
        if(precision_artifacts::Digest(plan)!=actual.planFileSha256)
            throw Refusal(Reason::Model,"public BF16 cached plan hash mismatch");
        // This lookup selects the current actual builder's plan recipe. Older
        // valid generations stay untouched; runtime/ABI admission is separate.
        if(actual.builderVersion!=build.builderVersion)continue;
        // Receipt text/pass fields are NEVER authority. The caller must actually
        // deserialize/check the engine and run all nine golden calls again.
        return {plan,actual.builderVersion,directory};
    }
    return {};
}
// Storage primitive, independently fixture-testable with plainly labelled
// non-model bytes. The production entry below enforces actual golden evidence.
inline bool PublishFilesNew(const QString& cacheRoot,const QString& parent,const QByteArray& key,
                            const QByteArray& plan,const QByteArray& receipt) {
    if(!runtime_precision::LocalAbsolute(cacheRoot) || !runtime_precision::Hash(key) ||
       !runtime_precision::Within(cacheRoot,parent))throw Refusal(Reason::Model,"invalid public cache publication path");
#ifdef Q_OS_WIN
    DirectoryPins parents;
    const auto pinnedParent=parents.Prepare(cacheRoot,parent);
    const auto destination=QDir(pinnedParent).filePath(QString::fromLatin1(key));
    if(QFileInfo::exists(destination))return false; // never overwrite or repair an old generation
    QTemporaryDir temporary(QDir(pinnedParent).filePath(".pending-XXXXXX"));
    if(!temporary.isValid())throw Refusal(Reason::Model,"cannot create local BF16 publication staging");
    DirectoryPins stagingPin; // closes before temporary cleanup on every failure
    const auto staging=stagingPin.Hold(temporary.path(),true);
    const auto write=[&](const char* name,const QByteArray& bytes) {
        QFile file(QDir(temporary.path()).filePath(name));
        if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly) || file.write(bytes)!=bytes.size() || !file.flush())
            throw Refusal(Reason::Model,"cannot write local BF16 publication staging");
    };
    write("recognizer.plan",plan);write("ready.json",receipt);
    // Rename the held exact staging directory, no replacement/copy fallback.
    // Ancestor and staging handles protect confinement through the transaction.
    if(!stagingPin.RenameNew(staging,destination)) {
        if(QFileInfo::exists(destination))return false;
        throw Refusal(Reason::Model,"cannot atomically publish local BF16 generation");
    }
    temporary.setAutoRemove(false);
    return true;
#else
    throw Refusal(Reason::Model,"public BF16 atomic publication requires native Windows directory ownership");
#endif
}
inline bool PublishNew(const Request& request,const LocalBuild& build,const Corpus& corpus,
                       const GoldenResult& golden,const precision_artifacts::Runtime& runtime,
                       const Publication& completion,const QByteArray& plan) {
    const auto receipt=ReceiptBytes(build,corpus,golden,runtime,completion);
    if(plan.isEmpty() || plan.size()>=runtime_precision::kPlanLimit || precision_artifacts::Digest(plan)!=build.planFileSha256)
        throw Refusal(Reason::Model,"public BF16 publication plan differs from tested bytes");
    return PublishFilesNew(request.cacheRoot,CacheParent(request,build,corpus),CacheKey(build,corpus),plan,receipt);
}
using AttestationObserver=std::function<void(const QJsonObject&)>;
} // namespace okuflow::uct::public_bf16

namespace okuflow::uct {
// Private explicit source-validation seam. No environment/catalog override and
// no default public activation. The caller supplies isolated local roots.
std::unique_ptr<Recognizer> CreatePublicBf16ValidationRecognizer(const QString& root,const QString& sdk,
    const QString& modelRoot,const QString& corpusRoot,const QString& cacheRoot,
    public_bf16::AttestationObserver observer={},TensorRtRetirementObserver retirementObserver={});
}
