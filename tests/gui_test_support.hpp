#pragma once

#include <QApplication>
#include <QDir>
#include <QLockFile>
#include <QtTest>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace okuflow::test {

// Shared by every native GUI fixture, including direct CTest invocations in
// different build trees. Refuse contention before constructing QApplication.
class DesktopLease final {
public:
    DesktopLease()
        : lock_(QDir(qEnvironmentVariable("LOCALAPPDATA"))
                    .filePath(QStringLiteral("OkuFlow/test-evidence/desktop.lock")))
    {
        const QString root = qEnvironmentVariable("LOCALAPPDATA");
        if (!root.isEmpty() && QDir().mkpath(root + QStringLiteral("/OkuFlow/test-evidence"))) {
            lock_.setStaleLockTime(0);
            acquired_ = lock_.tryLock(0);
        }
    }
    bool acquired() const { return acquired_; }
private:
    QLockFile lock_;
    bool acquired_{};
};

// A native activation failure is a harness precondition failure, never a pass.
inline bool ActivateFixtureWindow(QWidget* widget)
{
    QWidget* window = widget->window();
    window->raise();
#ifdef Q_OS_WIN
    const HWND handle = reinterpret_cast<HWND>(window->winId());
    const DWORD thread = GetCurrentThreadId();
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = foregroundThread && foregroundThread != thread &&
                          AttachThreadInput(thread, foregroundThread, TRUE);
    SetForegroundWindow(handle);
    SetActiveWindow(handle);
    window->activateWindow();
    if (attached) AttachThreadInput(thread, foregroundThread, FALSE);
    return QTest::qWaitFor([&] {
        return GetForegroundWindow() == handle && window->isActiveWindow();
    }, 1000);
#else
    window->activateWindow();
    return QTest::qWaitForWindowActive(window, 1000);
#endif
}

inline const char* ActivationFailureMessage()
{
#ifdef Q_OS_WIN
    DWORD foregroundProcess = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
    if (foregroundProcess && foregroundProcess != GetCurrentProcessId()) {
        return "OKUFLOW_DESKTOP_INTERRUPTED: another process owns native foreground";
    }
#endif
    return "Fixture activation failed without confirmed foreign foreground ownership";
}

} // namespace okuflow::test

// Keep Qt's standard main setup (including AA_Use96Dpi and source-path data).
#define OKUFLOW_GUI_TEST_MAIN(TestObject) \
    QTEST_MAIN_WRAPPER(TestObject, \
        okuflow::test::DesktopLease desktopLease; \
        if (!desktopLease.acquired()) { \
            fprintf(stderr, "OKUFLOW_DESKTOP_INTERRUPTED: desktop lease unavailable\n"); \
            return 73; \
        } \
        QTEST_MAIN_SETUP())
