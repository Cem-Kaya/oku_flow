#include "openzoom/common/view_transform.hpp"
#include "openzoom/ui/annotation_overlay.hpp"

#include <QLineEdit>
#include <QMouseEvent>
#include <QToolButton>
#include <QtTest>

#include <cmath>

namespace openzoom {
namespace {

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
    void textPlacementIsClickThenType();
    void moveToolMarqueeSelectsAndMovesMultipleAnnotations();
};

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
    overlay.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&overlay));

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
    overlay.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&overlay));

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

} // namespace openzoom

QTEST_MAIN(openzoom::AnnotationOverlayTests)

#include "annotation_overlay_tests.moc"
