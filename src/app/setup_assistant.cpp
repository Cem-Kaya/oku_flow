#ifdef _WIN32

#include "openzoom/app/setup_assistant.hpp"

#include "openzoom/common/maxine_superres.hpp"
#include "openzoom/ui/live_status_text.hpp"
#include "openzoom/ui/ui_translation.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <array>
#include <algorithm>

#include <windows.h>
#include <shellapi.h>

#if OPENZOOM_ENABLE_CUDA
#include <cuda_runtime_api.h>
#endif

namespace openzoom {
namespace {

constexpr char kTesseractUrl[] =
    "https://github.com/UB-Mannheim/tesseract/releases/download/"
    "v5.4.0.20240606/"
    "tesseract-ocr-w64-setup-5.4.0.20240606.exe";
constexpr char kTesseractAlternateUrl[] =
    "https://digi.bib.uni-mannheim.de/tesseract/"
    "tesseract-ocr-w64-setup-5.4.0.20240606.exe";
constexpr char kTesseractSha256[] =
    "c885fff6998e0608ba4bb8ab51436e1c6775c2bafc2559a19b423e18678b60c9";
constexpr char kTesseractVendorPage[] =
    "https://github.com/UB-Mannheim/tesseract/releases/tag/v5.4.0.20240606";
constexpr char kCodexInstallerUrl[] = "https://chatgpt.com/codex/install.ps1";
constexpr char kCodexInstallerSha256[] =
    "95923c2ac60b963c95435aaeaefeaab3cbc01559e21fce1fa501ee1f9793ac0e";
constexpr char kCodexVendorPage[] = "https://learn.chatgpt.com/docs/codex/cli";
constexpr char kNvidiaVendorPage[] =
    "https://www.nvidia.com/en-me/geforce/broadcasting/broadcast-sdk/resources/";

struct NvidiaInstaller {
    const char* architecture;
    const char* url;
    const char* sha256;
};

constexpr std::array<NvidiaInstaller, 4> kNvidiaInstallers{{
    {"blackwell",
     "https://international.download.nvidia.com/Windows/broadcast/sdk/VFX/"
     "nvidia_video_effects_sdk_installer_v0.7.6_blackwell.exe",
     "cef45592e16d0ea91dbff12a828b31874ab83829adf9a431466dfb88b714618d"},
    {"ada",
     "https://international.download.nvidia.com/Windows/broadcast/sdk/VFX/"
     "nvidia_video_effects_sdk_installer_v0.7.6_ada.exe",
     "4761fe1579e82de6d45efec8dc09492cf2cc601867269955e129707ac8ea8546"},
    {"ampere",
     "https://international.download.nvidia.com/Windows/broadcast/sdk/VFX/"
     "nvidia_video_effects_sdk_installer_v0.7.6_ampere.exe",
     "93bdf4ffec7c393037936349678d1864897fef45d7ba2ab81d4b1563e1328512"},
    {"turing",
     "https://international.download.nvidia.com/Windows/broadcast/sdk/VFX/"
     "nvidia_video_effects_sdk_installer_v0.7.6_turing.exe",
     "d51d8789f96a82375b04bcca6914eee301ad5b6cc45137d7b86fc45ddb205f2d"},
}};

SetupAssistantDialog::DependencyRow AddDependencyRow(QVBoxLayout* parent,
                                                      const QString& title,
                                                      const QString& description) {
    SetupAssistantDialog::DependencyRow row;
    row.container = new QWidget();
    auto* layout = new QVBoxLayout(row.container);
    layout->setContentsMargins(0, 8, 0, 12);
    layout->setSpacing(6);
    auto* titleLabel = new QLabel(title);
    QFont font = titleLabel->font();
    font.setBold(true);
    titleLabel->setFont(font);
    layout->addWidget(titleLabel);
    auto* descriptionLabel = new QLabel(description);
    descriptionLabel->setWordWrap(true);
    layout->addWidget(descriptionLabel);
    auto* statusLayout = new QHBoxLayout();
    statusLayout->setContentsMargins(0, 2, 0, 2);
    statusLayout->setSpacing(10);
    row.statusIcon = new QLabel();
    row.statusIcon->setAlignment(Qt::AlignCenter);
    row.statusIcon->setFixedSize(42, 42);
    QFont statusIconFont = row.statusIcon->font();
    statusIconFont.setBold(true);
    statusIconFont.setPointSize(24);
    row.statusIcon->setFont(statusIconFont);
    statusLayout->addWidget(row.statusIcon);
    row.status = new QLabel();
    row.status->setWordWrap(true);
    row.status->setAccessibleName(
        TranslateUi(QStringLiteral("%1 status").arg(title)));
    statusLayout->addWidget(row.status, 1);
    layout->addLayout(statusLayout);
    row.progress = new QProgressBar();
    row.progress->setRange(0, 100);
    row.progress->setVisible(false);
    row.progress->setAccessibleName(
        TranslateUi(
            QStringLiteral("%1 download progress").arg(title)));
    layout->addWidget(row.progress);
    auto* actions = new QHBoxLayout();
    row.install = new QPushButton(QStringLiteral("Install"));
    row.install->setAccessibleName(
        TranslateUi(QStringLiteral("Install %1").arg(title)));
    row.remove = new QPushButton(QStringLiteral("Remove"));
    row.remove->setAccessibleName(
        TranslateUi(QStringLiteral("Remove %1").arg(title)));
    actions->addWidget(row.install);
    actions->addWidget(row.remove);
    actions->addStretch(1);
    layout->addLayout(actions);
    parent->addWidget(row.container);
    return row;
}

void SetDependencyStatus(SetupAssistantDialog::DependencyRow& row,
                         bool installed,
                         const QString& text) {
    row.statusIcon->setText(installed ? QStringLiteral("\u2713") : QStringLiteral("\u2715"));
    row.statusIcon->setStyleSheet(installed
                                      ? QStringLiteral("color: #33d17a;")
                                      : QStringLiteral("color: #ff6b6b;"));
    row.statusIcon->setAccessibleName(installed
                                          ? QStringLiteral("Installed")
                                          : QStringLiteral("Not installed"));
    SetLiveText(row.status, text, LivePoliteness::kSilent,
                QStringLiteral("Setup status"));
}

void SetDependencyProgress(SetupAssistantDialog::DependencyRow& row, const QString& text) {
    row.statusIcon->setText(QStringLiteral("\u2026"));
    row.statusIcon->setStyleSheet(QStringLiteral("color: #f6c85f;"));
    row.statusIcon->setAccessibleName(QStringLiteral("Setup in progress"));
    SetLiveText(row.status, text, LivePoliteness::kPolite,
                QStringLiteral("Setup progress"));
}

QString RegistryString(HKEY key, const wchar_t* valueName) {
    DWORD bytes = 0;
    if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, valueName, RRF_RT_REG_SZ, nullptr,
                     value.data(), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return QString::fromStdWString(value);
}

QString FindUninstallCommandInView(REGSAM view) {
    constexpr wchar_t kUninstallKey[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_READ | view, &root) != ERROR_SUCCESS) {
        return {};
    }
    DWORD index = 0;
    std::array<wchar_t, 256> name{};
    DWORD nameLength = static_cast<DWORD>(name.size());
    while (RegEnumKeyExW(root, index++, name.data(), &nameLength,
                         nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        HKEY subkey = nullptr;
        if (RegOpenKeyExW(root, name.data(), 0, KEY_READ | view, &subkey) == ERROR_SUCCESS) {
            const QString displayName = RegistryString(subkey, L"DisplayName");
            if (displayName.contains(QStringLiteral("NVIDIA Video Effects"), Qt::CaseInsensitive)) {
                QString command = RegistryString(subkey, L"QuietUninstallString");
                if (command.isEmpty()) {
                    command = RegistryString(subkey, L"UninstallString");
                }
                RegCloseKey(subkey);
                RegCloseKey(root);
                return command;
            }
            RegCloseKey(subkey);
        }
        nameLength = static_cast<DWORD>(name.size());
    }
    RegCloseKey(root);
    return {};
}

} // namespace

SetupAssistantDialog::SetupAssistantDialog(const QString& configuredTesseractPath,
                                           const QString& configuredCodexPath,
                                           bool declined,
                                           QWidget* parent)
    : QDialog(parent),
      configuredTesseractPath_(configuredTesseractPath),
      configuredCodexPath_(configuredCodexPath),
      nvidiaArchitecture_(DetectNvidiaArchitecture()) {
    setWindowTitle(QStringLiteral("OpenZoom Setup Assistant"));
    setAttribute(Qt::WA_DeleteOnClose);
    setModal(false);
    resize(680, 560);
    setMinimumSize(560, 420);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(20, 18, 20, 18);
    root->setSpacing(12);
    auto* heading = new QLabel(QStringLiteral("Optional Reading and AI Tools"));
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    headingFont.setPointSize(16);
    heading->setFont(headingFont);
    root->addWidget(heading);
    auto* intro = new QLabel(QStringLiteral(
        "OpenZoom downloads these tools directly from their vendors and verifies each download "
        "before it runs. They are not included in the OpenZoom package."));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* rowsWidget = new QWidget();
    auto* rows = new QVBoxLayout(rowsWidget);
    rows->setContentsMargins(0, 0, 0, 0);
    tesseractRow_ = AddDependencyRow(
        rows, QStringLiteral("Tesseract OCR"),
        QStringLiteral("Reads printed text locally. Installs only for your Windows account."));
    codexRow_ = AddDependencyRow(
        rows, QStringLiteral("Codex CLI"),
        QStringLiteral("Powers subscription-backed Explain and Assistant features. "
                       "Installs the official Codex CLI for your Windows account; "
                       "ChatGPT sign-in remains a separate step."));
    nvidiaRow_ = AddDependencyRow(
        rows, QStringLiteral("NVIDIA Video Effects"),
        QStringLiteral("Provides Super Resolution on supported NVIDIA RTX GPUs. "
                       "The vendor installer may request administrator approval."));
    nvidiaRow_.container->setVisible(!nvidiaArchitecture_.isEmpty());
    rows->addStretch(1);
    scroll->setWidget(rowsWidget);
    root->addWidget(scroll, 1);

    declineCheckbox_ = new QCheckBox(QStringLiteral("Don't ask again automatically"));
    declineCheckbox_->setChecked(declined);
    declineCheckbox_->setAccessibleDescription(
        QStringLiteral("The Setup Assistant remains available from Advanced settings"));
    root->addWidget(declineCheckbox_);
    auto* bottom = new QHBoxLayout();
    cancelDownloadButton_ = new QPushButton(QStringLiteral("Cancel Download"));
    cancelDownloadButton_->setVisible(false);
    auto* closeButton = new QPushButton(QStringLiteral("Close"));
    closeButton->setDefault(true);
    bottom->addWidget(cancelDownloadButton_);
    bottom->addStretch(1);
    bottom->addWidget(closeButton);
    root->addLayout(bottom);

    network_ = new QNetworkAccessManager(this);
    inactivityTimer_ = new QTimer(this);
    inactivityTimer_->setSingleShot(true);
    inactivityTimer_->setInterval(60000);

    connect(tesseractRow_.install, &QPushButton::clicked,
            this, [this]() { BeginDownload(Dependency::Tesseract); });
    connect(codexRow_.install, &QPushButton::clicked,
            this, [this]() { BeginDownload(Dependency::CodexCli); });
    connect(nvidiaRow_.install, &QPushButton::clicked,
            this, [this]() { BeginDownload(Dependency::NvidiaVideoEffects); });
    connect(tesseractRow_.remove, &QPushButton::clicked,
            this, &SetupAssistantDialog::RemoveTesseract);
    connect(codexRow_.remove, &QPushButton::clicked,
            this, &SetupAssistantDialog::OpenCodexLocationOrGuide);
    connect(nvidiaRow_.remove, &QPushButton::clicked,
            this, &SetupAssistantDialog::RemoveNvidiaRuntime);
    connect(cancelDownloadButton_, &QPushButton::clicked,
            this, &SetupAssistantDialog::CancelDownload);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    connect(declineCheckbox_, &QCheckBox::toggled,
            this, &SetupAssistantDialog::DeclinePreferenceChanged);
    connect(inactivityTimer_, &QTimer::timeout, this, [this]() {
        if (reply_) {
            reply_->abort();
        }
    });

    RefreshStatus();
}

SetupAssistantDialog::DependencyRow& SetupAssistantDialog::RowForDependency(
    Dependency dependency) {
    switch (dependency) {
    case Dependency::Tesseract:
        return tesseractRow_;
    case Dependency::CodexCli:
        return codexRow_;
    case Dependency::NvidiaVideoEffects:
    case Dependency::None:
        return nvidiaRow_;
    }
    return nvidiaRow_;
}

SetupAssistantDialog::~SetupAssistantDialog() {
    if (reply_) {
        disconnect(reply_, nullptr, this, nullptr);
        reply_->abort();
    }
    if (fallbackDownloadProcess_) {
        disconnect(fallbackDownloadProcess_, nullptr, this, nullptr);
        if (fallbackDownloadProcess_->state() != QProcess::NotRunning) {
            fallbackDownloadProcess_->kill();
            if (!fallbackDownloadProcess_->waitForFinished(250)) {
                qWarning() << "Setup download helper did not exit within the "
                              "bounded shutdown window";
            }
        }
    }
    if (downloadFile_) {
        downloadFile_->close();
    }
    if (activeDependency_ != Dependency::None && !downloadPath_.isEmpty()) {
        QFile::remove(downloadPath_);
    }
    if (elevatedInstallerHandle_) {
        CloseHandle(static_cast<HANDLE>(elevatedInstallerHandle_));
        elevatedInstallerHandle_ = nullptr;
    }
}

void SetupAssistantDialog::closeEvent(QCloseEvent* event) {
    if ((installerProcess_ && installerProcess_->state() != QProcess::NotRunning) ||
        elevatedInstallerHandle_) {
        DependencyRow& row = RowForDependency(activeDependency_);
        SetLiveText(
            row.status,
            QStringLiteral(
                "The vendor installer is still running. Finish or cancel it "
                "before closing Setup."),
            LivePoliteness::kAssertive,
            QStringLiteral("Setup status"));
        event->ignore();
        return;
    }
    if (reply_) {
        downloadCancelled_ = true;
        reply_->abort();
    }
    if (fallbackDownloadProcess_ &&
        fallbackDownloadProcess_->state() != QProcess::NotRunning) {
        downloadCancelled_ = true;
        fallbackDownloadProcess_->kill();
    }
    QDialog::closeEvent(event);
}

QString SetupAssistantDialog::ManagedTesseractDirectory() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("OpenZoom/tools/tesseract"));
}

