#pragma once
// Round 11 diagnostics (opt-in, unregistered):
//
// - optionalAssetVerificationMatchesSequential: LoadPaddleAssets, which now
//   hashes the pinned files side by side, against a frozen copy of the
//   sequential loader of 6e454cc on the real medium model files, intact,
//   tampered and with downloaded roles. CPU only. OKUFLOW_UCT_ASSET_EQUIVALENCE=1;
//   the source files come from DefaultPaddleRoot().
// - optionalPreparationCancellation: cancels the production GPU preparation at
//   spread points, checks that destruction never waits for it and that every
//   retired State closes, and that a fresh reader afterwards becomes ready
//   with the same backend detail and the same exact output.
//   OKUFLOW_UCT_PREPARATION_CANCEL=1, OKUFLOW_UCT_TEST_IMAGE, optional
//   OKUFLOW_UCT_PREPARATION_CANCEL_POINTS (2-64, default 24) and
//   OKUFLOW_UCT_PREPARATION_CANCEL_EVIDENCE (new absolute JSON file).
// - optionalOverlappingPreparation: cancels preparation A at spread points up
//   to the end of its detector stage and starts preparation B with the same
//   models at once, without waiting for A's retirement; B must become ready
//   with the backend detail and output of an undisturbed start.
//   OKUFLOW_UCT_OVERLAP_PREPARATION=1, OKUFLOW_UCT_TEST_IMAGE, optional
//   OKUFLOW_UCT_OVERLAP_POINTS (2-64, default 16) and
//   OKUFLOW_UCT_OVERLAP_EVIDENCE (new absolute JSON file).
#include "okuflow/common/uct_paddle.hpp"
#include "okuflow/common/uct_model_pins.hpp"
#include "../src/common/uct_sha256.hpp"
#include "uct_batch_diagnostic.hpp"
#include "uct_resident_diagnostic.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>
#include <chrono>
#include <thread>
#include <Windows.h>

