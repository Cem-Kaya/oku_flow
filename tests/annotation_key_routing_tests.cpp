#include "okuflow/ui/annotation_overlay.hpp"

#include <QApplication>
#include <QKeyEvent>
#include <QFocusEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QSlider>
#include <QToolButton>
#include <QtTest>

namespace {

void SendKey(QWidget* target, QEvent::Type type, int key,
             Qt::KeyboardModifiers modifiers = Qt::NoModifier, bool repeat = false)
{
    QKeyEvent event(type, key, modifiers, QString(), repeat);
    QCoreApplication::sendEvent(target, &event);
}

void SendMouse(QWidget* target, QEvent::Type type, const QPointF& point,
               Qt::MouseButton button, Qt::MouseButtons held)
{
    QMouseEvent event(type, point, point, target->mapToGlobal(point.toPoint()),
                      button, held, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &event);
}

void ConfigureCanvas(QWidget& render, okuflow::AnnotationOverlay& overlay)
{
    render.resize(900, 600);
    overlay.SetViewTransform(okuflow::ComputeViewTransform(
        1280, 720, 900, 600, 1.0f, 0.5f, 0.5f, okuflow::ViewportFitMode::kFill));
    overlay.SetActive(true);
}

bool ClickTool(okuflow::AnnotationOverlay& overlay, const QString& name)
{
    for (auto* button : overlay.findChildren<QToolButton*>()) {
        if (button->text() == name) {
            button->click();
            return true;
        }
    }
    return false;
}

void DrawLine(okuflow::AnnotationOverlay& overlay)
{
    SendMouse(&overlay, QEvent::MouseButtonPress, {550, 300}, Qt::LeftButton, Qt::LeftButton);
    SendMouse(&overlay, QEvent::MouseMove, {650, 300}, Qt::NoButton, Qt::LeftButton);
    SendMouse(&overlay, QEvent::MouseButtonRelease, {650, 300}, Qt::LeftButton, Qt::NoButton);
}

class KeyOwner final : public QWidget {
public:
    bool acceptKeys{true};
    int deliveries{};
    int lastKey{};
    Qt::KeyboardModifiers lastModifiers{};
    bool lastAutoRepeat{};
    int lastCount{};
    int releases{};
    bool panHeld{};

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        ++deliveries;
        lastKey = event->key();
        lastModifiers = event->modifiers();
        lastAutoRepeat = event->isAutoRepeat();
        lastCount = event->count();
        panHeld = event->key() == Qt::Key_Left;
        event->setAccepted(acceptKeys);
    }
    void keyReleaseEvent(QKeyEvent* event) override
    {
        ++releases;
        if (event->key() == Qt::Key_Left) panHeld = false;
        event->accept();
    }
};

// Bound the broken pre-fix path so the reproducer fails without overflowing
// the process stack. Install after the production filter, so this runs first.
class RedispatchFence final : public QObject {
public:
    int deliveries{};

protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress && ++deliveries > 1) {
            event->accept();
            return true;
        }
        return false;
    }
};