QString SetupAssistantDialog::FindTesseractExecutable(const QString& configuredPath) {
    QStringList candidates;
    if (!configuredPath.trimmed().isEmpty()) {
        const QFileInfo configured(configuredPath.trimmed());
        candidates.push_back(configured.isDir()
                                 ? QDir(configured.absoluteFilePath()).filePath(QStringLiteral("tesseract.exe"))
                                 : configured.absoluteFilePath());
    }
    candidates.push_back(QDir(ManagedTesseractDirectory()).filePath(QStringLiteral("tesseract.exe")));
    const QString fromPath = QStandardPaths::findExecutable(QStringLiteral("tesseract.exe"));
    if (!fromPath.isEmpty()) {
        candidates.push_back(fromPath);
    }
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    if (!programFiles.isEmpty()) {
        candidates.push_back(QDir(programFiles).filePath(QStringLiteral("Tesseract-OCR/tesseract.exe")));
    }
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!localAppData.isEmpty()) {
        candidates.push_back(QDir(localAppData).filePath(
            QStringLiteral("Programs/Tesseract-OCR/tesseract.exe")));
    }
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate).isFile()) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

QString SetupAssistantDialog::FindCodexExecutable(const QString& configuredPath) {
    QStringList candidates;
    const QString configured = configuredPath.trimmed();
    if (!configured.isEmpty()) {
        const QFileInfo configuredInfo(configured);
        candidates.push_back(
            configuredInfo.isDir()
                ? QDir(configuredInfo.absoluteFilePath()).filePath(QStringLiteral("codex.exe"))
                : configuredInfo.absoluteFilePath());
        const QString configuredFromPath = QStandardPaths::findExecutable(configured);
        if (!configuredFromPath.isEmpty()) {
            candidates.push_back(configuredFromPath);
        }
    }
    for (const QString& name : {QStringLiteral("codex.exe"), QStringLiteral("codex")}) {
        const QString fromPath = QStandardPaths::findExecutable(name);
        if (!fromPath.isEmpty()) {
            candidates.push_back(fromPath);
        }
    }
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA").trimmed();
    if (!localAppData.isEmpty()) {
        const QDir local(localAppData);
        candidates.push_back(
            local.filePath(QStringLiteral("Programs/OpenAI/Codex/bin/codex.exe")));
        candidates.push_back(
            local.filePath(QStringLiteral("Microsoft/WinGet/Links/codex.exe")));
    }
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate).isFile()) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return {};
}

