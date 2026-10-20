#pragma once

// Opt-in, unregistered resident SubmitDevice baseline (roadmap Design A).
// uct_batch_test::CompareTensorRtBatches submits a CPU QImage, so its worker
// time includes an upload and CPU reference/color/ink work. Every timed job
// here instead enters RecognitionScheduler::SubmitDevice with an owned
// AnalysisPool image and runs the scheduler's actual resident analysis.
// Source upload and the analysis copy are setup outside steady timing. Only
// verified cached plans and an inference-only SDK are accepted: a missing plan
// fails before model work and is never compiled here. Not a display, FPS,
// glyph-admission or screen-reader measurement. See docs/uct_resident_baseline.md.

#include "uct_batch_diagnostic.hpp"
#include "okuflow/common/uct_region_planner.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#if OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT
#include "okuflow/cuda/uct_analysis_pool.hpp"
#include <cuda_runtime.h>
#endif

namespace uct_resident_test {
using namespace okuflow::uct;
using Clock = std::chrono::steady_clock;

inline double Ms(Clock::duration value) { return std::chrono::duration<double, std::milli>(value).count(); }
inline QString Text(std::string_view value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); }
inline QString Hex(const QByteArray& value) { return QString::fromLatin1(value.toHex()); }

// --- Selection: private test selectors, never application settings ----------

struct Pair {
    ModelTier detector{ModelTier::Medium}, recognizer{ModelTier::Medium};
    QString Token() const { return ModelTierToken(detector) + QLatin1Char('-') + ModelTierToken(recognizer); }
    bool operator==(const Pair&) const = default;
};

inline constexpr int kMaximumPairs = 4, kMaximumImages = 4, kMaximumRounds = 10, kMaximumPasses = 20;
inline constexpr int kMaximumBlocks = 60, kMaximumJobs = 1000;
inline constexpr int kStartupDeadlineMs = 60000, kJobDeadlineMs = 30000;

struct Selection {
    QVector<Pair> pairs;
    QVector<int> capacities;
    QStringList images;
    int rounds{3}, passes{5};
    quint32 seed{20261020};
    bool profile{true}, media{}, paired{}, frozenCrops{}, shadowPlanner{};
    QString sdk;
    qint64 watchdogMs{};
    QString error;
    int Blocks() const { return int(pairs.size() * capacities.size()) * rounds; }
    // One startup inference, then one warmup and `passes` timed jobs per source.
    int JobsPerBlock() const { return int(images.size()) * (passes + 1) + 1; }
    // Generous runtime allowance used only to refuse a QtTest watchdog that
    // could abort a GPU block midway; it is never a pass/fail threshold.
    qint64 BlockAllowanceMs() const { return 20000 + qint64(JobsPerBlock()) * 3000; }
    qint64 RunAllowanceMs() const { return qint64(Blocks()) * BlockAllowanceMs() + 120000; }
    QJsonObject Json() const
    {
        QJsonArray pairTokens, capacityValues;
        for (const auto& pair : pairs) pairTokens.push_back(pair.Token());
        for (const int capacity : capacities) capacityValues.push_back(capacity);
        return {{"pairs", pairTokens}, {"capacities", capacityValues},
                {"images", QJsonArray::fromStringList(images)}, {"rounds", rounds},
                {"warmupsPerSourcePerBlock", 1}, {"timedPassesPerSourcePerBlock", passes},
                {"seed", qint64(seed)}, {"profiling", profile}, {"stamp", media ? "media" : "camera"},
                {"input", paired ? "pair" : "single"}, {"sdk", sdk}, {"watchdogMs", watchdogMs},
                {"frozenCrops", frozenCrops},
                {"shadowPlanner", shadowPlanner},
                {"startupDeadlineMs", kStartupDeadlineMs}, {"jobDeadlineMs", kJobDeadlineMs}};
    }
};

using EnvironmentReader = std::function<QString(const char*)>;
inline QString ProcessEnvironment(const char* key) { return qEnvironmentVariable(key); }

inline Selection ParseSelection(const EnvironmentReader& read)
{
    Selection s;
    const auto refuse = [&s](const QString& message) { s.error = message; return s; };
    for (const auto& token : read("OKUFLOW_UCT_RESIDENT_PAIRS").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const auto parts = token.trimmed().split(QLatin1Char(':'));
        const auto detector = parts.size() == 2 ? ParseModelTier(parts[0].trimmed()) : std::nullopt;
        const auto recognizer = parts.size() == 2 ? ParseModelTier(parts[1].trimmed()) : std::nullopt;
        if (!detector || !recognizer)
            return refuse("Each OKUFLOW_UCT_RESIDENT_PAIRS entry must be detector:recognizer using tiny, small or medium.");
        const Pair pair{*detector, *recognizer};
        if (s.pairs.contains(pair)) return refuse("OKUFLOW_UCT_RESIDENT_PAIRS repeats " + pair.Token() + ".");
        s.pairs.push_back(pair);
    }
    if (s.pairs.isEmpty())
        return refuse("OKUFLOW_UCT_RESIDENT_PAIRS must name detector:recognizer pairs, e.g. medium:small,medium:medium.");
    if (s.pairs.size() > kMaximumPairs) return refuse(QString("At most %1 model pairs are accepted.").arg(kMaximumPairs));

    auto capacities = read("OKUFLOW_UCT_RESIDENT_CAPACITIES");
    if (capacities.isEmpty()) capacities = QStringLiteral("1,8,16");
    for (const auto& token : capacities.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const int capacity = token.trimmed().toInt(&ok);
        if (!ok || (capacity != 1 && capacity != 8 && capacity != 16))
            return refuse("OKUFLOW_UCT_RESIDENT_CAPACITIES accepts only 1, 8 and 16.");
        if (!s.capacities.contains(capacity)) s.capacities.push_back(capacity);
    }
    std::sort(s.capacities.begin(), s.capacities.end());
    if (s.capacities.isEmpty() || s.capacities.front() != 1)
        return refuse("Batch one must run in the same selection as the strict reference for wider capacities.");

    for (const auto& path : read("OKUFLOW_UCT_RESIDENT_IMAGES").split(QLatin1Char(';'), Qt::SkipEmptyParts))
        if (!path.trimmed().isEmpty()) s.images.push_back(QFileInfo(path.trimmed()).absoluteFilePath());
    if (s.images.isEmpty() && !read("OKUFLOW_UCT_TEST_IMAGE").isEmpty())
        s.images.push_back(QFileInfo(read("OKUFLOW_UCT_TEST_IMAGE")).absoluteFilePath());
    if (s.images.isEmpty())
        return refuse("Select private sources with OKUFLOW_UCT_RESIDENT_IMAGES (semicolon-separated) or OKUFLOW_UCT_TEST_IMAGE.");
    if (s.images.size() > kMaximumImages) return refuse(QString("At most %1 source images are accepted.").arg(kMaximumImages));
    if (s.images.removeDuplicates() != 0) return refuse("OKUFLOW_UCT_RESIDENT_IMAGES repeats a source.");

    const auto bounded = [&read](const char* key, int fallback, int maximum, int& value) {
        const auto text = read(key);
        if (text.isEmpty()) { value = fallback; return true; }
        bool ok = false;
        value = text.toInt(&ok);
        return ok && value >= 1 && value <= maximum;
    };
    if (!bounded("OKUFLOW_UCT_RESIDENT_ROUNDS", 3, kMaximumRounds, s.rounds))
        return refuse(QString("OKUFLOW_UCT_RESIDENT_ROUNDS must be 1-%1.").arg(kMaximumRounds));
    if (!bounded("OKUFLOW_UCT_RESIDENT_PASSES", 5, kMaximumPasses, s.passes))
        return refuse(QString("OKUFLOW_UCT_RESIDENT_PASSES must be 1-%1.").arg(kMaximumPasses));
    if (const auto seed = read("OKUFLOW_UCT_RESIDENT_SEED"); !seed.isEmpty()) {
        bool ok = false;
        s.seed = seed.toUInt(&ok);
        if (!ok) return refuse("OKUFLOW_UCT_RESIDENT_SEED must be an unsigned 32-bit integer.");
    }
    const auto stamp = read("OKUFLOW_UCT_RESIDENT_STAMP"), input = read("OKUFLOW_UCT_RESIDENT_INPUT"),
               profile = read("OKUFLOW_UCT_RESIDENT_PROFILE");
    if (!stamp.isEmpty() && stamp != "camera" && stamp != "media")
        return refuse("OKUFLOW_UCT_RESIDENT_STAMP must be camera or media.");
    if (!input.isEmpty() && input != "single" && input != "pair")
        return refuse("OKUFLOW_UCT_RESIDENT_INPUT must be single or pair.");
    if (!profile.isEmpty() && profile != "1" && profile != "0")
        return refuse("OKUFLOW_UCT_RESIDENT_PROFILE must be 1 or 0.");
    s.media = stamp == "media";
    s.paired = input == "pair";
    s.profile = profile != "0";
    const auto frozen = read("OKUFLOW_UCT_RESIDENT_FROZEN_CROPS");
    if (!frozen.isEmpty() && frozen != "0" && frozen != "1")
        return refuse("OKUFLOW_UCT_RESIDENT_FROZEN_CROPS accepts only 0 or 1.");
    s.frozenCrops = frozen == "1";
    const auto shadow = read("OKUFLOW_UCT_RESIDENT_SHADOW_PLANNER");
    if (!shadow.isEmpty() && shadow != "0" && shadow != "1")
        return refuse("OKUFLOW_UCT_RESIDENT_SHADOW_PLANNER accepts only 0 or 1.");
    s.shadowPlanner = shadow == "1";
    if (s.shadowPlanner && !s.frozenCrops)
        return refuse("Shadow integration requires frozen source identities; copies cannot manufacture fresh evidence.");
    if (s.frozenCrops && !s.profile) return refuse("Frozen-crop trials require profiling to prove detection was skipped.");
    if (s.frozenCrops && std::any_of(s.pairs.begin(), s.pairs.end(), [&s](const Pair& pair) {
            return pair.detector != s.pairs.front().detector;
        })) return refuse("Frozen-crop trials require the same pinned detector tier for all pairs.");
    s.sdk = read("OKUFLOW_UCT_TENSORRT_ROOT");
    if (s.sdk.isEmpty()) return refuse("Select the existing inference-only TensorRT SDK with OKUFLOW_UCT_TENSORRT_ROOT.");
    if (s.Blocks() > kMaximumBlocks || qint64(s.Blocks()) * s.JobsPerBlock() > kMaximumJobs)
        return refuse(QString("The selection exceeds %1 blocks or %2 jobs.").arg(kMaximumBlocks).arg(kMaximumJobs));
    bool watchdog = false;
    s.watchdogMs = read("QTEST_FUNCTION_TIMEOUT").toLongLong(&watchdog);
    if (!watchdog || s.watchdogMs < s.RunAllowanceMs())
        return refuse(QString("Set QTEST_FUNCTION_TIMEOUT to at least %1 ms for this selection, so QtTest's "
                              "watchdog cannot abort a GPU block midway.").arg(s.RunAllowanceMs()));
    return s;
}

struct Block {
    int index{}, round{}, pair{}, capacity{};
    QVector<int> sourceOrder;
    QJsonObject Json(const Selection& s) const
    {
        QJsonArray order;
        for (const int source : sourceOrder) order.push_back(source);
        return {{"block", index}, {"round", round}, {"pair", s.pairs[pair].Token()}, {"capacity", capacity},
                {"sourceOrder", order}};
    }
};

// Each round runs every pair/capacity configuration once in its own seeded
// order; sources are shuffled within each block. The order is saved first.
inline std::vector<Block> PlanBlocks(const Selection& s)
{
    std::mt19937 random(s.seed);
    std::vector<Block> blocks;
    for (int round = 0; round < s.rounds; ++round) {
        std::vector<std::pair<int, int>> configurations;
        for (int pair = 0; pair < int(s.pairs.size()); ++pair)
            for (const int capacity : s.capacities) configurations.push_back({pair, capacity});
        std::shuffle(configurations.begin(), configurations.end(), random);
        for (const auto& [pair, capacity] : configurations) {
            Block block;
            block.index = int(blocks.size());
            block.round = round;
            block.pair = pair;
            block.capacity = capacity;
            for (int source = 0; source < int(s.images.size()); ++source) block.sourceOrder.push_back(source);
            std::shuffle(block.sourceOrder.begin(), block.sourceOrder.end(), random);
            blocks.push_back(std::move(block));
        }
    }
    return blocks;
}

// --- Instrumentation ---------------------------------------------------------

