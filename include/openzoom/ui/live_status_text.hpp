#pragma once

#include <QString>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace openzoom {

enum class LivePoliteness {
    kSilent,
    kPolite,
    kAssertive,
};

// Updates visible dynamic text and its accessibility representation together.
// Call only on the Qt UI thread. Static widgets may continue to use a fixed
// accessibleName; dynamic QLabel and QAbstractButton text must use this helper.
void SetLiveText(QWidget* widget,
                 const QString& text,
                 LivePoliteness politeness,
                 const QString& rolePrefix = {});

// Trailing-edge variant for rapidly changing labels. The latest update wins.
void SetLiveTextCoalesced(QWidget* widget,
                          const QString& text,
                          LivePoliteness politeness,
                          const QString& rolePrefix = {},
                          int delayMs = 400);

} // namespace openzoom