QString SetupAssistantDialog::DetectNvidiaArchitecture() {
#if OPENZOOM_ENABLE_CUDA
    int count = 0;
    if (cudaGetDeviceCount(&count) != cudaSuccess || count <= 0) {
        return {};
    }
    int bestMajor = 0;
    int bestMinor = 0;
    for (int device = 0; device < count; ++device) {
        cudaDeviceProp properties{};
        if (cudaGetDeviceProperties(&properties, device) == cudaSuccess &&
            (properties.major > bestMajor ||
             (properties.major == bestMajor && properties.minor > bestMinor))) {
            bestMajor = properties.major;
            bestMinor = properties.minor;
        }
    }
    if (bestMajor >= 10) return QStringLiteral("blackwell");
    if (bestMajor == 8 && bestMinor >= 9) return QStringLiteral("ada");
    if (bestMajor == 8) return QStringLiteral("ampere");
    if (bestMajor == 7 && bestMinor >= 5) return QStringLiteral("turing");
#endif
    return {};
}

bool SetupAssistantDialog::NeedsSetup(const QString& configuredTesseractPath,
                                      const QString& configuredCodexPath) {
    if (FindTesseractExecutable(configuredTesseractPath).isEmpty()) {
        return true;
    }
    if (FindCodexExecutable(configuredCodexPath).isEmpty()) {
        return true;
    }
    return !DetectNvidiaArchitecture().isEmpty() && !MaxineSuperRes::IsRuntimeInstalled();
}