// Collects TensorRtProfile's "UCT_TRT_PROFILE {json}" lines and forwards every
// message to the previous handler. Restores that handler on destruction.
class ProfileCapture {
public:
    ProfileCapture()
    {
        auto& gate = Gate();
        std::lock_guard lock(gate.mutex);
        gate.active = this;
        gate.previous = qInstallMessageHandler(&ProfileCapture::Handle);
        previous_ = gate.previous;
    }
    ~ProfileCapture()
    {
        qInstallMessageHandler(previous_);
        auto& gate = Gate();
        std::lock_guard lock(gate.mutex);
        gate.active = nullptr;
    }
    ProfileCapture(const ProfileCapture&) = delete;
    ProfileCapture& operator=(const ProfileCapture&) = delete;
    QVector<QJsonObject> Take()
    {
        std::lock_guard lock(Gate().mutex);
        return std::exchange(lines_, {});
    }
    int Malformed() const
    {
        std::lock_guard lock(Gate().mutex);
        return malformed_;
    }

private:
    struct Shared {
        std::mutex mutex;
        ProfileCapture* active{};
        QtMessageHandler previous{};
    };
    static Shared& Gate()
    {
        static Shared shared;
        return shared;
    }
    static void Handle(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        QtMessageHandler forward{};
        {
            auto& gate = Gate();
            std::lock_guard lock(gate.mutex);
            forward = gate.previous;
            if (gate.active && type == QtInfoMsg && message.startsWith(QLatin1String("UCT_TRT_PROFILE"))) {
                const auto brace = message.indexOf(QLatin1Char('{'));
                const auto document = brace < 0 ? QJsonDocument() : QJsonDocument::fromJson(message.mid(brace).toUtf8());
                if (document.isObject()) gate.active->lines_.push_back(document.object());
                else ++gate.active->malformed_;
            }
        }
        if (forward) forward(type, context, message);
    }
    QtMessageHandler previous_{};
    QVector<QJsonObject> lines_;
    int malformed_{};
};

// Forwards every DeviceImage call to the real owned AnalysisPool image, so the
// scheduler and TensorRT run their production paths. It only adds clock reads
// around WaitReady/PrepareResidentAnalysis and counts any full-image egress.
class MeteredImage final : public DeviceImage {
public:
    explicit MeteredImage(SharedDeviceImage owned) : owned_(std::move(owned))
    {
        width = owned_->width;
        height = owned_->height;
        device = owned_->device;
        pitch = owned_->pitch;
        sourceEpoch = owned_->sourceEpoch;
        frameSequence = owned_->frameSequence;
        processingGeneration = owned_->processingGeneration;
        timestampMs = owned_->timestampMs;
        kind = owned_->kind;
        historyEpoch = owned_->historyEpoch;
        seedFrameSequence = owned_->seedFrameSequence;
        blendFrameCount = owned_->blendFrameCount;
        temporalAlpha = owned_->temporalAlpha;
    }
    const void* Data() const override { return owned_->Data(); }
    bool WaitReady(void* stream, QString& error) const override
    {
        const auto start = Clock::now();
        const bool ready = owned_->WaitReady(stream, error);
        waitNs_ += Elapsed(start);
        ++waitCalls_;
        return ready;
    }
    const void* ValidationData() const override { return owned_->ValidationData(); }
    std::size_t ValidationPitch() const override { return owned_->ValidationPitch(); }
    bool WaitValidationReady(void* stream, QString& error) const override
    {
        ++validationWaitCalls_;
        return owned_->WaitValidationReady(stream, error);
    }
    bool HasResidentAnalysis() const override { return owned_->HasResidentAnalysis(); }
    bool PrepareResidentAnalysis(RecognitionResult& result, int decimation, bool probeOnly,
                                 const std::atomic<bool>& cancelled, QString& error) const override
    {
        const auto start = Clock::now();
        const bool prepared = owned_->PrepareResidentAnalysis(result, decimation, probeOnly, cancelled, error);
        residentNs_ += Elapsed(start);
        ++residentCalls_;
        residentOk_ = prepared;
        return prepared;
    }
    QImage Readback(const std::atomic<bool>& cancelled, QString& error) const override
    {
        ++egress_;
        return owned_->Readback(cancelled, error);
    }
    QImage ReadValidation(const std::atomic<bool>& cancelled, QString& error) const override
    {
        ++egress_;
        return owned_->ReadValidation(cancelled, error);
    }
    QImage ReadProbe(const std::atomic<bool>& cancelled, QString& error) const override
    {
        ++egress_;
        return owned_->ReadProbe(cancelled, error);
    }
    bool HasHoldProvenance() const override { return owned_->HasHoldProvenance(); }
    HoldProvenance ReadHoldProvenance(const std::atomic<bool>& cancelled, QString& error) const override
    {
        return owned_->ReadHoldProvenance(cancelled, error);
    }
    struct Meters {
        int waitCalls{}, validationWaitCalls{}, residentCalls{}, egress{};
        double waitMs{}, residentMs{};
        bool residentOk{};
    };
    Meters Read() const
    {
        return {waitCalls_.load(), validationWaitCalls_.load(), residentCalls_.load(), egress_.load(),
                waitNs_.load() / 1e6, residentNs_.load() / 1e6, residentOk_.load()};
    }

private:
    static long long Elapsed(Clock::time_point start)
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    }
    SharedDeviceImage owned_;
    mutable std::atomic<long long> waitNs_{}, residentNs_{};
    mutable std::atomic<int> waitCalls_{}, validationWaitCalls_{}, residentCalls_{}, egress_{};
    mutable std::atomic<bool> residentOk_{};
};

struct Completion {
    RecognitionResult result;
    BackendStatus backend;
    Clock::time_point at;
};
struct Mailbox {
    std::mutex mutex;
    std::condition_variable ready;
    BackendStatus backend;
    std::optional<Completion> completed;
};
inline std::optional<Completion> Await(Mailbox& box, Clock::time_point deadline)
{
    std::unique_lock lock(box.mutex);
    if (!box.ready.wait_until(lock, deadline, [&box] { return box.completed.has_value(); })) return std::nullopt;
    return std::exchange(box.completed, std::nullopt);
}

// --- Identities --------------------------------------------------------------

