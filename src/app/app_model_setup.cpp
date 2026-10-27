#include "okuflow/app/app.hpp"
#include "okuflow/common/uct_model_installation.hpp"
#include "okuflow/common/uct_paddle.hpp"
#include "okuflow/common/uct_model_preparation.hpp"
#include "okuflow/common/uct_formula.hpp"
#include "okuflow/common/uct_formula_workflow.hpp"
#include "okuflow/ui/main_window.hpp"
#include "okuflow/ui/ultra_clear_text_panel.hpp"
#include <QThread>
#include <mutex>
#include <QDir>

namespace okuflow {
namespace {
struct ModelSetupState {
    std::shared_ptr<std::atomic<bool>> cancel=std::make_shared<std::atomic<bool>>(false);
    uct::ModelSettings desired,previous;
    std::uint64_t generation{};
    uct::installation::Result result;
    std::unique_ptr<uct::Recognizer> prepared;
    std::shared_ptr<uct::formula::Engine> formula;
    bool cachedOnly{};
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
        if(state_->cachedOnly) {
            // Startup never starts a download or activates merely from files.
            // Exact source checks and real bounded TensorRT compatibility still run.
            try {
                const auto package=uct::installation::FormulaPackage(models.mathModel);
                const auto root=uct::installation::SourceDirectory(uct::DefaultPaddleRoot(),uct::installation::DefaultCacheRoot(),package);
                if(!root.isEmpty()) {
                    auto math=uct::formula::Prepare(models.mathModel,root,
                        QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/model-cache/math-engines"),
                        *state_->cancel,std::chrono::steady_clock::now()+std::chrono::minutes(10));
                    state_->formula=std::move(math.engine);state_->result.detail=math.detail;
                    state_->result.ok=bool(state_->formula);state_->result.cancelled=math.cancelled;
                }
            } catch(const std::exception& error) {state_->result.detail=QString::fromUtf8(error.what()).left(4096);}
            return;
        }
        state_->result=uct::installation::Run(uct::installation::Select(models),uct::DefaultPaddleRoot(),
            uct::installation::DefaultCacheRoot(),*state_->cancel,[this](uct::installation::Progress value) {
                emit progress(value.stage==uct::installation::Progress::Stage::Preparing,
                    value.total>0 ? int(value.completed*100/value.total) : 0);
            },[&](const std::atomic<bool>& cancelled) {
                auto prepared=uct::PrepareInstalledModels(models,cancelled);
                const bool ready=bool(prepared.recognizer);
                state_->prepared=std::move(prepared.recognizer);
                if(!ready)return uct::installation::Preparation{false,prepared.error};
                if(models.installMath) {
                    const auto package=uct::installation::FormulaPackage(models.mathModel);
                    const auto root=uct::installation::SourceDirectory(uct::DefaultPaddleRoot(),uct::installation::DefaultCacheRoot(),package);
                    auto math=uct::formula::Prepare(models.mathModel,root,
                        QDir(qEnvironmentVariable("LOCALAPPDATA")).filePath("OkuFlow/model-cache/math-engines"),
                        cancelled,std::chrono::steady_clock::now()+std::chrono::minutes(10));
                    if(!math.engine)return uct::installation::Preparation{false,math.detail};
                    state_->formula=std::move(math.engine);
                }
                return uct::installation::Preparation{true,prepared.backend.detail};
            });
        if(!state_->result.ok){state_->prepared.reset();state_->formula.reset();}
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
            ultraClearText_->SetFormulaFactory(state->formula ?
                std::function<uct::FormulaWork(const QRect&)>([engine=state->formula](const QRect& region){return uct::formula::Work(engine,region);}) :
                std::function<uct::FormulaWork(const QRect&)>{});
            panel->SetModelSettings(models);
        } else panel->SetModelSettings(settingsController_->Settings().ultraClearTextModels);
        panel->FinishModelSetup(ready,state->result.cancelled || state->cancel->load(),state->result.detail);
    });
    connect(worker,&QThread::finished,worker,&QObject::deleteLater);
    worker->start();
}
void OkuFlowApp::RestoreUltraClearTextMath() {
    if(!mainWindow_ || !ultraClearText_ || uctModelSetupCancel_)return;
    const auto models=settingsController_->Settings().ultraClearTextModels;
    if(!models.installMath)return;
    auto state=std::make_shared<ModelSetupState>();state->desired=models;state->previous=models;
    state->cachedOnly=true;state->generation=++uctModelSetupGeneration_;uctModelSetupCancel_=state->cancel;
    auto* panel=mainWindow_->ultraClearTextPanel();panel->SetModelSetupProgress(true,0);
    auto* worker=new ModelSetupWorker(state);
    connect(this,&QObject::destroyed,worker,[state]{state->cancel->store(true);},Qt::DirectConnection);
    connect(worker,&QThread::finished,this,[this,state] {
        if(uctModelSetupCancel_!=state->cancel)return;
        uctModelSetupCancel_.reset();
        const bool ready=state->result.ok && !state->cancel->load() && ultraClearText_ &&
            state->generation==uctModelSetupGeneration_ && settingsController_->Settings().ultraClearTextModels==state->previous;
        auto* panel=mainWindow_->ultraClearTextPanel();
        if(ready)ultraClearText_->SetFormulaFactory([engine=state->formula](const QRect& region){return uct::formula::Work(engine,region);});
        panel->FinishModelSetup(ready,state->cancel->load(),state->result.detail);
        if(!ready)panel->SetFormulaStatus(QStringLiteral("Install and set up Math first."));
    });
    connect(worker,&QThread::finished,worker,&QObject::deleteLater);worker->start();
}
} // namespace okuflow
#include "app_model_setup.moc"