void SetupAssistantDialog::RefreshStatus() {
    const QString tesseract = FindTesseractExecutable(configuredTesseractPath_);
    const bool tesseractInstalled = !tesseract.isEmpty();
    const bool managed = !tesseract.isEmpty() &&
                         QFileInfo(tesseract).absoluteFilePath().startsWith(
                             QFileInfo(ManagedTesseractDirectory()).absoluteFilePath(),
                             Qt::CaseInsensitive);
    SetDependencyStatus(
        tesseractRow_, tesseractInstalled,
        !tesseractInstalled
            ? QStringLiteral("Not installed")
            : managed
                  ? QStringLiteral("Installed and managed by OpenZoom\n%1").arg(tesseract)
                  : QStringLiteral(
                        "Installed system-wide\n%1\nRemoval is managed by Windows.")
                        .arg(tesseract));
    tesseractRow_.install->setEnabled(!tesseractInstalled && activeDependency_ == Dependency::None);
    tesseractRow_.remove->setText(managed ? QStringLiteral("Remove")
                                          : tesseractInstalled
                                                ? QStringLiteral("Open Windows Apps")
                                                : QStringLiteral("Remove"));
    tesseractRow_.remove->setAccessibleName(
        managed ? QStringLiteral("Remove OpenZoom-managed Tesseract OCR")
                : QStringLiteral("Open Windows Installed Apps for Tesseract OCR"));
    tesseractRow_.remove->setEnabled(tesseractInstalled && activeDependency_ == Dependency::None);
    tesseractRow_.remove->setToolTip(
        managed ? QStringLiteral("Remove OpenZoom's per-user Tesseract installation")
                : tesseractInstalled
                      ? QStringLiteral("This system-wide copy must be removed through Windows Installed Apps")
                      : QStringLiteral("Tesseract OCR is not installed"));

    const QString codex = FindCodexExecutable(configuredCodexPath_);
    const bool codexInstalled = !codex.isEmpty();
    SetDependencyStatus(
        codexRow_, codexInstalled,
        codexInstalled
            ? QStringLiteral("Installed\n%1\nUse Connect ChatGPT in AI Settings to sign in.")
                  .arg(codex)
            : QStringLiteral("Not installed"));
    codexRow_.install->setText(
        codexInstalled ? QStringLiteral("Update") : QStringLiteral("Install"));
    codexRow_.install->setAccessibleName(
        codexInstalled ? QStringLiteral("Update Codex CLI")
                       : QStringLiteral("Install Codex CLI"));
    codexRow_.install->setEnabled(activeDependency_ == Dependency::None);
    codexRow_.remove->setText(
        codexInstalled ? QStringLiteral("Open Install Folder")
                       : QStringLiteral("Setup Guide"));
    codexRow_.remove->setAccessibleName(
        codexInstalled ? QStringLiteral("Open Codex CLI install folder")
                       : QStringLiteral("Open Codex CLI setup guide"));
    codexRow_.remove->setToolTip(
        codexInstalled
            ? QStringLiteral("Open the folder containing the detected Codex CLI")
            : QStringLiteral("Open the official Codex CLI documentation"));
    codexRow_.remove->setEnabled(activeDependency_ == Dependency::None);

    if (!nvidiaArchitecture_.isEmpty()) {
        const bool installed = MaxineSuperRes::IsRuntimeInstalled();
        SetDependencyStatus(nvidiaRow_, installed,
                            installed
                                ? QStringLiteral("Installed")
                                : QStringLiteral("Not installed - %1 installer selected")
                                      .arg(nvidiaArchitecture_));
        nvidiaRow_.install->setEnabled(!installed && activeDependency_ == Dependency::None);
        nvidiaRow_.remove->setEnabled(installed && activeDependency_ == Dependency::None);
    }
}

