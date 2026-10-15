#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include <QDialog>
#include <QString>

#include <memory>
#include <functional>
#include "okuflow/app/windows_runtime.hpp"

QT_BEGIN_NAMESPACE
class QCheckBox;
class QCloseEvent;
class QCryptographicHash;
class QFile;
class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QProgressBar;
class QPushButton;
class QTimer;
class QTemporaryDir;
class QVBoxLayout;
QT_END_NAMESPACE

namespace okuflow {

// Installs the bundled Windows runtime and optional vendor tools. Every
// installer is pinned and SHA-256 verified before it is started.
class SetupAssistantDialog : public QDialog {
    Q_OBJECT
public:
    explicit SetupAssistantDialog(const QString& configuredCodexPath,
                                  bool declined,
                                  QWidget* parent = nullptr,
                                  std::function<WindowsRuntimeStatus()> runtimeCheck = {});
    ~SetupAssistantDialog() override;

    // True when a dependency of the core Read/Explain/Assistant features is
    // missing, which is the only case that opens the assistant at startup.
    // The optional NVIDIA runtime is offered from its Super Resolution control.
    static bool NeedsSetup(const QString& configuredCodexPath);
    static QString FindCodexExecutable(const QString& configuredPath = {});
    void SetDeclinedPreference(bool declined);

    struct DependencyRow {
        QWidget* container{};
        QLabel* statusIcon{};
        QLabel* status{};
        QProgressBar* progress{};
        QPushButton* install{};
        QPushButton* remove{};
    };

signals:
    void CodexPathChanged(const QString& path);
    void DeclinePreferenceChanged(bool declined);
    void DependenciesChanged();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    enum class Dependency { None, WindowsRuntime, CodexCli, NvidiaVideoEffects };

    DependencyRow& RowForDependency(Dependency dependency);
    void RefreshStatus();
    void BeginDownload(Dependency dependency);
    void CancelDownload();
    void FinishDownload();
    void StartWindowsDownloadFallback(const QString& primaryError,
                                      bool useAlternateUrl = false);
    void FinishWindowsDownloadFallback(int exitCode, bool normalExit);
    bool VerifyDownloadedInstaller(QString& error);
    void ResetDownloadState();
    void StartVerifiedInstaller();
    void BeginWindowsRuntimeInstall();
    void StartElevatedInstaller(Dependency dependency);
    void CompleteInstaller(Dependency dependency,
                           bool success,
                           const QString& detail = {});
    void ShowDownloadFailure(const QString& message, const QString& vendorPage);
    void OpenCodexLocationOrGuide();
    void RemoveNvidiaRuntime();
    static QString DetectNvidiaArchitecture();
    static QString FindNvidiaUninstallCommand();

    QString configuredCodexPath_;
    QString nvidiaArchitecture_;
    std::function<WindowsRuntimeStatus()> runtimeCheck_;
    Dependency activeDependency_{Dependency::None};
    DependencyRow codexRow_;
    DependencyRow nvidiaRow_;
    DependencyRow runtimeRow_;
    std::unique_ptr<QTemporaryDir> runtimeInstallerDirectory_;
    QCheckBox* declineCheckbox_{};
    QPushButton* cancelDownloadButton_{};
    QNetworkAccessManager* network_{};
    QNetworkReply* reply_{};
    std::unique_ptr<QFile> downloadFile_;
    std::unique_ptr<QCryptographicHash> hash_;
    QTimer* inactivityTimer_{};
    QProcess* fallbackDownloadProcess_{};
    QProcess* installerProcess_{};
    QTimer* elevatedInstallerTimer_{};
    void* elevatedInstallerHandle_{};
    QString downloadPath_;
    QString downloadUrl_;
    QString alternateDownloadUrl_;
    QString expectedSha256_;
    QString vendorPage_;
    QString primaryDownloadError_;
    bool downloadCancelled_{};
    bool fallbackUsingAlternateUrl_{};
};

} // namespace okuflow

#endif // _WIN32