namespace okuflow::uct::uct_preparation_diagnostic {

// --- Frozen sequential loader (6e454cc), the oracle. Do not edit. ---------
inline bool SequentialVerify(const QString &file, const char *hash)
{
    QFile input(file);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    const auto digest = sha256::Read(input);
    return !digest.isEmpty() && input.error() == QFileDevice::NoError && digest.toHex() == hash;
}
inline PaddleAssets SequentialLoadPaddleAssets(const QString &root, ModelTier detector, ModelTier recognizer)
{
    PaddleAssets assets;
    assets.root = root;
    const auto* det=FindPaddleModelPin(true,detector);
    const auto* rec=FindPaddleModelPin(false,recognizer);
    if(!det || !rec) {assets.error="Unsupported PaddleOCR model tier.";return assets;}
    const auto directory = [&](const PaddleModelPin& pin) {
        const auto original = QDir(root).filePath(QString::fromUtf8(pin.directory.data(),qsizetype(pin.directory.size())));
        // Explicit diagnostic roots remain isolated. The production source root
        // may use an optional downloaded role without relocating runtime DLLs.
        if (QDir::cleanPath(root).compare(QDir::cleanPath(DefaultPaddleRoot()),Qt::CaseInsensitive) != 0 ||
            (SequentialVerify(QDir(original).filePath("inference.onnx"),pin.modelSha256.data()) &&
             SequentialVerify(QDir(original).filePath("inference.yml"),pin.configSha256.data()))) return original;
        const auto revision=QString::fromUtf8(pin.revision.data(),qsizetype(pin.revision.size()));
        return QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/model-cache/sources/ocr-"+
            QString(pin.detection ? "det-" : "rec-")+ModelTierToken(pin.tier)+"-"+revision);
    };
    const auto detectorDirectory=directory(*det), recognizerDirectory=directory(*rec);
    assets.detector = QDir(detectorDirectory).filePath("inference.onnx");
    assets.recognizer = QDir(recognizerDirectory).filePath("inference.onnx");
    const QString config=QDir(recognizerDirectory).filePath("inference.yml");
    assets.detectorSha256=QByteArray(det->modelSha256.data(),qsizetype(det->modelSha256.size()));
    assets.recognizerSha256=QByteArray(rec->modelSha256.data(),qsizetype(rec->modelSha256.size()));
    if (!SequentialVerify(assets.detector,det->modelSha256.data()) ||
        !SequentialVerify(assets.recognizer,rec->modelSha256.data()))
    {
        assets.error = "Pinned PaddleOCR model/config files are missing or changed.";
        return assets;
    }
    QFile input(config);
    if (!input.open(QIODevice::ReadOnly))
    {
        assets.error = "Cannot read OCR dictionary.";
        return assets;
    }
    // Verify and parse the SAME bounded snapshot. Reopening a previously
    // verified file could give token IDs a different meaning during replacement.
    constexpr qint64 kMaximumConfigBytes = 8 * 1024 * 1024;
    const auto configBytes = input.read(kMaximumConfigBytes + 1);
    if (input.error() != QFileDevice::NoError || !input.atEnd() || configBytes.size() > kMaximumConfigBytes ||
        QCryptographicHash::hash(configBytes,QCryptographicHash::Sha256).toHex() != rec->configSha256.data())
    {
        assets.error = "Pinned PaddleOCR model/config files are missing or changed.";
        return assets;
    }
    bool reading = false;
    assets.dictionary.push_back(QString());
    for (const QByteArray &bytes : configBytes.split('\n'))
    {
        QString line = QString::fromUtf8(bytes).remove('\r');
        if (line == "  character_dict:")
        {
            reading = true;
            continue;
        }
        if (!reading)
            continue;
        if (!line.startsWith("  - "))
            break;
        QString value = line.mid(4);
        if (value.startsWith('\'') && value.endsWith('\''))
            value = value.mid(1, value.size() - 2).replace("''", "'");
        else if (value.startsWith('"'))
        {
            QJsonParseError error;
            const auto array =
                QJsonDocument::fromJson(("[" + value + "]").toUtf8(), &error).array();
            if (error.error != QJsonParseError::NoError || array.size() != 1 ||
                !array[0].isString())
            {
                assets.error = "Invalid pinned OCR dictionary.";
                return assets;
            }
            value = array[0].toString();
        }
        if (value.isEmpty())
        {
            assets.error = "Empty OCR dictionary entry.";
            return assets;
        }
        assets.dictionary.push_back(value);
    }
    if (assets.dictionary.size() < 100)
        assets.error = "Incomplete OCR dictionary.";
    assets.dictionary.push_back(" ");
    return assets;
}
// --- End of the frozen loader. ----------------------------------------------

inline bool SameAssets(const PaddleAssets& a, const PaddleAssets& b, QString& why)
{
    const auto field = [&](const char* name, const auto& x, const auto& y) {
        if (x == y) return true;
        why = QString::fromLatin1(name);
        return false;
    };
    return field("root", a.root, b.root) && field("detector", a.detector, b.detector) &&
           field("recognizer", a.recognizer, b.recognizer) &&
           field("detectorSha256", a.detectorSha256, b.detectorSha256) &&
           field("recognizerSha256", a.recognizerSha256, b.recognizerSha256) &&
           field("dictionary", a.dictionary, b.dictionary) && field("error", a.error, b.error) &&
           field("valid", a.IsValid(), b.IsValid());
}

// A hard link to an existing model file (same volume), or a copy. Nothing in
// this test writes to a placed file; tampered files are new files.
inline bool Place(const QString& source, const QString& target)
{
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) return false;
    const auto from = QDir::toNativeSeparators(source).toStdWString();
    const auto to = QDir::toNativeSeparators(target).toStdWString();
    return CreateHardLinkW(to.c_str(), from.c_str(), nullptr) || QFile::copy(source, target);
}
inline bool Tamper(const QString& target)
{
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) return false;
    QFile file(target);
    return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write("not the pinned file") > 0;
}