// Mirrors Network's cache identity in src/common/uct_tensorrt.cpp. Any drift
// makes the expected plan look missing, so this check fails closed.
inline QString PlanKey(const QByteArray& modelSha256Hex, const QByteArray& gpuUuid, int driver,
                       bool detection, int capacity)
{
    QByteArray identity = QByteArray::fromHex(modelSha256Hex) + gpuUuid + QByteArray::number(driver) +
                          "-trt11.4.0.106-fp32-v1";
    if (!detection && (capacity == 8 || capacity == 16))
        identity += "-rec-min1x3x48x1024-opt" + QByteArray::number(capacity) + "x3x48x1024-max" +
                    QByteArray::number(capacity) + "x3x48x1024-v2";
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

struct PlanRecord {
    QString role, tier, key, path, payloadSha256, modifiedUtc, error;
    int capacity{};
    qint64 bytes{};
    bool valid{};
    QJsonObject Json() const
    {
        return {{"role", role}, {"tier", tier}, {"capacity", capacity}, {"key", key}, {"path", path},
                {"bytes", bytes}, {"payloadSha256", payloadSha256}, {"modifiedUtc", modifiedUtc},
                {"valid", valid}, {"error", error}};
    }
};

// Read-only check with the loader's own acceptance rule: 32-byte SHA-256
// header over the payload, size within (32 B, 256 MiB). Nothing is created.
inline PlanRecord InspectPlan(const QString& cache, const QString& role, ModelTier tier, int capacity,
                              const QString& key)
{
    PlanRecord plan;
    plan.role = role;
    plan.tier = ModelTierToken(tier);
    plan.capacity = capacity;
    plan.key = key;
    plan.path = QDir(cache).filePath(key + ".plan");
    const QFileInfo info(plan.path);
    if (!info.exists()) { plan.error = "missing"; return plan; }
    plan.bytes = info.size();
    plan.modifiedUtc = info.lastModified().toUTC().toString(Qt::ISODateWithMs);
    if (plan.bytes <= 32 || plan.bytes >= 256ll * 1024 * 1024) { plan.error = "size outside loader limits"; return plan; }
    QFile file(plan.path);
    if (!file.open(QIODevice::ReadOnly)) { plan.error = "unreadable"; return plan; }
    const QByteArray header = file.read(32);
    QCryptographicHash payload(QCryptographicHash::Sha256);
    if (header.size() != 32 || !payload.addData(&file)) { plan.error = "unreadable payload"; return plan; }
    plan.payloadSha256 = Hex(header);
    plan.valid = payload.result() == header;
    if (!plan.valid) plan.error = "payload digest mismatch";
    return plan;
}

// Fresh full-file hashes, not metadata/header equality, prove unchanged bytes.
inline QJsonArray CacheInventory(const QString& cache)
{
    QJsonArray files;
    for (const auto& info : QDir(cache).entryInfoList(QDir::Files | QDir::Hidden | QDir::System, QDir::Name)) {
        QFile file(info.filePath());
        const QByteArray header = file.open(QIODevice::ReadOnly) ? file.read(32) : QByteArray();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        const bool hashed = file.isOpen() && file.seek(0) && hash.addData(&file);
        files.push_back(QJsonObject{{"name", info.fileName()}, {"bytes", qint64(info.size())},
            {"modifiedUtc", info.lastModified().toUTC().toString(Qt::ISODateWithMs)}, {"header", Hex(header)},
            {"fileSha256", hashed ? Hex(hash.result()) : QString()}, {"hashed", hashed}});
    }
    return files;
}

inline QJsonObject PinJson(bool detection, ModelTier tier)
{
    const auto* pin = FindPaddleModelPin(detection, tier);
    if (!pin) return {};
    return {{"tier", ModelTierToken(tier)}, {"directory", Text(pin->directory)},
            {"modelSha256", Text(pin->modelSha256)}, {"configSha256", Text(pin->configSha256)},
            {"repository", Text(pin->repository)}, {"revision", Text(pin->revision)}};
}

inline QJsonObject ModelsJson(const Pair& pair, const PaddleAssets& assets)
{
    const auto dictionary = assets.dictionary.join(QLatin1Char('\n')).toUtf8();
    return {{"detector", PinJson(true, pair.detector)}, {"recognizer", PinJson(false, pair.recognizer)},
            {"root", assets.root}, {"dictionaryEntries", int(assets.dictionary.size())},
            {"dictionarySha256", Hex(QCryptographicHash::hash(dictionary, QCryptographicHash::Sha256))}};
}

// CPU oracle of RecognitionResult::DigestKind::GpuTreeSha256V1, as in the
// existing resident fingerprint fixture: 4096-byte leaves, 16-way parents.
inline QByteArray TreeDigest(const QImage& page)
{
    QByteArray packed;
    packed.reserve(qsizetype(page.width()) * 4 * page.height());
    for (int y = 0; y < page.height(); ++y)
        packed.append(reinterpret_cast<const char*>(page.constScanLine(y)), qsizetype(page.width()) * 4);
    QVector<QByteArray> nodes;
    for (qsizetype i = 0; i < packed.size(); i += 4096)
        nodes.push_back(QCryptographicHash::hash(QByteArray(1, char(0)) + packed.mid(i, 4096), QCryptographicHash::Sha256));
    while (nodes.size() > 1) {
        QVector<QByteArray> next;
        for (qsizetype i = 0; i < nodes.size(); i += 16) {
            QByteArray bytes(1, char(1));
            for (qsizetype k = i; k < std::min(i + 16, nodes.size()); ++k) bytes += nodes[k];
            next.push_back(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        }
        nodes = std::move(next);
    }
    return nodes.isEmpty() ? QByteArray() : nodes.front();
}

inline QString FileSha256(const QString& path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? Hex(hash.result()) : QString();
}

// --- Raw output records ------------------------------------------------------

inline QJsonObject OutputJson(const RecognitionOutput& output)
{
    QJsonArray lines;
    for (const auto& line : output.lines) {
        QJsonArray words;
        for (const auto& word : line.words) {
            QJsonObject value{{"text", word.text}, {"disposition", int(word.disposition)}, {"confidence", word.confidence},
                {"bounds", uct_batch_test::Rect(word.boundsPx)}, {"protectedBounds", uct_batch_test::Rect(word.protectedBoundsPx)},
                {"inkSearch", uct_batch_test::Rect(word.inkSearchPx)}, {"paddedBounds", word.paddedBounds}};
            if (word.colors) {
                value["foreground"] = word.colors->foreground.name();
                value["background"] = word.colors->background.name();
            }
            words.push_back(value);
        }
        lines.push_back(words);
    }
    return {{"ok", output.ok}, {"cancelled", output.cancelled}, {"inputFault", output.inputFault},
            {"backendPreparing", output.backendPreparing}, {"compositeInput", output.compositeInput},
            {"error", output.error}, {"lines", lines}};
}

inline QJsonObject ResultJson(const RecognitionResult& result)
{
    QJsonObject shadow;
    if (result.shadowPlan) {
        const auto& plan = *result.shadowPlan;
        shadow = {{"status", int(plan.status)}, {"rejectReason", int(plan.rejectReason)},
            {"planGeneration", qint64(plan.planGeneration)}, {"plannerEpoch", qint64(plan.plannerEpoch)},
            {"fullOcrAuthoritative", plan.authoritativeAction == RegionAuthoritativeAction::kRunFullOcr},
            {"jobId", qint64(plan.jobId)}, {"authority", qint64(plan.authority)},
            {"acceptedObservations", qint64(plan.coverage.acceptedObservations)},
            {"fullCandidateWords", plan.comparison.fullCandidateWords},
            {"fullOmittedWords", plan.comparison.fullOmittedWords},
            {"selectedRegions", plan.coverage.selectedRegions}, {"coverageDebt", plan.coverage.coverageDebt},
            {"deadlineObservation", qint64(plan.coverage.coverageDeadlineObservation)}};
    }
    QJsonArray colors, matches;
    for (const auto& line : result.lineColors)
        colors.push_back(QJsonArray{line.foreground.name(), line.background.name()});
    for (const bool match : result.currentInputMatches) matches.push_back(match);
    int validReferences = 0, residentReferences = 0;
    for (const auto& line : result.prepared) {
        validReferences += line.reference.valid;
        residentReferences += line.reference.valid && line.reference.resident && line.reference.values.isEmpty();
    }
    return {{"jobId", qint64(result.jobId)}, {"authority", qint64(result.authority)},
            {"shadowPlan", shadow}, {"shadowPlannerMs", result.shadowPlannerMs},
            {"shadowPlannerError", result.shadowPlannerError},
            {"geometryGeneration", qint64(result.stamp.geometryGeneration)},
            {"timestampMs", qint64(result.stamp.timestampMs)},
            {"viewport", QJsonArray{qint64(result.stamp.viewportWidth), qint64(result.stamp.viewportHeight)}},
            {"historyEpoch", qint64(result.stamp.historyEpoch)}, {"seedFrame", qint64(result.stamp.seedFrameSeq)},
            {"blendFrameCount", qint64(result.stamp.blendFrameCount)}, {"temporalAlpha", result.stamp.temporalAlpha},
            {"deviceInput", result.deviceInput}, {"workerMs", result.durationMs},
            {"frameSequence", qint64(result.stamp.frameSeq)}, {"sourceEpoch", qint64(result.stamp.sourceEpoch)},
            {"liveCameraCapture", result.stamp.liveCameraCapture}, {"temporallySmoothed", result.stamp.temporallySmoothed},
            {"output", OutputJson(result.output)}, {"lineColors", colors}, {"currentInputMatches", matches},
            {"preparedLines", int(result.prepared.size())}, {"validLineReferences", validReferences},
            {"residentLineReferences", residentReferences},
            {"referenceValid", result.reference.IsValid()}, {"referenceResident", bool(result.reference.resident)},
            {"sourcePixelDigest", Hex(result.sourcePixelDigest)},
            {"sourcePixelDigestKind", result.sourcePixelDigestKind == RecognitionResult::DigestKind::GpuTreeSha256V1
                                          ? "GpuTreeSha256V1" : "Sha256"}};
}

// --- Validity: a non-GPU, non-resident or fallback result is never timed -----

inline int CandidateWords(const RecognitionOutput& output)
{
    int candidates = 0;
    for (const auto& line : output.lines)
        for (const auto& word : line.words) candidates += word.disposition == TokenDisposition::kCandidate;
    return candidates;
}

inline QString Validate(const Completion& completion, const MeteredImage::Meters& meters,
                        const QVector<QJsonObject>& profiles, int capacity, const Selection& selection,
                        const QByteArray& sourceTree)
{
    const auto& result = completion.result;
    const auto& output = result.output;
    if (!output.ok || output.cancelled || output.backendPreparing || output.inputFault || output.compositeInput)
        return "The resident job did not complete successfully: " + output.error + " Backend: " +
               completion.backend.detail;
    if (!completion.backend.usesGpu || completion.backend.name != QStringLiteral("PaddleOCR / TensorRT CUDA"))
        return "The result did not come from the TensorRT GPU backend: " + completion.backend.name;
    if (!completion.backend.detail.contains(QStringLiteral("recognizer batch=%1.").arg(capacity)))
        return "The actual recognizer capacity differs from the request; a fallback is not benchmarked: " +
               completion.backend.detail;
    if (!result.deviceInput) return "The scheduler did not take the device-input path.";
    if (selection.shadowPlanner && (!result.shadowPlan || !result.shadowPlannerError.isEmpty() ||
        result.shadowPlan->authoritativeAction != RegionAuthoritativeAction::kRunFullOcr ||
        result.shadowPlan->jobId != result.jobId || result.shadowPlan->authority != result.authority))
        return "Shadow observer failed or lost the authoritative full-OCR ticket.";
    if (meters.waitCalls < 1) return "TensorRT did not consume the owned device image.";
    if (meters.residentCalls != 1 || !meters.residentOk)
        return "Scheduler resident analysis did not run exactly once successfully.";
    if (meters.egress != 0) return "Full-image egress occurred (Readback/ReadValidation/ReadProbe).";
    if (!result.reference.IsValid() || !result.reference.resident || !result.reference.luma.isNull() ||
        !result.reference.integral.isEmpty())
        return "The job reference is not resident.";
    if (result.prepared.size() != output.lines.size() || result.lineColors.size() != output.lines.size())
        return "Prepared lines or resident line colors are not parallel to the output lines.";
    for (const auto& line : result.prepared)
        if (line.reference.valid && (!line.reference.resident || !line.reference.values.isEmpty()))
            return "A prepared line reference is not resident.";
    if (selection.media) {
        if (result.sourcePixelDigestKind != RecognitionResult::DigestKind::GpuTreeSha256V1 ||
            result.sourcePixelDigest != sourceTree)
            return "The resident fingerprint does not match the exact source bytes.";
    } else if (!result.sourcePixelDigest.isEmpty()) {
        return "A camera stamp unexpectedly fingerprinted the source.";
    }
    if (selection.paired && result.currentInputMatches.size() != output.lines.size())
        return "Paired-input registration verdicts are missing.";
    if (selection.profile) {
        if (profiles.size() != 1) return QString("Expected exactly one TensorRT profile, received %1.").arg(profiles.size());
        const auto& profile = profiles.front();
        if (profile["batchCapacity"].toInt() != capacity || profile["maximumActualBatch"].toInt() > capacity)
            return "The TensorRT profile reports a different batch capacity.";
        if (profile["omittedEventPairs"].toInt() != 0)
            return "The stage trace was truncated; omitted intervals cannot be attributed to host overhead.";
        const auto hash = profile["orderedCropDescriptorSha256"].toString().toLatin1();
        if (hash.size() != 64 || QByteArray::fromHex(hash).toHex() != hash)
            return "The profiler did not retain an exact ordered-crop descriptor fingerprint.";
    } else if (!profiles.isEmpty()) {
        return "Profiling was disabled, yet a TensorRT profile was emitted.";
    }
    if (CandidateWords(output) == 0) return "The source produced no candidate words, so recognition was not exercised.";
    return {};
}

// --- Strict and diagnostic comparisons ---------------------------------------

struct TimedOutput {
    int round{}, pair{}, capacity{}, source{}, pass{};
    QString file;
    RecognitionOutput output;
    QString orderedCropHash;
};

inline std::vector<const RecognizedWord*> Flatten(const RecognitionOutput& output)
{
    std::vector<const RecognizedWord*> words;
    for (const auto& line : output.lines)
        for (const auto& word : line.words) words.push_back(&word);
    return words;
}
inline QRectF Envelope(const RecognizedWord& word) { return word.protectedBoundsPx.isEmpty() ? word.boundsPx : word.protectedBoundsPx; }
inline double Iou(const QRectF& a, const QRectF& b)
{
    const auto shared = a.intersected(b);
    const double overlap = shared.isEmpty() ? 0 : shared.width() * shared.height();
    const double area = a.width() * a.height() + b.width() * b.height() - overlap;
    return area > 0 ? overlap / area : 0;
}
inline double RectDelta(const QRectF& a, const QRectF& b)
{
    return std::max({std::abs(a.x() - b.x()), std::abs(a.y() - b.y()), std::abs(a.width() - b.width()),
                     std::abs(a.height() - b.height())});
}

// Matches spatial occurrences instead of array positions, so a swapped line
// pair is not reported as hundreds of pixels of movement. Never a gate.
struct IdentityMatch {
    int referenceWords{}, actualWords{}, matched{}, sameTextCandidates{}, changedText{}, changedDisposition{}, orderDescents{};
    double bounds{}, protectedBounds{}, inkSearch{}, confidence{};
    bool skipped{};
    QJsonArray examples;
    QJsonObject Json() const
    {
        return {{"referenceWords", referenceWords}, {"actualWords", actualWords}, {"matchedOccurrences", matched},
                {"unmatchedReference", referenceWords - matched}, {"unmatchedActual", actualWords - matched},
                {"sameTextCandidates", sameTextCandidates}, {"changedText", changedText},
                {"changedDisposition", changedDisposition}, {"orderDescents", orderDescents},
                {"maximumBoundsDeltaPx", bounds}, {"maximumProtectedDeltaPx", protectedBounds},
                {"maximumInkSearchDeltaPx", inkSearch}, {"maximumConfidenceDelta", confidence},
                {"skipped", skipped}, {"examples", examples},
                {"rule", "Greedy one-to-one occurrence match, protected-envelope IoU >= 0.5; diagnostic only."}};
    }
};

inline IdentityMatch MatchByOccurrence(const RecognitionOutput& reference, const RecognitionOutput& actual)
{
    IdentityMatch m;
    const auto a = Flatten(reference), b = Flatten(actual);
    m.referenceWords = int(a.size());
    m.actualWords = int(b.size());
    if (qint64(a.size()) * qint64(b.size()) > 4000000) { m.skipped = true; return m; }
    struct Candidate { double overlap; int i, j; };
    std::vector<Candidate> candidates;
    for (int i = 0; i < int(a.size()); ++i)
        for (int j = 0; j < int(b.size()); ++j)
            if (const double overlap = Iou(Envelope(*a[i]), Envelope(*b[j])); overlap >= .5)
                candidates.push_back({overlap, i, j});
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& x, const Candidate& y) { return x.overlap > y.overlap; });
    std::vector<int> forward(a.size(), -1), backward(b.size(), -1);
    for (const auto& candidate : candidates)
        if (forward[candidate.i] < 0 && backward[candidate.j] < 0) {
            forward[candidate.i] = candidate.j;
            backward[candidate.j] = candidate.i;
        }
    int previous = -1;
    for (int i = 0; i < int(a.size()); ++i) {
        const int j = forward[i];
        if (j < 0) continue;
        ++m.matched;
        m.orderDescents += j < previous;
        previous = j;
        const auto& x = *a[i];
        const auto& y = *b[j];
        const bool text = x.text != y.text, disposition = x.disposition != y.disposition;
        m.changedText += text;
        m.changedDisposition += disposition;
        m.sameTextCandidates += !text && x.disposition == TokenDisposition::kCandidate &&
                                y.disposition == TokenDisposition::kCandidate;
        m.bounds = std::max(m.bounds, RectDelta(x.boundsPx, y.boundsPx));
        m.protectedBounds = std::max(m.protectedBounds, RectDelta(x.protectedBoundsPx, y.protectedBoundsPx));
        m.inkSearch = std::max(m.inkSearch, RectDelta(x.inkSearchPx, y.inkSearchPx));
        m.confidence = std::max(m.confidence, std::abs(x.confidence - y.confidence));
        if ((text || disposition) && m.examples.size() < 16)
            m.examples.push_back(QJsonObject{{"reference", x.text}, {"actual", y.text},
                {"referenceDisposition", int(x.disposition)}, {"actualDisposition", int(y.disposition)},
                {"envelope", uct_batch_test::Rect(Envelope(x))}});
    }
    return m;
}

// Mirrors PaddleDecodeTokens' candidate rule (mean confidence >= 0.7) only to
// report how many decisions lie within an observed drift. Never a gate.
inline constexpr double kCandidateConfidenceRule = 0.7;
inline QJsonObject ThresholdWindow(const RecognitionOutput& output, double drift)
{
    double margin = 1;
    int decided = 0, within = 0;
    for (const auto& line : output.lines)
        for (const auto& word : line.words) {
            if (word.disposition != TokenDisposition::kCandidate && word.disposition != TokenDisposition::kLowConfidence)
                continue;
            ++decided;
            const double distance = std::abs(word.confidence - kCandidateConfidenceRule);
            margin = std::min(margin, distance);
            within += distance <= drift;
        }
    return {{"confidenceRule", kCandidateConfidenceRule}, {"wordsDecidedByConfidence", decided},
            {"minimumMargin", decided ? QJsonValue(margin) : QJsonValue(QJsonValue::Null)},
            {"observedDrift", drift}, {"wordsWithinObservedDrift", within}};
}