void SetupAssistantDialog::BeginDownload(Dependency dependency) {
    if (activeDependency_ != Dependency::None) {
        return;
    }
    QUrl url;
    QString fileName;
    if (dependency == Dependency::Tesseract) {
        url = QUrl(QString::fromLatin1(kTesseractUrl));
        alternateDownloadUrl_ = QString::fromLatin1(kTesseractAlternateUrl);
        expectedSha256_ = QString::fromLatin1(kTesseractSha256);
        vendorPage_ = QString::fromLatin1(kTesseractVendorPage);
        fileName = QStringLiteral("tesseract-ocr-w64-setup-5.4.0.20240606.exe");
    } else if (dependency == Dependency::CodexCli) {
        url = QUrl(QString::fromLatin1(kCodexInstallerUrl));
        alternateDownloadUrl_.clear();
        expectedSha256_ = QString::fromLatin1(kCodexInstallerSha256);
        vendorPage_ = QString::fromLatin1(kCodexVendorPage);
        fileName = QStringLiteral("codex-install.ps1");
    } else {
        alternateDownloadUrl_.clear();
        const auto installer = std::find_if(
            kNvidiaInstallers.begin(), kNvidiaInstallers.end(), [this](const NvidiaInstaller& item) {
                return nvidiaArchitecture_ == QString::fromLatin1(item.architecture);
            });
        if (installer == kNvidiaInstallers.end()) {
            return;
        }
        url = QUrl(QString::fromLatin1(installer->url));
        expectedSha256_ = QString::fromLatin1(installer->sha256);
        vendorPage_ = QString::fromLatin1(kNvidiaVendorPage);
        fileName = QFileInfo(url.path()).fileName();
    }
    downloadUrl_ = url.toString();
    downloadCancelled_ = false;
    fallbackUsingAlternateUrl_ = false;
    primaryDownloadError_.clear();

    const QString downloadDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                          .filePath(QStringLiteral("OpenZoom/downloads"));
    QDir().mkpath(downloadDirectory);
    downloadPath_ = QDir(downloadDirectory).filePath(fileName);
    downloadFile_ = std::make_unique<QFile>(downloadPath_);
    if (!downloadFile_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ShowDownloadFailure(QStringLiteral("The temporary download file could not be created."), vendorPage_);
        downloadFile_.reset();
        return;
    }

    activeDependency_ = dependency;
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("OpenZoom Setup Assistant"));
    reply_ = network_->get(request);
    DependencyRow& row = RowForDependency(dependency);
    row.progress->setValue(0);
    row.progress->setVisible(true);
    cancelDownloadButton_->setVisible(true);
    RefreshStatus();
    SetDependencyProgress(row, QStringLiteral("Downloading from vendor..."));
    inactivityTimer_->start();

    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        inactivityTimer_->start();
        const QByteArray chunk = reply_->readAll();
        if (downloadFile_ && downloadFile_->write(chunk) == chunk.size()) {
            hash_->addData(chunk);
        } else {
            reply_->abort();
        }
    });
    QProgressBar* progress = row.progress;
    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this, progress](qint64 received, qint64 total) {
                inactivityTimer_->start();
                progress->setRange(0, total > 0 ? 100 : 0);
                if (total > 0) {
                    progress->setValue(static_cast<int>((received * 100) / total));
                }
            });
    connect(reply_, &QNetworkReply::finished, this, &SetupAssistantDialog::FinishDownload);
}

void SetupAssistantDialog::CancelDownload() {
    downloadCancelled_ = true;
    if (reply_) {
        reply_->abort();
    } else if (fallbackDownloadProcess_ &&
               fallbackDownloadProcess_->state() != QProcess::NotRunning) {
        fallbackDownloadProcess_->kill();
    }
}