inline void AssetsMatchSequential()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_ASSET_EQUIVALENCE") != "1")
        QSKIP("Explicit asset verification equivalence not selected.");
    const auto source = DefaultPaddleRoot();
    const auto* det = FindPaddleModelPin(true, ModelTier::Medium);
    const auto* rec = FindPaddleModelPin(false, ModelTier::Medium);
    QVERIFY(det && rec);
    const auto text = [](std::string_view value) { return QString::fromUtf8(value.data(), qsizetype(value.size())); };
    const auto role = [&](const QString& root, const PaddleModelPin& pin, const char* name) {
        return QDir(QDir(root).filePath(text(pin.directory))).filePath(QString::fromLatin1(name));
    };
    for (const auto* pin : {det, rec})
        for (const char* name : {"inference.onnx", "inference.yml"})
            QVERIFY2(QFile::exists(role(source, *pin, name)), qPrintable("Missing source model file " + role(source, *pin, name)));
    // Work under the redirected LOCALAPPDATA (same volume as the runtime root).
    QTemporaryDir work(QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("uct-asset-equivalence-XXXXXX"));
    QVERIFY(work.isValid());
    const auto downloaded = [&](const QString& localData, const PaddleModelPin& pin, const char* name) {
        return QDir(QDir(localData).filePath("OkuFlow/model-cache/sources/ocr-" + QString(pin.detection ? "det-" : "rec-") +
                                             ModelTierToken(pin.tier) + "-" + text(pin.revision))).filePath(QString::fromLatin1(name));
    };
    const auto intact = work.filePath("intact"), tampered = work.filePath("tampered"), empty = work.filePath("empty");
    const auto noDownloads = work.filePath("lad-none"), downloads = work.filePath("lad-valid"),
               badConfig = work.filePath("lad-bad-config");
    QVERIFY(QDir().mkpath(empty) && QDir().mkpath(noDownloads));
    for (const auto* pin : {det, rec})
        for (const char* name : {"inference.onnx", "inference.yml"}) {
            QVERIFY(Place(role(source, *pin, name), role(intact, *pin, name)));
            QVERIFY(Place(role(source, *pin, name), downloaded(downloads, *pin, name)));
        }
    // Tampered root: detector config and recognizer graph changed.
    QVERIFY(Place(role(source, *det, "inference.onnx"), role(tampered, *det, "inference.onnx")));
    QVERIFY(Tamper(role(tampered, *det, "inference.yml")));
    QVERIFY(Tamper(role(tampered, *rec, "inference.onnx")));
    QVERIFY(Place(role(source, *rec, "inference.yml"), role(tampered, *rec, "inference.yml")));
    // Downloads with a valid detector and a recognizer whose config changed.
    for (const char* name : {"inference.onnx", "inference.yml"})
        QVERIFY(Place(role(source, *det, name), downloaded(badConfig, *det, name)));
    QVERIFY(Place(role(source, *rec, "inference.onnx"), downloaded(badConfig, *rec, "inference.onnx")));
    QVERIFY(Tamper(downloaded(badConfig, *rec, "inference.yml")));

    struct Case { const char* name; QString root, defaultRoot, localData; bool valid; };
    const QList<Case> cases{
        {"production-root-intact", intact, intact, noDownloads, true},
        {"production-root-tampered-no-downloads", tampered, tampered, noDownloads, false},
        {"production-root-tampered-valid-downloads", tampered, tampered, downloads, true},
        {"production-root-tampered-download-config-changed", tampered, tampered, badConfig, false},
        {"explicit-root-intact", intact, tampered, downloads, true},
        {"explicit-root-tampered-downloads-ignored", tampered, intact, downloads, false},
        {"production-root-empty", empty, empty, noDownloads, false},
    };
    QJsonArray report;
    for (const auto& c : cases) {
        uct_batch_test::Environment root("OKUFLOW_UCT_ROOT", c.defaultRoot.toUtf8());
        uct_batch_test::Environment localData("LOCALAPPDATA", c.localData.toUtf8());
        const auto started = std::chrono::steady_clock::now();
        const auto sequential = SequentialLoadPaddleAssets(c.root, ModelTier::Medium, ModelTier::Medium);
        const auto middle = std::chrono::steady_clock::now();
        const auto concurrent = LoadPaddleAssets(c.root, ModelTier::Medium, ModelTier::Medium);
        const auto ended = std::chrono::steady_clock::now();
        QString why;
        QVERIFY2(SameAssets(sequential, concurrent, why), qPrintable(QString::fromLatin1(c.name) + ": " + why +
                 " sequential=" + sequential.error + " concurrent=" + concurrent.error));
        QCOMPARE(concurrent.IsValid(), c.valid);
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        report.push_back(QJsonObject{{"case", c.name}, {"valid", concurrent.IsValid()}, {"error", concurrent.error},
            {"detector", concurrent.detector}, {"recognizer", concurrent.recognizer},
            {"sequentialMs", ms(middle - started)}, {"concurrentMs", ms(ended - middle)}});
    }
    qInfo().noquote() << "UCT_ASSET_EQUIVALENCE" << QJsonDocument(report).toJson(QJsonDocument::Compact);
}

