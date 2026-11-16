// Model-free contract for the phone link decision unit: every diagnosis,
// every event reset, every budget, and the owner's reconnect sequences.
#include "okuflow/capture/phone_link_doctor.hpp"
#include <QtTest>
#include <algorithm>
#include <functional>
#include <vector>

#define QCOMPARE_MS(actual, expected) QCOMPARE(std::int64_t(actual), std::int64_t(expected))

using namespace okuflow;
using D = PhoneLinkDiagnosis;
using A = PhoneLinkAction;

namespace {
// A ready phone on OkuFlow's own server.
PhoneLinkObservation Ready()
{
    PhoneLinkObservation o;
    o.phoneWanted = true;
    o.adbInterface = PhoneAdbInterface::Present;
    o.server = PhoneServerKind::Owned;
    o.serverProbe = PhoneServerProbe::Responsive;
    o.phoneState = PhoneAdbState::Device;
    o.adbd = PhoneAdbdHealth::Answers;
    o.companion = PhoneCompanionProbe::Ready;
    return o;
}
PhoneLinkObservation With(std::function<void(PhoneLinkObservation&)> change)
{
    auto o = Ready();
    change(o);
    return o;
}
PhoneLinkObservation Blind(PhoneServerKind kind = PhoneServerKind::Owned)
{
    return With([kind](auto& o) {
        o.server = kind; o.phoneState = PhoneAdbState::NotListed;
        o.adbd = PhoneAdbdHealth::Unknown; o.companion = PhoneCompanionProbe::Unknown;
    });
}
PhoneLinkObservation Refused(PhoneFact awake, PhoneFact keyguard, PhoneFact installed = PhoneFact::Yes)
{
    return With([&](auto& o) {
        o.companion = PhoneCompanionProbe::Refused;
        o.shell = {awake, keyguard, installed, PhoneFact::No};
    });
}
PhoneLinkObservation Problem(const char* code)
{
    return With([code](auto& o) { o.companion = PhoneCompanionProbe::Problem; o.problem = code; });
}
} // namespace

Q_DECLARE_METATYPE(PhoneLinkObservation)
Q_DECLARE_METATYPE(PhoneLinkDiagnosis)

