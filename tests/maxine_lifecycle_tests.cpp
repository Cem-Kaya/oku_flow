#include "okuflow/common/maxine_superres.hpp"
#include "maxine_fake_sdk.hpp"
#include <cuda_runtime_api.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Cuda(cudaError_t status) {
    if (status != cudaSuccess) throw std::runtime_error(cudaGetErrorString(status));
}
struct Module {
    HMODULE handle{};
    MaxineFakeRead read{};
    MaxineFakeReset reset{};
    MaxineFakeFailLoad failLoad{};
    MaxineFakeFailStage failStage{};
    MaxineFakeBlockRun blockRun{};
    explicit Module(const wchar_t* path) {
        handle = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(handle != nullptr, "private fake DLL could not load");
        read = reinterpret_cast<MaxineFakeRead>(GetProcAddress(handle, "OkuFlowFakeRead"));
        reset = reinterpret_cast<MaxineFakeReset>(GetProcAddress(handle, "OkuFlowFakeReset"));
        failLoad = reinterpret_cast<MaxineFakeFailLoad>(GetProcAddress(handle, "OkuFlowFakeFailLoad"));
        failStage = reinterpret_cast<MaxineFakeFailStage>(GetProcAddress(handle, "OkuFlowFakeFailStage"));
        blockRun = reinterpret_cast<MaxineFakeBlockRun>(GetProcAddress(handle, "OkuFlowFakeBlockRun"));
        Require(read && reset && failLoad && failStage && blockRun, "private fake control exports absent");
        reset();
    }
    ~Module() { if (handle) FreeLibrary(handle); }
    MaxineFakeSnapshot Stats() const { MaxineFakeSnapshot result; read(&result); return result; }
};
struct GpuBuffers {
    cudaStream_t stream{};
    void* source{};
    void* destination{};
    GpuBuffers() {
        Cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
        Cuda(cudaMalloc(&source, 8 * 4 * 4));
        Cuda(cudaMalloc(&destination, 16 * 8 * 4));
    }
    ~GpuBuffers() {
        if (source) cudaFree(source);
        if (destination) cudaFree(destination);
        if (stream) cudaStreamDestroy(stream);
    }
    bool Ensure(okuflow::MaxineSuperRes& adapter) const { return adapter.Ensure(8u, 4u, 16u, 8u, stream); }
    bool Run(okuflow::MaxineSuperRes& adapter) const { return adapter.Run(source, 8 * 4, destination, 16 * 4); }
};
std::mutex profileMutex;
std::vector<QJsonObject> profiles;
QtMessageHandler previousHandler{};
void CaptureProfile(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    const QByteArray bytes = message.toUtf8();
    const QByteArray prefix("OKUFLOW_MAXINE_PROFILE ");
    if (bytes.startsWith(prefix)) {
        std::lock_guard lock(profileMutex);
        profiles.push_back(QJsonDocument::fromJson(bytes.mid(prefix.size())).object());
    }
    if (previousHandler) previousHandler(type, context, message);
    else std::fprintf(stderr, "%s\n", bytes.constData());
}
QJsonObject TakeProfile() {
    std::lock_guard lock(profileMutex);
    Require(profiles.size() == 1, "expected exactly one worker profile");
    QJsonObject result = profiles.back();
    profiles.clear();
    return result;
}
int Calls(const QJsonObject& profile, const char* stage) {
    return profile.value("stages").toObject().value(QLatin1String(stage)).toObject().value("completed").toInt(-1);
}
void CheckDevice(const Module& image, const Module& effects, int expected) {
    Require(image.Stats().lastDevice == expected && effects.Stats().lastDevice == expected,
            "SDK calls did not use the adapter's original CUDA device");
    Require(image.Stats().lastThread != GetCurrentThreadId() && effects.Stats().lastThread != GetCurrentThreadId(),
            "SDK calls escaped onto the caller thread");
}
void RetiredTeardownDoesNotDispatch(Module& image, Module& effects, GpuBuffers& buffers, int devices) {
    image.reset(); effects.reset();
    {
        okuflow::MaxineSuperRes adapter;
        adapter.Teardown(); // A fresh adapter has nothing to retire.
        if (devices > 1) Cuda(cudaSetDevice(1));
        Require(buffers.Ensure(adapter), "initial model setup failed");
        Require(buffers.Run(adapter), "initial fake frame failed");
        adapter.Teardown();
        adapter.Teardown();
        Require(!adapter.IsReady() && !adapter.IsFaulted(), "completed teardown did not retire cleanly");
        CheckDevice(image, effects, 0);
        Cuda(cudaSetDevice(0));
    }
    const auto profile = TakeProfile();
    Require(Calls(profile, "teardown") == 1, "retired adapter dispatched redundant teardown");
    Require(effects.Stats().created == 1 && effects.Stats().destroyed == 1, "effect ownership was not balanced");
    Require(image.Stats().allocated == 2 && image.Stats().deallocated == 2, "image ownership was not balanced");
}
void ExplicitRetirementRearmsRuntime(Module& image, Module& effects, GpuBuffers& buffers) {
    image.reset(); effects.reset();
    {
        okuflow::MaxineSuperRes adapter;
        for (int cycle = 0; cycle != 2; ++cycle) {
            Require(buffers.Ensure(adapter), "Ensure after explicit retirement could not reload runtime");
            Require(adapter.IsReady() && !adapter.IsFaulted(), "reloaded adapter not ready");
            Require(buffers.Run(adapter), "reloaded adapter could not process a fake frame");
            adapter.Teardown();
        }
    }
    const auto profile = TakeProfile();
    Require(Calls(profile, "model load") == 2 && Calls(profile, "frame processing") == 2 &&
            Calls(profile, "teardown") == 2, "reload lifecycle dispatched unexpected calls");
    Require(effects.Stats().created == 2 && effects.Stats().destroyed == 2, "reloaded effects leaked");
    Require(image.Stats().allocated == 4 && image.Stats().deallocated == 4, "reloaded images leaked");
}
void FailedSetupCanBeRetiredAndRetried(Module& image, Module& effects, GpuBuffers& buffers) {
    image.reset(); effects.reset();
    effects.failLoad(-1); // Any non-success is sufficient; no real model is loaded.
    {
        okuflow::MaxineSuperRes adapter;
        Require(!buffers.Ensure(adapter), "controlled model-load failure unexpectedly succeeded");
        Require(!adapter.IsFaulted() && !adapter.IsReady(), "ordinary SDK failure became a worker fault");
        Require(!adapter.LastError().empty() && effects.Stats().loaded == 1,
                "load failure lost its diagnostic or failed before the controlled stage");
        Require(effects.Stats().destroyed == 1 && image.Stats().deallocated == 2, "failed setup leaked partial resources");
        adapter.Teardown();
        Require(buffers.Ensure(adapter) && buffers.Run(adapter), "retired failed setup could not recover");
    }
    const auto profile = TakeProfile();
    Require(Calls(profile, "teardown") == 2, "failed/recovered adapter retirement was not exact");
    Require(effects.Stats().created == effects.Stats().destroyed &&
            image.Stats().allocated == image.Stats().deallocated, "recovery leaked resources");
}
void FrameFailuresRetainStageAndShortCircuit(Module& image, Module& effects, GpuBuffers& buffers) {
    image.reset(); effects.reset();
    {
        okuflow::MaxineSuperRes adapter;
        Require(buffers.Ensure(adapter), "frame-failure fixture setup failed");
        struct Failure { MaxineFakeStage stage; int status; };
        const Failure failures[] = {
            {MaxineFakeStage::InputTransfer, -35},
            {MaxineFakeStage::EffectRun, -99},
            {MaxineFakeStage::OutputTransfer, -112},
        };
        for (const auto& failure : failures) {
            const auto beforeImage = image.Stats();
            const auto beforeEffect = effects.Stats();
            auto& module = failure.stage == MaxineFakeStage::EffectRun ? effects : image;
            module.failStage(failure.stage, failure.status);
            Require(!buffers.Run(adapter), "controlled frame-stage failure unexpectedly succeeded");
            // Preserve the SDK's numeric status and verify the actual stage
            // below through call counters; English diagnostic prose is free
            // to improve without breaking a behavioral regression.
            Require(adapter.LastError().find(std::to_string(failure.status)) != std::string::npos,
                    "frame failure lost its SDK status");
            Require(adapter.IsReady() && !adapter.IsFaulted(), "ordinary frame failure changed worker-fault policy");
            const auto afterImage = image.Stats();
            const auto afterEffect = effects.Stats();
            Require(afterImage.inputTransfers == beforeImage.inputTransfers + 1, "input transfer was not attempted exactly once");
            Require(afterEffect.ran == beforeEffect.ran + (failure.stage == MaxineFakeStage::InputTransfer ? 0u : 1u),
                    "input failure did not short-circuit effect execution");
            Require(afterImage.outputTransfers == beforeImage.outputTransfers + (failure.stage == MaxineFakeStage::OutputTransfer ? 1u : 0u),
                    "earlier failure did not short-circuit output transfer");
            Require(buffers.Run(adapter) && adapter.LastError().empty(), "same loaded owner did not recover after one-shot failure");
            Require(effects.Stats().loaded == 1 && effects.Stats().created == 1 && image.Stats().allocated == 2,
                    "frame recovery reloaded the model or replaced SDK images");
        }
    }
    const auto profile = TakeProfile();
    const auto frame = profile.value("stages").toObject().value("frame processing").toObject();
    Require(frame.value("completed").toInt() == 6 && frame.value("sdk_success").toInt() == 3,
            "ordinary failed frame calls were misclassified in the profile");
    Require(effects.Stats().destroyed == 1 && image.Stats().deallocated == 2, "frame-failure owner did not retire cleanly");
}
struct BlockedRun {
    HANDLE entered{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE release{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    HANDLE returned{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    explicit BlockedRun(Module& effects) {
        Require(entered && release && returned, "could not create controlled SDK gate");
        effects.blockRun(entered, release, returned);
    }
    ~BlockedRun() {
        SetEvent(release);
        // If completion is unknown, preserve the exact handles instead of
        // closing resources still borrowed by the controlled SDK task.
        if (WaitForSingleObject(entered, 0) == WAIT_OBJECT_0 &&
            WaitForSingleObject(returned, 2000) != WAIT_OBJECT_0) return;
        CloseHandle(entered); CloseHandle(release); CloseHandle(returned);
    }
};
void FaultedOwnershipRemainsQuarantined(Module& image, Module& effects, GpuBuffers& buffers) {
    image.reset(); effects.reset();
    BlockedRun gate(effects);
    {
        auto adapter = std::make_unique<okuflow::MaxineSuperRes>();
        Require(buffers.Ensure(*adapter), "timeout fixture setup failed");
        Require(!buffers.Run(*adapter), "blocked SDK frame did not meet its host deadline");
        Require(WaitForSingleObject(gate.entered, 0) == WAIT_OBJECT_0 && adapter->IsFaulted(), "blocked call did not fault");
        adapter->Teardown();
        Require(effects.Stats().destroyed == 0 && image.Stats().deallocated == 0, "faulted teardown freed live SDK ownership");
        adapter.reset();
        Require(effects.Stats().destroyed == 0 && image.Stats().deallocated == 0, "faulted destructor freed live SDK ownership");
    }
    const auto profile = TakeProfile();
    Require(Calls(profile, "teardown") == 0, "faulted adapter dispatched teardown");
    Require(profile.value("stages").toObject().value("frame processing").toObject().value("timed_out").toInt() == 1,
            "timeout was not retained in diagnostics");
    SetEvent(gate.release);
    Require(WaitForSingleObject(gate.returned, 2000) == WAIT_OBJECT_0, "controlled late SDK call did not return");
    const auto deadline = GetTickCount64() + 2000;
    while (image.Stats().transferred < 2 && GetTickCount64() < deadline) Sleep(1);
    Require(image.Stats().transferred == 2, "late owned SDK call did not finish its final transfer");
    Require(effects.Stats().destroyed == 0 && image.Stats().deallocated == 0, "late completion bypassed quarantine");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    int devices = 0;
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices == 0) return 77;
    try {
        Cuda(cudaSetDevice(0));
        GpuBuffers buffers;
        const QString imagePath = QString::fromUtf8(OKUFLOW_MAXINE_FAKE_IMAGE);
        const QString effectsPath = QString::fromUtf8(OKUFLOW_MAXINE_FAKE_EFFECTS);
        Require(QFileInfo(imagePath).absolutePath() == QFileInfo(effectsPath).absolutePath(), "fake runtime DLLs separated");
        qputenv("OKUFLOW_MAXINE_PATH", QFileInfo(imagePath).absolutePath().toUtf8());
        qputenv("OKUFLOW_MAXINE_PROFILE", "1");
        Module image(imagePath.toStdWString().c_str());
        Module effects(effectsPath.toStdWString().c_str());
        Require(QDir(QString::fromStdWString(okuflow::MaxineSuperRes::FindRuntimeDirectory())) == QDir(QFileInfo(imagePath).absolutePath()),
                "adapter did not select the private fake runtime");
        previousHandler = qInstallMessageHandler(CaptureProfile);
        RetiredTeardownDoesNotDispatch(image, effects, buffers, devices);
        ExplicitRetirementRearmsRuntime(image, effects, buffers);
        FailedSetupCanBeRetiredAndRetried(image, effects, buffers);
        FrameFailuresRetainStageAndShortCircuit(image, effects, buffers);
        FaultedOwnershipRemainsQuarantined(image, effects, buffers);
        qInstallMessageHandler(previousHandler);
        std::puts("PASS: actual Maxine adapter reload, idempotent retirement, device ownership, staged failure diagnostics and timeout quarantine");
        return 0;
    } catch (const std::exception& error) {
        qInstallMessageHandler(previousHandler);
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