// The unchanged gate of the earlier batch diagnostic: identical text, order,
// dispositions and padding, geometry <= 0.001 px, confidence <= 0.001, versus
// the pair/source's first timed batch-one output. Batch one must also repeat.
// Separately authorized user-facing policy (2026-10-20). Historical strict
// comparisons remain intact: confidence may vary, but text/order/dispositions
// and every source-box edge must retain the stated behavior and <=10 px drift.
inline bool UserTolerancePass(const RecognitionOutput& reference, const RecognitionOutput& actual)
{
    const auto agreement = uct_batch_test::Compare(reference, actual);
    if (!reference.ok || !actual.ok || reference.cancelled || actual.cancelled ||
        reference.inputFault || actual.inputFault || reference.backendPreparing || actual.backendPreparing ||
        reference.compositeInput || actual.compositeInput ||
        !agreement.structure) return false;
    const auto edges = [](const QRectF& a, const QRectF& b) {
        return std::max({std::abs(a.left()-b.left()), std::abs(a.right()-b.right()),
                         std::abs(a.top()-b.top()), std::abs(a.bottom()-b.bottom())}) <= 10.0;
    };
    for (qsizetype l = 0; l < reference.lines.size(); ++l)
        for (qsizetype w = 0; w < reference.lines[l].words.size(); ++w) {
            const auto& a = reference.lines[l].words[w];
            const auto& b = actual.lines[l].words[w];
            if (!edges(a.boundsPx, b.boundsPx) || !edges(a.protectedBoundsPx, b.protectedBoundsPx) ||
                !edges(a.inkSearchPx, b.inkSearchPx)) return false;
        }
    return true;
}

inline bool StrictComparisons(const std::vector<TimedOutput>& timed, QJsonArray& records, int& failures)
{
    std::map<std::pair<int, int>, const TimedOutput*> reference;
    for (const auto& output : timed)
        if (output.capacity == 1) reference.try_emplace({output.pair, output.source}, &output);
    records = QJsonArray();
    failures = 0;
    bool pass = true;
    for (const auto& output : timed) {
        const auto found = reference.find({output.pair, output.source});
        if (found == reference.end()) {
            records.push_back(QJsonObject{{"file", output.file}, {"strictPass", false},
                                          {"error", "No batch-one reference ran for this pair and source."}});
            pass = false;
            ++failures;
            continue;
        }
        if (found->second == &output) continue;
        const auto agreement = uct_batch_test::Compare(found->second->output, output.output);
        const bool cropKnown = !found->second->orderedCropHash.isEmpty() && !output.orderedCropHash.isEmpty();
        const bool cropEqual = cropKnown && found->second->orderedCropHash == output.orderedCropHash;
        const bool ok = agreement.structure && agreement.geometryDelta <= 1e-3 && agreement.confidenceDelta <= 1e-3 &&
                        (!cropKnown || cropEqual);
        records.push_back(QJsonObject{{"file", output.file}, {"referenceFile", found->second->file},
            {"pair", output.pair}, {"capacity", output.capacity}, {"source", output.source},
            {"round", output.round}, {"pass", output.pass}, {"strictPass", ok},
            {"userTenPixelConfidenceVariationPass", UserTolerancePass(found->second->output, output.output)},
            {"orderedCropWorkEqual", cropKnown ? QJsonValue(cropEqual) : QJsonValue(QJsonValue::Null)},
            {"structureMatchesBatchOne", agreement.structure},
            {"maximumGeometryDeltaPx", agreement.geometryDelta}, {"maximumConfidenceDelta", agreement.confidenceDelta},
            {"identity", MatchByOccurrence(found->second->output, output.output).Json()},
            {"thresholds", ThresholdWindow(output.output, agreement.confidenceDelta)}});
        pass &= ok;
        failures += !ok;
    }
    return pass;
}

// --- Optional reading-order truth (never a gate) -----------------------------

inline QString CandidateReading(const RecognitionOutput& output)
{
    QStringList lines;
    for (const auto& line : output.lines) {
        QStringList words;
        for (const auto& word : line.words)
            if (word.disposition == TokenDisposition::kCandidate) words.push_back(word.text);
        if (!words.isEmpty()) lines.push_back(words.join(QLatin1Char(' ')));
    }
    return lines.join(QLatin1Char('\n'));
}

template <class Sequence> inline qint64 Levenshtein(const Sequence& a, const Sequence& b)
{
    std::vector<qint64> previous(std::size_t(b.size()) + 1), current(previous.size());
    std::iota(previous.begin(), previous.end(), qint64(0));
    for (qsizetype i = 0; i < a.size(); ++i) {
        current[0] = i + 1;
        for (qsizetype j = 0; j < b.size(); ++j)
            current[std::size_t(j) + 1] = std::min({previous[std::size_t(j) + 1] + 1, current[std::size_t(j)] + 1,
                                                    previous[std::size_t(j)] + (a[i] == b[j] ? 0 : 1)});
        std::swap(previous, current);
    }
    return previous.back();
}

inline bool Sensitive(const QString& token)
{
    return std::any_of(token.begin(), token.end(),
                       [](QChar c) { return c.isDigit() || c.isPunct() || c.isSymbol(); });
}

inline QJsonObject TruthJson(const QString& truthText, const RecognitionOutput& output)
{
    const QString truth = truthText.simplified(), reading = CandidateReading(output).simplified();
    const auto truthTokens = truth.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const auto readingTokens = reading.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QJsonObject json{{"truthCharacters", int(truth.size())}, {"candidateCharacters", int(reading.size())},
                     {"truthTokens", int(truthTokens.size())}, {"candidateTokens", int(readingTokens.size())},
                     {"scope", "Whitespace-normalized, reading-order-sensitive candidate text against a truth file. "
                               "Not spatial-occurrence ground truth; protected omissions count as missing text."}};
    if (qint64(truth.size()) * reading.size() <= 4000000) {
        const auto distance = Levenshtein(truth, reading);
        json["characterEditDistance"] = distance;
        json["cer"] = truth.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(double(distance) / truth.size());
    } else {
        json["characterEditDistance"] = QJsonValue(QJsonValue::Null);
        json["cer"] = QJsonValue(QJsonValue::Null);
    }
    if (qint64(truthTokens.size()) * readingTokens.size() <= 4000000) {
        const auto words = Levenshtein(truthTokens, readingTokens);
        json["wordEditDistance"] = words;
        json["wer"] = truthTokens.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(double(words) / truthTokens.size());
    } else {
        json["wordEditDistance"] = QJsonValue(QJsonValue::Null);
        json["wer"] = QJsonValue(QJsonValue::Null);
    }
    QHash<QString, int> available;
    for (const auto& token : readingTokens) ++available[token];
    int sensitive = 0, found = 0;
    for (const auto& token : truthTokens) {
        if (!Sensitive(token)) continue;
        ++sensitive;
        if (available.value(token) > 0) { --available[token]; ++found; }
    }
    json["sensitiveTruthTokens"] = sensitive;
    json["sensitiveExactRecall"] = sensitive ? QJsonValue(double(found) / sensitive) : QJsonValue(QJsonValue::Null);
    return json;
}

// --- Statistics: raw run order is retained; no p95 below 20 samples ----------

inline double Median(std::vector<double> values)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

inline QJsonObject Stats(const std::vector<double>& runOrder)
{
    QJsonObject json{{"n", int(runOrder.size())}};
    if (runOrder.empty()) return json;
    auto sorted = runOrder;
    std::sort(sorted.begin(), sorted.end());
    const auto n = sorted.size();
    json["min"] = sorted.front();
    json["max"] = sorted.back();
    json["median"] = Median(sorted);
    json["mean"] = std::accumulate(sorted.begin(), sorted.end(), 0.0) / double(n);
    json["p95NearestRank"] = n >= 20 ? QJsonValue(sorted[std::size_t(std::ceil(.95 * double(n))) - 1])
                                     : QJsonValue(QJsonValue::Null);
    QJsonArray raw;
    for (const double value : runOrder) raw.push_back(value);
    json["runOrder"] = raw;
    return json;
}

inline QString Key(const Pair& pair, int capacity, int source)
{
    return QStringLiteral("%1/n%2/s%3").arg(pair.Token()).arg(capacity).arg(source);
}

