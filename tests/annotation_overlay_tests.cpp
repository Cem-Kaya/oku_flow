#include "gui_test_support.hpp"
#include "okuflow/common/view_transform.hpp"
#include "okuflow/common/visible_chat_session.hpp"
#include "okuflow/app/assistive_feature_manager.hpp"
#include "okuflow/ui/annotation_overlay.hpp"
#include "okuflow/ui/assistive_overlay.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QMenu>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QJsonArray>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QFrame>
#include <QToolButton>
#include <QSplitter>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTranslator>
#include <QScopeGuard>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtTest>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <cmath>
#include <algorithm>

namespace okuflow {
namespace {

// Explicit user admission shows chat; later snapshots only update its view.
void ShowFixtureChat(AssistiveOverlay& chat,const QString& title,const QString& body,bool visible)
{
    chat.SetChatSnapshot({{"conversationId","native-fixture"},{"kind","explain"},{"title",title},{"draft",chat.QuestionDraft()},
        {"messages",QJsonArray{QJsonObject{{"id","answer"},{"role","assistant"},{"text",body},{"status","completed"},{"complete",true}}}}});
    chat.ShowChat(visible);
}

// The compact header contains actionable buttons. Drag its bare layout margin,
// matching a native pointer press outside those controls.
QPoint HeaderDragPoint(QWidget* header)
{
    return QPoint(6, header->height() / 2);
}

class PlacementTestTranslator final : public QTranslator {
public:
    QString prefix{QStringLiteral("First: ")};
    bool isEmpty() const override { return false; }
    QString translate(const char* context, const char* source,
                      const char*, int) const override
    {
        const QString text = QString::fromUtf8(source);
        if (QString::fromUtf8(context) == QStringLiteral("OkuFlow") &&
            (text == QStringLiteral("Floating") || text == QStringLiteral("Dock left") ||
             text == QStringLiteral("Dock right"))) {
            return prefix + text;
        }
        return {};
    }
};

class MouseSink final : public QWidget {
public:
    explicit MouseSink(QWidget* parent = nullptr)
        : QWidget(parent)
    {
    }

    int middlePresses{};
    int middleMoves{};
    int middleReleases{};

protected:
    bool event(QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::MiddleButton) {
                ++middlePresses;
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseMove: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->buttons() & Qt::MiddleButton) {
                ++middleMoves;
                event->accept();
                return true;
            }
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::MiddleButton) {
                ++middleReleases;
                event->accept();
                return true;
            }
            break;
        }
        default:
            break;
        }
        return QWidget::event(event);
    }
};

// Exercise our native move notifications without entering Windows' modal
// mouse loop or moving the user's real pointer during automated tests.
class SimulatedNativeMoveChat final : public AssistiveOverlay {
public:
    using AssistiveOverlay::AssistiveOverlay;
    int moveStarts{};

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::UngrabMouse) {
            qInfo() << "Synthetic drag cancellation event" << event->type()
                    << "grabber" << (mouseGrabber() == this)
                    << "owner active" << window()->isActiveWindow();
        }
        if (event->spontaneous() && event->type() == QEvent::MouseMove) {
            // grabMouse also routes the user's physical pointer here. These
            // tests own a synthetic pointer and must not let unrelated native
            // moves cancel or re-arm its held-pull latch. SendMouse events are
            // non-spontaneous and still exercise the production handlers.
            return true;
        }
        return AssistiveOverlay::event(event);
    }

    bool nativeEvent(const QByteArray& type, void* message, qintptr* result) override
    {
        const auto* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_SYSCOMMAND && (msg->wParam & 0xfff0) == SC_MOVE) {
            ++moveStarts;
            *result = 0;
            return true;
        }
        return AssistiveOverlay::nativeEvent(type, message, result);
    }
};

class VisibilityCounter final : public QObject {
public:
    int shows{};
    int hides{};
protected:
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::Show) ++shows;
        if (event->type() == QEvent::Hide) ++hides;
        return false;
    }
};

QToolButton* ToolButton(AnnotationOverlay& overlay, const QString& text)
{
    const auto buttons = overlay.findChildren<QToolButton*>();
    for (QToolButton* button : buttons) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

void SendMouse(QWidget& target,
               QEvent::Type type,
               const QPointF& local,
               Qt::MouseButton button,
               Qt::MouseButtons buttons,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF global = target.mapToGlobal(local.toPoint());
    QMouseEvent event(type,
                      local,
                      local,
                      global,
                      button,
                      buttons,
                      modifiers);
    QCoreApplication::sendEvent(&target, &event);
}

} // namespace

class AnnotationOverlayTests final : public QObject {
    Q_OBJECT

private slots:
    void middleDragCrossesTheToolWindowBoundary();
    void shortViewportKeepsDrawRailsReachable();
    void controlledToggleRetainsInkAndSelectedTool();
    void textPlacementIsClickThenType();
    void moveToolMarqueeSelectsAndMovesMultipleAnnotations();
    void persistentActionPanelUsesNativeScreenCoordinates_data();
    void persistentActionPanelUsesNativeScreenCoordinates();
    void toolChromeMirrorsForRightToLeftLayouts();
    void floatingChatRemainsInteractiveAndExcludesInkAfterMoving();
    void floatingChatDefaultsTallAndKeepsRestoredSize();
    void suppressedChatRetainsStreamingContentAndDock();
    void assistantReadingActionsAndSafeDefault();
    void assistiveModesCannotBypassUiSuppression();
    void clearingFocusWarningPreservesAssistantResults();
    void assistantPlacementActionsRetranslate();
    void dockingChatResizesViewportAndRestoresFloatingPlacement();
    void fullyCoveredCanvasDoesNotClearItsNativeMask();
    void floatingDragPreviewsAndDocksOnRelease_data();
    void floatingDragPreviewsAndDocksOnRelease();
    void leavingOrCancelingDockPreviewKeepsChatFloating_data();
    void leavingOrCancelingDockPreviewKeepsChatFloating();
    void edgeJitterKeepsOnePreviewAndOneDrop_data();
    void edgeJitterKeepsOnePreviewAndOneDrop();
    void briefEdgeEntryDoesNotLatchOrDrop_data();
    void briefEdgeEntryDoesNotLatchOrDrop();
    void dockedHeaderRequiresHeldPullAndDoesNotImmediatelyRedock_data();
    void dockedHeaderRequiresHeldPullAndDoesNotImmediatelyRedock();
    void releasingOrCancelingThePullKeepsTheDockLatched_data();
    void releasingOrCancelingThePullKeepsTheDockLatched();
};