class AnnotationKeyRoutingTests final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QCOMPARE(QGuiApplication::platformName(), QStringLiteral("offscreen"));
    }

    void navigationDeliveredOnce_data()
    {
        QTest::addColumn<int>("key");
        QTest::addColumn<int>("modifiers");
        QTest::addColumn<int>("target");
        QTest::addColumn<bool>("accepted");
        const int keys[] = {Qt::Key_Left, Qt::Key_Right, Qt::Key_Up,
                            Qt::Key_Down, Qt::Key_PageUp, Qt::Key_PageDown,
                            Qt::Key_Plus, Qt::Key_Equal, Qt::Key_Minus};
        for (int key : keys) {
            for (int target = 0; target < 3; ++target) {
                for (bool accepted : {false, true}) {
                    const QByteArray row = QByteArray::number(key) + '-' +
                        QByteArray::number(target) + '-' + QByteArray::number(accepted);
                    QTest::newRow(row) << key << int(Qt::NoModifier) << target << accepted;
                }
            }
        }
        for (int key : {Qt::Key_Plus, Qt::Key_Equal, Qt::Key_Minus}) {
            for (int target = 0; target < 3; ++target) {
                for (bool accepted : {false, true}) {
                    const QByteArray row = "ctrl-" + QByteArray::number(key) + '-' +
                        QByteArray::number(target) + '-' + QByteArray::number(accepted);
                    QTest::newRow(row) << key << int(Qt::ControlModifier) << target << accepted;
                }
            }
        }
    }

    void navigationDeliveredOnce()
    {
        QFETCH(int, key);
        QFETCH(int, modifiers);
        QFETCH(int, target);
        QFETCH(bool, accepted);
        KeyOwner owner;
        owner.acceptKeys = accepted;
        QWidget render(&owner);
        render.resize(640, 360);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        overlay.SetActive(true);
        RedispatchFence fence;
        owner.installEventFilter(&fence);
        QKeyEvent event(QEvent::KeyPress, key, Qt::KeyboardModifiers(modifiers),
                        QStringLiteral("test"), true, 3);
        QWidget* receiver = target == 0 ? &owner :
            (target == 1 ? &render : static_cast<QWidget*>(&overlay));
        QCoreApplication::sendEvent(receiver, &event);
        QCOMPARE(fence.deliveries, 1);
        QCOMPARE(owner.deliveries, 1);
        QCOMPARE(owner.lastKey, key);
        QCOMPARE(int(owner.lastModifiers), modifiers);
        QVERIFY(owner.lastAutoRepeat);
        QCOMPARE(owner.lastCount, 3);
    }

    void inactiveDrawLeavesOwnerKeysAlone()
    {
        KeyOwner owner;
        QWidget render(&owner);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        QSignalSpy exit(&overlay, &okuflow::AnnotationOverlay::ExitRequested);
        QSignalSpy tool(&overlay, &okuflow::AnnotationOverlay::ToolChanged);
        for (int key : {Qt::Key_P, Qt::Key_Escape, Qt::Key_Z, Qt::Key_S}) {
            QKeyEvent event(QEvent::KeyPress, key,
                            key == Qt::Key_Z || key == Qt::Key_S
                                ? Qt::ControlModifier : Qt::NoModifier);
            QCoreApplication::sendEvent(&owner, &event);
        }
        QCOMPARE(owner.deliveries, 4);
        QCOMPARE(exit.count(), 0);
        QCOMPARE(tool.count(), 0);
    }

    void inactiveNavigationDeliveredOnce_data()
    {
        QTest::addColumn<bool>("renderTarget");
        QTest::newRow("owner") << false;
        QTest::newRow("render") << true;
    }

    void inactiveNavigationDeliveredOnce()
    {
        QFETCH(bool, renderTarget);
        KeyOwner owner;
        QWidget render(&owner);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        RedispatchFence fence;
        owner.installEventFilter(&fence);
        SendKey(renderTarget ? &render : &owner, QEvent::KeyPress, Qt::Key_Left);
        QCOMPARE(owner.deliveries, 1);
        QCOMPARE(fence.deliveries, 1);
    }

    void activeDrawConsumesItsOwnShortcut()
    {
        KeyOwner owner;
        QWidget render(&owner);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        overlay.SetActive(true);
        QSignalSpy tool(&overlay, &okuflow::AnnotationOverlay::ToolChanged);
        QKeyEvent event(QEvent::KeyPress, Qt::Key_L, Qt::NoModifier);
        QCoreApplication::sendEvent(&owner, &event);
        QCOMPARE(tool.count(), 1);
        QCOMPARE(owner.deliveries, 0);
    }

    void hiddenDrawLeavesOwnerKeysAlone()
    {
        KeyOwner owner;
        QWidget render(&owner);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        overlay.SetActive(true);
        overlay.hide();
        QSignalSpy exit(&overlay, &okuflow::AnnotationOverlay::ExitRequested);
        QSignalSpy tool(&overlay, &okuflow::AnnotationOverlay::ToolChanged);
        for (int key : {Qt::Key_L, Qt::Key_Escape, Qt::Key_Z}) {
            QKeyEvent event(QEvent::KeyPress, key,
                            key == Qt::Key_Z ? Qt::ControlModifier : Qt::NoModifier);
            QCoreApplication::sendEvent(&owner, &event);
        }
        QCOMPARE(owner.deliveries, 3);
        QCOMPARE(exit.count(), 0);
        QCOMPARE(tool.count(), 0);
    }

    void sliderConsumesItsArrow()
    {
        KeyOwner owner;
        QWidget render(&owner);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        overlay.SetActive(true);
        const auto sliders = overlay.findChildren<QSlider*>();
        QVERIFY(!sliders.empty());
        QSlider* slider = sliders.front();
        const int before = slider->value();
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
        QCoreApplication::sendEvent(slider, &event);
        QCOMPARE(slider->value(), before + slider->singleStep());
        QCOMPARE(owner.deliveries, 0);
        QVERIFY(event.isAccepted());
    }

    void releaseStopsForwardedPan_data()
    {
        QTest::addColumn<int>("transition");
        QTest::newRow("physical-release") << 0;
        QTest::newRow("hide") << 1;
        QTest::newRow("draw-off") << 2;
        QTest::newRow("focus-out") << 3;
        QTest::newRow("window-deactivate") << 4;
    }

    void releaseStopsForwardedPan()
    {
        QFETCH(int, transition);
        KeyOwner owner;
        QWidget render(&owner);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        overlay.SetActive(true);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
        QCoreApplication::sendEvent(&overlay, &press);
        QVERIFY(owner.panHeld);
        if (transition == 0) SendKey(&overlay, QEvent::KeyRelease, Qt::Key_Left);
        if (transition == 1) overlay.hide();
        if (transition == 2) overlay.SetActive(false);
        if (transition == 3) {
            QFocusEvent lost(QEvent::FocusOut);
            QCoreApplication::sendEvent(&overlay, &lost);
        }
        if (transition == 4) {
            QEvent lost(QEvent::WindowDeactivate);
            QCoreApplication::sendEvent(&overlay, &lost);
        }
        QCOMPARE(owner.deliveries, 1);
        QCOMPARE(owner.releases, 1);
        QVERIFY(!owner.panHeld);
        // A later physical release, hide, or deactivation must not retire it twice.
        SendKey(&overlay, QEvent::KeyRelease, Qt::Key_Left);
        overlay.SetActive(false);
        QCOMPARE(owner.releases, 1);
    }

    void repeatsDoNotEndPan()
    {
        KeyOwner owner;
        QWidget render(&owner);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        overlay.SetActive(true);
        SendKey(&overlay, QEvent::KeyPress, Qt::Key_Left);
        SendKey(&overlay, QEvent::KeyRelease, Qt::Key_Left, Qt::NoModifier, true);
        QCOMPARE(owner.releases, 0);
        QVERIFY(owner.panHeld);
        SendKey(&overlay, QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier, true);
        QCOMPARE(owner.deliveries, 2);
        QVERIFY(owner.lastAutoRepeat);
        SendKey(&overlay, QEvent::KeyRelease, Qt::Key_Left);
        QCOMPARE(owner.releases, 1);
        QVERIFY(!owner.panHeld);
    }

    void inlineEditorOwnsNavigationAndUndo()
    {
        KeyOwner owner;
        QWidget render(&owner);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        ConfigureCanvas(render, overlay);
        QVERIFY(ClickTool(overlay, QStringLiteral("Line")));
        DrawLine(overlay);
        QCOMPARE(overlay.Strokes().size(), 1);
        const auto before = overlay.Strokes();
        QVERIFY(ClickTool(overlay, QStringLiteral("Text")));
        SendMouse(&overlay, QEvent::MouseButtonPress, {600, 400}, Qt::LeftButton, Qt::LeftButton);
        SendMouse(&overlay, QEvent::MouseButtonRelease, {600, 400}, Qt::LeftButton, Qt::NoButton);
        auto* editor = overlay.findChild<QLineEdit*>(QStringLiteral("annotationInlineTextEditor"));
        QVERIFY(editor && editor->isVisible());
        for (int key : {Qt::Key_Up, Qt::Key_Down, Qt::Key_PageUp, Qt::Key_PageDown}) {
            SendKey(editor, QEvent::KeyPress, key);
            SendKey(editor, QEvent::KeyRelease, key);
        }
        QTest::keyClicks(editor, "label");
        QVERIFY(editor->isUndoAvailable());
        SendKey(editor, QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
        QVERIFY(editor->text().isEmpty());
        QCOMPARE(overlay.Strokes(), before);
        QCOMPARE(owner.deliveries, 0);
        QCOMPARE(owner.releases, 0);
        SendKey(editor, QEvent::KeyPress, Qt::Key_Escape);
        QVERIFY(!editor->isVisible());
        QCOMPARE(overlay.Strokes(), before);
    }

    void inlineEditorFocusLoss_data()
    {
        QTest::addColumn<QString>("text");
        QTest::newRow("untouched-empty") << QString();
        QTest::newRow("whitespace") << QStringLiteral("   ");
        QTest::newRow("typed-label") << QStringLiteral("label");
    }

    void inlineEditorFocusLoss()
    {
        QFETCH(QString, text);
        KeyOwner owner;
        owner.setFocusPolicy(Qt::StrongFocus);
        owner.resize(900, 600);
        QWidget render(&owner);
        owner.show();
        render.show();
        okuflow::AnnotationOverlay overlay(&render, &owner);
        ConfigureCanvas(render, overlay);
        QApplication::setActiveWindow(&overlay);
        QVERIFY(ClickTool(overlay, QStringLiteral("Text")));
        SendMouse(&overlay, QEvent::MouseButtonPress, {600, 400}, Qt::LeftButton, Qt::LeftButton);
        SendMouse(&overlay, QEvent::MouseButtonRelease, {600, 400}, Qt::LeftButton, Qt::NoButton);
        auto* editor = overlay.findChild<QLineEdit*>(QStringLiteral("annotationInlineTextEditor"));
        QVERIFY(editor && editor->isVisible());
        editor->setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(editor->hasFocus());
        if (!text.isEmpty()) QTest::keyClicks(editor, text);
        QSignalSpy editingFinished(editor, &QLineEdit::editingFinished);

        // Move actual Qt focus; a synthetic FocusOut leaves hasFocus() true
        // and cannot verify the production editingFinished guard.
        QApplication::setActiveWindow(&owner);
        owner.setFocus(Qt::OtherFocusReason);
        QTRY_VERIFY(owner.hasFocus());
        QVERIFY(!editor->hasFocus());
        QVERIFY(!editor->isVisible());
        if (text.trimmed().isEmpty()) {
            QVERIFY(overlay.Strokes().empty());
            SendKey(&owner, QEvent::KeyPress, Qt::Key_Up);
            QCOMPARE(owner.deliveries, 1);
            if (text.isEmpty()) QCOMPARE(editingFinished.count(), 0);
        } else {
            QCOMPARE(overlay.Strokes().size(), 1);
            QCOMPARE(overlay.Strokes().front().text, text);
        }
        const auto strokes = overlay.Strokes();
        owner.clearFocus();
        owner.setFocus();
        QCOMPARE(overlay.Strokes(), strokes);
    }

    void selectedInkOwnsNavigation_data()
    {
        QTest::addColumn<bool>("ownerTarget");
        QTest::newRow("owner") << true;
        QTest::newRow("overlay") << false;
    }

    void selectedInkOwnsNavigation()
    {
        QFETCH(bool, ownerTarget);
        KeyOwner owner;
        QWidget render(&owner);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        ConfigureCanvas(render, overlay);
        QVERIFY(ClickTool(overlay, QStringLiteral("Line")));
        DrawLine(overlay);
        QVERIFY(ClickTool(overlay, QStringLiteral("Move")));
        SendMouse(&overlay, QEvent::MouseButtonPress, {600, 300}, Qt::LeftButton, Qt::LeftButton);
        SendMouse(&overlay, QEvent::MouseButtonRelease, {600, 300}, Qt::LeftButton, Qt::NoButton);
        QCOMPARE(overlay.Strokes().size(), 1);
        const auto before = overlay.Strokes();
        auto* receiver = ownerTarget ? static_cast<QWidget*>(&owner) : &overlay;
        SendKey(receiver, QEvent::KeyPress, Qt::Key_Right);
        const auto moved = overlay.Strokes();
        QVERIFY(moved != before);
        QCOMPARE(moved.front().points.front().y(), before.front().points.front().y());
        QVERIFY(overlay.Undo());
        QCOMPARE(overlay.Strokes(), before);
        QVERIFY(overlay.Redo());
        QCOMPARE(overlay.Strokes(), moved);
        SendKey(receiver, QEvent::KeyPress, Qt::Key_Equal, Qt::ControlModifier);
        QCOMPARE(overlay.Strokes().front().sceneWidth, moved.front().sceneWidth * 1.05);
        QVERIFY(overlay.Undo());
        QCOMPARE(overlay.Strokes(), moved);
        QCOMPARE(owner.deliveries, 0);
    }

    void ignoredOwnerIsForwardedOnceWithoutFence()
    {
        KeyOwner owner;
        owner.acceptKeys = false;
        QWidget render(&owner);
        okuflow::AnnotationOverlay overlay(&render, &owner);
        overlay.SetActive(true);
        SendKey(&overlay, QEvent::KeyPress, Qt::Key_Left);
        QCOMPARE(owner.deliveries, 1);
        overlay.hide();
        QCOMPARE(owner.releases, 0);
    }

    void nullOwnerAndDestroyedRenderAreSafe()
    {
        QWidget render;
        render.resize(900, 600);
        okuflow::AnnotationOverlay unowned(&render, nullptr);
        unowned.SetActive(true);
        SendKey(&unowned, QEvent::KeyPress, Qt::Key_Left);
        SendKey(&unowned, QEvent::KeyRelease, Qt::Key_Left);
        unowned.hide();
        KeyOwner owner;
        auto* target = new QWidget(&owner);
        okuflow::AnnotationOverlay overlay(target, &owner);
        overlay.SetActive(true);
        delete target;
        SendKey(&owner, QEvent::KeyPress, Qt::Key_Left);
        QCOMPARE(owner.deliveries, 1);
    }
};

} // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    AnnotationKeyRoutingTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "annotation_key_routing_tests.moc"
