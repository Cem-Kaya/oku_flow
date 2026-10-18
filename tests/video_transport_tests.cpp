#include "okuflow/ui/video_transport.hpp"

#include <QAccessible>
#include <QApplication>
#include <QFontDatabase>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTranslator>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtTest>

#include <cmath>

using okuflow::VideoTransport;

namespace {
QVector<QString> announcements;
QObject* announcementTarget{};
void CaptureAnnouncement(QAccessibleEvent* event)
{
    if (event->object() == announcementTarget && event->type() == QAccessible::Announcement) {
        announcements.push_back(static_cast<QAccessibleAnnouncementEvent*>(event)->message());
    }
}

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

VideoTransport::Snapshot Video()
{
    auto snapshot = Recording();
    snapshot.source = VideoTransport::Source::Video;
    snapshot.sourceName = QStringLiteral("lecture.mp4");
    return snapshot;
}

struct FocusFixture {
    QWidget host;
    QPushButton* viewport;
    VideoTransport* transport;
    FocusFixture()
    {
        auto* layout = new QVBoxLayout(&host);
        viewport = new QPushButton(QStringLiteral("Viewport"), &host);
        transport = new VideoTransport(&host);
        transport->SetFocusReturnTarget(viewport);
        layout->addWidget(viewport);
        layout->addWidget(transport);
        transport->SetSnapshot(Video());
        host.resize(1200, 320);
        host.show();
        QApplication::setActiveWindow(&host);
        QApplication::processEvents();
    }
    void Focus(QWidget* widget)
    {
        widget->setFocus(Qt::TabFocusReason);
        QApplication::processEvents();
    }
    void Key(Qt::Key key)
    {
        QVERIFY(QApplication::focusWidget());
        QTest::keyClick(QApplication::focusWidget(), key);
        QApplication::processEvents();
    }
    QPushButton* Button(const char* name)
    {
        return transport->findChild<QPushButton*>(QString::fromLatin1(name));
    }
};

class ActionTranslator final : public QTranslator {
public:
    QString translate(const char* context, const char* source, const char*, int) const override
    {
        if (QByteArray(context) == "OkuFlow" && QByteArray(source) == "Resume video")
            return QStringLiteral("Videoyu s\u00fcrd\u00fcr");
        if (QByteArray(context) == "OkuFlow" && QByteArray(source) == "Unmute playback")
            return QStringLiteral("Wiedergabeton einschalten");
        return {};
    }
    bool isEmpty() const override { return false; }
};
}