void AnnotationOverlayTests::controlledToggleRetainsInkAndSelectedTool()
{
    QMainWindow owner;
    auto* render=new MouseSink;
    owner.setCentralWidget(render);
    owner.resize(1000,700);owner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));
    AnnotationOverlay overlay(render,&owner);
    const auto transform=ComputeViewTransform(1000,700,1000,700,1.0f,0.5f,0.5f,ViewportFitMode::kFill);
    overlay.SetViewTransform(transform);overlay.SetActive(true);
    QCoreApplication::processEvents();
    SendMouse(overlay,QEvent::MouseButtonPress,QPointF(400,300),Qt::LeftButton,Qt::LeftButton);
    SendMouse(overlay,QEvent::MouseMove,QPointF(500,400),Qt::NoButton,Qt::LeftButton);
    SendMouse(overlay,QEvent::MouseButtonRelease,QPointF(500,400),Qt::LeftButton,Qt::NoButton);
    QVERIFY(overlay.HasInk());const auto count=overlay.Strokes().size();
    render->setFocusPolicy(Qt::StrongFocus);
    QVERIFY2(test::ActivateFixtureWindow(&owner), test::ActivationFailureMessage());
    render->setFocus();QTRY_VERIFY(render->hasFocus());
    auto* focus = QApplication::focusWidget();
    overlay.SetTool(AnnotationTool::kPointer, true);
    QCOMPARE(QApplication::focusWidget(), focus);
    overlay.SetActive(false,overlay.SelectedTool(),true,true);
    QCOMPARE(QApplication::focusWidget(), focus);
    QVERIFY(!overlay.IsActive());QCOMPARE(overlay.Strokes().size(),count);
    overlay.SetActive(true,overlay.SelectedTool(),true,true);
    QCoreApplication::processEvents();
    QCOMPARE(QApplication::focusWidget(), focus);
    overlay.SetTool(AnnotationTool::kEraser, true);
    QCOMPARE(QApplication::focusWidget(), focus);
    overlay.SetTool(AnnotationTool::kPen, true);
    QCOMPARE(QApplication::focusWidget(), focus);
    overlay.SetTool(AnnotationTool::kPointer, true);
    QCOMPARE(QApplication::focusWidget(), focus);
    QCOMPARE(overlay.SelectedTool(),AnnotationTool::kPointer);QCOMPARE(overlay.Strokes().size(),count);
    QVERIFY(overlay.Undo());QVERIFY(!overlay.HasInk());
    QVERIFY(overlay.Redo());QCOMPARE(overlay.Strokes().size(),count);
    overlay.SetActive(false); // Ordinary manual exit keeps its original clearing contract.
    QVERIFY(!overlay.HasInk());
}

void AnnotationOverlayTests::shortViewportKeepsDrawRailsReachable()
{
    QMainWindow owner;
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    owner.resize(800, 320);
    owner.show();
    QVERIFY(QTest::qWaitForWindowExposed(&owner));
    AnnotationOverlay overlay(render, &owner);
    overlay.SetActive(true);
    QCoreApplication::processEvents();
    for (const char* name : {"annotationToolbar", "annotationActions"}) {
        auto* rail = overlay.findChild<QFrame*>(QString::fromLatin1(name));
        QVERIFY(rail);
        QVERIFY(overlay.rect().contains(rail->geometry()));
        auto* scroll = rail->findChild<QScrollArea*>();
        QVERIFY(scroll);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        const auto buttons = scroll->widget()->findChildren<QToolButton*>();
        QVERIFY(!buttons.isEmpty());
        scroll->ensureWidgetVisible(buttons.last());
        QCoreApplication::processEvents();
        const QRect lastButton(buttons.last()->mapTo(scroll->viewport(), QPoint()), buttons.last()->size());
        QVERIFY(scroll->viewport()->rect().contains(lastButton));
    }
}

void AnnotationOverlayTests::floatingChatDefaultsTallAndKeepsRestoredSize()
{
    QMainWindow owner;
    owner.resize(1400, 900);
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    owner.show();
    AssistiveOverlay chat(render);
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("A long answer"), true);
    QTRY_VERIFY(chat.isVisible());
    QVERIFY(chat.isFloating());
    QVERIFY(chat.height() >= render->height() * 7 / 10);
    QVERIFY(chat.height() <= render->height() * 8 / 10);

    const QRect custom(180, 140, std::max(520,chat.minimumSizeHint().width()), std::max(380,chat.minimumSizeHint().height()));
    chat.RestoreRelativeGeometry(custom);
    QCoreApplication::processEvents();
    QCOMPARE(chat.RelativeGeometry().size(), custom.size());
    chat.SetUiSuppressed(true);
    QVERIFY(!chat.isVisible());
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("A longer streamed answer"), true);
    QVERIFY(!chat.isVisible());
    auto* body = chat.chatView()->transcript();
    QVERIFY(body);
    QVERIFY(body->toPlainText().contains(QStringLiteral("A longer streamed answer")));
    chat.SetUiSuppressed(false);
    QTRY_VERIFY(chat.isVisible());
    QCOMPARE(chat.RelativeGeometry().size(), custom.size());
}

void AnnotationOverlayTests::suppressedChatRetainsStreamingContentAndDock()
{
    QMainWindow owner;
    owner.resize(1400, 900);
    auto* render = new MouseSink;
    render->setMinimumSize(320, 240);
    owner.setCentralWidget(render);
    owner.show();
    AssistiveOverlay chat(render);
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("First part"), true);
    chat.SetDockPosition(QStringLiteral("right"));
    QTRY_VERIFY(chat.isVisible());
    QCOMPARE(chat.DockPosition(), QStringLiteral("right"));
    const int dockWidth = chat.width();
    const int dockedRenderWidth = render->width();
    chat.SetUiSuppressed(true);
    QTRY_VERIFY(!chat.isVisible());
    QTRY_VERIFY(render->width() > dockedRenderWidth);
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("First part, completed"), true);
    QVERIFY(!chat.isVisible());
    chat.SetUiSuppressed(false);
    QTRY_VERIFY(chat.isVisible());
    QCOMPARE(chat.DockPosition(), QStringLiteral("right"));
    QTRY_COMPARE(chat.width(), dockWidth);
    auto* body = chat.chatView()->transcript();
    QVERIFY(body);
    QVERIFY(body->toPlainText().contains(QStringLiteral("First part, completed")));

    chat.SetUiSuppressed(true);
    ShowFixtureChat(chat,QString(), QString(), false);
    chat.SetUiSuppressed(false);
    QVERIFY(!chat.isVisible());
}