#if OKUFLOW_ENABLE_CUDA && OKUFLOW_UCT_TRT
inline void PreparationCancellation()
{
    using Clock = std::chrono::steady_clock;
    if (qEnvironmentVariable("OKUFLOW_UCT_PREPARATION_CANCEL") != "1")
        QSKIP("Explicit preparation cancellation diagnostic not selected.");
    const auto source = qEnvironmentVariable("OKUFLOW_UCT_TEST_IMAGE");
    const auto image = QImage(source).convertToFormat(QImage::Format_ARGB32);
    QVERIFY2(!image.isNull(), "OKUFLOW_UCT_TEST_IMAGE must select an image.");
    bool ok = true;
    const int points = qEnvironmentVariableIsSet("OKUFLOW_UCT_PREPARATION_CANCEL_POINTS")
        ? qEnvironmentVariable("OKUFLOW_UCT_PREPARATION_CANCEL_POINTS").toInt(&ok) : 24;
    QVERIFY2(ok && points >= 2 && points <= 64, "OKUFLOW_UCT_PREPARATION_CANCEL_POINTS must be 2-64.");
    const auto evidence = qEnvironmentVariable("OKUFLOW_UCT_PREPARATION_CANCEL_EVIDENCE");
    QVERIFY2(evidence.isEmpty() || (QDir::isAbsolutePath(evidence) && !QFileInfo::exists(evidence)),
             "The evidence file must be a new absolute path.");
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    std::atomic<bool> never{};
    // A ready reader: its backend detail and exact output.
    struct Ready { QString detail; QByteArray output; double readyMs{}; QString error; };
    const auto prepare = [&](uct_batch_test::RetirementReceipts& retirement) {
        Ready ready;
        auto reader = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium, retirement.Observer());
        const auto started = Clock::now();
        const auto status = reader->Initialize();
        if (status.state != BackendState::kReady) { ready.error = "Initialize: " + status.detail; return ready; }
        while (reader->Preparing() && Clock::now() - started < std::chrono::seconds(120))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ready.readyMs = ms(Clock::now() - started);
        if (reader->Preparing()) { ready.error = "Not ready within 120 s."; return ready; }
        const auto output = reader->Recognize(image, never);
        if (!output.ok) { ready.error = "Recognize: " + output.error + " / " + reader->Status().detail; return ready; }
        ready.output = uct_resident_test::ExactOutputBytes(output);
        ready.detail = reader->Status().detail;
        return ready;
    };
    uct_batch_test::RetirementReceipts referenceRetirement;
    const auto reference = prepare(referenceRetirement);
    QVERIFY2(reference.error.isEmpty(), qPrintable(reference.error));
    QVERIFY2(reference.detail.contains("Selected detector=medium/BF16-linear") &&
             reference.detail.contains("smoke=4/4"), qPrintable(reference.detail));
    // Spread over the whole preparation, from construction to just past ready.
    const double span = reference.readyMs * 1.1;
    QJsonArray rows;
    for (int point = 0; point < points; ++point) {
        const double delayMs = span * point / (points - 1);
        uct_batch_test::RetirementReceipts cancelledRetirement;
        double destroyMs{}, initializeMs{};
        bool preparingAtCancel{};
        {
            auto reader = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium,
                                                             cancelledRetirement.Observer());
            const auto started = Clock::now();
            const auto status = reader->Initialize();
            initializeMs = ms(Clock::now() - started);
            QVERIFY2(status.state == BackendState::kReady, qPrintable(status.detail));
            while (ms(Clock::now() - started) < delayMs) std::this_thread::yield();
            preparingAtCancel = reader->Preparing();
            const auto destroying = Clock::now();
            reader.reset(); // cancels; must not wait for the preparation thread
            destroyMs = ms(Clock::now() - destroying);
        }
        const auto& retired = cancelledRetirement.Wait();
        uct_batch_test::RetirementReceipts freshRetirement;
        const auto fresh = prepare(freshRetirement);
        QVERIFY2(fresh.error.isEmpty(), qPrintable(QString("point %1: %2").arg(point).arg(fresh.error)));
        const bool sameDetail = fresh.detail == reference.detail, sameOutput = fresh.output == reference.output;
        const auto& freshRetired = freshRetirement.Wait();
        rows.push_back(QJsonObject{{"point", point}, {"cancelAfterMs", delayMs}, {"initializeMs", initializeMs},
            {"preparingAtCancel", preparingAtCancel}, {"destroyMs", destroyMs},
            {"cancelledRetirement", retired.Json()}, {"freshReadyMs", fresh.readyMs},
            {"sameBackendDetail", sameDetail}, {"sameOutput", sameOutput}, {"freshRetirement", freshRetired.Json()}});
        QVERIFY2(sameDetail, qPrintable(QString("point %1 detail differs:\n%2\n%3").arg(point).arg(fresh.detail, reference.detail)));
        QVERIFY2(sameOutput, qPrintable(QString("point %1 output differs").arg(point)));
        // Destruction cancels without joining the preparation.
        QVERIFY2(destroyMs < 50, qPrintable(QString("point %1: destruction took %2 ms").arg(point).arg(destroyMs)));
        // A cancelled preparation that had created its State retires it completely.
        QVERIFY2(!retired.cleanupFailed && (!retired.observed || retired.allCompleted),
                 qPrintable(QString("point %1: cancelled State did not close").arg(point)));
        QVERIFY(freshRetired.Passed());
    }
    const auto& referenceRetired = referenceRetirement.Wait();
    QVERIFY(referenceRetired.Passed());
    const QJsonObject report{{"schema", 1}, {"image", source}, {"referenceReadyMs", reference.readyMs},
        {"referenceDetail", reference.detail}, {"points", rows},
        {"outputSha256", QString::fromLatin1(QCryptographicHash::hash(reference.output, QCryptographicHash::Sha256).toHex())}};
    qInfo().noquote() << "UCT_PREPARATION_CANCEL" << QJsonDocument(report).toJson(QJsonDocument::Compact);
    if (!evidence.isEmpty()) QVERIFY(uct_batch_test::Save(evidence, report));
}