class VideoTransportTests : public QObject {
    Q_OBJECT
private slots:
    void init()
    {
        announcements.clear();
        announcementTarget = nullptr;
    }
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
            if (button->isHidden()) continue;
            QVERIFY(button->width() >= 52);
            QVERIFY(button->height() >= 52);
            QVERIFY(!button->accessibleName().isEmpty());
        }
        const auto* label = transport.findChild<QLabel*>(QStringLiteral("videoSourceLabel"));
        QCOMPARE(label->accessibleName(), snapshot.sourceName);
        QCOMPARE(label->textFormat(), Qt::PlainText);
        const auto targets = transport.FocusTargets();
        QCOMPARE(targets.size(), std::size_t(5));
        QCOMPARE(targets.back(), transport.timeline());
    }

    void focusedPlaySurvivesSeekingAndEndWithoutClosingFile()
    {
        FocusFixture fixture;
        auto* transport = fixture.transport;
        auto snapshot = Video();
        QSignalSpy toggles(transport, &VideoTransport::PlayPauseRequested);
        QSignalSpy returns(transport, &VideoTransport::ReturnToCameraRequested);
        fixture.Focus(transport->playPauseButton());
        fixture.Key(Qt::Key_Space);
        QCOMPARE(toggles.size(), 1);
        snapshot.state = VideoTransport::State::Seeking;
        snapshot.playing = true;
        snapshot.canSaveFrame = false;
        transport->SetSnapshot(snapshot);
        QCOMPARE(QApplication::focusWidget(), transport->playPauseButton());
        QVERIFY(!transport->IsActionReady(transport->playPauseButton()));
        fixture.Key(Qt::Key_Space);
        QCOMPARE(toggles.size(), 1);
        QCOMPARE(returns.size(), 0);
        snapshot.state = VideoTransport::State::Video;
        snapshot.canSaveFrame = true;
        transport->SetSnapshot(snapshot);
        fixture.Key(Qt::Key_Space);
        QCOMPARE(toggles.size(), 2);
        snapshot.state = VideoTransport::State::End;
        snapshot.playing = false;
        snapshot.positionMs = snapshot.durationMs;
        transport->SetSnapshot(snapshot);
        QCOMPARE(QApplication::focusWidget(), transport->playPauseButton());
        fixture.Key(Qt::Key_Space);
        QCOMPARE(toggles.size(), 2);
        QCOMPARE(returns.size(), 0);
        fixture.Key(Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), transport->jumpButton());
        QVERIFY(!transport->playPauseButton()->isEnabled());
    }

    void savePendingAndZeroRewindKeepSemanticFocus()
    {
        FocusFixture fixture;
        auto snapshot = Video();
        auto* save = fixture.Button("videoSaveFrameButton");
        QSignalSpy saves(fixture.transport, &VideoTransport::SaveFrameRequested);
        QSignalSpy returns(fixture.transport, &VideoTransport::ReturnToCameraRequested);
        QSignalSpy seeks(fixture.transport, &VideoTransport::SeekRequested);
        fixture.Focus(save);
        fixture.Key(Qt::Key_Space);
        QCOMPARE(saves.size(), 1);
        snapshot.canSaveFrame = false;
        fixture.transport->SetSnapshot(snapshot);
        QCOMPARE(QApplication::focusWidget(), save);
        fixture.Key(Qt::Key_Space);
        QCOMPARE(saves.size(), 1);
        snapshot.canSaveFrame = true;
        fixture.transport->SetSnapshot(snapshot);
        fixture.Key(Qt::Key_Space);
        QCOMPARE(saves.size(), 2);
        auto* rewind = fixture.Button("videoRewindButton");
        fixture.Focus(rewind);
        snapshot.positionMs = 0;
        fixture.transport->SetSnapshot(snapshot);
        QCOMPARE(QApplication::focusWidget(), rewind);
        fixture.Key(Qt::Key_Space);
        QCOMPARE(seeks.size(), 0);
        QCOMPARE(returns.size(), 0);
        fixture.Key(Qt::Key_Tab);
        QCOMPARE(QApplication::focusWidget(), fixture.transport->playPauseButton());
        QVERIFY(!rewind->isEnabled());
    }

    void hideSourceLossAndRecordingSwitchReturnUsableFocus()
    {
        FocusFixture fixture;
        fixture.Focus(fixture.transport->timeline());
        fixture.transport->SetUiHidden(true);
        QCOMPARE(QApplication::focusWidget(), fixture.viewport);
        fixture.transport->SetUiHidden(false);
        QCOMPARE(QApplication::focusWidget(), fixture.viewport);
        fixture.Focus(fixture.transport->playPauseButton());
        auto replacement = Video();
        ++replacement.sourceGeneration;
        replacement.state = VideoTransport::State::Opening;
        replacement.seekable = false;
        replacement.canSaveFrame = false;
        fixture.transport->SetSnapshot(replacement);
        QCOMPARE(QApplication::focusWidget(), fixture.transport->playPauseButton());
        fixture.transport->SetSnapshot({});
        QCOMPARE(QApplication::focusWidget(), fixture.viewport);
        fixture.transport->SetSnapshot(Video());
        fixture.Focus(fixture.Button("videoReturnCameraButton"));
        fixture.transport->SetSnapshot(Recording());
        QCOMPARE(QApplication::focusWidget(), fixture.transport->playPauseButton());
        QVERIFY(fixture.Button("videoReturnCameraButton")->isHidden());
        QVERIFY(fixture.Button("videoMuteButton")->isHidden());
        fixture.transport->SetSnapshot(Video());
        fixture.Focus(fixture.Button("videoMuteButton"));
        fixture.transport->SetSnapshot(Recording());
        QCOMPARE(QApplication::focusWidget(), fixture.transport->playPauseButton());
    }

    void fileReturnIsLastInActualTabSequence()
    {
        FocusFixture fixture;
        auto* transport = fixture.transport;
        auto* returnButton = fixture.Button("videoReturnCameraButton");
        const auto targets = transport->FocusTargets();
        QCOMPARE(targets.size(), std::size_t(7));
        QCOMPARE(targets.back(), returnButton);
        fixture.Focus(fixture.Button("videoRewindButton"));
        for (std::size_t index = 1; index < targets.size(); ++index) {
            fixture.Key(Qt::Key_Tab);
            QCOMPARE(QApplication::focusWidget(), targets[index]);
        }
        auto snapshot = Video();
        snapshot.state = VideoTransport::State::Seeking;
        snapshot.seekable = false;
        snapshot.canSaveFrame = false;
        snapshot.hasAudio = false;
        fixture.Focus(transport->playPauseButton());
        transport->SetSnapshot(snapshot);
        QCOMPARE(QApplication::focusWidget(), transport->playPauseButton());
        QSignalSpy returns(transport, &VideoTransport::ReturnToCameraRequested);
        fixture.Key(Qt::Key_Space);
        QCOMPARE(returns.size(), 0);
    }

    void focusedTimelineRelativeKeysAndWheelUseCurrentMediaAndPendingTarget()
    {
        FocusFixture fixture;
        auto snapshot = Video();
        snapshot.state = VideoTransport::State::Video;
        snapshot.playing = true;
        fixture.transport->SetSnapshot(snapshot);
        auto* slider = fixture.transport->timeline();
        fixture.Focus(slider);
        QCOMPARE(slider->value(), 30000);
        snapshot.positionMs = 45000;
        snapshot.durationMs = 90000;
        fixture.transport->SetSnapshot(snapshot);
        QCOMPARE(slider->value(), 30000); // Reader's inspected thumb remains still.
        QCOMPARE(slider->maximum(), 60000);
        QSignalSpy seeks(fixture.transport, &VideoTransport::SeekRequested);
        fixture.Key(Qt::Key_Right);
        QCOMPARE(seeks.last().front().toLongLong(), 46000);
        QCOMPARE(slider->maximum(), 90000);
        fixture.Key(Qt::Key_Right);
        QCOMPARE(seeks.last().front().toLongLong(), 47000);
        fixture.Key(Qt::Key_PageDown);
        QCOMPARE(seeks.last().front().toLongLong(), 37000);
        snapshot.state = VideoTransport::State::Seeking;
        snapshot.positionMs = 37000;
        fixture.transport->SetSnapshot(snapshot);
        fixture.Key(Qt::Key_Left);
        QCOMPARE(seeks.last().front().toLongLong(), 36000);
        snapshot.positionMs = 36000;
        fixture.transport->SetSnapshot(snapshot);
        snapshot.state = VideoTransport::State::Video;
        snapshot.positionMs = 50000;
        fixture.transport->SetSnapshot(snapshot);
        QCOMPARE(slider->value(), 36000);
        auto* actions = QAccessible::queryAccessibleInterface(slider)->actionInterface();
        QVERIFY(actions);
        actions->doAction(QAccessibleActionInterface::increaseAction());
        QCOMPARE(seeks.last().front().toLongLong(), 51000);
        QWheelEvent wheel(QPointF(10, 10), QPointF(10, 10), QPoint(), QPoint(0, 120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(slider, &wheel);
        QCOMPARE(seeks.last().front().toLongLong(), 52000);
        QApplication::sendEvent(slider, &wheel);
        QCOMPARE(seeks.last().front().toLongLong(), 53000);
        fixture.Key(Qt::Key_End);
        QCOMPARE(seeks.last().front().toLongLong(), 90000);
        fixture.Key(Qt::Key_Right);
        QCOMPARE(seeks.last().front().toLongLong(), 90000);
        fixture.Key(Qt::Key_Home);
        QCOMPARE(seeks.last().front().toLongLong(), 0);
    }

    void muteIsAnOrdinarySnapshotDrivenAction()
    {
        FocusFixture fixture;
        auto* mute = fixture.Button("videoMuteButton");
        QVERIFY(!mute->isCheckable());
        auto* accessible = QAccessible::queryAccessibleInterface(mute);
        QVERIFY(accessible);
        QCOMPARE(accessible->role(), QAccessible::Button);
        QVERIFY(!accessible->state().checkable);
        QSignalSpy changes(fixture.transport, &VideoTransport::MutedChanged);
        fixture.Focus(mute);
        QCOMPARE(mute->accessibleName(), QStringLiteral("Unmute playback"));
        fixture.Key(Qt::Key_Space);
        QCOMPARE(changes.last().front().toBool(), false);
        fixture.Key(Qt::Key_Space);
        QCOMPARE(changes.last().front().toBool(), false); // No optimistic toggle.
        auto snapshot = Video();
        snapshot.muted = false;
        fixture.transport->SetSnapshot(snapshot);
        QCOMPARE(mute->accessibleName(), QStringLiteral("Mute playback"));
        fixture.Key(Qt::Key_Space);
        QCOMPARE(changes.last().front().toBool(), true);
    }

    void labelsFollowExistingTranslationsAndCompactWithAccessibleNames()
    {
        ActionTranslator translator;
        QApplication::installTranslator(&translator);
        FocusFixture fixture;
        fixture.host.resize(2000, 400);
        QApplication::processEvents();
        auto* play = fixture.transport->playPauseButton();
        auto* mute = fixture.Button("videoMuteButton");
        QCOMPARE(play->text(), QStringLiteral("Videoyu s\u00fcrd\u00fcr"));
        QCOMPARE(mute->text(), QStringLiteral("Wiedergabeton einschalten"));
        for (auto* button : fixture.transport->findChildren<QPushButton*>()) {
            QVERIFY(button->width() >= button->fontMetrics().horizontalAdvance(button->text()) + 16);
        }
        fixture.host.resize(340, 400);
        QApplication::processEvents();
        QVERIFY(play->text() != play->accessibleName());
        QCOMPARE(play->accessibleName(), QStringLiteral("Videoyu s\u00fcrd\u00fcr"));
        for (auto* button : fixture.transport->findChildren<QPushButton*>()) {
            QVERIFY(button->width() >= 52);
            QVERIFY(button->height() >= 52);
            QVERIFY(fixture.transport->rect().contains(button->mapTo(fixture.transport, QPoint())));
            QVERIFY(fixture.transport->rect().contains(button->mapTo(fixture.transport, button->rect().bottomRight())));
        }
        QApplication::removeTranslator(&translator);
        QEvent languageChange(QEvent::LanguageChange);
        QApplication::sendEvent(fixture.transport, &languageChange);
        QCOMPARE(play->accessibleName(), QStringLiteral("Resume video"));
    }

    void liveBadgesHaveReadableContrast()
    {
        VideoTransport transport;
        auto snapshot = Recording();
        auto* badge = transport.findChild<QLabel*>(QStringLiteral("videoStateLabel"));
        const auto luminance = [](const QColor& color) {
            const auto linear = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
            return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
        };
        for (auto state : {VideoTransport::State::Live, VideoTransport::State::BehindLive}) {
            snapshot.state = state;
            transport.SetSnapshot(snapshot);
            badge->ensurePolished();
            QCOMPARE(badge->text(), state == VideoTransport::State::Live ? QStringLiteral("Live") : QStringLiteral("Behind live"));
            QVERIFY((luminance(badge->palette().color(QPalette::WindowText)) + 0.05) /
                    (luminance(badge->palette().color(QPalette::Window)) + 0.05) >= 7.0);
        }
    }

    void onlySettledStateAnnouncesCurrentTimeAndDeduplicates()
    {
        VideoTransport transport;
        announcementTarget = &transport;
        auto snapshot = Video();
        snapshot.state = VideoTransport::State::Opening;
        snapshot.positionMs = 0;
        transport.SetSnapshot(snapshot);
        QTest::qWait(250);
        QVERIFY(announcements.isEmpty());
        snapshot.state = VideoTransport::State::Seeking;
        snapshot.positionMs = 40000;
        transport.SetSnapshot(snapshot);
        QTest::qWait(250);
        QVERIFY(announcements.isEmpty());
        snapshot.positionMs = 45000;
        transport.SetSnapshot(snapshot);
        snapshot.state = VideoTransport::State::Paused;
        transport.SetSnapshot(snapshot);
        QTRY_COMPARE(announcements.size(), 1);
        QCOMPARE(announcements.front(), QStringLiteral("lecture.mp4: Paused, 00:45"));
        transport.SetSnapshot(snapshot);
        snapshot.state = VideoTransport::State::Seeking;
        transport.SetSnapshot(snapshot);
        snapshot.state = VideoTransport::State::Paused;
        transport.SetSnapshot(snapshot);
        QTest::qWait(250);
        QCOMPARE(announcements.size(), 1);
        snapshot.state = VideoTransport::State::Video;
        snapshot.playing = true;
        transport.SetSnapshot(snapshot);
        snapshot.positionMs = 46000;
        transport.SetSnapshot(snapshot);
        QTRY_COMPARE(announcements.size(), 2);
        QCOMPARE(announcements.back(), QStringLiteral("lecture.mp4: Video, 00:46"));
        snapshot.positionMs = 47000;
        transport.SetSnapshot(snapshot);
        QTest::qWait(250);
        QCOMPARE(announcements.size(), 2);
        snapshot.state = VideoTransport::State::Paused;
        transport.SetSnapshot(snapshot);
        transport.SetUiHidden(true);
        QTest::qWait(250);
        QCOMPARE(announcements.size(), 2);
    }
};

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf"));
    application.setFont(QFont(QStringLiteral("Segoe UI"), 10));
    QLocale::setDefault(QLocale::c());
    QAccessible::setActive(true);
    QAccessible::installUpdateHandler(CaptureAnnouncement);
    VideoTransportTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "video_transport_tests.moc"