class PhoneLinkDoctorTests : public QObject {
    Q_OBJECT
private slots:
    void diagnosis_data()
    {
        QTest::addColumn<PhoneLinkObservation>("observation");
        QTest::addColumn<PhoneLinkDiagnosis>("expected");
        QTest::newRow("ready") << Ready() << D::Connected;
        QTest::newRow("live session wins") << With([](auto& o) {
            o.sessionStreaming = true; o.phoneState = PhoneAdbState::Offline; }) << D::Connected;
        QTest::newRow("not probed while occupied") << With([](auto& o) {
            o.companion = PhoneCompanionProbe::Unknown; }) << D::Connected;
        QTest::newRow("no intent") << With([](auto& o) { o.phoneWanted = false; }) << D::NotWanted;
        QTest::newRow("unsupported endpoint") << With([](auto& o) { o.endpointSupported = false; })
                                              << D::EndpointUnsupported;
        QTest::newRow("abandoned capture") << With([](auto& o) { o.captureAbandoned = true; })
                                           << D::RestartRequired;
        QTest::newRow("no server yet") << With([](auto& o) {
            o.server = PhoneServerKind::None; o.serverProbe = PhoneServerProbe::NoListener;
            o.phoneState = PhoneAdbState::NotListed; }) << D::ServerStarting;
        QTest::newRow("hung server") << With([](auto& o) {
            o.server = PhoneServerKind::BorrowedDefault; o.serverProbe = PhoneServerProbe::Unresponsive;
            o.phoneState = PhoneAdbState::NotListed; }) << D::ServerUnresponsive;
        QTest::newRow("no ADB interface") << With([](auto& o) {
            o.adbInterface = PhoneAdbInterface::Absent; o.phoneState = PhoneAdbState::NotListed; })
                                          << D::NoAdbInterface;
        QTest::newRow("blind server") << Blind() << D::NotListedByServer;
        QTest::newRow("unauthorized") << With([](auto& o) { o.phoneState = PhoneAdbState::Unauthorized; })
                                      << D::Unauthorized;
        QTest::newRow("authorizing") << With([](auto& o) { o.phoneState = PhoneAdbState::Authorizing; })
                                     << D::Authorizing;
        QTest::newRow("connecting") << With([](auto& o) { o.phoneState = PhoneAdbState::Connecting; })
                                    << D::Authorizing;
        QTest::newRow("offline") << With([](auto& o) { o.phoneState = PhoneAdbState::Offline; }) << D::Offline;
        QTest::newRow("adbd silent") << With([](auto& o) {
            o.adbd = PhoneAdbdHealth::TimedOut; o.companion = PhoneCompanionProbe::TimedOut; })
                                     << D::TransportStalled;
        QTest::newRow("locked") << Refused(PhoneFact::Yes, PhoneFact::Yes) << D::PhoneLocked;
        QTest::newRow("asleep") << Refused(PhoneFact::No, PhoneFact::Unknown) << D::PhoneLocked;
        QTest::newRow("app killed") << Refused(PhoneFact::Yes, PhoneFact::No) << D::AppNotRunning;
        QTest::newRow("frozen app") << With([](auto& o) { o.companion = PhoneCompanionProbe::TimedOut; })
                                    << D::AppNotRunning;
        QTest::newRow("not installed") << Refused(PhoneFact::Yes, PhoneFact::No, PhoneFact::No)
                                       << D::AppNotInstalled;
        QTest::newRow("paused") << Problem("paused") << D::CompanionPaused;
        QTest::newRow("other host") << With([](auto& o) { o.companion = PhoneCompanionProbe::SessionBusy; })
                                    << D::CompanionBusyOtherHost;
        QTest::newRow("sessionBusy code") << Problem("sessionBusy") << D::CompanionBusyOtherHost;
        QTest::newRow("blocked") << Problem("blocked") << D::CompanionBlocked;
        QTest::newRow("hot") << Problem("hot") << D::CompanionHot;
        QTest::newRow("camera busy") << Problem("busy") << D::CompanionFailed;
        QTest::newRow("failed") << Problem("failed") << D::CompanionFailed;
        QTest::newRow("unknown code") << Problem("future") << D::CompanionFailed;
        QTest::newRow("two instances, phone busy") << With([](auto& o) {
            o.otherInstanceRunning = true; o.companion = PhoneCompanionProbe::SessionBusy; })
                                                   << D::OtherInstanceHoldsPhone;
        QTest::newRow("two instances, phone held") << With([](auto& o) {
            o.otherInstanceRunning = true; o.phoneState = PhoneAdbState::NotListed; })
                                                   << D::OtherInstanceHoldsPhone;
        QTest::newRow("stop refused") << With([](auto& o) {
            o.defaultStopRefused = true; o.phoneState = PhoneAdbState::NotListed; })
                                      << D::OtherProgramHoldsPhone;
    }
    void diagnosis()
    {
        QFETCH(PhoneLinkObservation, observation);
        QFETCH(PhoneLinkDiagnosis, expected);
        QCOMPARE(int(DiagnosePhoneLink(observation)), int(expected));
    }
    void namesCoverEveryDiagnosis()
    {
        for (int i = 0; i < kPhoneLinkDiagnosisCount; ++i)
            QVERIFY(PhoneLinkDiagnosisName(static_cast<D>(i)) != "unknown");
    }
    void eventMakesNextCycleImmediateAndPollBacksOffToCeiling()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        QCOMPARE_MS(doctor.NextPollMs(), 0);
        const auto starting = With([](auto& o) {
            o.server = PhoneServerKind::None; o.serverProbe = PhoneServerProbe::NoListener;
            o.phoneState = PhoneAdbState::NotListed; });
        std::int64_t now = 0;
        std::vector<std::int64_t> delays;
        for (int i = 0; i < 12; ++i) {
            const auto decision = doctor.Decide(starting, now);
            QCOMPARE(int(decision.action), int(A::Relist));
            delays.push_back(decision.nextPollMs - now);
            now = decision.nextPollMs;
        }
        QCOMPARE_MS(delays.front(), 1000);
        QCOMPARE_MS(delays[4], 1000);
        QCOMPARE_MS(delays[5], 2000);
        QCOMPARE_MS(delays.back(), 10000); // Ceiling: a lecture still recovers within seconds.
        QVERIFY(*std::max_element(delays.begin(), delays.end()) <= 10000);
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, now);
        QCOMPARE_MS(doctor.Decide(starting, now).nextPollMs - now, 1000);
    }
    void waitingForUserPollsSteadily()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto unauthorized = With([](auto& o) { o.phoneState = PhoneAdbState::Unauthorized; });
        for (std::int64_t now = 0; now < 120000; now += 2000) {
            const auto decision = doctor.Decide(unauthorized, now);
            QCOMPARE(int(decision.action), int(A::WaitForUser));
            QCOMPARE_MS(decision.nextPollMs - now, 2000);
        }
    }
    void unauthorizedThenAuthorized()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        auto first = doctor.Decide(With([](auto& o) { o.phoneState = PhoneAdbState::Unauthorized; }), 0);
        QCOMPARE(int(first.diagnosis), int(D::Unauthorized));
        QVERIFY(first.diagnosisChanged);
        auto second = doctor.Decide(Ready(), 2000);
        QCOMPARE(int(second.diagnosis), int(D::Connected));
        QVERIFY(second.diagnosisChanged);
        QCOMPARE(int(second.action), int(A::None));
    }
    void offlineThatAReconnectFixes()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::Resume, 0);
        const auto offline = With([](auto& o) { o.phoneState = PhoneAdbState::Offline; });
        QCOMPARE(int(doctor.Decide(offline, 0).action), int(A::ReconnectTransport)); // Immediate.
        QCOMPARE(int(doctor.Decide(Ready(), 1000).diagnosis), int(D::Connected));
        // A new offline episode after Connected starts with an immediate reconnect again.
        QCOMPARE(int(doctor.Decide(offline, 1500).action), int(A::ReconnectTransport));
    }
    void offlineThatNeedsAServerRestart()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::Resume, 0);
        const auto offline = With([](auto& o) { o.phoneState = PhoneAdbState::Offline; });
        QCOMPARE(int(doctor.Decide(offline, 0).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(offline, 1000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(offline, 3000).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(offline, 6000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(offline, 9000).action), int(A::RestartOwnedServer));
        // Restart budget: next one 15 s later, then the 60 s ceiling.
        int restarts = 0;
        for (std::int64_t now = 9500; now <= 40000; now += 500)
            restarts += doctor.Decide(offline, now).action == A::RestartOwnedServer;
        QCOMPARE(restarts, 1);
    }
    void borrowedServerOfflineOnlyReconnects()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto offline = With([](auto& o) {
            o.server = PhoneServerKind::BorrowedDefault; o.phoneState = PhoneAdbState::Offline; });
        int reconnects = 0;
        for (std::int64_t now = 0; now <= 40000; now += 500) {
            const auto action = doctor.Decide(offline, now).action;
            QVERIFY(action != A::RestartOwnedServer && action != A::TerminateDefaultServer &&
                    action != A::AskDefaultServerToExit);
            reconnects += action == A::ReconnectTransport;
        }
        QCOMPARE(reconnects, 3); // 0 s, 3 s, 33 s.
    }
    void authorizingGetsAShortGraceFirst()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        const auto authorizing = With([](auto& o) { o.phoneState = PhoneAdbState::Authorizing; });
        QCOMPARE(int(doctor.Decide(authorizing, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(authorizing, 1000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(authorizing, 2000).action), int(A::ReconnectTransport));
    }
    void blindOwnedServerRestartsAfterGrace()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        const auto blind = Blind();
        QCOMPARE(int(doctor.Decide(blind, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 4999).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 5000).action), int(A::RestartOwnedServer));
        QCOMPARE(int(doctor.Decide(blind, 6000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 20000).action), int(A::RestartOwnedServer));
        QCOMPARE(int(doctor.Decide(blind, 60000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 80000).action), int(A::RestartOwnedServer));
        // An event renews both grace and budget.
        doctor.OnEvent(PhoneLinkEvent::Resume, 81000);
        QCOMPARE(int(doctor.Decide(blind, 81000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 86000).action), int(A::RestartOwnedServer));
    }
    void blindWithoutWindowsInterfaceNeverEscalates()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto unknown = Blind();
        unknown.adbInterface = PhoneAdbInterface::Unknown;
        for (std::int64_t now = 0; now < 60000; now += 1000)
            QCOMPARE(int(doctor.Decide(unknown, now).action), int(A::Relist));
        auto absent = Blind();
        absent.adbInterface = PhoneAdbInterface::Absent;
        const auto decision = doctor.Decide(absent, 61000);
        QCOMPARE(int(decision.diagnosis), int(D::NoAdbInterface));
        QCOMPARE(int(decision.action), int(A::WaitForUser));
    }
    void blindBorrowedServerWithoutOtherDevicesIsTakenOver()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto borrowed = Blind(PhoneServerKind::BorrowedDefault);
        QCOMPARE(int(doctor.Decide(borrowed, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(borrowed, 5000).action), int(A::StartShadowServer));
        auto shadow = Blind(PhoneServerKind::Owned);
        shadow.shadowRunning = true;
        QCOMPARE(int(doctor.Decide(shadow, 6000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(shadow, 11999).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(shadow, 12000).action), int(A::AskDefaultServerToExit));
        QCOMPARE(int(doctor.Decide(shadow, 13000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(shadow, 14000).action), int(A::TerminateDefaultServer));
        QCOMPARE(int(doctor.Decide(shadow, 15000).action), int(A::Relist));
    }
    void blindBorrowedServerWithOtherDevicesIsNeverStopped()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        QCOMPARE(int(doctor.Decide(Blind(PhoneServerKind::BorrowedDefault), 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(Blind(PhoneServerKind::BorrowedDefault), 5000).action),
                 int(A::StartShadowServer));
        auto shadow = Blind(PhoneServerKind::Owned);
        shadow.shadowRunning = true;
        shadow.defaultHasOtherDevices = true;
        for (std::int64_t now = 6000; now < 120000; now += 1000) {
            const auto decision = doctor.Decide(shadow, now);
            QVERIFY(decision.action != A::AskDefaultServerToExit && decision.action != A::TerminateDefaultServer);
            if (now >= 12000) {
                QCOMPARE(int(decision.diagnosis), int(D::OtherProgramHoldsPhone));
                QCOMPARE(int(decision.action), int(A::WaitForUser));
            }
        }
    }
    void overriddenEndpointIsNeverStopped()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto blind = Blind(PhoneServerKind::Override);
        auto hung = blind;
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        for (std::int64_t now = 0; now < 120000; now += 1000) {
            for (const auto& o : {blind, hung}) {
                const auto action = doctor.Decide(o, now).action;
                QVERIFY(action == A::Relist);
            }
        }
    }
    void confirmedHungDefaultIsTerminatedWithBudget()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        QCOMPARE(int(doctor.Decide(hung, 0).action), int(A::Relist)); // Not confirmed yet.
        hung.defaultUnresponsiveConfirmed = true;
        QCOMPARE(int(doctor.Decide(hung, 1000).action), int(A::TerminateDefaultServer));
        QCOMPARE(int(doctor.Decide(hung, 2000).action), int(A::Relist));
        doctor.NoteDefaultStopRefused();
        const auto refused = doctor.Decide(hung, 20000);
        QCOMPARE(int(refused.action), int(A::Relist));
        auto listed = hung;
        listed.serverProbe = PhoneServerProbe::Responsive;
        QCOMPARE(int(doctor.Decide(listed, 21000).diagnosis), int(D::OtherProgramHoldsPhone));
    }
    void lockedPhoneIsWokenThenUserIsAsked()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        const auto locked = Refused(PhoneFact::No, PhoneFact::Yes);
        QCOMPARE(int(doctor.Decide(locked, 0).action), int(A::WakeAndLaunchApp));
        QCOMPARE(int(doctor.Decide(locked, 1000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(locked, 3000).action), int(A::WakeAndLaunchApp));
        QCOMPARE(int(doctor.Decide(locked, 5000).action), int(A::WaitForUser));
        QCOMPARE(int(doctor.Decide(locked, 9000).action), int(A::WakeAndLaunchApp));
        // Slow periodic retry while the user wants the phone.
        int launches = 0;
        for (std::int64_t now = 10000; now <= 50000; now += 1000)
            launches += doctor.Decide(locked, now).action == A::WakeAndLaunchApp;
        QCOMPARE(launches, 2); // 29 s and 49 s.
    }
    void appKilledIsRelaunchedAndManualPickRenewsLaunches()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto killed = Refused(PhoneFact::Yes, PhoneFact::No);
        QCOMPARE(int(doctor.Decide(killed, 0).action), int(A::WakeAndLaunchApp));
        QCOMPARE(int(doctor.Decide(killed, 100).action), int(A::Relist));
        doctor.OnEvent(PhoneLinkEvent::ManualPick, 200);
        QCOMPARE(int(doctor.Decide(killed, 200).action), int(A::WakeAndLaunchApp));
    }
    void pausedAndBusyNeverLaunchOrReset()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        for (const auto& o : {Problem("paused"), Problem("hot"), Problem("blocked"),
                              With([](auto& p) { p.companion = PhoneCompanionProbe::SessionBusy; })}) {
            for (std::int64_t now = 0; now < 30000; now += 1000) {
                const auto action = doctor.Decide(o, now).action;
                QCOMPARE(int(action), int(A::WaitForUser));
            }
        }
    }
    void sleepWithPhoneAttachedAndResume()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        QCOMPARE(int(doctor.Decide(With([](auto& o) { o.sessionStreaming = true; }), 0).diagnosis),
                 int(D::Connected));
        // Resume: pre-sleep budgets are void; the transport comes back offline first.
        doctor.OnEvent(PhoneLinkEvent::Resume, 3600000);
        const auto offline = With([](auto& o) { o.phoneState = PhoneAdbState::Offline; });
        QCOMPARE(int(doctor.Decide(offline, 3600000).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(Ready(), 3601000).diagnosis), int(D::Connected));
    }
    void pullAndReplugWithinOneSecond()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto offline = With([](auto& o) { o.phoneState = PhoneAdbState::Offline; });
        QCOMPARE(int(doctor.Decide(offline, 0).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(offline, 3000).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(offline, 4000).action), int(A::Relist)); // Budget spent.
        doctor.OnEvent(PhoneLinkEvent::DeviceRemoval, 4100);
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 4900);
        QCOMPARE_MS(doctor.NextPollMs(), 4900);
        QCOMPARE(int(doctor.Decide(offline, 4900).action), int(A::ReconnectTransport));
    }
    void everyEventResetsEveryBudget()
    {
        for (auto event : {PhoneLinkEvent::AppStart, PhoneLinkEvent::DeviceArrival, PhoneLinkEvent::DeviceRemoval,
                           PhoneLinkEvent::Resume, PhoneLinkEvent::ManualPick,
                           PhoneLinkEvent::WindowActivatedAfterIdle}) {
            PhoneLinkDoctor doctor;
            doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
            const auto killed = Refused(PhoneFact::Yes, PhoneFact::No);
            QCOMPARE(int(doctor.Decide(killed, 0).action), int(A::WakeAndLaunchApp));
            QCOMPARE(int(doctor.Decide(killed, 10).action), int(A::Relist));
            doctor.NoteDefaultStopRefused();
            doctor.OnEvent(event, 20);
            QVERIFY(!doctor.DefaultStopRefused());
            QCOMPARE_MS(doctor.LastEventMs(), 20);
            QCOMPARE(int(doctor.Decide(killed, 20).action), int(A::WakeAndLaunchApp));
        }
        QCOMPARE(PhoneLinkEventName(PhoneLinkEvent::Resume), std::string_view("resume"));
    }
    void twoInstancesWaitForTheUser()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto held = Blind();
        held.otherInstanceRunning = true;
        for (std::int64_t now = 0; now < 30000; now += 1000) {
            const auto decision = doctor.Decide(held, now);
            QCOMPARE(int(decision.diagnosis), int(D::OtherInstanceHoldsPhone));
            QCOMPARE(int(decision.action), int(A::WaitForUser)); // Never restarts against a sibling.
        }
    }
};

QTEST_APPLESS_MAIN(PhoneLinkDoctorTests)
#include "phone_link_doctor_tests.moc"