void SetupAssistantDialog::FinishDownload() {
    inactivityTimer_->stop();
    const Dependency finishedDependency = activeDependency_;
    const QNetworkReply::NetworkError error = reply_ ? reply_->error() : QNetworkReply::UnknownNetworkError;
    const QString errorText = reply_ ? reply_->errorString() : QStringLiteral("Unknown download error");
    if (downloadFile_) {
        downloadFile_->close();
        downloadFile_.reset();
    }
    if (reply_) {
        reply_->deleteLater();
        reply_ = nullptr;
    }
    cancelDownloadButton_->setVisible(false);
    DependencyRow& row = RowForDependency(finishedDependency);
    row.progress->setVisible(false);

    if (error != QNetworkReply::NoError) {
        QFile::remove(downloadPath_);
        if (downloadCancelled_) {
            ResetDownloadState();
            RefreshStatus();
            return;
        }
        StartWindowsDownloadFallback(errorText);
        return;
    }
    QString verificationError;
    if (!VerifyDownloadedInstaller(verificationError)) {
        QFile::remove(downloadPath_);
        activeDependency_ = Dependency::None;
        ShowDownloadFailure(verificationError, vendorPage_);
        RefreshStatus();
        return;
    }
    SetDependencyProgress(row, QStringLiteral("Verified. Starting installer..."));
    StartVerifiedInstaller();
}

void SetupAssistantDialog::StartWindowsDownloadFallback(
    const QString& primaryError,
    bool useAlternateUrl) {
    primaryDownloadError_ = primaryError;
    fallbackUsingAlternateUrl_ = useAlternateUrl;
    inactivityTimer_->stop();
    const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl.exe"));
    if (curl.isEmpty()) {
        activeDependency_ = Dependency::None;
        cancelDownloadButton_->setVisible(false);
        ShowDownloadFailure(
            QStringLiteral("Download failed: %1\n\nWindows curl.exe was not available "
                           "for the automatic retry.")
                .arg(primaryError),
            vendorPage_);
        RefreshStatus();
        return;
    }

    DependencyRow& row = RowForDependency(activeDependency_);
    row.progress->setRange(0, 0);
    row.progress->setVisible(true);
    cancelDownloadButton_->setVisible(true);
    SetDependencyProgress(
        row,
        useAlternateUrl
            ? QStringLiteral("Retrying from the alternate vendor host...")
            : QStringLiteral(
                  "The first transfer failed. Retrying with the Windows downloader..."));

    fallbackDownloadProcess_ = new QProcess(this);
    fallbackDownloadProcess_->setProcessChannelMode(QProcess::MergedChannels);
    fallbackDownloadProcess_->setProgram(curl);
    fallbackDownloadProcess_->setArguments(
        {QStringLiteral("--fail"),
         QStringLiteral("--location"),
         QStringLiteral("--silent"),
         QStringLiteral("--show-error"),
         QStringLiteral("--retry"),
         QStringLiteral("2"),
         QStringLiteral("--connect-timeout"),
         QStringLiteral("30"),
         QStringLiteral("--max-time"),
         QStringLiteral("300"),
         QStringLiteral("--output"),
         QDir::toNativeSeparators(downloadPath_),
         useAlternateUrl ? alternateDownloadUrl_ : downloadUrl_});
    connect(fallbackDownloadProcess_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus status) {
                FinishWindowsDownloadFallback(
                    exitCode, status == QProcess::NormalExit);
            });
    fallbackDownloadProcess_->start();
    if (!fallbackDownloadProcess_->waitForStarted(5000)) {
        const QString fallbackError = fallbackDownloadProcess_->errorString();
        fallbackDownloadProcess_->deleteLater();
        fallbackDownloadProcess_ = nullptr;
        QFile::remove(downloadPath_);
        activeDependency_ = Dependency::None;
        cancelDownloadButton_->setVisible(false);
        ShowDownloadFailure(
            QStringLiteral("Download failed: %1\n\nThe Windows downloader could "
                           "not start: %2")
                .arg(primaryError, fallbackError),
            vendorPage_);
        RefreshStatus();
    }
}

void SetupAssistantDialog::FinishWindowsDownloadFallback(
    int exitCode,
    bool normalExit) {
    const QString fallbackOutput =
        fallbackDownloadProcess_
            ? QString::fromLocal8Bit(fallbackDownloadProcess_->readAll()).trimmed()
            : QString();
    if (fallbackDownloadProcess_) {
        fallbackDownloadProcess_->deleteLater();
        fallbackDownloadProcess_ = nullptr;
    }

    DependencyRow& row = RowForDependency(activeDependency_);
    row.progress->setVisible(false);
    cancelDownloadButton_->setVisible(false);
    if (downloadCancelled_) {
        QFile::remove(downloadPath_);
        ResetDownloadState();
        RefreshStatus();
        return;
    }
    if (!normalExit || exitCode != 0) {
        QFile::remove(downloadPath_);
        const QString detail = fallbackOutput.isEmpty()
                                   ? QStringLiteral("curl.exe exited with code %1.")
                                         .arg(exitCode)
                                   : fallbackOutput;
        if (!fallbackUsingAlternateUrl_ && !alternateDownloadUrl_.isEmpty()) {
            StartWindowsDownloadFallback(
                QStringLiteral("%1\nWindows retry: %2")
                    .arg(primaryDownloadError_, detail),
                true);
            return;
        }
        activeDependency_ = Dependency::None;
        ShowDownloadFailure(
            QStringLiteral("The Qt transfer failed: %1\n\nThe Windows retry also "
                           "failed: %2")
                .arg(primaryDownloadError_, detail),
            vendorPage_);
        RefreshStatus();
        return;
    }

    QString verificationError;
    if (!VerifyDownloadedInstaller(verificationError)) {
        QFile::remove(downloadPath_);
        activeDependency_ = Dependency::None;
        ShowDownloadFailure(verificationError, vendorPage_);
        RefreshStatus();
        return;
    }
    SetDependencyProgress(row, QStringLiteral("Verified. Starting installer..."));
    StartVerifiedInstaller();
}