void AnnotationOverlayTests::assistantReadingActionsAndSafeDefault()
{
    QMainWindow owner;
    owner.resize(1024, 600);
    auto* render = new MouseSink;
    render->setFocusPolicy(Qt::StrongFocus);
    owner.setCentralWidget(render);
    owner.show();
    AssistiveOverlay chat(render);
    const QRect safe(16, 64, render->width() - 32, render->height() - 96);
    chat.SetSafeArea(safe);
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("Scene Explain\nA readable answer"), true);
    QTRY_VERIFY(chat.isVisible());
    QVERIFY(safe.contains(chat.RelativeGeometry()));
    auto* body = chat.chatView()->transcript();
    auto* read = chat.chatView()->readAloudButton();
    auto* placement = chat.findChild<QToolButton*>(QStringLiteral("assistiveDockPosition"));
    auto* question = chat.chatView()->composer();
    QVERIFY(body && read && placement && question);
    QVERIFY(body->font().pixelSize() >= 18);
    QVERIFY(read->height() >= 44);
    const auto targets=chat.FocusTargets();
    QCOMPARE(targets.front(), body);
    const auto composer=std::find(targets.begin(),targets.end(),question);
    QVERIFY(composer!=targets.end());
    QVERIFY(std::distance(composer,targets.end())>=4);
    QCOMPARE(*(composer+1),read);
    QCOMPARE(*(composer+2),chat.chatView()->attachmentCheckbox());
    QCOMPARE(*(composer+3),chat.chatView()->sendButton());
    QVERIFY(chat.findChildren<QComboBox*>().isEmpty());
    QCOMPARE(placement->menu()->actions().size(), 3);
    QSignalSpy speech(chat.chatView(), &AssistantChatView::ChatReadAloudRequested);
    read->click();
    QCOMPARE(speech.size(), 1);
    QCOMPARE(speech.first().at(1).toString(), QStringLiteral("Scene Explain\nA readable answer"));
    QSignalSpy dismissed(&chat, &AssistiveOverlay::Dismissed);
    QVERIFY2(test::ActivateFixtureWindow(&chat), test::ActivationFailureMessage());
    question->setFocus();
    QTRY_VERIFY(question->hasFocus());
    question->setPlainText(QStringLiteral("Draft"));
    QTest::keyClick(question, Qt::Key_Escape);
    QTRY_VERIFY(question->toPlainText().isEmpty());
    QVERIFY(chat.isVisible());
    QVERIFY(dismissed.isEmpty());
    QTest::keyClick(question, Qt::Key_Escape);
    QTRY_VERIFY(render->hasFocus());
    QVERIFY(chat.isVisible());
    QVERIFY(dismissed.isEmpty());
    // A saved size must accommodate the platform font's layout minimum;
    // offscreen fallback fonts differ from native Windows font metrics.
    const QRect saved(120, 20, std::max(520, chat.minimumSizeHint().width()), std::max(380,chat.minimumSizeHint().height()));
    chat.RestoreRelativeGeometry(saved);
    chat.SetSafeArea(QRect(30, 150, 700, 300));
    QCOMPARE(chat.RelativeGeometry(), saved);

    AssistiveOverlay rtl(render);
    rtl.setLayoutDirection(Qt::RightToLeft);
    rtl.SetSafeArea(safe);
    ShowFixtureChat(rtl,QStringLiteral("Assistant"), QStringLiteral("Answer"), true);
    QTRY_VERIFY(rtl.isVisible());
    QCOMPARE(rtl.RelativeGeometry().right(), safe.right());
    QVERIFY(safe.contains(rtl.RelativeGeometry()));
    const QRect narrowSafe(40, 120, rtl.minimumSizeHint().width() + 20,
                           std::max(300, rtl.minimumSizeHint().height() + 20));
    QVERIFY(QRect(QPoint(), render->size()).contains(narrowSafe));
    rtl.SetSafeArea(narrowSafe);
    QCOMPARE(rtl.RelativeGeometry().right(), narrowSafe.right());
    QVERIFY(narrowSafe.contains(rtl.RelativeGeometry()));
}

void AnnotationOverlayTests::clearingFocusWarningPreservesAssistantResults()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QMainWindow owner;
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    const UserDataPaths paths(temporary.path());
    AssistiveFeatureManager manager(*render, owner, {}, {}, paths);
    auto* body = manager.Overlay().ReadBody();
    QVERIFY(body);
    manager.SetModes(false, false); // No periodic analysis can replace a lingering warning.
    manager.ShowFocusWarning();
    QVERIFY(body->toPlainText().contains(QStringLiteral("out of focus")));
    manager.ClearFocusWarning();
    QVERIFY(body->toPlainText().isEmpty());
    QVERIFY(!manager.Overlay().isVisible());
    // A later assistant update relinquishes focus-warning ownership.
    manager.ShowFocusWarning();
    manager.Runtime().OverlayUpdated(QStringLiteral("Assistant"), QStringLiteral("Retain this answer"), true);
    manager.ClearFocusWarning();
    QCOMPARE(body->toPlainText(), QStringLiteral("Retain this answer"));
}

void AnnotationOverlayTests::assistiveModesCannotBypassUiSuppression()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QMainWindow owner;
    owner.resize(1000, 700);
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    owner.show();
    const UserDataPaths paths(temporary.path());
    AssistiveFeatureManager manager(*render, owner, {}, {}, paths);
    auto& chat = manager.Overlay();
    QSignalSpy dismissed(&chat, &AssistiveOverlay::Dismissed);
    chat.SetUiSuppressed(true);
    auto& session = manager.Runtime().ChatSession();
    connect(&session, &VisibleChatSession::SnapshotChanged, &chat, &AssistiveOverlay::SetChatSnapshot);
    const QString answer = QStringLiteral("Retained completed Explain answer");
    session.Restore(QStringLiteral("suppressed-explain"), ChatBackendKind::Explain,
                    QJsonArray{QJsonObject{{"role", "assistant"}, {"text", answer}}});
    chat.ShowChat(true); // Explicit user admission, still suppressed.
    manager.SetModes(true, true);
    // A synthetic periodic/Read presentation exercises visibility without
    // asking a provider or capturing a frame. Idle modes need not invent text.
    manager.Runtime().OverlayUpdated(QStringLiteral("Read"), QStringLiteral("Retained reading result"), true);
    QVERIFY(!chat.isVisible());
    QVERIFY(!chat.ReadResultPane()->isVisible());
    auto* body = chat.ReadBody();
    QVERIFY(body);
    QVERIFY(!body->toPlainText().isEmpty());
    const QString chatText = chat.chatView()->transcript()->toPlainText();
    QVERIFY(chatText.contains(answer));
    chat.SetUiSuppressed(false);
    QTRY_VERIFY(chat.ReadResultPane()->isVisible());
    QTRY_VERIFY(chat.isVisible());
    chat.SetUiSuppressed(true);
    chat.ShowChat(false); // A closed chat must not be reopened by mode changes.
    manager.SetModes(false, true);
    chat.SetUiSuppressed(false);
    QVERIFY(!chat.isVisible());
    QVERIFY(!chat.ReadResultPane()->isVisible());
    QCOMPARE(chat.chatView()->transcript()->toPlainText(), chatText);
    chat.SetUiSuppressed(true);
    manager.SetModes(true, false);
    chat.SetUiSuppressed(false);
    QVERIFY(!chat.isVisible());
    QVERIFY(!chat.ReadResultPane()->isVisible());
    QCOMPARE(chat.chatView()->transcript()->toPlainText(), chatText);
    QVERIFY(dismissed.isEmpty());
}

void AnnotationOverlayTests::assistantPlacementActionsRetranslate()
{
    PlacementTestTranslator translator;
    QVERIFY(QCoreApplication::installTranslator(&translator));
    const auto removeTranslator = qScopeGuard([&translator]() {
        QCoreApplication::removeTranslator(&translator);
    });
    QMainWindow owner;
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    AssistiveOverlay chat(render);
    auto* placement = chat.findChild<QToolButton*>(QStringLiteral("assistiveDockPosition"));
    QVERIFY(placement && placement->menu());
    const auto actions = placement->menu()->actions();
    QCOMPARE(actions.size(), 3);
    QCOMPARE(actions[0]->text(), QStringLiteral("First: Floating"));
    QCOMPARE(actions[1]->text(), QStringLiteral("First: Dock left"));
    QCOMPARE(actions[2]->text(), QStringLiteral("First: Dock right"));
    translator.prefix = QStringLiteral("Second: ");
    // Exercise menu-open refresh without opening a native popup.
    QVERIFY(QMetaObject::invokeMethod(placement->menu(), "aboutToShow", Qt::DirectConnection));
    QCOMPARE(actions[1]->text(), QStringLiteral("Second: Dock left"));
    translator.prefix = QStringLiteral("Third: ");
    QEvent change(QEvent::LanguageChange);
    QCoreApplication::sendEvent(&chat, &change);
    QCOMPARE(actions[2]->text(), QStringLiteral("Third: Dock right"));
    QCOMPARE(actions[1]->data().toString(), QStringLiteral("left"));
    QVERIFY(!chat.isVisible());
}

