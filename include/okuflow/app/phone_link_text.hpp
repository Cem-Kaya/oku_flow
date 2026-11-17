#pragma once
#include "okuflow/capture/phone_link_doctor.hpp"

#include <QString>

namespace okuflow {

// One plain sentence per phone link diagnosis for the camera placeholder and
// the status live region, written for a low-vision user who cannot read the
// phone. Strings are English catalog sources; MainWindow translates them
// with TranslateUi. An empty title means "no phone-specific text": the caller
// keeps its ordinary placeholder.
struct PhoneLinkText {
    QString title;
    QString detail;
};

// userMustAct: the ladder has nothing left to try before the user acts
// (it waits for the user); until then recoverable states read
// "Restoring USB connection…".
PhoneLinkText PhoneLinkPlaceholderText(PhoneLinkDiagnosis diagnosis, bool userMustAct);

} // namespace okuflow
