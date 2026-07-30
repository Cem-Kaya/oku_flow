#ifdef _WIN32

#include "debug_log.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfoList>
#include <QMessageLogContext>
#include <QRegularExpression>

#include <windows.h>

#include <cstdio>
#include <deque>
#include <mutex>

namespace openzoom::debug_log {

namespace {

constexpr qsizetype kMaximumBufferedBytes = 1024 * 1024;
constexpr size_t kMaximumBufferedLines = 1000;
constexpr int kMaximumRetainedLogs = 20;

struct DebugLogState {
    std::mutex mutex;
    bool enabled{false};
    QtMessageHandler previousHandler{nullptr};
    QFile file;
    QString path;
    std::deque<QByteArray> startupLines;
    qsizetype startupBytes{0};
};

DebugLogState& State()
{
    static DebugLogState state;
    return state;
}

bool HasAttachedConsole()
{
    if (GetConsoleWindow() != nullptr) {
        return true;
    }

    const HANDLE standardError = GetStdHandle(STD_ERROR_HANDLE);
    if (standardError == nullptr || standardError == INVALID_HANDLE_VALUE) {
        return false;
    }
    return GetFileType(standardError) != FILE_TYPE_UNKNOWN;
}

void WriteConsoleLine(const QByteArray& line)
{
    if (!line.isEmpty()) {
        std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
    }
    std::fwrite("\n", 1, 1, stderr);
    std::fflush(stderr);
}

void BufferStartupLine(DebugLogState& state, const QByteArray& line)
{
    const qsizetype storedBytes = line.size() + 1;
    while (!state.startupLines.empty() &&
           (state.startupLines.size() >= kMaximumBufferedLines ||
            state.startupBytes + storedBytes > kMaximumBufferedBytes)) {
        state.startupBytes -= state.startupLines.front().size() + 1;
        state.startupLines.pop_front();
    }
    if (storedBytes <= kMaximumBufferedBytes) {
        state.startupLines.push_back(line);
        state.startupBytes += storedBytes;
    }
}

void MessageHandler(QtMsgType type,
                    const QMessageLogContext& context,
                    const QString& message)
{
    DebugLogState& state = State();
    const QByteArray line = qFormatLogMessage(type, context, message).toUtf8();
    QtMessageHandler previousHandler = nullptr;

    {
        std::lock_guard lock(state.mutex);
        previousHandler = state.previousHandler;
        if (state.enabled) {
            if (state.file.isOpen()) {
                state.file.write(line);
                state.file.write("\n");
                state.file.flush();
            } else {
                BufferStartupLine(state, line);
            }
        }
    }

    if (previousHandler) {
        previousHandler(type, context, message);
    } else {
        WriteConsoleLine(line);
    }
}

void PruneOldLogs(const QString& directory)
{
    QDir logDirectory(directory);
    const QFileInfoList existingLogs = logDirectory.entryInfoList(
        {QStringLiteral("OpenZoom_*.log")},
        QDir::Files | QDir::NoSymLinks,
        QDir::Time);

    // Keep room for the file opened by this launch.
    for (int index = kMaximumRetainedLogs - 1;
         index < existingLogs.size();
         ++index) {
        QFile::remove(existingLogs.at(index).absoluteFilePath());
    }
}

} // namespace

void InstallIfConsoleAttached()
{
    DebugLogState& state = State();
    std::lock_guard lock(state.mutex);
    if (state.enabled || !HasAttachedConsole()) {
        return;
    }

    state.enabled = true;
    state.previousHandler = qInstallMessageHandler(MessageHandler);
}

bool SetOutputDirectory(const QString& directory, QString* error)
{
    DebugLogState& state = State();
    {
        std::lock_guard lock(state.mutex);
        if (!state.enabled) {
            if (error) {
                error->clear();
            }
            return true;
        }
    }

    if (directory.trimmed().isEmpty() || !QDir().mkpath(directory)) {
        if (error) {
            *error = QStringLiteral(
                         "OpenZoom could not create its Debug log folder: %1")
                         .arg(directory);
        }
        return false;
    }

    PruneOldLogs(directory);
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    const QString filename =
        QStringLiteral("OpenZoom_%1_pid%2.log")
            .arg(timestamp)
            .arg(static_cast<qulonglong>(GetCurrentProcessId()));
    const QString path = QDir(directory).filePath(filename);

    {
        std::lock_guard lock(state.mutex);
        state.file.setFileName(path);
        if (!state.file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (error) {
                *error =
                    QStringLiteral("OpenZoom could not open Debug log: %1")
                        .arg(path);
            }
            state.file.setFileName({});
            return false;
        }

        state.path = QDir::toNativeSeparators(path);
        for (const QByteArray& startupLine : state.startupLines) {
            state.file.write(startupLine);
            state.file.write("\n");
        }
        state.startupLines.clear();
        state.startupBytes = 0;
        state.file.flush();
    }

    if (error) {
        error->clear();
    }
    qInfo().noquote() << "Debug log:" << QDir::toNativeSeparators(path);
    return true;
}

bool IsEnabled()
{
    DebugLogState& state = State();
    std::lock_guard lock(state.mutex);
    return state.enabled;
}

QString CurrentPath()
{
    DebugLogState& state = State();
    std::lock_guard lock(state.mutex);
    return state.path;
}

void Shutdown()
{
    DebugLogState& state = State();
    QtMessageHandler previousHandler = nullptr;
    {
        std::lock_guard lock(state.mutex);
        if (!state.enabled) {
            return;
        }
        previousHandler = state.previousHandler;
    }

    qInstallMessageHandler(previousHandler);

    std::lock_guard lock(state.mutex);
    if (state.file.isOpen()) {
        state.file.flush();
        state.file.close();
    }
    state.enabled = false;
    state.previousHandler = nullptr;
    state.path.clear();
    state.startupLines.clear();
    state.startupBytes = 0;
}

} // namespace openzoom::debug_log

#endif