bool SetupAssistantDialog::VerifyDownloadedInstaller(QString& error) {
    QFile installer(downloadPath_);
    if (!installer.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("The downloaded installer could not be opened for verification.");
        return false;
    }
    QCryptographicHash verifier(QCryptographicHash::Sha256);
    if (!verifier.addData(&installer)) {
        error = QStringLiteral("The downloaded installer could not be read for verification.");
        return false;
    }
    const QString actualHash = QString::fromLatin1(verifier.result().toHex());
    if (actualHash.compare(expectedSha256_, Qt::CaseInsensitive) != 0) {
        error = QStringLiteral(
            "The installer failed SHA-256 verification and was deleted.");
        return false;
    }
    return true;
}

void SetupAssistantDialog::ResetDownloadState() {
    activeDependency_ = Dependency::None;
    downloadCancelled_ = false;
    downloadUrl_.clear();
    alternateDownloadUrl_.clear();
    expectedSha256_.clear();
    vendorPage_.clear();
    primaryDownloadError_.clear();
    fallbackUsingAlternateUrl_ = false;
    hash_.reset();
    cancelDownloadButton_->setVisible(false);
}

void SetupAssistantDialog::StartVerifiedInstaller() {
    const Dependency installing = activeDependency_;
    if (installing == Dependency::NvidiaVideoEffects) {
        StartElevatedNvidiaInstaller();
        return;
    }

    QString program = downloadPath_;
    QStringList arguments;
    if (installing == Dependency::Tesseract) {
        QDir().mkpath(ManagedTesseractDirectory());
        arguments = {QStringLiteral("/VERYSILENT"), QStringLiteral("/SUPPRESSMSGBOXES"),
                     QStringLiteral("/NORESTART"), QStringLiteral("/CURRENTUSER"),
                     QStringLiteral("/DIR=%1").arg(QDir::toNativeSeparators(ManagedTesseractDirectory()))};
    } else if (installing == Dependency::CodexCli) {
        program = QStandardPaths::findExecutable(QStringLiteral("powershell.exe"));
        if (program.isEmpty()) {
            program = QDir(qEnvironmentVariable("SystemRoot", "C:\\Windows"))
                          .filePath(QStringLiteral(
                              "System32/WindowsPowerShell/v1.0/powershell.exe"));
        }
        arguments = {
            QStringLiteral("-NoLogo"),
            QStringLiteral("-NoProfile"),
            QStringLiteral("-NonInteractive"),
            QStringLiteral("-ExecutionPolicy"),
            QStringLiteral("Bypass"),
            QStringLiteral("-File"),
            QDir::toNativeSeparators(downloadPath_)};
        SetDependencyProgress(
            codexRow_,
            QStringLiteral("Verified. Installing the official Codex CLI..."));
    }
    installerProcess_ = new QProcess(this);
    installerProcess_->setProcessChannelMode(QProcess::MergedChannels);
    if (installing == Dependency::CodexCli) {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("CODEX_NON_INTERACTIVE"), QStringLiteral("1"));
        installerProcess_->setProcessEnvironment(environment);
    }
    connect(installerProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, installing](int exitCode, QProcess::ExitStatus status) {
                const QString detail =
                    QString::fromLocal8Bit(installerProcess_->readAll()).trimmed();
                installerProcess_->deleteLater();
                installerProcess_ = nullptr;
                CompleteInstaller(installing,
                                  status == QProcess::NormalExit && exitCode == 0,
                                  detail);
            });
    installerProcess_->start(program, arguments);
    if (!installerProcess_->waitForStarted(5000)) {
        const QString startError = installerProcess_->errorString();
        installerProcess_->deleteLater();
        installerProcess_ = nullptr;
        QFile::remove(downloadPath_);
        activeDependency_ = Dependency::None;
        ShowDownloadFailure(
            QStringLiteral("Windows could not start the verified installer.\n\n%1")
                .arg(startError),
            vendorPage_);
        RefreshStatus();
    }
}