void AnnotationOverlayTests::edgeJitterKeepsOnePreviewAndOneDrop_data()
{
    QTest::addColumn<QString>("side");
    QTest::newRow("left") << QStringLiteral("left");
    QTest::newRow("right") << QStringLiteral("right");
}

void AnnotationOverlayTests::edgeJitterKeepsOnePreviewAndOneDrop()
{
    QFETCH(QString, side);
    QMainWindow owner;
    owner.resize(1400, 900);
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    owner.show();
    SimulatedNativeMoveChat chat(render);
    chat.RestoreRelativeGeometry(QRect(180, 140, 520, 360));
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("Answer"), true);
    QCoreApplication::processEvents();
    auto* header = chat.chatView()->headerWidget();
    auto* preview = chat.findChild<QLabel*>(QStringLiteral("assistiveDockPreview"));
    QVERIFY(header && header->isVisible());
    QVERIFY(!header->childAt(HeaderDragPoint(header)));
    QVERIFY(preview);
    VisibilityCounter visibility;
    preview->installEventFilter(&visibility);
    QSignalSpy floatingChanges(&chat, &QDockWidget::topLevelChanged);
    SendMouse(*header, QEvent::MouseButtonPress, HeaderDragPoint(header),
              Qt::LeftButton, Qt::LeftButton);
    QTRY_COMPARE(chat.moveStarts, 1);
    const auto moveFromEdge = [&](int distance) {
        chat.move(owner.mapToGlobal(QPoint(side == QStringLiteral("left")
                                               ? distance : owner.width() - chat.width() - distance, 140)));
        QCoreApplication::processEvents();
    };
    // Unstable entry never acquires the latch, even across several timer periods.
    for (int distance : {35, 40, 34, 40, 36, 48}) {
        moveFromEdge(distance);
        QTest::qWait(60);
        QVERIFY(!preview->isVisible());
    }
    moveFromEdge(35);
    QTRY_VERIFY_WITH_TIMEOUT(preview->isVisible(), 1000);
    for (int distance : {37, 34, 40, 36, 48, 90, 95}) {
        moveFromEdge(distance);
        QVERIFY(preview->isVisible());
        QVERIFY(chat.isFloating());
    }
    QCOMPARE(visibility.shows, 1);
    QCOMPARE(visibility.hides, 0);
    QCOMPARE(floatingChanges.size(), 0);
    // Brief excursions outside the wider release zone preserve the latch.
    for (int distance : {110, 80, 120, 90}) {
        moveFromEdge(distance);
        QTest::qWait(80);
        QVERIFY(preview->isVisible());
    }
    QCOMPARE(visibility.shows, 1);
    QCOMPARE(visibility.hides, 0);
    moveFromEdge(110);
    QTRY_VERIFY_WITH_TIMEOUT(!preview->isVisible(), 1000);
    moveFromEdge(40);
    QVERIFY(!preview->isVisible());
    moveFromEdge(35);
    QTRY_VERIFY_WITH_TIMEOUT(preview->isVisible(), 1000);
    const HWND hwnd = reinterpret_cast<HWND>(chat.winId());
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    SendMessageW(hwnd, WM_EXITSIZEMOVE, 0, 0);
    QTRY_COMPARE(chat.DockPosition(), side);
    QCOMPARE(floatingChanges.size(), 1);
    QTest::qWait(80);
    QCOMPARE(chat.DockPosition(), side);
    QCOMPARE(floatingChanges.size(), 1);
}

void AnnotationOverlayTests::briefEdgeEntryDoesNotLatchOrDrop_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("release-before-latch") << 0;
    QTest::newRow("leave-before-latch") << 1;
    QTest::newRow("hide-before-latch") << 2;
}

void AnnotationOverlayTests::briefEdgeEntryDoesNotLatchOrDrop()
{
    QFETCH(int, action);
    QMainWindow owner;
    owner.resize(1400, 900);
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    owner.show();
    SimulatedNativeMoveChat chat(render);
    chat.RestoreRelativeGeometry(QRect(180, 140, 520, 360));
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("Answer"), true);
    QCoreApplication::processEvents();
    auto* header = chat.chatView()->headerWidget();
    auto* preview = chat.findChild<QLabel*>(QStringLiteral("assistiveDockPreview"));
    QVERIFY(header && header->isVisible());
    QVERIFY(!header->childAt(HeaderDragPoint(header)));
    QVERIFY(preview);
    QSignalSpy floatingChanges(&chat, &QDockWidget::topLevelChanged);
    SendMouse(*header, QEvent::MouseButtonPress, HeaderDragPoint(header),
              Qt::LeftButton, Qt::LeftButton);
    QTRY_COMPARE(chat.moveStarts, 1);
    chat.move(owner.mapToGlobal(QPoint(0, 140)));
    QTest::qWait(60);
    QVERIFY(!preview->isVisible());
    if (action == 1) {
        chat.move(owner.mapToGlobal(QPoint(200, 140)));
    } else if (action == 2) {
        chat.hide();
    } else {
        SendMessageW(reinterpret_cast<HWND>(chat.winId()), WM_EXITSIZEMOVE, 0, 0);
    }
    QTest::qWait(300);
    QVERIFY(!preview->isVisible());
    SendMessageW(reinterpret_cast<HWND>(chat.winId()), WM_EXITSIZEMOVE, 0, 0);
    QCoreApplication::processEvents();
    QVERIFY(chat.isFloating());
    QCOMPARE(floatingChanges.size(), 0);
}

void AnnotationOverlayTests::dockedHeaderRequiresHeldPullAndDoesNotImmediatelyRedock_data()
{
    QTest::addColumn<QString>("side");
    QTest::newRow("left") << QStringLiteral("left");
    QTest::newRow("right") << QStringLiteral("right");
}

