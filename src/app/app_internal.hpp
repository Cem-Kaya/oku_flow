#pragma once

#ifdef _WIN32

#include "openzoom/app/app.hpp"
#include "openzoom/cuda/cuda_interop.hpp"
#include "openzoom/d3d12/presenter.hpp"
#include "openzoom/app/constants.hpp"
#include "openzoom/app/interaction_controller.hpp"
#include "openzoom/ui/main_window.hpp"
#include "openzoom/ui/ai_settings_dialog.hpp"
#include "openzoom/ui/color_scheme_picker.hpp"
#include "openzoom/app/setup_assistant.hpp"
#include "openzoom/common/maxine_superres.hpp"
#include <QAbstractButton>
#include <QAccessible>
#include <QAccessibleAnnouncementEvent>
#include <QAccessibleEvent>
#include <QApplication>
#include <QDesktopServices>
#include <QUrl>
#include <QByteArray>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QKeyEvent>
#include <QPainter>
#include <QEvent>
#include <QResizeEvent>
#include <QRegion>
#include <QWheelEvent>
#include <QTimer>
#include <QToolButton>
#include <QListWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QImage>
#include <QIcon>
#include <QString>
#include <QStringList>
#include <QSizePolicy>
#include <QPaintEngine>
#include <QResizeEvent>
#include <QShowEvent>
#include <QDebug>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QProcess>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QTextCursor>

#include <windows.h>
#include <combaseapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>
#include <shobjidl_core.h>

#include <cwchar>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <wrl/client.h>

namespace openzoom {

namespace {

constexpr int kOpenZoomIconResourceId = 101;

void ThrowIfFailed(HRESULT hr, const char* message)
{
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(message) + " (hr=0x" + std::to_string(static_cast<unsigned long>(hr)) + ")");
    }
}

void ApplyNativeWindowIcon(QWidget* window)
{
    if (!window) {
        return;
    }
    const HINSTANCE module = GetModuleHandleW(nullptr);
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    const auto loadIcon = [module](int width, int height) -> HICON {
        return static_cast<HICON>(LoadImageW(
            module, MAKEINTRESOURCEW(kOpenZoomIconResourceId), IMAGE_ICON,
            width, height, LR_DEFAULTCOLOR | LR_SHARED));
    };
    if (const HICON largeIcon = loadIcon(GetSystemMetrics(SM_CXICON),
                                         GetSystemMetrics(SM_CYICON))) {
        SendMessageW(handle, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(largeIcon));
    }
    if (const HICON smallIcon = loadIcon(GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON))) {
        SendMessageW(handle, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(smallIcon));
    }
}

namespace processing = openzoom::processing;
using namespace openzoom::app_constants;

CudaBufferFormat ParseCudaBufferFormatToken(const QString& token, bool* ok)
{
    const QString normalized = token.trimmed().toLower();
    if (normalized == QStringLiteral("rgba8") ||
        normalized == QStringLiteral("bgra8") ||
        normalized == QStringLiteral("uint8") ||
        normalized == QStringLiteral("8bit")) {
        if (ok) {
            *ok = true;
        }
        return CudaBufferFormat::kRgba8;
    }

    if (normalized == QStringLiteral("rgba16f") ||
        normalized == QStringLiteral("fp16") ||
        normalized == QStringLiteral("half") ||
        normalized == QStringLiteral("16f")) {
        if (ok) {
            *ok = true;
        }
        return CudaBufferFormat::kRgba16F;
    }

    if (ok) {
        *ok = false;
    }
    return CudaBufferFormat::kRgba8;
}

int QueryDisplayRefreshHz(HWND hwnd)
{
    if (!hwnd) {
        return 60;
    }
    const HMONITOR monitor =
        MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return 60;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(
            monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
        return 60;
    }
    const int refresh = static_cast<int>(mode.dmDisplayFrequency);
    return (refresh >= 24 && refresh <= 500) ? refresh : 60;
}

} // namespace

constexpr int kPresetIdRole = Qt::UserRole + 1;


} // namespace openzoom

#endif // _WIN32
