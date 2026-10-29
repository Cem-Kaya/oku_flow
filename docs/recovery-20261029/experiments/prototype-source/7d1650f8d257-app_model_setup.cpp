#include "okuflow/app/app.hpp"
#include "okuflow/common/uct_model_installation.hpp"
#include "okuflow/common/uct_paddle.hpp"
#include "okuflow/common/uct_model_preparation.hpp"
#include "okuflow/ui/main_window.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"
#include <QThread>
#include <mutex>

namespace okuflow {
namespace {
struct ModelSetupState {
    std::shared_ptr<std::atomic<bool>> cancel=std::make_shared<std::atomic<bool>>(false);
    uct::ModelSettings desired,previous;
    std::uint64_t generation{};
    uct::installation::Result result;
    std::unique_ptr<uct::Recognizer> prepared;
    std::mutex mutex;
};
class ModelSetupWorker final : public QThread {
    Q_OBJECT
public:
    explicit ModelSetupWorker(std::shared_ptr<ModelSetupState> state):state_(std::move(state)){}
signals:
    void progress(bool preparing,int percent);
private:
    void run() override {
        const auto& models=state_->desired;
        state_->result=uct::installation::Run(uct::installation::Select(models),uct::DefaultPaddleRoot(),
            uct::installation::DefaultCacheRoot(),*state_->cancel,[this](uct::installation::Progress value) {
                emit progress(value.stage==uct::installation::Progress::Stage::Preparing,
                    value.total>0 ? int(value.completed*100/value.total) : 0);
            },[&](const std::atomic<bool>& cancelled) {
                auto prepared=uct::PrepareInstalledModels(models,cancelled);
                const bool ready=bool(prepared.recognizer);
                state_->prepared=std::move(prepared.recognizer);
                return uct::installation::Preparation{ready,ready?prepared.backend.detail:prepared.error};
            });
        if(!state_->result.ok)state_->prepared.reset();
    }
    std::shared_ptr<ModelSetupState> state_;
};
}

void OkuFlowApp::StartUltraClearTextModelSetup() {
    if(!mainWindow_ || !ultraClearText_ || uctModelSetupCancel_)return;
    auto* panel=mainWindow_->ultraClearTextPanel();
    auto state=std::make_shared<ModelSetupState>();
    state->desired=panel->ModelSettings();state->previous=settingsController_->Settings().ultraClearTextModels;
    state->generation=++uctModelSetupGeneration_;
    panel->SetModelSettings(state->previous);
    uctModelSetupCancel_=state->cancel;
    panel->SetModelSetupProgress(false,0);
    auto* worker=new ModelSetupWorker(state);
    // The QObject connection owns delivery. No UI/app pointer is captured by
    // the worker, and app destruction cancels without joining vendor work.
    connect(this,&QObject::destroyed,worker,[state]{state->cancel->store(true);},Qt::DirectConnection);
    connect(worker,&ModelSetupWorker::progress,this,[this,state](bool preparing,int percent) {
        if(uctModelSetupCancel_==state->cancel && mainWindow_)
            mainWindow_->ultraClearTextPanel()->SetModelSetupProgress(preparing,percent);
    });
    connect(worker,&QThread::finished,this,[this,state] {
        if(uctModelSetupCancel_!=state->cancel)return;
        uctModelSetupCancel_.reset();
        auto* panel=mainWindow_->ultraClearTextPanel();
        bool ready=state->result.ok && !state->cancel->load() &&
            uctModelSetupGeneration_==state->generation &&
            settingsController_->Settings().ultraClearTextModels==state->previous && ultraClearText_;
        if(ready) {
            ready=settingsController_->SaveModelSelection(CaptureCurrentAdvancedConfig(),state->desired);
            if(!ready)state->result.detail=settingsController_->LastError();
        }
        if(ready) {
            const auto models=state->desired;
            ultraClearText_->ReplaceRecognizerFactory([state,models] {
                std::lock_guard lock(state->mutex);
                if(state->prepared)return std::move(state->prepared);
                return uct::CreateDefaultRecognizer(models);
            });
            panel->SetModelSettings(models);
        } else panel->SetModelSettings(settingsController_->Settings().ultraClearTextModels);
        panel->FinishModelSetup(ready,state->result.cancelled || state->cancel->load(),state->result.detail);
    });
    connect(worker,&QThread::finished,worker,&QObject::deleteLater);
    worker->start();
}
} // namespace okuflow
#include "app_model_setup.moc"
