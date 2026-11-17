#include "okuflow/app/phone_link_text.hpp"

namespace okuflow {

PhoneLinkText PhoneLinkPlaceholderText(PhoneLinkDiagnosis diagnosis, bool userMustAct)
{
    using D = PhoneLinkDiagnosis;
    const QString waiting = QStringLiteral("Waiting for phone…");
    const PhoneLinkText restoring{
        QStringLiteral("Restoring USB connection…"),
        QStringLiteral("OkuFlow is checking the USB connection and will reconnect automatically.")};
    switch (diagnosis) {
    case D::Connected:
    case D::NotWanted:
        return {};
    case D::EndpointUnsupported:
        return {waiting, QStringLiteral("Open OkuFlow Camera on the phone, connect the USB cable, and allow USB "
                                        "debugging. OkuFlow checks the connection automatically.")};
    case D::RestartRequired:
        return {QStringLiteral("Camera did not stop in time. Restart OkuFlow before changing cameras."), {}};
    case D::ServerStarting:
    case D::ServerUnresponsive:
    case D::NotListedByServer:
    case D::Offline:
    case D::TransportStalled:
    case D::CompanionFailed:
        return restoring;
    case D::NoAdbInterface:
        return {waiting, QStringLiteral("Connect the phone by USB, then choose File transfer and turn on USB "
                                        "debugging on the phone.")};
    case D::Unauthorized:
    case D::Authorizing:
        return {waiting, QStringLiteral("Unlock your phone and tap Allow on the USB debugging question.")};
    case D::PhoneLocked:
        return userMustAct ? PhoneLinkText{waiting, QStringLiteral("Unlock your phone.")} : restoring;
    case D::AppNotRunning:
        return userMustAct ? PhoneLinkText{waiting, QStringLiteral("Open OkuFlow Camera on the phone.")} : restoring;
    case D::AppNotInstalled:
        return {waiting, QStringLiteral("Install OkuFlow Camera on the phone.")};
    case D::CompanionPaused:
        return {waiting, QStringLiteral("On the phone, open OkuFlow Camera and tap Resume streaming.")};
    case D::CompanionBusyOtherHost:
        return {waiting, QStringLiteral("Another computer or program is using the phone camera.")};
    case D::CompanionBlocked:
        return {waiting, QStringLiteral("Camera access is blocked on the phone.")};
    case D::CompanionHot:
        return {waiting, QStringLiteral("The phone is too hot. Let it cool down.")};
    case D::OtherInstanceHoldsPhone:
        return {waiting, QStringLiteral("Another OkuFlow window is using the phone. Close it to use the phone here.")};
    case D::OtherProgramHoldsPhone:
        return {waiting, QStringLiteral("Another program is using the phone's USB connection. Close it or restart "
                                        "the computer.")};
    }
    return restoring;
}

} // namespace okuflow