void AnnotationOverlayTests::dockedHeaderRequiresHeldPullAndDoesNotImmediatelyRedock()
{
    QFETCH(QString, side);
    QMainWindow owner;
    owner.resize(1400, 900);
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    owner.show();
    SimulatedNativeMoveChat chat(render);
    chat.RestoreRelativeGeometry(QRect(180, 140, 520, 360));
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("Answer"), true);
    chat.SetDockPosition(side);
    QCoreApplication::processEvents();
    // Docking closes the floating native window and asynchronously activates
    // its owner. Settle that transition before the synthetic held press;
    // a real WindowDeactivate must still cancel an in-progress pull.
    QVERIFY2(test::ActivateFixtureWindow(&owner), test::ActivationFailureMessage());
    auto* header = chat.chatView()->headerWidget();
    auto* preview = chat.findChild<QLabel*>(QStringLiteral("assistiveDockPreview"));
    QVERIFY(header && header->isVisible());
    QVERIFY(!header->childAt(HeaderDragPoint(header)));
    QVERIFY(preview);
    const QPoint press = header->mapToGlobal(HeaderDragPoint(header));
    const QPoint pull = press + QPoint(side == QStringLiteral("left") ? 48 : -48, 0);
    QSignalSpy floatingChanges(&chat, &QDockWidget::topLevelChanged);
    SendMouse(*header, QEvent::MouseButtonPress, HeaderDragPoint(header),
              Qt::LeftButton, Qt::LeftButton);
    SendMouse(chat, QEvent::MouseMove, chat.mapFromGlobal(pull), Qt::NoButton, Qt::LeftButton);
    QTest::qWait(80);
    QVERIFY(!chat.isFloating());
    QCOMPARE(floatingChanges.size(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(chat.isFloating(), 1000);
    QTRY_COMPARE(chat.moveStarts, 1);
    QCOMPARE(floatingChanges.size(), 1);
    // Still near the released side: suppress immediate re-docking.
    chat.move(owner.mapToGlobal(QPoint(side == QStringLiteral("left")
                                           ? 0 : owner.width() - chat.width(), 140)));
    QTest::qWait(260);
    QVERIFY(!preview->isVisible());
    SendMessageW(reinterpret_cast<HWND>(chat.winId()), WM_EXITSIZEMOVE, 0, 0);
    QCoreApplication::processEvents();
    QVERIFY(chat.isFloating());
    QCOMPARE(floatingChanges.size(), 1);
}

void AnnotationOverlayTests::releasingOrCancelingThePullKeepsTheDockLatched_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("quick-release") << 0;
    QTest::newRow("return-to-start") << 1;
    QTest::newRow("escape") << 2;
    QTest::newRow("window-deactivate") << 3;
}

void AnnotationOverlayTests::releasingOrCancelingThePullKeepsTheDockLatched()
{
    QFETCH(int, action);
    QMainWindow owner;
    owner.resize(1400, 900);
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    owner.show();
    SimulatedNativeMoveChat chat(render);
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("Answer"), true);
    chat.SetDockPosition(QStringLiteral("left"));
    QCoreApplication::processEvents();
    QVERIFY2(test::ActivateFixtureWindow(&owner), test::ActivationFailureMessage());
    auto* header = chat.chatView()->headerWidget();
    QVERIFY(header && header->isVisible());
    QVERIFY(!header->childAt(HeaderDragPoint(header)));
    const QPoint press = header->mapToGlobal(HeaderDragPoint(header));
    SendMouse(*header, QEvent::MouseButtonPress, HeaderDragPoint(header),
              Qt::LeftButton, Qt::LeftButton);
    SendMouse(chat, QEvent::MouseMove, chat.mapFromGlobal(press + QPoint(48, 0)),
              Qt::NoButton, Qt::LeftButton);
    if (action == 0) {
        SendMouse(chat, QEvent::MouseButtonRelease, chat.mapFromGlobal(press + QPoint(48, 0)),
                  Qt::LeftButton, Qt::NoButton);
    } else if (action == 1) {
        SendMouse(chat, QEvent::MouseMove, chat.mapFromGlobal(press), Qt::NoButton, Qt::LeftButton);
    } else if (action == 2) {
        QTest::keyClick(&chat, Qt::Key_Escape);
    } else {
        QEvent deactivate(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&chat, &deactivate);
    }
    QTest::qWait(420);
    QCOMPARE(chat.DockPosition(), QStringLiteral("left"));
    QCOMPARE(chat.moveStarts, 0);
    QVERIFY(chat.isVisible());
}

void AnnotationOverlayTests::floatingDragPreviewsAndDocksOnRelease_data()
{
    QTest::addColumn<QString>("side");
    QTest::addColumn<bool>("grabAtEdge");
    QTest::newRow("left-window-edge") << QStringLiteral("left") << false;
    QTest::newRow("right-window-edge") << QStringLiteral("right") << false;
    QTest::newRow("left-grab-point") << QStringLiteral("left") << true;
    QTest::newRow("right-grab-point") << QStringLiteral("right") << true;
}

void AnnotationOverlayTests::floatingDragPreviewsAndDocksOnRelease()
{
    QFETCH(QString, side);
    QFETCH(bool, grabAtEdge);
    QMainWindow owner;
    owner.resize(1400, 900);
    auto* render = new MouseSink;
    render->setMinimumSize(320, 240);
    owner.setCentralWidget(render);
    owner.show();
    SimulatedNativeMoveChat chat(render);
    chat.RestoreRelativeGeometry(QRect(180, 140, 520, 360));
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("Answer"), true);
    AnnotationOverlay overlay(render, &owner);
    overlay.SetExcludedWidget(&chat);
    overlay.SetActive(true);
    QCoreApplication::processEvents();
    const int fullWidth = render->width();
    auto* header = chat.chatView()->headerWidget();
    auto* preview = chat.findChild<QLabel*>(QStringLiteral("assistiveDockPreview"));
    QVERIFY(header && header->isVisible());
    QVERIFY(!header->childAt(HeaderDragPoint(header)));
    QVERIFY(preview);
    QVERIFY(!preview->isVisible());
    SendMouse(*header, QEvent::MouseButtonPress, HeaderDragPoint(header),
              Qt::LeftButton, Qt::LeftButton);
    QTRY_COMPARE(chat.moveStarts, 1);
    QVERIFY(!preview->isVisible()); // A header click alone never snaps.
    int targetX = side == QStringLiteral("left") ? 0 : owner.width() - chat.width();
    if (grabAtEdge) {
        const int grabOffset = header->mapToGlobal(HeaderDragPoint(header)).x() - chat.x();
        targetX = (side == QStringLiteral("left") ? 0 : owner.width() - 1) - grabOffset;
    }
    chat.move(owner.mapToGlobal(QPoint(targetX, 140)));
    QCoreApplication::processEvents();
    QVERIFY(chat.isFloating());
    QCOMPARE(render->width(), fullWidth); // Layout changes only on release.
    QTRY_VERIFY_WITH_TIMEOUT(preview->isVisible(), 1000);
    QCOMPARE(preview->text(), side == QStringLiteral("left")
                                 ? QStringLiteral("Release to dock left")
                                 : QStringLiteral("Release to dock right"));
    const QRect bounds(owner.mapToGlobal(QPoint()), owner.size());
    QCOMPARE(side == QStringLiteral("left") ? preview->geometry().left()
                                            : preview->geometry().right(),
             side == QStringLiteral("left") ? bounds.left() : bounds.right());
    const auto previewWindow = reinterpret_cast<HWND>(preview->winId());
    QVERIFY(GetWindowLongPtrW(previewWindow, GWL_EXSTYLE) & WS_EX_TRANSPARENT);
    QVERIFY(preview->testAttribute(Qt::WA_ShowWithoutActivating));
    const QImage previewImage = preview->grab().toImage();
    QVERIFY(previewImage.pixelColor(2, previewImage.height() / 2).alpha() > 0);
    QVERIFY(previewImage.pixelColor(previewImage.width() / 2,
                                    previewImage.height() * 3 / 4).alpha() > 0);
    QVERIFY(!overlay.mask().contains(
        overlay.mapFromGlobal(chat.mapToGlobal(chat.rect().center()))));
    SendMessageW(reinterpret_cast<HWND>(chat.winId()), WM_EXITSIZEMOVE, 0, 0);
    QTRY_COMPARE(chat.DockPosition(), side);
    QVERIFY(!preview->isVisible());
    QVERIFY(render->width() < fullWidth);
    QCOMPARE(overlay.geometry(), QRect(render->mapToGlobal(QPoint()), render->size()));
}