// Follow-up of round 11: preparation A is cancelled part-way (its reader is
// destroyed, which never waits) and preparation B with the same detector and
// recognizer starts at once, while A may still be inside a detector or helper
// vendor call. A's helper holds no latch, so B's recognizer must always come
// up on BF16 with the plan receipt of an undisturbed start. B's whole backend
// detail and output must be those of an undisturbed start, except where B's
// BF16 detector met A's own detector attempt still in flight: that latch rule
// is the sequential code's (unchanged since before round 11) and selects the
// FP32 detector, which such a point reports instead.
inline void OverlappingPreparation()
{
    using Clock = std::chrono::steady_clock;
    if (qEnvironmentVariable("OKUFLOW_UCT_OVERLAP_PREPARATION") != "1")
        QSKIP("Explicit overlapping preparation diagnostic not selected.");
    const auto source = qEnvironmentVariable("OKUFLOW_UCT_TEST_IMAGE");
    const auto image = QImage(source).convertToFormat(QImage::Format_ARGB32);
    QVERIFY2(!image.isNull(), "OKUFLOW_UCT_TEST_IMAGE must select an image.");
    bool ok = true;
    const int points = qEnvironmentVariableIsSet("OKUFLOW_UCT_OVERLAP_POINTS")
        ? qEnvironmentVariable("OKUFLOW_UCT_OVERLAP_POINTS").toInt(&ok) : 16;
    QVERIFY2(ok && points >= 2 && points <= 64, "OKUFLOW_UCT_OVERLAP_POINTS must be 2-64.");
    const auto evidence = qEnvironmentVariable("OKUFLOW_UCT_OVERLAP_EVIDENCE");
    QVERIFY2(evidence.isEmpty() || (QDir::isAbsolutePath(evidence) && !QFileInfo::exists(evidence)),
             "The evidence file must be a new absolute path.");
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    const auto recognizerReceipt = [](const QString& detail) {
        const QRegularExpression receipt("recognizerPlanKey=[^;]*;recognizerPlanPayloadSha256=[^;]*;"
                                         "recognizerPlanCache=[^;]*;recognizerContextBytes=[^;]*;");
        return detail.contains("BF16 model selected") ? receipt.match(detail).captured(0) : QString();
    };
    std::atomic<bool> never{};
    struct Ready { QString detail; QByteArray output; double readyMs{}; QString error; };
    const auto ready = [&](Recognizer& reader, Clock::time_point started) {
        Ready result;
        while (reader.Preparing() && Clock::now() - started < std::chrono::seconds(120))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        result.readyMs = ms(Clock::now() - started);
        if (reader.Preparing()) { result.error = "Not ready within 120 s."; return result; }
        const auto output = reader.Recognize(image, never);
        if (!output.ok) { result.error = "Recognize: " + output.error + " / " + reader.Status().detail; return result; }
        result.output = uct_resident_test::ExactOutputBytes(output);
        result.detail = reader.Status().detail;
        return result;
    };
    uct_batch_test::RetirementReceipts referenceRetirement;
    Ready reference;
    {
        auto reader = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium, referenceRetirement.Observer());
        const auto started = Clock::now();
        const auto status = reader->Initialize();
        QVERIFY2(status.state == BackendState::kReady, qPrintable(status.detail));
        reference = ready(*reader, started);
    }
    QVERIFY2(reference.error.isEmpty(), qPrintable(reference.error));
    QVERIFY2(reference.detail.contains("Selected detector=medium/BF16-linear") &&
             reference.detail.contains("BF16 model selected") && reference.detail.contains("smoke=4/4"),
             qPrintable(reference.detail));
    QVERIFY(referenceRetirement.Wait().Passed());
    const auto referenceReceipt = recognizerReceipt(reference.detail);
    QVERIFY2(!referenceReceipt.isEmpty(), qPrintable(reference.detail));
    int identical = 0, detectorInFlight = 0;
    // A's cancellation points: from the start of its preparation thread to the
    // end of its detector stage (about 90 percent of an undisturbed start).
    const double span = reference.readyMs * 0.9;
    QJsonArray rows;
    for (int point = 0; point < points; ++point) {
        const double delayMs = span * point / (points - 1);
        uct_batch_test::RetirementReceipts retirementA, retirementB;
        bool preparingAtCancel{};
        double destroyMs{};
        Ready b;
        {
            auto a = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium, retirementA.Observer());
            const auto startedA = Clock::now();
            const auto statusA = a->Initialize();
            QVERIFY2(statusA.state == BackendState::kReady, qPrintable(statusA.detail));
            while (ms(Clock::now() - startedA) < delayMs) std::this_thread::yield();
            preparingAtCancel = a->Preparing();
            const auto destroying = Clock::now();
            a.reset(); // cancels A; never waits for its preparation thread
            destroyMs = ms(Clock::now() - destroying);
            // B at once, without waiting for A's retirement.
            auto readerB = CreateProductionTensorRtRecognizer(ModelTier::Medium, ModelTier::Medium, retirementB.Observer());
            const auto startedB = Clock::now();
            const auto statusB = readerB->Initialize();
            QVERIFY2(statusB.state == BackendState::kReady, qPrintable(statusB.detail));
            b = ready(*readerB, startedB);
        }
        QVERIFY2(b.error.isEmpty(), qPrintable(QString("point %1: %2").arg(point).arg(b.error)));
        const auto& retiredA = retirementA.Wait();
        const auto& retiredB = retirementB.Wait();
        const bool sameDetail = b.detail == reference.detail, sameOutput = b.output == reference.output;
        const bool sameRecognizer = recognizerReceipt(b.detail) == referenceReceipt;
        const bool inFlight = b.detail.contains("Detector FP32 fallback: BF16 detector already failed or in flight this session");
        rows.push_back(QJsonObject{{"point", point}, {"cancelAAfterMs", delayMs}, {"aPreparingAtCancel", preparingAtCancel},
            {"aDestroyMs", destroyMs}, {"aRetirement", retiredA.Json()}, {"bReadyMs", b.readyMs},
            {"bSameBackendDetail", sameDetail}, {"bSameOutput", sameOutput},
            {"bRecognizerBf16SameReceipt", sameRecognizer}, {"bDetectorMetAInFlight", inFlight},
            {"bDetail", sameDetail ? QString() : b.detail}, {"bRetirement", retiredB.Json()}});
        QVERIFY2(sameRecognizer, qPrintable(QString("point %1 recognizer differs:\n%2\n%3").arg(point).arg(b.detail, reference.detail)));
        if (sameDetail) {
            QVERIFY2(sameOutput, qPrintable(QString("point %1 output differs").arg(point)));
            ++identical;
        } else {
            QVERIFY2(inFlight && preparingAtCancel,
                     qPrintable(QString("point %1 detail differs:\n%2\n%3").arg(point).arg(b.detail, reference.detail)));
            ++detectorInFlight;
        }
        QVERIFY2(destroyMs < 50, qPrintable(QString("point %1: destruction took %2 ms").arg(point).arg(destroyMs)));
        QVERIFY2(!retiredA.cleanupFailed && (!retiredA.observed || retiredA.allCompleted),
                 qPrintable(QString("point %1: cancelled State did not close").arg(point)));
        QVERIFY(retiredB.Passed());
    }
    const QJsonObject report{{"schema", 1}, {"image", source}, {"referenceReadyMs", reference.readyMs},
        {"referenceDetail", reference.detail}, {"points", rows}, {"identical", identical},
        {"detectorMetAInFlight", detectorInFlight},
        {"outputSha256", QString::fromLatin1(QCryptographicHash::hash(reference.output, QCryptographicHash::Sha256).toHex())}};
    qInfo().noquote() << "UCT_OVERLAP_PREPARATION" << QJsonDocument(report).toJson(QJsonDocument::Compact);
    if (!evidence.isEmpty()) QVERIFY(uct_batch_test::Save(evidence, report));
}
#else
inline void PreparationCancellation()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_PREPARATION_CANCEL") != "1")
        QSKIP("Explicit preparation cancellation diagnostic not selected.");
    QFAIL("This build lacks the TensorRT adapter.");
}
inline void OverlappingPreparation()
{
    if (qEnvironmentVariable("OKUFLOW_UCT_OVERLAP_PREPARATION") != "1")
        QSKIP("Explicit overlapping preparation diagnostic not selected.");
    QFAIL("This build lacks the TensorRT adapter.");
}
#endif
} // namespace okuflow::uct::uct_preparation_diagnostic
