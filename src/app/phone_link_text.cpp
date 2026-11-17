#include "okuflow/app/phone_link_text.hpp"

namespace okuflow {

namespace {
const QString& Waiting()
{
    static const QString waiting = QStringLiteral("Waiting for phone…");
    return waiting;
}

PhoneLinkText Sentence(PhoneLinkDiagnosis diagnosis, bool userMustAct)
{
    using D = PhoneLinkDiagnosis;
    const QString waiting = Waiting();
    const PhoneLinkText restoring{
        QStringLiteral("Restoring USB connection…"),
        QStringLiteral("OkuFlow is checking the USB connection and will reconnect automatically.")};
    const QString allow = QStringLiteral(
        "Unlock your phone and tap Allow on the USB debugging question. If no question appears, unplug the "
        "USB cable and plug it back in.");
    const QString connect = QStringLiteral(
        "Connect the phone by USB, then choose File transfer and turn on USB debugging on the phone.");
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
    case D::WantedPhoneNotAttached:
        return {waiting, connect};
    case D::Unauthorized:
        return {waiting, allow};
    case D::Authorizing:
        // The Allow handshake of every normal re-plug reads "Restoring"
        // until it lasts longer than the short grace.
        return userMustAct ? PhoneLinkText{waiting, allow} : restoring;
    case D::PhoneLocked:
        return userMustAct ? PhoneLinkText{waiting, QStringLiteral("Unlock your phone.")} : restoring;
    case D::AppNotRunning:
        return userMustAct ? PhoneLinkText{waiting, QStringLiteral("Open OkuFlow Camera on the phone.")} : restoring;
    case D::AppNotInstalled:
        return {waiting, QStringLiteral("Install OkuFlow Camera on the phone.")};
    case D::CompanionPaused:
    // The companion installed today answers nothing while paused; its
    // process runs and its listener refuses. Opening it shows Resume.
    case D::CompanionListenerClosed:
        return {waiting, QStringLiteral("On the phone, open OkuFlow Camera and tap Resume streaming.")};
    case D::CompanionNotResponding:
        return {waiting, QStringLiteral("OkuFlow Camera is not responding on the phone. Close it on the phone and "
                                        "open it again.")};
    case D::ReplugCable:
        return {waiting, QStringLiteral("Unplug the phone's USB cable and plug it back in.")};
    case D::AdbCannotStart:
        return {waiting, QStringLiteral("OkuFlow cannot start its USB phone connection. Reinstall OkuFlow, or "
                                        "allow it in your antivirus program.")};
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
} // namespace

PhoneLinkText PhoneLinkPlaceholderText(PhoneLinkDiagnosis diagnosis, bool userMustAct)
{
    auto text = Sentence(diagnosis, userMustAct);
    if (!text.title.isEmpty())
        text.spoken = text.title == Waiting() && !text.detail.isEmpty() ? text.detail : text.title;
    return text;
}

} // namespace okuflow