void AnnotationOverlayTests::leavingOrCancelingDockPreviewKeepsChatFloating_data()
{
    QTest::addColumn<int>("action");
    QTest::newRow("leave-edge") << 0;
    QTest::newRow("native-cancel") << 1;
    QTest::newRow("escape") << 2;
    QTest::newRow("hide-before-deferred-dock") << 3;
    QTest::newRow("click-without-moving") << 4;
}

void AnnotationOverlayTests::leavingOrCancelingDockPreviewKeepsChatFloating()
{
    QFETCH(int, action);
    QMainWindow owner;
    owner.resize(1400, 900);
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    owner.show();
    SimulatedNativeMoveChat chat(render);
    chat.RestoreRelativeGeometry(QRect(180, 140, 520, 360));
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("Answer"), true);
    QCoreApplication::processEvents();
    auto* header = chat.chatView()->headerWidget();
    auto* preview = chat.findChild<QLabel*>(QStringLiteral("assistiveDockPreview"));
    QVERIFY(header && header->isVisible());
    QVERIFY(!header->childAt(HeaderDragPoint(header)));
    QVERIFY(preview);
    // Ordinary placement changes must not offer or trigger docking.
    chat.move(owner.mapToGlobal(QPoint(0, 140)));
    QVERIFY(!preview->isVisible());
    const QRect startGeometry = chat.geometry();
    SendMouse(*header, QEvent::MouseButtonPress, HeaderDragPoint(header),
              Qt::LeftButton, Qt::LeftButton);
    QTRY_COMPARE(chat.moveStarts, 1);
    const HWND chatWindow = reinterpret_cast<HWND>(chat.winId());
    if (action != 4) {
        chat.move(chat.pos() + QPoint(0, 60));
        QTRY_VERIFY_WITH_TIMEOUT(preview->isVisible(), 1000);
    }
    if (action == 0) {
        chat.move(owner.mapToGlobal(QPoint(200, 140)));
        QTRY_VERIFY_WITH_TIMEOUT(!preview->isVisible(), 1000);
    } else if (action == 1) {
        SendMessageW(chatWindow, WM_CANCELMODE, 0, 0);
        QCOMPARE(chat.geometry(), startGeometry);
    } else if (action == 2) {
        SendMessageW(chatWindow, WM_KEYDOWN, VK_ESCAPE, 0);
        QCOMPARE(chat.geometry(), startGeometry);
    }
    SendMessageW(chatWindow, WM_EXITSIZEMOVE, 0, 0);
    if (action == 3) {
        chat.hide();
    }
    QCoreApplication::processEvents();
    QVERIFY(chat.isFloating());
    QVERIFY(!preview->isVisible());
    if (action != 3) {
        QVERIFY(chat.isVisible());
    }
}

void AnnotationOverlayTests::floatingChatRemainsInteractiveAndExcludesInkAfterMoving()
{
    QMainWindow owner;
    owner.resize(1400, 900);
    auto* render = new MouseSink;
    owner.setCentralWidget(render);
    owner.show();
    AssistiveOverlay chat(render);
    chat.RestoreRelativeGeometry(QRect(20, 140, 520, 360));
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("An answer"), true);
    AnnotationOverlay overlay(render, &owner);
    overlay.SetExcludedWidget(&chat);
    overlay.SetViewTransform(ComputeViewTransform(1400, 900, 1400, 900,
                                                 1.0f, 0.5f, 0.5f, ViewportFitMode::kFill));
    overlay.SetActive(true);
    overlay.raise();
    QVERIFY2(test::ActivateFixtureWindow(&overlay), test::ActivationFailureMessage());
    // Close the initial Pen flyout so the uncovered point below tests canvas
    // input rather than the flyout that normally occupies that location.
    QTest::keyClick(&overlay, Qt::Key_Escape);
    QCoreApplication::processEvents();

    const auto centerInCanvas = [&]() {
        return overlay.mapFromGlobal(chat.mapToGlobal(chat.rect().center()));
    };
    const QPoint originalCenter = centerInCanvas();
    QVERIFY(!overlay.mask().contains(originalCenter));
    auto* tools = overlay.findChild<QFrame*>(QStringLiteral("annotationToolbar"));
    QVERIFY(tools);
    QVERIFY(!tools->geometry().intersects(
        QRect(overlay.mapFromGlobal(chat.mapToGlobal(QPoint())), chat.size())));
    SendMouse(overlay, QEvent::MouseButtonPress, originalCenter,
              Qt::LeftButton, Qt::LeftButton);
    SendMouse(overlay, QEvent::MouseButtonRelease, originalCenter,
              Qt::LeftButton, Qt::NoButton);
    QVERIFY(!overlay.HasInk());

    // Inspect the actual Windows hit test and native region, not only Qt's
    // direct widget dispatch, which bypasses window stacking.
    const HWND overlayWindow = reinterpret_cast<HWND>(overlay.winId());
    RECT chatRect{};
    RECT overlayRect{};
    QVERIFY(GetWindowRect(reinterpret_cast<HWND>(chat.winId()), &chatRect));
    QVERIFY(GetWindowRect(overlayWindow, &overlayRect));
    const int screenX = (chatRect.left + chatRect.right) / 2;
    const int screenY = (chatRect.top + chatRect.bottom) / 2;
    QCOMPARE(SendMessageW(overlayWindow, WM_NCHITTEST, 0, MAKELPARAM(screenX, screenY)),
             static_cast<LRESULT>(HTTRANSPARENT));
    const HRGN nativeRegion = CreateRectRgn(0, 0, 0, 0);
    const int regionType = GetWindowRgn(overlayWindow, nativeRegion);
    const bool chatIsCovered = PtInRegion(nativeRegion, screenX - overlayRect.left,
                                         screenY - overlayRect.top);
    DeleteObject(nativeRegion);
    QVERIFY(regionType != ERROR);
    QVERIFY(!chatIsCovered);

    auto* question = chat.chatView()->composer();
    QVERIFY(question);
    QSignalSpy submitted(&chat, &AssistiveOverlay::QuestionSubmitted);
    QTest::mouseClick(question, Qt::LeftButton);
    QTest::keyClicks(question, "Explain this");
    QTest::keyClick(question, Qt::Key_Return);
    QCOMPARE(submitted.size(), 1);
    QCOMPARE(submitted.front().front().toString(), QStringLiteral("Explain this"));
    QVERIFY(!overlay.HasInk());

    chat.move(chat.pos() + QPoint(600, 40));
    chat.resize(440, 300);
    QCoreApplication::processEvents();
    QVERIFY(overlay.mask().contains(originalCenter));
    QVERIFY(!overlay.mask().contains(centerInCanvas()));
    const QRect movedGeometry = chat.geometry();
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("An answer with more text"), true);
    QCOMPARE(chat.geometry(), movedGeometry);

    SendMouse(overlay, QEvent::MouseButtonPress, originalCenter,
              Qt::LeftButton, Qt::LeftButton);
    SendMouse(overlay, QEvent::MouseButtonRelease, originalCenter,
              Qt::LeftButton, Qt::NoButton);
    QVERIFY(overlay.HasInk());
    chat.hide();
    QCoreApplication::processEvents();
    QCOMPARE(overlay.mask(), QRegion(overlay.rect()));
}

