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

namespace okuflow::uct::public_bf16 {
enum class CatalogUse { Production, CandidateValidation };
struct Request {
    QString modelRoot, corpusRoot, cacheRoot;
    CatalogUse use{CatalogUse::Production};
    bool enabled{};
};
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
inline QString CacheParent(const Request& request,const LocalBuild& build,const Corpus& corpus) {
    // Parent deliberately omits builder as well as driver/runtime provenance.
    // Each atomic child still binds its actual builder via CacheKey.
    auto identity=BuildIdentity(build,corpus);identity.remove("builderTensorRtVersion");
    return QDir(request.cacheRoot).filePath("public-bf16/"+QString::fromLatin1(
        precision_artifacts::Digest(QJsonDocument(identity).toJson(QJsonDocument::Compact))));
}
inline CachedPlan FindPlan(const Request& request,const LocalBuild& build,const Corpus& corpus) {
    if(!runtime_precision::LocalAbsolute(request.cacheRoot))throw Refusal(Reason::Model,"public BF16 cache root must be local/absolute");
    const QDir parent(CacheParent(request,build,corpus));
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
    const auto destination=QDir(parent).filePath(QString::fromLatin1(key));
    if(QFileInfo::exists(destination))return false; // never overwrite or repair an old generation
    if(!QDir().mkpath(parent))throw Refusal(Reason::Model,"cannot create local BF16 cache parent");
    runtime_precision::CanonicalWithin(cacheRoot,parent);
    QTemporaryDir temporary(QDir(parent).filePath(".pending-XXXXXX"));
    if(!temporary.isValid())throw Refusal(Reason::Model,"cannot create local BF16 publication staging");
    const auto write=[&](const char* name,const QByteArray& bytes) {
        QFile file(QDir(temporary.path()).filePath(name));
        if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly) || file.write(bytes)!=bytes.size() || !file.flush())
            throw Refusal(Reason::Model,"cannot write local BF16 publication staging");
    };
    write("recognizer.plan",plan);write("ready.json",receipt);
    // Same-parent directory rename exposes both files together, new-only. Qt's
    // directory rename refuses an existing destination; no copy/replace fallback.
    if(!QDir().rename(temporary.path(),destination)) {
        if(QFileInfo::exists(destination))return false;
        throw Refusal(Reason::Model,"cannot atomically publish local BF16 generation");
    }
    temporary.setAutoRemove(false);
    return true;
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
