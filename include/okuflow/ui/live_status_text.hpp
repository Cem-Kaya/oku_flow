#pragma once

#include <QString>

QT_BEGIN_NAMESPACE
class QWidget;
QT_END_NAMESPACE

namespace okuflow {

enum class LivePoliteness {
    kSilent,
    kPolite,
    kAssertive,
};

// Updates visible dynamic text and its accessibility representation together.
// Selectable labels emit both text-content and accessible-name invalidations.
// Call only on the Qt UI thread. Static widgets may continue to use a fixed
// accessibleName; dynamic QLabel and QAbstractButton text must use this helper.
void SetLiveText(QWidget* widget,
                 const QString& text,
                 LivePoliteness politeness,
                 const QString& rolePrefix = {});

// Bounded-delay variant for rapidly changing labels. The latest update wins
// at the first pending deadline, including during a continuous update stream.
// SetLiveText supersedes any pending coalesced value immediately.
void SetLiveTextCoalesced(QWidget* widget,
                          const QString& text,
                          LivePoliteness politeness,
                          const QString& rolePrefix = {},
                          int delayMs = 400);

} // namespace okuflow