void AnnotationOverlayTests::dockingChatResizesViewportAndRestoresFloatingPlacement()
{
    QMainWindow owner;
    owner.resize(1400, 900);
    auto* render = new MouseSink;
    render->setMinimumSize(320, 240);
    // Match MainWindow's nested central-widget/splitter hierarchy: moving a
    // dock from left to right can move only the viewport's ancestor.
    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(render);
    layout->addWidget(splitter);
    owner.setCentralWidget(central);
    owner.show();
    AssistiveOverlay chat(render);
    chat.RestoreRelativeGeometry(QRect(180, 140, 520, 360));
    ShowFixtureChat(chat,QStringLiteral("Assistant"), QStringLiteral("An answer"), true);
    AnnotationOverlay overlay(render, &owner);
    overlay.SetExcludedWidget(&chat);
    overlay.SetActive(true);
    QCoreApplication::processEvents();
    const QRect floatingGeometry = chat.RelativeGeometry();
    const int fullWidth = render->width();
    auto* position = chat.findChild<QToolButton*>(QStringLiteral("assistiveDockPosition"));
    QVERIFY(position);

    for (QAction* action : position->menu()->actions()) {
        if (action->data().toString() == QStringLiteral("left")) action->trigger();
    }
    QCoreApplication::processEvents();
    QVERIFY(!chat.isFloating());
    QCOMPARE(owner.dockWidgetArea(&chat), Qt::LeftDockWidgetArea);
    QVERIFY(render->width() < fullWidth);
    QVERIFY(chat.geometry().right() < render->mapTo(&owner, QPoint()).x());
    QCOMPARE(overlay.geometry(), QRect(render->mapToGlobal(QPoint()), render->size()));
    QCOMPARE(overlay.mask(), QRegion(overlay.rect()));
    QCOMPARE(chat.RelativeGeometry(), floatingGeometry);

    for (QAction* action : position->menu()->actions()) {
        if (action->data().toString() == QStringLiteral("right")) action->trigger();
    }
    QCoreApplication::processEvents();
    QCOMPARE(owner.dockWidgetArea(&chat), Qt::RightDockWidgetArea);
    QVERIFY(render->mapTo(&owner, QPoint(render->width(), 0)).x() <= chat.x());
    QCOMPARE(overlay.geometry(), QRect(render->mapToGlobal(QPoint()), render->size()));

    chat.hide();
    QCoreApplication::processEvents();
    QCOMPARE(render->width(), fullWidth);
    chat.show();
    QCoreApplication::processEvents();
    QVERIFY(render->width() < fullWidth);
    for (QAction* action : position->menu()->actions()) {
        if (action->data().toString() == QStringLiteral("floating")) action->trigger();
    }
    QCoreApplication::processEvents();
    QVERIFY(chat.isFloating());
    QCOMPARE(render->width(), fullWidth);
    QCOMPARE(chat.RelativeGeometry(), floatingGeometry);
}

void AnnotationOverlayTests::fullyCoveredCanvasDoesNotClearItsNativeMask()
{
    QWidget owner;
    owner.resize(640, 480);
    MouseSink render(&owner);
    render.setGeometry(owner.rect());
    owner.show();
    AnnotationOverlay overlay(&render, &owner);
    QWidget panel(&owner, Qt::Tool | Qt::FramelessWindowHint);
    panel.setGeometry(QRect(render.mapToGlobal(QPoint()), render.size()));
    panel.show();
    overlay.SetExcludedWidget(&panel);
    overlay.SetActive(true);
    QCoreApplication::processEvents();
    QVERIFY(!overlay.mask().isEmpty());
    QVERIFY(overlay.mask().intersected(QRegion(overlay.rect())).isEmpty());
    panel.hide();
    QCoreApplication::processEvents();
    QCOMPARE(overlay.mask(), QRegion(overlay.rect()));
}

void AnnotationOverlayTests::middleDragCrossesTheToolWindowBoundary()
{
    QWidget owner;
    owner.resize(900, 600);
    MouseSink render(&owner);
    render.setGeometry(owner.rect());
    owner.show();

    AnnotationOverlay overlay(&render, &owner);
    overlay.SetViewTransform(
        ComputeViewTransform(1280, 720, 900, 600, 1.0f, 0.5f, 0.5f,
                             ViewportFitMode::kFill));
    overlay.SetActive(true);
    QCoreApplication::processEvents();

    SendMouse(overlay,
              QEvent::MouseButtonPress,
              QPointF(450, 300),
              Qt::MiddleButton,
              Qt::MiddleButton);
    SendMouse(overlay,
              QEvent::MouseMove,
              QPointF(500, 330),
              Qt::NoButton,
              Qt::MiddleButton);
    SendMouse(overlay,
              QEvent::MouseButtonRelease,
              QPointF(500, 330),
              Qt::MiddleButton,
              Qt::NoButton);

    QCOMPARE(render.middlePresses, 1);
    QCOMPARE(render.middleMoves, 1);
    QCOMPARE(render.middleReleases, 1);
    QVERIFY(!overlay.HasInk());
}

void AnnotationOverlayTests::textPlacementIsClickThenType()
{
    QWidget owner;
    owner.resize(900, 600);
    MouseSink render(&owner);
    render.setGeometry(owner.rect());
    owner.show();

    AnnotationOverlay overlay(&render, &owner);
    overlay.SetViewTransform(
        ComputeViewTransform(1280, 720, 900, 600, 1.0f, 0.5f, 0.5f,
                             ViewportFitMode::kFill));
    overlay.SetActive(true);
    QVERIFY2(test::ActivateFixtureWindow(&overlay), test::ActivationFailureMessage());

    QToolButton* textButton = ToolButton(overlay, QStringLiteral("Text"));
    QVERIFY(textButton);
    textButton->click();

    SendMouse(overlay,
              QEvent::MouseButtonPress,
              QPointF(450, 300),
              Qt::LeftButton,
              Qt::LeftButton);
    SendMouse(overlay,
              QEvent::MouseButtonRelease,
              QPointF(450, 300),
              Qt::LeftButton,
              Qt::NoButton);

    auto* editor =
        overlay.findChild<QLineEdit*>(QStringLiteral("annotationInlineTextEditor"));
    QVERIFY(editor);
    QVERIFY(editor->isVisible());
    QVERIFY(editor->hasFocus());
    QVERIFY(!overlay.HasInk());

    QTest::keyClicks(editor, QStringLiteral("cat"));
    QTest::keyClick(editor, Qt::Key_Return);

    QVERIFY(!editor->isVisible());
    QVERIFY(overlay.HasInk());
    QCOMPARE(overlay.Strokes().size(), 1);
    QCOMPARE(overlay.Strokes().front().kind, AnnotationItemKind::kText);
    QCOMPARE(overlay.Strokes().front().text, QStringLiteral("cat"));
    QVERIFY(std::abs(overlay.Strokes().front().points.front().x() - 0.5) <
            0.01);
    QVERIFY(std::abs(overlay.Strokes().front().points.front().y() - 0.5) <
            0.01);
}

