#include "okuflow/ui/video_transport.hpp"

#include <QAccessible>
#include <QApplication>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QWheelEvent>
#include <QtTest>

using okuflow::VideoTransport;

namespace {
VideoTransport::Snapshot Recording()
{
    VideoTransport::Snapshot snapshot;
    snapshot.source = VideoTransport::Source::Recording;
    snapshot.state = VideoTransport::State::Paused;
    snapshot.sourceGeneration = 1;
    snapshot.sourceName = QStringLiteral("Lecture recording");
    snapshot.positionMs = 30000;
    snapshot.durationMs = 60000;
    snapshot.seekable = true;
    snapshot.canSaveFrame = true;
    snapshot.hasAudio = true;
    return snapshot;
}
}

class VideoTransportTests : public QObject {
    Q_OBJECT
private slots:
    void ordinaryCameraHasNoTimeline()
    {
        VideoTransport transport;
        QVERIFY(transport.isHidden());
        transport.SetSnapshot({});
        QVERIFY(transport.isHidden());
        auto snapshot = Recording();
        transport.SetSnapshot(snapshot);
        QVERIFY(!transport.isHidden());
        transport.SetUiHidden(true);
        QVERIFY(transport.isHidden());
        transport.SetSnapshot(snapshot);
        QVERIFY(transport.isHidden());
        transport.SetUiHidden(false);
        QVERIFY(!transport.isHidden());
    }

    void snapshotsDoNotCommandPlayback()
    {
        VideoTransport transport;
        QSignalSpy seeks(&transport, &VideoTransport::SeekRequested);
        QSignalSpy toggles(&transport, &VideoTransport::PlayPauseRequested);
        QSignalSpy mute(&transport, &VideoTransport::MutedChanged);
        auto snapshot = Recording();
        transport.SetSnapshot(snapshot);
        snapshot.positionMs = 40000;
        snapshot.durationMs = 70000;
        snapshot.muted = false;
        transport.SetSnapshot(snapshot);
        QCOMPARE(seeks.size(), 0);
        QCOMPARE(toggles.size(), 0);
        QCOMPARE(mute.size(), 0);
        QCOMPARE(transport.timeline()->value(), 40000);
        QCOMPARE(transport.timeline()->maximum(), 70000);
    }

    void jumpMeaningFollowsSelectedSource()
    {
        VideoTransport transport;
        QSignalSpy jumps(&transport, &VideoTransport::JumpToEndRequested);
        auto snapshot = Recording();
        transport.SetSnapshot(snapshot);
        QCOMPARE(transport.jumpButton()->accessibleName(), QStringLiteral("Jump to live"));
        transport.jumpButton()->click();
        snapshot.source = VideoTransport::Source::Video;
        snapshot.sourceGeneration = 2;
        transport.SetSnapshot(snapshot);
        QCOMPARE(transport.jumpButton()->accessibleName(), QStringLiteral("Jump to end"));
        transport.jumpButton()->click();
        QCOMPARE(jumps.size(), 2);
        snapshot.state = VideoTransport::State::End;
        snapshot.positionMs = snapshot.durationMs;
        transport.SetSnapshot(snapshot);
        QVERIFY(!transport.playPauseButton()->isEnabled());
        QVERIFY(transport.jumpButton()->isEnabled());
    }

    void dragFreezesMappingAndCommitsLatestTargetOnce()
    {
        VideoTransport transport;
        auto snapshot = Recording();
        transport.SetSnapshot(snapshot);
        QSignalSpy seeks(&transport, &VideoTransport::SeekRequested);
        auto* slider = transport.timeline();
        slider->setSliderDown(true);
        slider->setSliderPosition(10000);
        slider->setSliderPosition(22000);
        snapshot.durationMs = 90000;
        snapshot.positionMs = 65000;
        transport.SetSnapshot(snapshot);
        QCOMPARE(slider->maximum(), 60000);
        QCOMPARE(slider->sliderPosition(), 22000);
        QCOMPARE(seeks.size(), 0);
        slider->setSliderDown(false);
        QCOMPARE(seeks.size(), 1);
        QCOMPARE(seeks.front().front().toLongLong(), 22000);
        QCOMPARE(slider->maximum(), 90000);
    }

