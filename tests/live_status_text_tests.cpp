#include "okuflow/ui/live_status_text.hpp"

#include <QAccessible>
#include <QAccessibleAnnouncementEvent>
#include <QLabel>
#include <QtTest>

#include <algorithm>
#include <utility>

namespace okuflow {
namespace {

struct AccessibilityRecord {
    QAccessible::Event type{QAccessible::InvalidEvent};
    QString message;
    QAccessible::AnnouncementPoliteness politeness{
        QAccessible::AnnouncementPoliteness::Polite};
};

QObject* g_target{};
QVector<AccessibilityRecord>* g_records{};

void CaptureAccessibilityEvent(QAccessibleEvent* event)
{
    if (!event || !g_records || event->object() != g_target) {
        return;
    }
    AccessibilityRecord record;
    record.type = event->type();
    if (event->type() == QAccessible::Announcement) {
        auto* announcement =
            static_cast<QAccessibleAnnouncementEvent*>(event);
        record.message = announcement->message();
        record.politeness = announcement->politeness();
    }
    g_records->push_back(std::move(record));
}

int CountEvents(const QVector<AccessibilityRecord>& records,
                QAccessible::Event type)
{
    return static_cast<int>(std::count_if(
        records.cbegin(), records.cend(),
        [type](const AccessibilityRecord& record) {
            return record.type == type;
        }));
}

} // namespace

class LiveStatusTextTests final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void changeUpdatesVisibleAndAccessibleText();
    void identicalTextProducesNoEvents();
    void announcementCarriesRequestedPoliteness();
    void repeatedAnnouncementIsDeduplicatedAcrossFlapping();
    void coalescerPublishesOnlyTheTrailingValue();

private:
    QAccessible::UpdateHandler previousHandler_{};
    QVector<AccessibilityRecord> records_;
};

void LiveStatusTextTests::init()
{
    records_.clear();
    g_records = &records_;
    g_target = nullptr;
    previousHandler_ =
        QAccessible::installUpdateHandler(CaptureAccessibilityEvent);
}

void LiveStatusTextTests::cleanup()
{
    QAccessible::installUpdateHandler(previousHandler_);
    g_target = nullptr;
    g_records = nullptr;
}

void LiveStatusTextTests::changeUpdatesVisibleAndAccessibleText()
{
    QLabel label;
    g_target = &label;

    SetLiveText(&label, QStringLiteral("GPU Ready"),
                LivePoliteness::kSilent,
                QStringLiteral("Pipeline status"));

    QCOMPARE(label.text(), QStringLiteral("GPU Ready"));
    QCOMPARE(label.accessibleName(),
             QStringLiteral("Pipeline status: GPU Ready"));
    QVERIFY(CountEvents(records_, QAccessible::NameChanged) >= 1);
    QVERIFY(CountEvents(records_, QAccessible::TextUpdated) >= 1);
    QCOMPARE(CountEvents(records_, QAccessible::Announcement), 0);
}

void LiveStatusTextTests::identicalTextProducesNoEvents()
{
    QLabel label(QStringLiteral("GPU Ready"));
    label.setAccessibleName(QStringLiteral("Pipeline status: GPU Ready"));
    g_target = &label;

    SetLiveText(&label, QStringLiteral("GPU Ready"),
                LivePoliteness::kPolite,
                QStringLiteral("Pipeline status"));

    QVERIFY(records_.isEmpty());
}

void LiveStatusTextTests::announcementCarriesRequestedPoliteness()
{
    QLabel label;
    g_target = &label;

    SetLiveText(&label, QStringLiteral("Camera Offline"),
                LivePoliteness::kAssertive,
                QStringLiteral("Pipeline status"));

    QVERIFY(CountEvents(records_, QAccessible::NameChanged) >= 1);
    QCOMPARE(CountEvents(records_, QAccessible::Announcement), 1);
    const auto announcement = std::find_if(
        records_.cbegin(), records_.cend(),
        [](const AccessibilityRecord& record) {
            return record.type == QAccessible::Announcement;
        });
    QVERIFY(announcement != records_.cend());
    QCOMPARE(announcement->message,
             QStringLiteral("Pipeline status: Camera Offline"));
    QCOMPARE(announcement->politeness,
             QAccessible::AnnouncementPoliteness::Assertive);
}

void LiveStatusTextTests::repeatedAnnouncementIsDeduplicatedAcrossFlapping()
{
    QLabel label;
    g_target = &label;

    SetLiveText(&label, QStringLiteral("Reconnecting"),
                LivePoliteness::kPolite,
                QStringLiteral("Pipeline status"));
    SetLiveText(&label, QStringLiteral("Camera Offline"),
                LivePoliteness::kPolite,
                QStringLiteral("Pipeline status"));
    SetLiveText(&label, QStringLiteral("Reconnecting"),
                LivePoliteness::kPolite,
                QStringLiteral("Pipeline status"));

    QVERIFY(CountEvents(records_, QAccessible::NameChanged) >= 3);
    QCOMPARE(CountEvents(records_, QAccessible::Announcement), 2);
}

void LiveStatusTextTests::coalescerPublishesOnlyTheTrailingValue()
{
    QLabel label;
    g_target = &label;

    SetLiveTextCoalesced(&label, QStringLiteral("p95 12 ms"),
                         LivePoliteness::kPolite,
                         QStringLiteral("Performance"), 30);
    SetLiveTextCoalesced(&label, QStringLiteral("p95 18 ms"),
                         LivePoliteness::kPolite,
                         QStringLiteral("Performance"), 30);
    SetLiveTextCoalesced(&label, QStringLiteral("p95 21 ms"),
                         LivePoliteness::kPolite,
                         QStringLiteral("Performance"), 30);

    QTRY_COMPARE_WITH_TIMEOUT(label.text(),
                              QStringLiteral("p95 21 ms"), 500);
    QCOMPARE(label.accessibleName(),
             QStringLiteral("Performance: p95 21 ms"));
    QVERIFY(CountEvents(records_, QAccessible::NameChanged) >= 1);
    QCOMPARE(CountEvents(records_, QAccessible::Announcement), 1);
}

} // namespace okuflow

QTEST_MAIN(okuflow::LiveStatusTextTests)
#include "live_status_text_tests.moc"