void AnnotationOverlayTests::moveToolMarqueeSelectsAndMovesMultipleAnnotations()
{
    QWidget owner;
    owner.resize(900, 600);
    MouseSink render(&owner);
    render.setGeometry(owner.rect());
    owner.show();

    AnnotationOverlay overlay(&render, &owner);
    overlay.SetViewTransform(
        ComputeViewTransform(1280, 720, 900, 600, 1.0f, 0.5f, 0.5f,
                             ViewportFitMode::kFill));
    overlay.SetActive(true);
    QVERIFY2(test::ActivateFixtureWindow(&overlay), test::ActivationFailureMessage());

    const auto drawStroke = [&overlay](const QPointF& start,
                                       const QPointF& end) {
        SendMouse(overlay,
                  QEvent::MouseButtonPress,
                  start,
                  Qt::LeftButton,
                  Qt::LeftButton);
        SendMouse(overlay,
                  QEvent::MouseMove,
                  end,
                  Qt::NoButton,
                  Qt::LeftButton);
        SendMouse(overlay,
                  QEvent::MouseButtonRelease,
                  end,
                  Qt::LeftButton,
                  Qt::NoButton);
    };
    drawStroke({350, 240}, {390, 260});
    drawStroke({520, 320}, {570, 340});
    QCOMPARE(overlay.Strokes().size(), 2);
    const QVector<AnnotationStroke> before = overlay.Strokes();

    QToolButton* moveButton = ToolButton(overlay, QStringLiteral("Move"));
    QVERIFY(moveButton);
    moveButton->click();
    SendMouse(overlay,
              QEvent::MouseButtonPress,
              {300, 200},
              Qt::LeftButton,
              Qt::LeftButton);
    SendMouse(overlay,
              QEvent::MouseMove,
              {620, 380},
              Qt::NoButton,
              Qt::LeftButton);
    SendMouse(overlay,
              QEvent::MouseButtonRelease,
              {620, 380},
              Qt::LeftButton,
              Qt::NoButton);

    SendMouse(overlay,
              QEvent::MouseButtonPress,
              {360, 245},
              Qt::LeftButton,
              Qt::LeftButton);
    SendMouse(overlay,
              QEvent::MouseMove,
              {410, 275},
              Qt::NoButton,
              Qt::LeftButton);
    SendMouse(overlay,
              QEvent::MouseButtonRelease,
              {410, 275},
              Qt::LeftButton,
              Qt::NoButton);

    const QVector<AnnotationStroke> after = overlay.Strokes();
    QCOMPARE(after.size(), 2);
    const QPointF firstDelta =
        after.at(0).points.front() - before.at(0).points.front();
    const QPointF secondDelta =
        after.at(1).points.front() - before.at(1).points.front();
    QVERIFY(std::abs(firstDelta.x()) > 0.01);
    QVERIFY(std::abs(firstDelta.y()) > 0.01);
    QVERIFY(std::abs(firstDelta.x() - secondDelta.x()) < 0.0001);
    QVERIFY(std::abs(firstDelta.y() - secondDelta.y()) < 0.0001);

    QVERIFY(overlay.Undo());
    QCOMPARE(overlay.Strokes(), before);
}

void AnnotationOverlayTests::persistentActionPanelUsesNativeScreenCoordinates_data()
{
    QTest::addColumn<QString>("panelName");
    QTest::newRow("camera-actions") << QStringLiteral("bottomRightPanel");
    QTest::newRow("hide-ui") << QStringLiteral("uiVisibilityPanel");
}

void AnnotationOverlayTests::persistentActionPanelUsesNativeScreenCoordinates()
{
    QFETCH(QString, panelName);
    QWidget owner;
    owner.resize(900, 600);
    owner.move(120, 90);
    MouseSink render(&owner);
    render.setGeometry(owner.rect());
    owner.show();

    const Qt::WindowFlags chromeFlags = Qt::Tool |
                                        Qt::FramelessWindowHint |
                                        Qt::NoDropShadowWindowHint;
    QWidget actionPanel(&owner, chromeFlags);
    actionPanel.setObjectName(panelName);
    actionPanel.setGeometry(590, 480, 280, 90);
    auto* recordButton = new QPushButton(QStringLiteral("Record"), &actionPanel);
    recordButton->setGeometry(12, 12, 110, 58);
    actionPanel.show();

    AnnotationOverlay overlay(&render, &owner);
    overlay.SetViewTransform(
        ComputeViewTransform(1280, 720, 900, 600, 1.0f, 0.5f, 0.5f,
                             ViewportFitMode::kFill));
    overlay.SetActive(true);
    actionPanel.raise();
    overlay.raise();
    QCoreApplication::processEvents();

    const HWND panelWindow = reinterpret_cast<HWND>(actionPanel.winId());
    const HWND overlayWindow = reinterpret_cast<HWND>(overlay.winId());
    QVERIFY(panelWindow);
    QVERIFY(overlayWindow);

    RECT nativePanelRect{};
    QVERIFY(GetWindowRect(panelWindow, &nativePanelRect));
    const int nativeX = nativePanelRect.left +
                        (nativePanelRect.right - nativePanelRect.left) / 2;
    const int nativeY = nativePanelRect.top +
                        (nativePanelRect.bottom - nativePanelRect.top) / 2;
    const LRESULT hit = SendMessageW(
        overlayWindow,
        WM_NCHITTEST,
        0,
        MAKELPARAM(nativeX, nativeY));
    QCOMPARE(hit, static_cast<LRESULT>(HTTRANSPARENT));
}

void AnnotationOverlayTests::toolChromeMirrorsForRightToLeftLayouts()
{
    QWidget owner;
    owner.resize(1100, 700);
    MouseSink render(&owner);
    render.setGeometry(owner.rect());
    owner.show();

    AnnotationOverlay overlay(&render, &owner);
    overlay.SetViewTransform(
        ComputeViewTransform(1280, 720, 1100, 700, 1.0f, 0.5f, 0.5f,
                             ViewportFitMode::kFill));
    overlay.SetActive(true);
    QCoreApplication::processEvents();

    auto* tools =
        overlay.findChild<QFrame*>(QStringLiteral("annotationToolbar"));
    auto* options =
        overlay.findChild<QFrame*>(QStringLiteral("annotationOptions"));
    auto* actions =
        overlay.findChild<QFrame*>(QStringLiteral("annotationActions"));
    QVERIFY(tools);
    QVERIFY(options);
    QVERIFY(actions);

    overlay.setLayoutDirection(Qt::LeftToRight);
    QCoreApplication::processEvents();
    QVERIFY(tools->x() < options->x());
    QVERIFY(options->x() < actions->x());

    overlay.setLayoutDirection(Qt::RightToLeft);
    QCoreApplication::processEvents();
    QVERIFY(actions->x() < options->x());
    QVERIFY(options->x() < tools->x());
}

} // namespace okuflow

OKUFLOW_GUI_TEST_MAIN(okuflow::AnnotationOverlayTests)

#include "annotation_overlay_tests.moc"