    void sourceChangeCancelsPendingDrag()
    {
        VideoTransport transport;
        auto snapshot = Recording();
        transport.SetSnapshot(snapshot);
        QSignalSpy seeks(&transport, &VideoTransport::SeekRequested);
        transport.timeline()->setSliderDown(true);
        transport.timeline()->setSliderPosition(12000);
        snapshot.source = VideoTransport::Source::Video;
        snapshot.sourceGeneration = 2;
        snapshot.durationMs = 120000;
        snapshot.positionMs = 0;
        transport.SetSnapshot(snapshot);
        transport.timeline()->setSliderDown(false);
        QCOMPARE(seeks.size(), 0);
        QCOMPARE(transport.timeline()->value(), 0);
        QCOMPARE(transport.timeline()->maximum(), 120000);
    }

    void keyboardSeekingAndSpaceAreScopedToTimeline()
    {
        VideoTransport transport;
        transport.SetSnapshot(Recording());
        QSignalSpy seeks(&transport, &VideoTransport::SeekRequested);
        QSignalSpy toggles(&transport, &VideoTransport::PlayPauseRequested);
        auto* slider = transport.timeline();
        QTest::keyClick(slider, Qt::Key_Right);
        QCOMPARE(seeks.last().front().toLongLong(), 31000);
        QTest::keyClick(slider, Qt::Key_PageDown);
        QCOMPARE(seeks.last().front().toLongLong(), 21000);
        QTest::keyClick(slider, Qt::Key_Home);
        QCOMPARE(seeks.last().front().toLongLong(), 0);
        QTest::keyClick(slider, Qt::Key_End);
        QCOMPARE(seeks.last().front().toLongLong(), 60000);
        QTest::keyClick(slider, Qt::Key_Space);
        QCOMPARE(toggles.size(), 1);
        const int count = seeks.size();
        QWheelEvent wheel(QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(slider, &wheel);
        QCOMPARE(seeks.size(), count);
        // Native buttons retain their own Space activation behavior.
        QSignalSpy jumps(&transport, &VideoTransport::JumpToEndRequested);
        QTest::keyClick(transport.jumpButton(), Qt::Key_Space);
        QCOMPARE(toggles.size(), 1);
        QCOMPARE(jumps.size(), 1);
    }

    void accessibilityExposesTimeAndNativeNumericRange()
    {
        VideoTransport transport;
        auto snapshot = Recording();
        snapshot.positionMs = 3661000;
        snapshot.durationMs = 7200000;
        transport.SetSnapshot(snapshot);
        auto* accessible = QAccessible::queryAccessibleInterface(transport.timeline());
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::Slider);
        auto* value = accessible->valueInterface();
        QVERIFY(value);
        QCOMPARE(value->currentValue().toLongLong(), 3661000);
        QCOMPARE(value->minimumValue().toInt(), 0);
        QCOMPARE(value->maximumValue().toLongLong(), 7200000);
        QCOMPARE(value->minimumStepSize().toInt(), 1000);
        QCOMPARE(accessible->text(QAccessible::Value), QStringLiteral("1:01:01 of 2:00:00"));
        QSignalSpy seeks(&transport, &VideoTransport::SeekRequested);
        value->setCurrentValue(5000);
        QCOMPARE(seeks.size(), 1);
        QCOMPARE(seeks.front().front().toLongLong(), 5000);
    }

    void narrowControlsKeepTouchTargetsAndSourceIdentity()
    {
        VideoTransport transport;
        auto snapshot = Recording();
        snapshot.sourceName = QStringLiteral("Very long lecture filename with <plain> & Unicode \u00f6.mp4");
        transport.resize(320, 240);
        transport.SetSnapshot(snapshot);
        QApplication::processEvents();
        for (auto* button : transport.findChildren<QPushButton*>()) {
            QVERIFY(button->width() >= 52);
            QVERIFY(button->height() >= 52);
            QVERIFY(!button->accessibleName().isEmpty());
        }
        const auto* label = transport.findChild<QLabel*>(QStringLiteral("videoSourceLabel"));
        QCOMPARE(label->accessibleName(), snapshot.sourceName);
        QCOMPARE(label->textFormat(), Qt::PlainText);
        const auto targets = transport.FocusTargets();
        QCOMPARE(targets.size(), std::size_t(7));
        QCOMPARE(targets.back(), transport.timeline());
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QLocale::setDefault(QLocale::c());
    VideoTransportTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "video_transport_tests.moc"