void SetupAssistantDialog::StartElevatedNvidiaInstaller() {
    const std::wstring installerPath =
        QDir::toNativeSeparators(downloadPath_).toStdWString();
    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    executeInfo.hwnd = reinterpret_cast<HWND>(winId());
    executeInfo.lpVerb = L"runas";
    executeInfo.lpFile = installerPath.c_str();
    executeInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&executeInfo) || !executeInfo.hProcess) {
        const DWORD error = GetLastError();
        QFile::remove(downloadPath_);
        activeDependency_ = Dependency::None;
        RefreshStatus();
        const QString message = error == ERROR_CANCELLED
                                    ? QStringLiteral(
                                          "Administrator approval was cancelled. Nothing was installed.")
                                    : QStringLiteral(
                                          "Windows could not start the elevated NVIDIA installer "
                                          "(error %1).").arg(error);
        ShowDownloadFailure(message, QString::fromLatin1(kNvidiaVendorPage));
        return;
    }

    elevatedInstallerHandle_ = executeInfo.hProcess;
    SetDependencyProgress(
        nvidiaRow_,
        QStringLiteral("NVIDIA installer running. Continue in the vendor installer window."));
    elevatedInstallerTimer_ = new QTimer(this);
    elevatedInstallerTimer_->setInterval(500);
    connect(elevatedInstallerTimer_, &QTimer::timeout, this, [this]() {
        if (!elevatedInstallerHandle_) {
            return;
        }
        DWORD exitCode = STILL_ACTIVE;
        if (!GetExitCodeProcess(static_cast<HANDLE>(elevatedInstallerHandle_), &exitCode) ||
            exitCode == STILL_ACTIVE) {
            return;
        }
        CloseHandle(static_cast<HANDLE>(elevatedInstallerHandle_));
        elevatedInstallerHandle_ = nullptr;
        elevatedInstallerTimer_->stop();
        elevatedInstallerTimer_->deleteLater();
        elevatedInstallerTimer_ = nullptr;
        CompleteInstaller(Dependency::NvidiaVideoEffects,
                          exitCode == 0 || exitCode == ERROR_SUCCESS_REBOOT_REQUIRED ||
                              exitCode == ERROR_SUCCESS_RESTART_REQUIRED);
    });
    elevatedInstallerTimer_->start();
}

void SetupAssistantDialog::CompleteInstaller(Dependency dependency,
                                             bool success,
                                             const QString& detail) {
    QFile::remove(downloadPath_);
    if (success && dependency == Dependency::Tesseract) {
        const QString executable = FindTesseractExecutable(ManagedTesseractDirectory());
        if (!executable.isEmpty()) {
            configuredTesseractPath_ = executable;
            emit TesseractPathChanged(executable);
        } else {
            success = false;
        }
    } else if (success && dependency == Dependency::CodexCli) {
        const QString executable = FindCodexExecutable();
        if (!executable.isEmpty()) {
            configuredCodexPath_ = executable;
            emit CodexPathChanged(executable);
        } else {
            success = false;
        }
    }
    ResetDownloadState();
    RefreshStatus();
    emit DependenciesChanged();
    if (!success) {
        QString message = QStringLiteral("The installer did not complete successfully.");
        if (!detail.isEmpty()) {
            message += QStringLiteral("\n\n%1").arg(detail.right(1600));
        }
        const QString vendorPage =
            dependency == Dependency::Tesseract
                ? QString::fromLatin1(kTesseractVendorPage)
                : dependency == Dependency::CodexCli
                      ? QString::fromLatin1(kCodexVendorPage)
                      : QString::fromLatin1(kNvidiaVendorPage);
        ShowDownloadFailure(message, vendorPage);
    }
}

void SetupAssistantDialog::ShowDownloadFailure(const QString& message, const QString& vendorPage) {
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Setup Assistant"), message,
                    QMessageBox::NoButton, this);
    QPushButton* vendorButton = box.addButton(QStringLiteral("Open Vendor Page"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (box.clickedButton() == vendorButton) {
        QDesktopServices::openUrl(QUrl(vendorPage));
    }
}

void SetupAssistantDialog::RemoveTesseract() {
    const QString managed = QFileInfo(ManagedTesseractDirectory()).absoluteFilePath();
    const QString executable = FindTesseractExecutable(configuredTesseractPath_);
    if (executable.isEmpty()) {
        return;
    }
    if (!QFileInfo(executable).absoluteFilePath().startsWith(managed, Qt::CaseInsensitive)) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:appsfeatures")));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Remove Tesseract OCR"),
                              QStringLiteral("Remove OpenZoom's per-user Tesseract installation?"))
        != QMessageBox::Yes) {
        return;
    }
    if (QDir(ManagedTesseractDirectory()).removeRecursively()) {
        configuredTesseractPath_.clear();
        emit TesseractPathChanged(QString());
        emit DependenciesChanged();
    }
    RefreshStatus();
}

void SetupAssistantDialog::OpenCodexLocationOrGuide() {
    const QString executable = FindCodexExecutable(configuredCodexPath_);
    if (!executable.isEmpty()) {
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(QFileInfo(executable).absolutePath()));
        return;
    }
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(kCodexVendorPage)));
}

QString SetupAssistantDialog::FindNvidiaUninstallCommand() {
    QString command = FindUninstallCommandInView(KEY_WOW64_64KEY);
    if (command.isEmpty()) {
        command = FindUninstallCommandInView(KEY_WOW64_32KEY);
    }
    return command;
}

void SetupAssistantDialog::RemoveNvidiaRuntime() {
    if (QMessageBox::question(this, QStringLiteral("Remove NVIDIA Video Effects"),
                              QStringLiteral("Open the NVIDIA runtime uninstaller?"))
        != QMessageBox::Yes) {
        return;
    }
    const QString command = FindNvidiaUninstallCommand();
    if (command.isEmpty()) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:appsfeatures")));
        return;
    }
    const QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) {
        return;
    }
    // Let the host drain CUDA work and unload the runtime DLLs before the
    // vendor uninstaller attempts to remove them.
    emit DependenciesChanged();
    QProcess::startDetached(parts.front(), parts.mid(1));
    QTimer::singleShot(3000, this, [this]() {
        RefreshStatus();
        emit DependenciesChanged();
    });
}

} // namespace openzoom

#endif // _WIN32
