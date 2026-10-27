#include "okuflow/common/uct_model_preparation.hpp"
#include "uct_runtime_precision.hpp"
#include <QElapsedTimer>
#include <QThread>

namespace okuflow::uct {
namespace {
class ReadyRecognizer final : public Recognizer {
public:
    explicit ReadyRecognizer(std::unique_ptr<Recognizer> ready):ready_(std::move(ready)){}
    BackendStatus Initialize() override{return ready_->Status();}
    BackendStatus Status() const override{return ready_->Status();}
    bool WantsDeviceInput() const override{return ready_->WantsDeviceInput();}
    bool Preparing() const override{return ready_->Preparing();}
    void SetDetailFocus(const std::optional<DetailFocus>& focus) override{ready_->SetDetailFocus(focus);}
    void SetCurvedRibbonInput(bool allowed) override{ready_->SetCurvedRibbonInput(allowed);}
    RecognitionOutput Recognize(const QImage& image,const std::atomic<bool>& cancel) override{return ready_->Recognize(image,cancel);}
    RecognitionOutput RecognizeDevice(const DeviceImage& image,const std::atomic<bool>& cancel) override{return ready_->RecognizeDevice(image,cancel);}
private:std::unique_ptr<Recognizer> ready_;
};
}
PreparedModels PrepareInstalledModels(const ModelSettings& models,const std::atomic<bool>& cancelled,
                                     const QString& preparationCache) {
    PreparedModels result;
    if(cancelled.load()){result.error="cancelled";return result;}
    std::vector<std::unique_ptr<Recognizer>> backends;
    QImage probe(96,96,QImage::Format_ARGB32);probe.fill(Qt::white);
    auto gpu=CreateProductionTensorRtRecognizer(models.gpuDetector,models.gpuRecognizer,{},preparationCache);
    if(gpu && gpu->Initialize().usesGpu) {
        QElapsedTimer clock;clock.start();RecognitionOutput output;
        do {
            if(cancelled.load()){result.error="cancelled";return result;}
            output=gpu->Recognize(probe,cancelled);
            if(!output.backendPreparing)break;
            QThread::msleep(20);
        } while(clock.elapsed()<600000);
        if(!output.ok || output.backendPreparing || gpu->Status().state!=BackendState::kReady) {
            result.error=output.error.isEmpty()?gpu->Status().detail:output.error;return result;
        }
        result.backend=gpu->Status();
        backends.push_back(std::make_unique<ReadyRecognizer>(std::move(gpu)));
    }
    if(cancelled.load()){result.error="cancelled";return result;}
    auto cpu=CreatePaddleCpuRecognizer({},models.cpuDetector,models.cpuRecognizer);
    const auto cpuStatus=cpu->Initialize();
    if(cpuStatus.state!=BackendState::kReady){result.error=cpuStatus.detail;return result;}
    const auto output=cpu->Recognize(probe,cancelled);
    if(!output.ok){result.error=output.error;return result;}
    if(backends.empty())result.backend=cpu->Status();
    backends.push_back(std::make_unique<ReadyRecognizer>(std::move(cpu)));
    backends.push_back(CreateWindowsRecognizer());
    if(cancelled.load()){result.error="cancelled";return result;}
    result.recognizer=CreateAutomaticRecognizer(std::move(backends));
    return result;
}
} // namespace okuflow::uct