// Model-free contract checks for the helpers above; no model, GPU or private
// image is used. The emitted UCT_TRT_PROFILE lines are synthetic.
inline void SelectionAndComparisonSelfTest()
{
    QHash<QByteArray, QString> values;
    const EnvironmentReader read = [&values](const char* key) { return values.value(QByteArray(key)); };
    QVERIFY(!ParseSelection(read).error.isEmpty()); // nothing selected is a refusal, never a default run
    values = {{"OKUFLOW_UCT_RESIDENT_PAIRS", "medium:small,medium:medium"}, {"OKUFLOW_UCT_TEST_IMAGE", "source.png"},
              {"OKUFLOW_UCT_TENSORRT_ROOT", "sdk"}, {"QTEST_FUNCTION_TIMEOUT", "100000000"}};
    const auto selection = ParseSelection(read);
    QVERIFY2(selection.error.isEmpty(), qPrintable(selection.error));
    QCOMPARE(selection.capacities, (QVector<int>{1, 8, 16}));
    QCOMPARE(selection.pairs.size(), qsizetype(2));
    QCOMPARE(selection.images.size(), qsizetype(1));
    QVERIFY(selection.profile && !selection.media && !selection.paired);
    const auto refused = [&values](QHash<QByteArray, QString> changes) {
        auto changed = values;
        for (auto it = changes.begin(); it != changes.end(); ++it) changed[it.key()] = it.value();
        return !ParseSelection([&changed](const char* key) { return changed.value(QByteArray(key)); }).error.isEmpty();
    };
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_CAPACITIES", "8,16"}})); // no strict batch-one reference
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_CAPACITIES", "1,4"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_SHADOW_PLANNER", "yes"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_SHADOW_PLANNER", "1"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_FROZEN_CROPS", "yes"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_FROZEN_CROPS", "1"}, {"OKUFLOW_UCT_RESIDENT_PROFILE", "0"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_FROZEN_CROPS", "1"},
                     {"OKUFLOW_UCT_RESIDENT_PAIRS", "medium:small,tiny:small"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_PAIRS", "medium:small,medium:small"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_PAIRS", "large:small"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_ROUNDS", "0"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_ROUNDS", "11"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_PASSES", "21"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_STAMP", "screen"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_INPUT", "triple"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_PROFILE", "yes"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_IMAGES", "a.png;b.png;c.png;d.png;e.png"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_IMAGES", "a.png;a.png"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_RESIDENT_IMAGES", "a.png;b.png;c.png;d.png"}, {"OKUFLOW_UCT_RESIDENT_PASSES", "20"}}));
    QVERIFY(refused({{"OKUFLOW_UCT_TENSORRT_ROOT", QString()}}));
    QVERIFY(refused({{"QTEST_FUNCTION_TIMEOUT", QString()}}));
    QVERIFY(refused({{"QTEST_FUNCTION_TIMEOUT", "300000"}})); // shorter than this selection's allowance
    values["OKUFLOW_UCT_RESIDENT_CAPACITIES"] = "16,1,16";
    QCOMPARE(ParseSelection(read).capacities, (QVector<int>{1, 16}));
    values.remove("OKUFLOW_UCT_RESIDENT_CAPACITIES");

    const auto blocks = PlanBlocks(selection);
    QCOMPARE(int(blocks.size()), selection.Blocks());
    for (int round = 0; round < selection.rounds; ++round) {
        QVector<QPair<int, int>> seen;
        for (const auto& block : blocks)
            if (block.round == round) seen.push_back({block.pair, block.capacity});
        QCOMPARE(seen.size(), selection.pairs.size() * selection.capacities.size());
        for (int pair = 0; pair < int(selection.pairs.size()); ++pair)
            for (const int capacity : selection.capacities)
                QCOMPARE(seen.count(qMakePair(pair, capacity)), qsizetype(1));
    }
    const auto replay = PlanBlocks(selection);
    for (std::size_t i = 0; i < blocks.size(); ++i)
        QVERIFY(replay[i].Json(selection) == blocks[i].Json(selection)); // the seed fixes the order

    const QByteArray model(64, 'a'), other(64, 'b'), uuid(16, '\x01');
    const auto detector = PlanKey(model, uuid, 13000, true, 1);
    QCOMPARE(detector.size(), qsizetype(64));
    QCOMPARE(PlanKey(model, uuid, 13000, false, 1), detector); // only wider recognizer profiles extend identity
    QVERIFY(PlanKey(model, uuid, 13000, false, 8) != detector);
    QVERIFY(PlanKey(model, uuid, 13000, false, 16) != PlanKey(model, uuid, 13000, false, 8));
    QVERIFY(PlanKey(model, uuid, 13001, true, 1) != detector);
    QVERIFY(PlanKey(other, uuid, 13000, true, 1) != detector);

    std::vector<double> samples(19);
    std::iota(samples.begin(), samples.end(), 1.0);
    const auto nineteen = Stats(samples);
    QVERIFY(nineteen["p95NearestRank"].isNull());
    samples.push_back(20);
    const auto twenty = Stats(samples);
    QCOMPARE(twenty["p95NearestRank"].toDouble(), 19.0);
    QCOMPARE(twenty["median"].toDouble(), 10.5);
    QCOMPARE(twenty["runOrder"].toArray().size(), qsizetype(20));

    const auto word = [](const QString& text, QRectF bounds, double confidence) {
        RecognizedWord value{text, bounds};
        value.protectedBoundsPx = bounds;
        value.inkSearchPx = bounds;
        value.confidence = confidence;
        return value;
    };
    RecognitionOutput n1;
    n1.ok = true;
    n1.lines = {RecognizedLine{{word("Annotate", {10, 100, 80, 12}, .91)}},
                RecognizedLine{{word("Edited", {10, 130, 60, 12}, .95)}}};
    auto swapped = n1;
    std::swap(swapped.lines[0], swapped.lines[1]);
    auto drift = n1;
    drift.lines[0].words[0].confidence += .002;
    std::vector<TimedOutput> timed{{0, 0, 1, 0, 1, "n1-a", n1}, {1, 0, 1, 0, 1, "n1-b", n1},
                                   {0, 0, 8, 0, 1, "n8", swapped}, {0, 0, 16, 0, 1, "n16", drift}};
    QJsonArray records;
    int failures = 0;
    QVERIFY(!StrictComparisons(timed, records, failures));
    QCOMPARE(failures, 2);
    QCOMPARE(records.size(), qsizetype(3));
    QVERIFY(records[0].toObject()["strictPass"].toBool()); // batch one repeated exactly
    const auto reordered = records[1].toObject();
    QVERIFY(!reordered["strictPass"].toBool());
    QCOMPARE(reordered["identity"].toObject()["matchedOccurrences"].toInt(), 2);
    QCOMPARE(reordered["identity"].toObject()["orderDescents"].toInt(), 1);
    QCOMPARE(reordered["identity"].toObject()["changedText"].toInt(), 0);
    QVERIFY(!records[2].toObject()["strictPass"].toBool()); // 0.002 confidence drift exceeds 0.001
    QVERIFY(UserTolerancePass(n1, drift));
    QVERIFY(!UserTolerancePass(n1, swapped));
    auto allowed = n1;
    allowed.lines[0].words[0].boundsPx.translate(10, 0);
    QVERIFY(UserTolerancePass(n1, allowed));
    allowed.lines[0].words[0].boundsPx.translate(.001, 0);
    QVERIFY(!UserTolerancePass(n1, allowed));
    allowed = n1;
    allowed.lines[0].words[0].disposition = TokenDisposition::kLowConfidence;
    QVERIFY(!UserTolerancePass(n1, allowed));
    allowed = n1;
    allowed.lines[0].words[0].text = "Annotatc";
    QVERIFY(!UserTolerancePass(n1, allowed));
    QVERIFY(!StrictComparisons({{0, 0, 8, 0, 1, "orphan", n1}}, records, failures)); // no reference fails closed
    std::vector<TimedOutput> changedCrops{{0,0,1,0,1,"fixed",n1,QString(64, QLatin1Char('a'))},
        {0,0,8,0,1,"other-crops",n1,QString(64, QLatin1Char('b'))}};
    QVERIFY(!StrictComparisons(changedCrops, records, failures));
    QCOMPARE(failures, 1); // equal count/text is insufficient when descriptors differ
    QVERIFY(!records[0].toObject()["orderedCropWorkEqual"].toBool());

    QTemporaryDir cacheFixture;
    QVERIFY(cacheFixture.isValid());
    QFile planFile(cacheFixture.filePath("fixture.plan"));
    QVERIFY(planFile.open(QIODevice::WriteOnly));
    QCOMPARE(planFile.write(QByteArray(96, 'a')), qint64(96));
    planFile.close();
    const auto modified = QFileInfo(planFile).lastModified();
    const auto before = CacheInventory(cacheFixture.path());
    QVERIFY(planFile.open(QIODevice::ReadWrite)); QVERIFY(planFile.seek(60));
    QCOMPARE(planFile.write("b", 1), qint64(1));
    QVERIFY(planFile.flush());
    QVERIFY(planFile.setFileTime(modified, QFileDevice::FileModificationTime)); planFile.close();
    const auto after = CacheInventory(cacheFixture.path());
    QCOMPARE(before[0].toObject()["bytes"], after[0].toObject()["bytes"]);
    QCOMPARE(before[0].toObject()["header"], after[0].toObject()["header"]);
    QCOMPARE(before[0].toObject()["modifiedUtc"], after[0].toObject()["modifiedUtc"]);
    QVERIFY(before != after); // freshly hashed bytes detect metadata-preserving mutation

    QCOMPARE(Levenshtein(QStringLiteral("kitten"), QStringLiteral("sitting")), qint64(3));
    QCOMPARE(Levenshtein(QStringList{"a", "b"}, QStringList{"a", "c", "b"}), qint64(1));
    RecognitionOutput reading;
    reading.ok = true;
    reading.lines = {RecognizedLine{{word("x", {0, 0, 10, 10}, .9), word("=", {12, 0, 10, 10}, .5),
                                     word("-1.5", {24, 0, 30, 10}, .9)}}};
    reading.lines[0].words[1].disposition = TokenDisposition::kLowConfidence; // protected ink, not claimed text
    const auto truth = TruthJson(QStringLiteral("x  = -1.5\n"), reading);
    QCOMPARE(truth["characterEditDistance"].toInt(), 2);
    QCOMPARE(truth["wordEditDistance"].toInt(), 1);
    QCOMPARE(truth["sensitiveTruthTokens"].toInt(), 2);
    QCOMPARE(truth["sensitiveExactRecall"].toDouble(), .5);

    QImage tiny(3, 2, QImage::Format_ARGB32);
    tiny.fill(Qt::white);
    const auto digest = TreeDigest(tiny);
    QCOMPARE(digest.size(), qsizetype(32));
    tiny.setPixel(2, 1, qRgb(254, 255, 255));
    QVERIFY(TreeDigest(tiny) != digest);

    struct Owned final : DeviceImage {
        mutable int prepared{}, reads{};
        const void* Data() const override { return this; }
        bool WaitReady(void*, QString&) const override { return true; }
        bool HasResidentAnalysis() const override { return true; }
        bool PrepareResidentAnalysis(RecognitionResult&, int, bool, const std::atomic<bool>&, QString&) const override
        {
            ++prepared;
            return true;
        }
        QImage Readback(const std::atomic<bool>&, QString&) const override
        {
            ++reads;
            return {};
        }
    };
    const auto owned = std::make_shared<Owned>();
    owned->width = 64; owned->height = 32; owned->pitch = 256; owned->sourceEpoch = 3; owned->frameSequence = 9;
    owned->kind = AnalysisKind::TemporallySmoothed; owned->blendFrameCount = 2;
    const auto metered = std::make_shared<MeteredImage>(owned);
    QVERIFY(metered->Data() == owned->Data());
    QVERIFY(metered->width == 64 && metered->height == 32 && metered->pitch == 256);
    QVERIFY(metered->sourceEpoch == 3 && metered->frameSequence == 9 && metered->blendFrameCount == 2);
    QVERIFY(metered->kind == AnalysisKind::TemporallySmoothed);
    RecognitionResult prepared;
    std::atomic<bool> cancelled{false};
    QString error;
    QVERIFY(metered->HasResidentAnalysis());
    QVERIFY(metered->WaitReady(nullptr, error));
    QVERIFY(metered->PrepareResidentAnalysis(prepared, 2, false, cancelled, error));
    QVERIFY(metered->Readback(cancelled, error).isNull());
    QVERIFY(metered->ReadProbe(cancelled, error).isNull());
    const auto meters = metered->Read();
    QCOMPARE(meters.waitCalls, 1);
    QCOMPARE(meters.residentCalls, 1);
    QVERIFY(meters.residentOk);
    QCOMPARE(meters.egress, 2);
    QCOMPARE(owned->prepared, 1);
    QCOMPARE(owned->reads, 2);

    {
        ProfileCapture capture;
        qInfo().noquote() << "UCT_TRT_PROFILE"
                         << QJsonDocument(QJsonObject{{"batchCapacity", 8}}).toJson(QJsonDocument::Compact);
        qInfo().noquote() << "UCT_TRT_PROFILE not-json";
        qInfo("An unrelated message is forwarded, not captured.");
        const auto lines = capture.Take();
        QCOMPARE(lines.size(), qsizetype(1));
        QCOMPARE(lines.front()["batchCapacity"].toInt(), 8);
        QCOMPARE(capture.Malformed(), 1);
    }

    struct StubProbe final : ResidentProbe {
        int Width() const noexcept override { return 64; }
        int Height() const noexcept override { return 32; }
        ReferencePatch Reference(const SourceStamp&, const QRectF&) const override { return {}; }
        PatchMatch Match(const SourceStamp&, const ReferencePatch&, const MatchTuning&, std::optional<QPointF>) const override { return {}; }
        std::optional<double> Similarity(const SourceStamp&, const ResidentProbe&, const SourceStamp&) const override { return {}; }
    };
    Completion valid;
    valid.backend = {BackendState::kReady, "PaddleOCR / TensorRT CUDA", "multilingual",
                     "PP-OCRv6 GPU ...; recognizer batch=8. ", true};
    valid.result.deviceInput = true;
    valid.result.output = n1;
    valid.result.lineColors.resize(2);
    valid.result.prepared.resize(2); // untrackable lines carry invalid references
    valid.result.reference.stamp.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
    valid.result.reference.resident = std::make_shared<StubProbe>();
    const MeteredImage::Meters resident{1, 0, 1, 0, 0.0, 1.0, true};
    const QVector<QJsonObject> profile{QJsonObject{{"batchCapacity", 8}, {"maximumActualBatch", 8},
        {"orderedCropDescriptorSha256", QString(64, QLatin1Char('a'))}}};
    QVERIFY2(Validate(valid, resident, profile, 8, selection, {}).isEmpty(),
             qPrintable(Validate(valid, resident, profile, 8, selection, {})));
    QVERIFY(!Validate(valid, resident, profile, 16, selection, {}).isEmpty()); // capacity fallback is refused
    QVERIFY(!Validate(valid, resident, {}, 8, selection, {}).isEmpty());
    auto egress = resident;
    egress.egress = 1;
    QVERIFY(!Validate(valid, egress, profile, 8, selection, {}).isEmpty());
    auto noResident = resident;
    noResident.residentCalls = 0;
    QVERIFY(!Validate(valid, noResident, profile, 8, selection, {}).isEmpty());
    auto cpu = valid;
    cpu.backend.usesGpu = false;
    cpu.backend.name = QStringLiteral("Windows.Media.Ocr");
    QVERIFY(!Validate(cpu, resident, profile, 8, selection, {}).isEmpty());
    auto host = valid;
    host.result.deviceInput = false;
    QVERIFY(!Validate(host, resident, profile, 8, selection, {}).isEmpty());
    auto downloaded = valid;
    downloaded.result.reference.resident.reset();
    QVERIFY(!Validate(downloaded, resident, profile, 8, selection, {}).isEmpty());
    auto preparing = valid;
    preparing.result.output.backendPreparing = true;
    QVERIFY(!Validate(preparing, resident, profile, 8, selection, {}).isEmpty());
}

// --- Actual resident GPU run -------------------------------------------------

#if OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT
struct DeviceStream {
    cudaStream_t value{};
    DeviceStream() { if (cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking) != cudaSuccess) value = nullptr; }
    ~DeviceStream()
    {
        if (!value) return;
        cudaStreamSynchronize(value);
        cudaStreamDestroy(value);
    }
    DeviceStream(const DeviceStream&) = delete;
    DeviceStream& operator=(const DeviceStream&) = delete;
};

