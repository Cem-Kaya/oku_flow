#include "openzoom/ui/live_status_text.hpp"

#include <QAbstractButton>
#include <QAccessible>
#include <QAccessibleAnnouncementEvent>
#include <QAccessibleEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QLabel>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <QWidget>

namespace openzoom {
namespace {

constexpr auto kTimerObjectName = "_openzoomLiveTextTimer";
constexpr auto kPendingText = "_openzoomLiveTextPendingText";
constexpr auto kPendingPrefix = "_openzoomLiveTextPendingPrefix";
constexpr auto kPendingPoliteness = "_openzoomLiveTextPendingPoliteness";
constexpr auto kAnnouncementHistory = "_openzoomLiveTextAnnouncementHistory";
constexpr qint64 kAnnouncementDeduplicationMs = 5000;

QString VisibleText(QWidget* widget)
{
    if (auto* label = qobject_cast<QLabel*>(widget)) {
        return label->text();
    }
    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        return button->text();
    }
    return {};
}

bool ApplyVisibleText(QWidget* widget, const QString& text)
{
    if (auto* label = qobject_cast<QLabel*>(widget)) {
        label->setText(text);
        return true;
    }
    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        button->setText(text);
        return true;
    }
    return false;
}

QString AccessibleText(const QString& text, const QString& rolePrefix)
{
    const QString prefix = rolePrefix.trimmed();
    if (prefix.isEmpty()) {
        return text;
    }
    if (text.isEmpty()) {
        return prefix;
    }
    return QStringLiteral("%1: %2").arg(prefix, text);
}

void Announce(QWidget* widget,
              const QString& accessibleText,
              LivePoliteness politeness)
{
    if (politeness == LivePoliteness::kSilent || accessibleText.isEmpty()) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QVariantMap history =
        widget->property(kAnnouncementHistory).toMap();
    const qint64 previousMs =
        history.value(accessibleText).toLongLong();
    if (previousMs > 0 &&
        nowMs - previousMs < kAnnouncementDeduplicationMs) {
        return;
    }

    for (auto it = history.begin(); it != history.end();) {
        if (nowMs - it.value().toLongLong() >=
            kAnnouncementDeduplicationMs) {
            it = history.erase(it);
        } else {
            ++it;
        }
    }
    history.insert(accessibleText, nowMs);
    widget->setProperty(kAnnouncementHistory, history);
    QAccessibleAnnouncementEvent event(widget, accessibleText);
    event.setPoliteness(
        politeness == LivePoliteness::kAssertive
            ? QAccessible::AnnouncementPoliteness::Assertive
            : QAccessible::AnnouncementPoliteness::Polite);
    QAccessible::updateAccessibility(&event);
}

} // namespace

void SetLiveText(QWidget* widget,
                 const QString& text,
                 LivePoliteness politeness,
                 const QString& rolePrefix)
{
    if (!widget) {
        return;
    }
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() ==
             QCoreApplication::instance()->thread());

    const QString accessibleText = AccessibleText(text, rolePrefix);
    const bool visibleChanged = VisibleText(widget) != text;
    const bool nameChanged = widget->accessibleName() != accessibleText;
    if (!visibleChanged && !nameChanged) {
        return;
    }
    if (!ApplyVisibleText(widget, text)) {
        qWarning("SetLiveText supports QLabel and QAbstractButton only");
        return;
    }

    widget->setAccessibleName(accessibleText);
    QAccessibleEvent nameEvent(widget, QAccessible::NameChanged);
    QAccessible::updateAccessibility(&nameEvent);
    Announce(widget, accessibleText, politeness);
}

void SetLiveTextCoalesced(QWidget* widget,
                          const QString& text,
                          LivePoliteness politeness,
                          const QString& rolePrefix,
                          int delayMs)
{
    if (!widget) {
        return;
    }
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() ==
             QCoreApplication::instance()->thread());

    widget->setProperty(kPendingText, text);
    widget->setProperty(kPendingPrefix, rolePrefix);
    widget->setProperty(kPendingPoliteness,
                        static_cast<int>(politeness));

    QTimer* timer = widget->findChild<QTimer*>(
        QString::fromLatin1(kTimerObjectName),
        Qt::FindDirectChildrenOnly);
    if (!timer) {
        timer = new QTimer(widget);
        timer->setObjectName(QString::fromLatin1(kTimerObjectName));
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, widget, [widget]() {
            SetLiveText(
                widget,
                widget->property(kPendingText).toString(),
                static_cast<LivePoliteness>(
                    widget->property(kPendingPoliteness).toInt()),
                widget->property(kPendingPrefix).toString());
        });
    }
    timer->start(qMax(0, delayMs));
}

} // namespace openzoom