// Uploaded sources are freed only after their copy stream has completed.
struct DeviceUploads {
    cudaStream_t stream{};
    std::vector<void*> buffers;
    void* Allocate(std::size_t bytes)
    {
        void* data{};
        if (cudaMalloc(&data, bytes) != cudaSuccess) return nullptr;
        buffers.push_back(data);
        return data;
    }
    DeviceUploads() = default;
    ~DeviceUploads()
    {
        if (stream) cudaStreamSynchronize(stream);
        for (void* data : buffers) cudaFree(data);
    }
    DeviceUploads(const DeviceUploads&) = delete;
    DeviceUploads& operator=(const DeviceUploads&) = delete;
};

struct Source {
    QString path, truthPath, truth;
    QImage image;
    QByteArray rowSha256, treeSha256;
    QString fileSha256, truthSha256;
    void* device{};
    QJsonObject Json() const
    {
        return {{"path", path}, {"fileSha256", fileSha256}, {"width", image.width()}, {"height", image.height()},
                {"activeRowSha256", Hex(rowSha256)}, {"gpuTreeSha256V1", Hex(treeSha256)},
                {"truthFile", truthPath.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(truthPath)},
                {"truthSha256", truthSha256}};
    }
};

struct PairAssets {
    PaddleAssets assets;
    QJsonObject models;
    QVector<PlanRecord> plans;
};

struct Input {
    std::shared_ptr<MeteredImage> image;
    double setupMs{};
    int retries{};
    QString error;
};

struct Run {
    Selection selection;
    Clock::time_point started;
    QString evidence;
    std::vector<PairAssets> pairs;
    std::vector<Source> sources;
    DeviceStream stream;      // destroyed last
    DeviceUploads uploads;    // freed after the pool
    std::unique_ptr<AnalysisPool> pool;
    // Diagnostic-only: one bounded pool and one immutable lease per source.
    // The normal complete-path experiment still uses fresh two-slot captures.
    std::vector<std::unique_ptr<AnalysisPool>> frozenPools;
    std::vector<SharedDeviceImage> frozenInputs;
    std::shared_ptr<TensorRtCropDiagnostic> cropDiagnostic;
    std::uint64_t nextFrame{1};
    int jobs{};
    std::vector<TimedOutput> timed; // execution order
    std::map<QString, std::map<QString, std::vector<double>>> metrics;
    std::map<QString, std::map<int, std::vector<double>>> roundWorker, roundSubmit;
    std::map<QString, std::vector<double>> startup;
    QJsonArray blocks;

    // Fresh owned capture per job, completed before submission: the analysis
    // copy is setup, never steady timing. Two pool slots alternate.
    Input Capture(int sourceIndex)
    {
        Input input;
        if (selection.frozenCrops && frozenInputs[std::size_t(sourceIndex)]) {
            input.image = std::make_shared<MeteredImage>(frozenInputs[std::size_t(sourceIndex)]);
            return input;
        }
        const auto& source = sources[std::size_t(sourceIndex)];
        const int w = source.image.width(), h = source.image.height();
        const std::size_t pitch = std::size_t(w) * 4;
        const std::uint64_t frame = nextFrame++, epoch = std::uint64_t(sourceIndex) + 1;
        const auto begin = Clock::now(), deadline = begin + std::chrono::seconds(2);
        for (;;) {
            auto* selectedPool = selection.frozenCrops ? frozenPools[std::size_t(sourceIndex)].get() : pool.get();
            auto owned = selection.paired
                ? selectedPool->CapturePair(source.device, pitch, source.device, pitch, w, h, stream.value, epoch, frame, 1,
                                    1, frame, 1, .25f)
                : selectedPool->Capture(source.device, pitch, w, h, stream.value, epoch, frame, 1);
            if (owned) {
                if (cudaStreamSynchronize(stream.value) != cudaSuccess) {
                    input.error = "The analysis copy did not complete.";
                    return input;
                }
                if (selection.frozenCrops) frozenInputs[std::size_t(sourceIndex)] = owned;
                input.image = std::make_shared<MeteredImage>(std::move(owned));
                input.setupMs = Ms(Clock::now() - begin);
                return input;
            }
            if (selectedPool->HasFailure()) {
                input.error = "The analysis pool reported allocation pressure or quarantine.";
                return input;
            }
            if (Clock::now() >= deadline) {
                input.error = "Both analysis-pool slots stayed leased for two seconds.";
                return input;
            }
            ++input.retries;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    SourceStamp Stamp(const DeviceImage& image) const
    {
        SourceStamp stamp;
        stamp.sourceEpoch = image.sourceEpoch;
        stamp.geometryGeneration = 1;
        stamp.frameSeq = image.frameSequence;
        stamp.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
        stamp.viewportWidth = static_cast<std::uint32_t>(image.width);
        stamp.viewportHeight = static_cast<std::uint32_t>(image.height);
        stamp.viewTransform = {0, 0, 1, 1, 0, 0, 1, 1, true};
        stamp.liveCameraCapture = !selection.media;
        if (selection.paired) {
            stamp.temporallySmoothed = true;
            stamp.historyEpoch = image.historyEpoch;
            stamp.seedFrameSeq = image.seedFrameSequence;
            stamp.blendFrameCount = image.blendFrameCount;
            stamp.temporalAlpha = image.temporalAlpha;
        }
        return stamp;
    }
    QString JobFile(const Block& block, int source, const QString& kind)
    {
        return QDir(evidence).filePath(QStringLiteral("jobs/%1-%2-n%3-s%4-r%5-%6.json")
            .arg(++jobs, 4, 10, QLatin1Char('0')).arg(selection.pairs[block.pair].Token())
            .arg(block.capacity).arg(source).arg(block.round).arg(kind));
    }
    QJsonObject JobJson(const Block& block, int source, int pass, const Completion& completion,
                        Clock::time_point submitted, const Input& input, const QVector<QJsonObject>& profiles) const
    {
        const auto meters = input.image->Read();
        const auto& result = completion.result;
        const double submitToCallback = Ms(completion.at - submitted);
        QJsonObject measured{{"submitToCallbackMs", submitToCallback}, {"schedulerWorkerMs", result.durationMs},
            {"shadowPlannerMs", result.shadowPlannerMs},
            {"residentAnalysisMs", meters.residentMs}, {"inputWaitReadyMs", meters.waitMs},
            {"workerMinusResidentMs", result.durationMs - meters.residentMs},
            {"dispatchAndCallbackMs", submitToCallback - result.durationMs - result.shadowPlannerMs},
            {"analysisCaptureSetupMs", input.setupMs}, {"candidateWords", CandidateWords(result.output)},
            {"lines", int(result.output.lines.size())}};
        if (profiles.size() == 1) {
            const auto& profile = profiles.front();
            const auto stages = profile["streamStageMs"].toObject();
            double sum = 0;
            for (auto it = stages.begin(); it != stages.end(); ++it) {
                sum += it.value().toDouble();
                measured[QStringLiteral("stage.%1Ms").arg(it.key())] = it.value().toDouble();
            }
            const double wall = profile["workerWallMs"].toDouble();
            measured["recognizeDeviceProfiledWallMs"] = wall;
            measured["streamStageSumMs"] = sum;
            measured["streamIntervalGapMs"] = wall - sum;
            // Ensure, post-profile status publication, PrepareLines and the
            // empty corroboration pass; WaitReady is reported separately.
            measured["unattributedWorkerRemainderMs"] = result.durationMs - wall - meters.residentMs - meters.waitMs;
            for (const char* key : {"networkHostMs", "mergeHostMs", "metadataBytes", "recognizerCalls",
                                    "recognizerSamples", "detectorPasses", "cudaFreeBytesDeltaApprox",
                                    "detectorCountHostWaitMs", "recognizerGroupHostWaitMs"})
                measured[QString::fromLatin1(key)] = profile[QString::fromLatin1(key)].toDouble();
        }
        return {{"kind", pass < 0 ? "startup" : pass == 0 ? "warmup" : "timed"}, {"block", block.index},
                {"round", block.round}, {"pair", selection.pairs[block.pair].Token()}, {"capacity", block.capacity},
                {"source", source}, {"pass", pass}, {"metrics", measured},
                {"meters", QJsonObject{{"inputWaitReadyCalls", meters.waitCalls},
                    {"validationWaitCalls", meters.validationWaitCalls}, {"residentAnalysisCalls", meters.residentCalls},
                    {"residentAnalysisSucceeded", meters.residentOk}, {"imageEgressCalls", meters.egress},
                    {"captureRetries", input.retries}}},
                {"profile", profiles.size() == 1 ? QJsonValue(profiles.front()) : QJsonValue(QJsonValue::Null)},
                {"profileLines", int(profiles.size())},
                {"backend", QJsonObject{{"name", completion.backend.name}, {"usesGpu", completion.backend.usesGpu},
                                        {"detail", completion.backend.detail}}},
                {"result", ResultJson(result)}};
    }
    void Accumulate(const Block& block, int source, int pass, const QJsonObject& record, const QString& file,
                    RecognitionOutput output)
    {
        const auto key = Key(selection.pairs[block.pair], block.capacity, source);
        const auto values = record["metrics"].toObject();
        for (auto it = values.begin(); it != values.end(); ++it) metrics[key][it.key()].push_back(it.value().toDouble());
        roundWorker[key][block.round].push_back(values["schedulerWorkerMs"].toDouble());
        roundSubmit[key][block.round].push_back(values["submitToCallbackMs"].toDouble());
        timed.push_back({block.round, block.pair, block.capacity, source, pass, file, std::move(output),
            record["profile"].toObject()["orderedCropDescriptorSha256"].toString()});
    }
};

// One scheduler/recognizer lifetime for one pair and capacity: cached-plan
// startup, then per source one warmup and `passes` timed jobs, then exact
// State retirement before the next configuration may start.
inline void RunBlock(Run& run, const Block& block, ProfileCapture& capture)
{
    const auto& pair = run.selection.pairs[block.pair];
    const auto& assets = run.pairs[std::size_t(block.pair)].assets;
    const QString configuration = pair.Token() + QStringLiteral("/n%1").arg(block.capacity);
    uct_batch_test::Environment batch("OKUFLOW_UCT_TRT_BATCH", QByteArray::number(block.capacity));
    uct_batch_test::RetirementReceipts retirement; // outlives the scheduler, including early assertion exits
    const auto mailbox = std::make_shared<Mailbox>();
    capture.Take();
    const auto constructed = Clock::now();
    auto scheduler = std::make_unique<RecognitionScheduler>(
        CreateTensorRtRecognizer(assets.root, run.selection.sdk, pair.detector, pair.recognizer, retirement.Observer(),
                                 run.cropDiagnostic),
        [mailbox](RecognitionResult result) {
            const auto at = Clock::now();
            {
                std::lock_guard lock(mailbox->mutex);
                mailbox->completed = Completion{std::move(result), mailbox->backend, at};
            }
            mailbox->ready.notify_all();
        },
        [mailbox](BackendStatus status) {
            std::lock_guard lock(mailbox->mutex);
            mailbox->backend = std::move(status);
        },
        2, ModelTuning{}.minimumWordHeightPx, false, run.selection.shadowPlanner);

    // Startup: cached-plan deserialization, contexts and the first inference.
    const int firstSource = block.sourceOrder.front();
    auto startupInput = run.Capture(firstSource);
    QVERIFY2(startupInput.image, qPrintable(startupInput.error));
    const auto startupDeadline = constructed + std::chrono::milliseconds(kStartupDeadlineMs);
    int preparingPolls = 0;
    std::optional<Completion> first;
    Clock::time_point firstSubmitted;
    while (!first && Clock::now() < startupDeadline) {
        firstSubmitted = Clock::now();
        if (!scheduler->SubmitDevice(run.Stamp(*startupInput.image), startupInput.image))
            QFAIL(qPrintable("Startup submission refused: " + scheduler->Status().detail));
        auto completion = Await(*mailbox, startupDeadline);
        QVERIFY2(completion, "Cached TensorRT startup exceeded 60 seconds; this diagnostic never prepares plans.");
        if (completion->result.output.backendPreparing && !completion->result.output.ok) {
            ++preparingPolls;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } else {
            first = std::move(completion);
        }
    }
    QVERIFY2(first, "Cached TensorRT startup exceeded 60 seconds; this diagnostic never prepares plans.");
    const double startupMs = Ms(first->at - constructed);
    const auto startupProfiles = capture.Take();
    auto startupRecord = run.JobJson(block, firstSource, -1, *first, firstSubmitted, startupInput, startupProfiles);
    startupRecord["startupFirstResultMs"] = startupMs;
    startupRecord["preparingPolls"] = preparingPolls;
    const auto startupFile = run.JobFile(block, firstSource, QStringLiteral("startup"));
    QVERIFY(uct_batch_test::Save(startupFile, startupRecord));
    const auto startupInvalid = Validate(*first, startupInput.image->Read(), startupProfiles, block.capacity,
                                         run.selection, run.sources[std::size_t(firstSource)].treeSha256);
    QVERIFY2(startupInvalid.isEmpty(), qPrintable(startupInvalid + " Evidence: " + startupFile));
    std::uint64_t shadowEpoch = 0;
    if (run.selection.shadowPlanner) {
        const auto& plan = *first->result.shadowPlan;
        QCOMPARE(plan.status, RegionPlanStatus::kAccepted);
        QCOMPARE(plan.coverage.acceptedObservations, std::uint64_t{1});
        shadowEpoch = plan.plannerEpoch;
    }
    const QString backendDetail = first->backend.detail;
    first.reset();
    startupInput = {};

    int previousSource = firstSource;
    for (const int source : block.sourceOrder) {
        // A selected source change is an authority boundary. Retained copies
        // within that source keep the same acquisition and must be rejected
        // as duplicate shadow evidence, including between warmup/timed jobs.
        const bool sourceBoundary = source != previousSource;
        if (run.selection.shadowPlanner && sourceBoundary) scheduler->Cancel();
        for (int pass = 0; pass <= run.selection.passes; ++pass) { // pass 0 is this source's warmup
            auto input = run.Capture(source);
            QVERIFY2(input.image, qPrintable(input.error));
            const int stray = int(capture.Take().size());
            const auto stamp = run.Stamp(*input.image);
            const auto submitted = Clock::now();
            QVERIFY2(scheduler->SubmitDevice(stamp, input.image) != 0,
                     qPrintable("Resident submission refused: " + scheduler->Status().detail));
            auto completion = Await(*mailbox, submitted + std::chrono::milliseconds(kJobDeadlineMs));
            QVERIFY2(completion, "A resident job exceeded 30 seconds.");
            const auto profiles = capture.Take();
            auto record = run.JobJson(block, source, pass, *completion, submitted, input, profiles);
            record["strayProfilesBeforeSubmit"] = stray;
            const auto file = run.JobFile(block, source, pass ? QStringLiteral("pass%1").arg(pass) : QStringLiteral("warmup"));
            QVERIFY(uct_batch_test::Save(file, record));
            const auto invalid = Validate(*completion, input.image->Read(), profiles, block.capacity, run.selection,
                                          run.sources[std::size_t(source)].treeSha256);
            QVERIFY2(invalid.isEmpty(), qPrintable(invalid + " Evidence: " + file));
            if (run.selection.shadowPlanner) {
                const auto& plan = *completion->result.shadowPlan;
                // Startup already observed the first frozen acquisition.
                // Only an actual selected-source change resets its history.
                if (sourceBoundary && pass == 0) {
                    QCOMPARE(plan.status, RegionPlanStatus::kAccepted);
                    QCOMPARE(plan.resetReason, RegionResetReason::kAuthorityChanged);
                    QVERIFY(plan.plannerEpoch > shadowEpoch);
                    shadowEpoch = plan.plannerEpoch;
                } else {
                    QCOMPARE(plan.status, RegionPlanStatus::kRejected);
                    QCOMPARE(plan.rejectReason, RegionRejectReason::kDuplicateAcquisition);
                    QCOMPARE(plan.plannerEpoch, shadowEpoch);
                }
                QCOMPARE(plan.coverage.acceptedObservations, std::uint64_t{1});
            }
            if (run.selection.frozenCrops && pass > 0) {
                QCOMPARE(profiles.size(), 1);
                QCOMPARE(profiles.front()["detectorPasses"].toInt(), 0);
            }
            if (pass > 0) run.Accumulate(block, source, pass, record, file, std::move(completion->result.output));
        }
        previousSource = source;
    }

    const auto stats = scheduler->Stats();
    scheduler->Cancel();
    scheduler.reset(); // joins the worker; its recognizer publishes the exact receipt
    const auto& retired = retirement.Wait();
    auto report = retired.Json();
    const auto blockJson = block.Json(run.selection);
    for (auto it = blockJson.begin(); it != blockJson.end(); ++it) report.insert(it.key(), it.value());
    report.insert("models", run.pairs[std::size_t(block.pair)].models);
    report.insert("startupFirstResultMs", startupMs);
    report.insert("preparingPolls", preparingPolls);
    report.insert("backendDetail", backendDetail);
    report.insert("scheduler", QJsonObject{{"submitted", qint64(stats.submitted)}, {"coalesced", qint64(stats.coalesced)},
        {"started", qint64(stats.started)}, {"completed", qint64(stats.completed)}, {"revoked", qint64(stats.revoked)}});
    QVERIFY(uct_batch_test::Save(QDir(run.evidence).filePath(QStringLiteral("blocks/%1-%2-n%3-r%4.json")
        .arg(block.index, 3, 10, QLatin1Char('0')).arg(pair.Token()).arg(block.capacity).arg(block.round)), report));
    run.blocks.push_back(report);
    run.startup[configuration].push_back(startupMs);
    QVERIFY2(retired.Passed(), "Exact TensorRT State retirement failed or exceeded 10 seconds; later configurations were not started.");
    QVERIFY2(stats.coalesced == 0 && stats.revoked == 0, "A measured job overlapped another submission.");
}

inline QJsonObject MemoryJson()
{
    std::size_t available{}, total{};
    if (cudaMemGetInfo(&available, &total) != cudaSuccess) return {};
    return {{"cudaFreeBytes", qint64(available)}, {"cudaTotalBytes", qint64(total)},
            {"note", "Device-wide free memory; not peak, not exclusive to this process."}};
}
#endif

inline QJsonArray Caveats()
{
    return {
        "Steady timing excludes source upload and the device-to-device analysis copy, both completed before submission and recorded as setup.",
        "schedulerWorkerMs covers the actual SubmitDevice worker: TensorRT RecognizeDevice, scheduler resident reference/color/ink "
        "(plus GPU fingerprint for media stamps and registration for paired input), PrepareLines and status publication. "
        "Corroboration requests are empty.",
        "A metering decorator forwards every DeviceImage call to the real AnalysisPool image, adding a virtual hop and clock "
        "reads around WaitReady and PrepareResidentAnalysis.",
        "Stream stage intervals include launch gaps and external GPU contention. streamIntervalGapMs is profiled wall time not "
        "covered by any stage interval (host waits, merges, launch latency): an upper bound for host-control savings, not a "
        "measured saving.",
        "Receipts cover exact TensorRT State closure only. The scheduler worker's thread_local resident context, AnalysisPool "
        "storage, the resident retirement queue and TensorRT/CUDA vendor threads have separate lifetimes. Record the process "
        "exit code separately.",
        "No display, FPS, sensor-to-photon, first validated glyph, admission or screen-reader latency is measured.",
        "This process runs OCR without decoder or presenter work; other GPU clients on the machine are not controlled here.",
        "Candidate counts and cross-pair agreement are not accuracy. Truth-file CER is reading-order-sensitive and candidate-only.",
        "The recognizer is created directly: a fallback, preparing or capacity-reduced result fails the run instead of being "
        "timed. The production automatic CPU fallback is unchanged."};
}

inline void ResidentBaselineDiagnostic()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_RESIDENT_DIAGNOSTIC") != "1")
        QSKIP("Explicit resident SubmitDevice baseline not selected.");
#if !(OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT)
    QFAIL("The resident baseline was selected, but this build lacks the CUDA analysis pool or TensorRT adapter.");
#else
    const auto started = Clock::now();
    auto selection = ParseSelection(&ProcessEnvironment);
    QVERIFY2(selection.error.isEmpty(), qPrintable(selection.error));
    const QDir bin(QDir(selection.sdk).filePath("bin"));
    QVERIFY2(QFile::exists(bin.filePath("nvinfer_11.dll")), "Select the existing inference-only TensorRT SDK explicitly.");
    QVERIFY2(!QFile::exists(bin.filePath("nvonnxparser_11.dll")) &&
             bin.entryList({"nvinfer_builder_resource*.dll"}, QDir::Files).isEmpty(),
             "The resident baseline refuses SDK parser/builder resources, so it cannot compile a missing plan.");

    int devices{};
    QVERIFY2(cudaGetDeviceCount(&devices) == cudaSuccess && devices > 0, "No CUDA device.");
    QVERIFY(cudaSetDevice(0) == cudaSuccess);
    cudaDeviceProp properties{};
    QVERIFY(cudaGetDeviceProperties(&properties, 0) == cudaSuccess);
    QVERIFY2(properties.major * 10 + properties.minor >= 75, "TensorRT OCR requires Turing or newer.");
    int driver{}, runtime{};
    QVERIFY(cudaDriverGetVersion(&driver) == cudaSuccess);
    cudaRuntimeGetVersion(&runtime);
    const QByteArray uuid(reinterpret_cast<const char*>(&properties.uuid), sizeof(properties.uuid));

    Run run;
    run.selection = selection;
    run.started = started;
    const QString cache = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/ocr-engines");
    QStringList missing;
    for (const auto& pair : selection.pairs) {
        PairAssets entry;
        entry.assets = LoadPaddleAssets(DefaultPaddleRoot(), pair.detector, pair.recognizer);
        QVERIFY2(entry.assets.IsValid(), qPrintable(entry.assets.error));
        entry.models = ModelsJson(pair, entry.assets);
        entry.plans.push_back(InspectPlan(cache, "detector", pair.detector, 1,
                                          PlanKey(entry.assets.detectorSha256, uuid, driver, true, 1)));
        for (const int capacity : selection.capacities)
            entry.plans.push_back(InspectPlan(cache, "recognizer", pair.recognizer, capacity,
                                              PlanKey(entry.assets.recognizerSha256, uuid, driver, false, capacity)));
        for (const auto& plan : entry.plans)
            if (!plan.valid)
                missing << QStringLiteral("%1 %2 N%3 %4: %5").arg(pair.Token(), plan.role).arg(plan.capacity)
                               .arg(plan.key, plan.error);
        run.pairs.push_back(std::move(entry));
    }
    QVERIFY2(missing.isEmpty(), qPrintable("Required cached TensorRT plans are missing or invalid. Nothing was prepared "
                                           "or created; prepare them only in a separately authorized step:\n" +
                                           missing.join(QLatin1Char('\n'))));

    for (const auto& path : selection.images) {
        Source source;
        source.path = path;
        source.image = QImage(path).convertToFormat(QImage::Format_ARGB32);
        QVERIFY2(!source.image.isNull(), qPrintable("Unreadable source image: " + path));
        const qint64 bytes = qint64(source.image.width()) * source.image.height() * 4;
        QVERIFY2(source.image.width() <= 8192 && source.image.height() <= 8192 &&
                 bytes <= (selection.paired ? 64ll : 128ll) * 1024 * 1024,
                 qPrintable("The source exceeds the analysis-pool limits: " + path));
        QVERIFY(source.image.bytesPerLine() == qsizetype(source.image.width()) * 4);
        source.rowSha256 = uct_test::SourcePixelsDigest(source.image);
        source.treeSha256 = TreeDigest(source.image);
        source.fileSha256 = FileSha256(path);
        if (QFile truth(path + ".truth.txt"); truth.open(QIODevice::ReadOnly)) {
            QVERIFY2(truth.size() <= 128*1024, "Truth input exceeds the 128 KiB diagnostic bound.");
            const auto bytesRead = truth.readAll();
            source.truthPath = truth.fileName();
            source.truth = QString::fromUtf8(bytesRead);
            source.truthSha256 = Hex(QCryptographicHash::hash(bytesRead, QCryptographicHash::Sha256));
        }
        run.sources.push_back(std::move(source));
    }

    run.evidence = QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath(
        "OkuFlow/uct-resident-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + "-" +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(QDir().mkpath(QDir(run.evidence).filePath("jobs")) && QDir().mkpath(QDir(run.evidence).filePath("blocks")));
    const auto plan = PlanBlocks(selection);
    const auto cacheBefore = CacheInventory(cache);
    QVERIFY(uct_batch_test::Save(QDir(run.evidence).filePath("cache-before.json"), {{"cache", cache}, {"files", cacheBefore}}));
    QJsonArray planned, pairs, sources;
    for (const auto& block : plan) planned.push_back(block.Json(selection));
    for (std::size_t i = 0; i < run.pairs.size(); ++i) {
        QJsonArray plans;
        for (const auto& record : run.pairs[i].plans) plans.push_back(record.Json());
        pairs.push_back(QJsonObject{{"pair", selection.pairs[int(i)].Token()}, {"models", run.pairs[i].models},
                                    {"plans", plans}});
    }
    for (const auto& source : run.sources) sources.push_back(source.Json());
    const QJsonObject gpu{{"name", QString::fromUtf8(properties.name)},
        {"computeCapability", QStringLiteral("%1.%2").arg(properties.major).arg(properties.minor)},
        {"uuid", Hex(uuid)}, {"totalGlobalMemBytes", qint64(properties.totalGlobalMem)},
        {"multiprocessors", properties.multiProcessorCount}, {"pciBusId", properties.pciBusID},
        {"cudaDriverVersion", driver}, {"cudaRuntimeVersion", runtime}};
    QStringList sdkFiles = bin.entryList(QDir::Files, QDir::Name);
    const QJsonObject manifest{{"startedUtc", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"selection", selection.Json()}, {"plannedBlocks", planned}, {"pairs", pairs}, {"sources", sources},
        {"gpu", gpu}, {"sdk", QJsonObject{{"root", selection.sdk}, {"binFiles", QJsonArray::fromStringList(sdkFiles)},
            {"inferenceOnly", true}}},
        {"host", QJsonObject{{"os", QSysInfo::prettyProductName()}, {"kernel", QSysInfo::kernelVersion()},
            {"cpuArchitecture", QSysInfo::currentCpuArchitecture()}, {"logicalProcessors", QThread::idealThreadCount()},
            {"qt", QString::fromLatin1(qVersion())}, {"processId", qint64(QCoreApplication::applicationPid())},
            {"testExecutable", QCoreApplication::applicationFilePath()},
            {"testExecutableSha256", FileSha256(QCoreApplication::applicationFilePath())}}},
        {"memoryBeforeUpload", MemoryJson()}, {"caveats", Caveats()},
        {"receiptScope", "Exact TensorRT State closure only; nested AnalysisPool storage and vendor threads excluded."}};
    QVERIFY(uct_batch_test::Save(QDir(run.evidence).filePath("manifest.json"), manifest));
    qInfo().noquote() << "UCT resident baseline evidence=" << run.evidence << "blocks=" << plan.size()
                     << "jobsPerBlock=" << selection.JobsPerBlock();
    if (qEnvironmentVariable("OKUFLOW_UCT_RESIDENT_PRECHECK_ONLY") == "1") {
        // Selection, SDK, plan, model and source identities only: no model is
        // loaded, nothing is uploaded and no recognition runs.
        QVERIFY(uct_batch_test::Save(QDir(run.evidence).filePath("precheck.json"),
            {{"complete", true}, {"modelsLoaded", false}, {"plansValid", true}, {"blocksPlanned", int(plan.size())}}));
        qInfo("UCT resident precheck passed; no model was loaded and no timing was recorded.");
        return;
    }

    // Marks an early exit; the job/block records already written stay intact.
    QJsonObject progress{{"complete", false}, {"blocksPlanned", int(plan.size())}, {"blocksCompleted", 0},
                         {"note", "The function exited before its summary; see the QtTest log."}};
    struct IncompleteMarker {
        QString file;
        const QJsonObject& progress;
        bool dismissed{};
        ~IncompleteMarker()
        {
            if (!dismissed) uct_batch_test::Save(file, progress);
        }
    } marker{QDir(run.evidence).filePath("incomplete.json"), progress};

    uct_batch_test::Environment profiling("OKUFLOW_UCT_TRT_PROFILE", selection.profile ? "1" : "0");
    ProfileCapture capture;
    QVERIFY2(run.stream.value, "Fixture CUDA stream unavailable.");
    run.uploads.stream = run.stream.value;
    run.pool = std::make_unique<AnalysisPool>();
    if (selection.frozenCrops) {
        run.cropDiagnostic = std::make_shared<TensorRtCropDiagnostic>();
        run.frozenInputs.resize(run.sources.size());
        for (std::size_t i = 0; i < run.sources.size(); ++i)
            run.frozenPools.push_back(std::make_unique<AnalysisPool>());
    }
    for (auto& source : run.sources) {
        const auto bytes = std::size_t(source.image.sizeInBytes());
        source.device = run.uploads.Allocate(bytes);
        QVERIFY2(source.device, "Source upload allocation failed.");
        QVERIFY(cudaMemcpyAsync(source.device, source.image.constBits(), bytes, cudaMemcpyHostToDevice,
                                run.stream.value) == cudaSuccess);
    }
    QVERIFY(cudaStreamSynchronize(run.stream.value) == cudaSuccess);
    const auto memoryAfterUpload = MemoryJson();

    bool truncated = false;
    int completed = 0;
    for (const auto& block : plan) {
        if (Ms(Clock::now() - run.started) + double(selection.BlockAllowanceMs()) > double(selection.watchdogMs - 60000)) {
            truncated = true; // do not start a block the QtTest watchdog could abort midway
            break;
        }
        RunBlock(run, block, capture);
        if (QTest::currentTestFailed()) return;
        progress["blocksCompleted"] = ++completed;
    }

    // Analysis after every block: randomized order means wider capacities may
    // run before their batch-one reference.
    QJsonArray comparisons;
    int strictFailures = 0;
    const bool strictPass = StrictComparisons(run.timed, comparisons, strictFailures);
    QVERIFY(uct_batch_test::Save(QDir(run.evidence).filePath("comparisons.json"),
                                 {{"comparisons", comparisons}, {"strictPass", strictPass}, {"failures", strictFailures}}));

    QJsonObject configurations;
    for (const auto& [key, metrics] : run.metrics) {
        QJsonObject stats, perRound;
        for (const auto& [metric, values] : metrics) stats[metric] = Stats(values);
        for (const auto& [round, values] : run.roundWorker[key]) perRound[QString::number(round)] = Median(values);
        configurations[key] = QJsonObject{{"metrics", stats}, {"perRoundMedianSchedulerWorkerMs", perRound}};
    }
    QJsonObject startup;
    for (const auto& [configuration, values] : run.startup) startup[configuration] = Stats(values);

    // Paired by randomized round: per-round median differences between pairs.
    using RoundTable = std::map<QString, std::map<int, std::vector<double>>>;
    const auto perRound = [](const RoundTable& table, const QString& key) {
        const auto found = table.find(key);
        return found == table.end() ? std::map<int, std::vector<double>>() : found->second;
    };
    QJsonArray paired;
    for (const int capacity : selection.capacities)
        for (int source = 0; source < int(run.sources.size()); ++source)
            for (int a = 0; a < int(selection.pairs.size()); ++a)
                for (int b = a + 1; b < int(selection.pairs.size()); ++b)
                    for (const bool worker : {true, false}) {
                        const RoundTable& table = worker ? run.roundWorker : run.roundSubmit;
                        const auto x = perRound(table, Key(selection.pairs[a], capacity, source));
                        const auto y = perRound(table, Key(selection.pairs[b], capacity, source));
                        std::vector<double> differences;
                        for (const auto& [round, values] : x)
                            if (const auto found = y.find(round); found != y.end())
                                differences.push_back(Median(values) - Median(found->second));
                        paired.push_back(QJsonObject{{"a", selection.pairs[a].Token()}, {"b", selection.pairs[b].Token()},
                            {"capacity", capacity}, {"source", source},
                            {"metric", worker ? "schedulerWorkerMs" : "submitToCallbackMs"},
                            {"aMinusBPerRoundMedians", Stats(differences)},
                            {"note", "Paired by randomized round; not a significance test."}});
                    }

    // Informational only: cross-pair occurrence agreement and optional truth.
    std::map<std::tuple<int, int, int>, const TimedOutput*> firstTimed;
    for (const auto& output : run.timed) firstTimed.try_emplace({output.pair, output.capacity, output.source}, &output);
    QJsonArray crossPair, truth;
    for (int source = 0; source < int(run.sources.size()); ++source) {
        for (int a = 0; a < int(selection.pairs.size()); ++a)
            for (int b = a + 1; b < int(selection.pairs.size()); ++b) {
                const auto x = firstTimed.find({a, 1, source}), y = firstTimed.find({b, 1, source});
                if (x == firstTimed.end() || y == firstTimed.end()) continue;
                const auto match = MatchByOccurrence(x->second->output, y->second->output);
                // Counts alone cannot establish fixed crop geometry/content.
                const auto samples = [&run, &selection, source](int pair) {
                    const auto found = run.metrics.find(Key(selection.pairs[pair], 1, source));
                    if (found == run.metrics.end()) return Stats({});
                    const auto values = found->second.find(QStringLiteral("recognizerSamples"));
                    return values == found->second.end() ? Stats({}) : Stats(values->second);
                };
                crossPair.push_back(QJsonObject{{"a", selection.pairs[a].Token()}, {"b", selection.pairs[b].Token()},
                    {"source", source}, {"aFile", x->second->file}, {"bFile", y->second->file},
                    {"sameDetector", selection.pairs[a].detector == selection.pairs[b].detector},
                    {"orderedCropWorkEqual", !x->second->orderedCropHash.isEmpty() && !y->second->orderedCropHash.isEmpty()
                        ? QJsonValue(x->second->orderedCropHash == y->second->orderedCropHash) : QJsonValue(QJsonValue::Null)},
                    {"aCropDescriptorSha256", x->second->orderedCropHash},
                    {"bCropDescriptorSha256", y->second->orderedCropHash},
                    {"aRecognizerSamples", samples(a)}, {"bRecognizerSamples", samples(b)},
                    {"aCandidates", CandidateWords(x->second->output)}, {"bCandidates", CandidateWords(y->second->output)},
                    {"identity", match.Json()}, {"notAccuracy", true}});
            }
        if (run.sources[std::size_t(source)].truth.isEmpty()) continue;
        for (const auto& [identity, output] : firstTimed) {
            if (std::get<2>(identity) != source) continue;
            auto record = TruthJson(run.sources[std::size_t(source)].truth, output->output);
            record["pair"] = selection.pairs[std::get<0>(identity)].Token();
            record["capacity"] = std::get<1>(identity);
            record["source"] = source;
            record["file"] = output->file;
            truth.push_back(record);
        }
    }

    const auto cacheAfter = CacheInventory(cache);
    const bool cacheUnchanged = cacheAfter == cacheBefore;
    QVERIFY(uct_batch_test::Save(QDir(run.evidence).filePath("cache-after.json"), {{"cache", cache}, {"files", cacheAfter}}));
    const QJsonObject summary{{"complete", !truncated}, {"truncatedBeforeWatchdog", truncated},
        {"frozenCrops", selection.frozenCrops},
        {"frozenSources", run.cropDiagnostic ? run.cropDiagnostic->CapturedSources() : 0},
        {"frozenReplayCalls", run.cropDiagnostic ? qint64(run.cropDiagnostic->ReplayCalls()) : qint64(0)},
        {"blocksPlanned", int(plan.size())}, {"blocksCompleted", completed}, {"configurations", configurations},
        {"startupFirstResultMs", startup}, {"pairedByRound", paired},
        {"strict", QJsonObject{{"pass", strictPass}, {"comparisons", int(comparisons.size())},
                               {"failures", strictFailures}, {"file", "comparisons.json"}}},
        {"crossPair", crossPair}, {"truth", truth}, {"planCacheUnchanged", cacheUnchanged},
        {"profileMalformedLines", capture.Malformed()}, {"blocks", run.blocks},
        {"memoryAfterUpload", memoryAfterUpload}, {"memoryAtEnd", MemoryJson()},
        {"elapsedMs", Ms(Clock::now() - run.started)}, {"caveats", Caveats()}};
    QVERIFY(uct_batch_test::Save(QDir(run.evidence).filePath("summary.json"), summary));
    marker.dismissed = true;
    for (const auto& [key, metrics] : run.metrics) {
        const auto worker = metrics.find(QStringLiteral("schedulerWorkerMs"));
        const auto submit = metrics.find(QStringLiteral("submitToCallbackMs"));
        if (worker == metrics.end() || submit == metrics.end()) continue;
        qInfo().noquote() << "UCT resident" << key << "timed=" << worker->second.size()
                         << "median schedulerWorkerMs=" << Median(worker->second)
                         << "median submitToCallbackMs=" << Median(submit->second);
    }
    qInfo().noquote() << "UCT resident strictPass=" << strictPass << "failures=" << strictFailures
                     << "planCacheUnchanged=" << cacheUnchanged << "truncated=" << truncated;
    qInfo("Resident worker and stage timing only; no display, FPS, glyph-admission or screen-reader claim. "
          "Record the process exit code separately from the State receipts.");
    QVERIFY2(!truncated, "The selection did not finish within the QtTest watchdog allowance; completed evidence is retained.");
    QVERIFY2(cacheUnchanged, "The TensorRT plan cache changed during a run that must never create or replace plans.");
    QVERIFY2(strictPass, "A capacity changed text/order/disposition, geometry >0.001 px or confidence >0.001 versus "
                         "batch one, or batch one did not repeat. All outputs preserved; thresholds unchanged.");
#endif
}

} // namespace uct_resident_test
