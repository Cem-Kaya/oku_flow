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
        // Iteration 3 (A2): a hung listener with the phone unplugged.
        QTest::newRow("hung server, phone unplugged") << With([](auto& o) {
            o.server = PhoneServerKind::BorrowedDefault; o.serverProbe = PhoneServerProbe::Unresponsive;
            o.phoneState = PhoneAdbState::NotListed; o.adbInterface = PhoneAdbInterface::Absent; })
                                                      << D::NoAdbInterface;
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
        // adbd answers (the shell query replied) while the listener hangs.
        QTest::newRow("frozen app") << With([](auto& o) {
            o.companion = PhoneCompanionProbe::TimedOut; o.adbd = PhoneAdbdHealth::Answers; })
                                    << D::AppNotRunning;
        QTest::newRow("app process alive, listener refuses") << With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::Yes, PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes}; })
                                                             << D::CompanionListenerClosed;
        // Iteration 3 (C3): the listener refuses while the app process runs,
        // whatever the keyguard shows (paused on a locked phone).
        QTest::newRow("paused and locked") << With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes, PhoneFact::Yes}; })
                                           << D::CompanionListenerClosed;
        QTest::newRow("no permissions") << With([](auto& o) { o.phoneState = PhoneAdbState::NoPermissions; })
                                        << D::Offline;
        QTest::newRow("unknown state") << With([](auto& o) { o.phoneState = PhoneAdbState::Other; }) << D::Offline;
        QTest::newRow("another phone listed") << With([](auto& o) {
            o.phoneState = PhoneAdbState::NotListed; o.otherUsbListed = true; }) << D::WantedPhoneNotAttached;
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
        QTest::newRow("windowless leftover instance") << With([](auto& o) {
            o.leftoverInstanceRunning = true; o.phoneState = PhoneAdbState::NotListed; })
                                                      << D::OtherProgramHoldsPhone;
        QTest::newRow("stop refused") << With([](auto& o) {
            o.defaultStopRefused = true; o.phoneState = PhoneAdbState::NotListed; })
                                      << D::OtherProgramHoldsPhone;
        // Refusal wins over the probe that the hung server keeps failing.
        QTest::newRow("stop refused, server unresponsive") << With([](auto& o) {
            o.defaultStopRefused = true; o.phoneState = PhoneAdbState::NotListed;
            o.server = PhoneServerKind::BorrowedDefault; o.serverProbe = PhoneServerProbe::Unresponsive; })
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
        QVERIFY(PhoneLinkActionName(A::HandBackToDefault) != "unknown");
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
        // Changed deliberately (iteration 2, A11): after a quiet period in
        // Unauthorized exactly one targeted reconnect re-asks the question.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto unauthorized = With([](auto& o) { o.phoneState = PhoneAdbState::Unauthorized; });
        int reconnects = 0;
        for (std::int64_t now = 0; now < 120000; now += 2000) {
            const auto decision = doctor.Decide(unauthorized, now);
            if (decision.action == A::ReconnectTransport) {
                ++reconnects;
                QVERIFY(now >= doctor.Policy().unauthorizedReconnectMs);
            } else {
                QCOMPARE(int(decision.action), int(A::WaitForUser));
            }
            QCOMPARE(int(decision.diagnosis), int(D::Unauthorized));
            QCOMPARE_MS(decision.nextPollMs - now, 2000);
        }
        QCOMPARE(reconnects, 1);
        // A new event (re-plug) renews that single reconnect.
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 120000);
        QCOMPARE(int(doctor.Decide(unauthorized, 120000).action), int(A::WaitForUser));
        QCOMPARE(int(doctor.Decide(unauthorized, 140000).action), int(A::ReconnectTransport));
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
        // Changed deliberately (iteration 2, C8): Connected renews the
        // budgets only after it held; a flip back half a second later waits
        // for the reconnect budget instead of resetting at once.
        QCOMPARE(int(doctor.Decide(offline, 1500).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(offline, 3000).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(Ready(), 4000).diagnosis), int(D::Connected));
        QCOMPARE(int(doctor.Decide(Ready(), 14000).diagnosis), int(D::Connected)); // Held 10 s.
        QCOMPARE(int(doctor.Decide(offline, 14500).action), int(A::ReconnectTransport));
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
    void stuckPhoneOnAnotherServerEscalates_data()
    {
        QTest::addColumn<PhoneLinkObservation>("stuck");
        const auto borrowed = [](PhoneAdbState state, PhoneAdbdHealth adbd = PhoneAdbdHealth::Answers) {
            return With([=](auto& o) {
                o.server = PhoneServerKind::BorrowedDefault; o.phoneState = state; o.adbd = adbd;
                if (adbd == PhoneAdbdHealth::TimedOut) o.companion = PhoneCompanionProbe::TimedOut;
            });
        };
        QTest::newRow("offline") << borrowed(PhoneAdbState::Offline);
        QTest::newRow("connecting") << borrowed(PhoneAdbState::Connecting);
        QTest::newRow("no permissions") << borrowed(PhoneAdbState::NoPermissions);
        QTest::newRow("stalled") << borrowed(PhoneAdbState::Device, PhoneAdbdHealth::TimedOut);
    }
    void stuckPhoneOnAnotherServerEscalates()
    {
        // Iteration 3 (A1), replaces borrowedServerOfflineOnlyReconnects,
        // which only asserted what did not happen: a default that keeps the
        // phone stuck holds the interface without serving it. Two targeted
        // reconnects, then the takeover rungs; never an owned restart.
        QFETCH(PhoneLinkObservation, stuck);
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        std::vector<A> sequence;
        std::int64_t now = 0;
        for (; now <= 30000; now += 500) {
            const auto decision = doctor.Decide(stuck, now);
            QVERIFY(decision.action != A::RestartOwnedServer);
            if (decision.action != A::Relist) sequence.push_back(decision.action);
            if (decision.action == A::StartShadowServer) break;
        }
        QVERIFY(sequence == (std::vector<A>{A::ReconnectTransport, A::ReconnectTransport, A::StartShadowServer}));
        // The private server now runs beside it; the default still lists the
        // phone stuck, so it is asked to exit, then stopped. Offline,
        // connecting and no permissions there are not a usable listing.
        // Iteration 4 (A1): a stalled phone is listed "device" there, which
        // is usable, but adbd timed out for it: that listing does not serve
        // it either (the earlier row paired it with defaultListsPhone=false,
        // which cannot occur).
        auto beside = Blind(PhoneServerKind::Owned);
        beside.shadowRunning = true;
        if (stuck.phoneState == PhoneAdbState::Device) beside.defaultListsPhone = beside.defaultPhoneStalled = true;
        sequence.clear();
        for (now += 500; now <= 60000; now += 500) {
            const auto decision = doctor.Decide(beside, now);
            if (decision.action != A::Relist) sequence.push_back(decision.action);
            if (decision.action == A::TerminateDefaultServer) break;
        }
        QVERIFY(sequence == (std::vector<A>{A::AskDefaultServerToExit, A::TerminateDefaultServer}));
    }
    void ownedOfflineLadderStartsFreshAfterForeignReconnects()
    {
        // Iteration 4 (B5): two reconnects to another program's server, then
        // OkuFlow's own server lists the phone offline (after a takeover): its
        // first reconnect goes out at once, not after the 30 s ceiling.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto foreign = With([](auto& o) {
            o.server = PhoneServerKind::BorrowedDefault; o.phoneState = PhoneAdbState::Offline; });
        const auto owned = With([](auto& o) { o.phoneState = PhoneAdbState::Offline; });
        QCOMPARE(int(doctor.Decide(foreign, 0).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(foreign, 3000).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(owned, 4000).action), int(A::ReconnectTransport));
        QCOMPARE(int(doctor.Decide(owned, 5000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(owned, 7000).action), int(A::ReconnectTransport)); // 3 s spacing.
    }
    void slowDefaultServingAnotherDeviceIsNamed()
    {
        // Iteration 4 (B6): a confirmed "hung" default whose listing showed
        // another device serves someone: named, never terminated.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        hung.defaultHasOtherDevices = true;
        for (std::int64_t now = 0; now < 60000; now += 1000) {
            const auto decision = doctor.Decide(hung, now);
            QCOMPARE(int(decision.diagnosis), int(D::OtherProgramHoldsPhone));
            QCOMPARE(int(decision.action), int(A::WaitForUser));
        }
    }
    void stuckPhoneOnAServerServingAnotherDeviceIsNamed()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto offline = With([](auto& o) {
            o.server = PhoneServerKind::BorrowedDefault; o.phoneState = PhoneAdbState::Offline;
            o.defaultHasOtherDevices = true; });
        PhoneLinkDecision last;
        int reconnects = 0;
        for (std::int64_t now = 0; now <= 60000; now += 500) {
            last = doctor.Decide(offline, now);
            QVERIFY(last.action != A::StartShadowServer && last.action != A::AskDefaultServerToExit &&
                    last.action != A::TerminateDefaultServer && last.action != A::RestartOwnedServer);
            reconnects += last.action == A::ReconnectTransport;
        }
        QCOMPARE(reconnects, doctor.Policy().foreignReconnects);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(last.action), int(A::WaitForUser));
    }
    void stuckPhoneOnAUserEndpointAsksForAReplug()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto offline = With([](auto& o) {
            o.server = PhoneServerKind::Override; o.phoneState = PhoneAdbState::Offline; });
        PhoneLinkDecision last;
        int reconnects = 0;
        for (std::int64_t now = 0; now <= 60000; now += 500) {
            last = doctor.Decide(offline, now);
            QVERIFY(last.action == A::ReconnectTransport || last.action == A::Relist || last.action == A::WaitForUser);
            reconnects += last.action == A::ReconnectTransport;
        }
        QCOMPARE(reconnects, doctor.Policy().foreignReconnects);
        QCOMPARE(int(last.diagnosis), int(D::ReplugCable));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        // An event (re-plug) renews the reconnects.
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 61000);
        QCOMPARE(int(doctor.Decide(offline, 61000).action), int(A::ReconnectTransport));
    }
    void defaultThatAlternatelyListsAndLosesThePhoneIsNamed()
    {
        // A8: shadow, hand back, shadow, hand back... is capped per episode.
        // Changed deliberately (iteration 5, 3): a hand-back counts when the
        // transport retires its private server (NoteHandOver), as here.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto lost = Blind(PhoneServerKind::BorrowedDefault);
        auto listing = Blind(PhoneServerKind::Owned);
        listing.shadowRunning = true;
        listing.defaultListsPhone = true;
        int handOvers = 0;
        PhoneLinkDecision last;
        std::int64_t now = 0;
        for (int round = 0; round < 10; ++round) {
            for (int i = 0; i < 200; ++i, now += 500) {
                last = doctor.Decide(lost, now);
                if (last.action != A::Relist) break;
            }
            if (last.action != A::StartShadowServer) break;
            ++handOvers;
            for (int i = 0; i < 200; ++i, now += 500) {
                last = doctor.Decide(listing, now);
                if (last.action != A::Relist) break;
            }
            if (last.action != A::HandBackToDefault) break;
            doctor.NoteHandOver();
            ++handOvers;
        }
        QCOMPARE(handOvers, doctor.Policy().handOverCap);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, now);
        QCOMPARE(int(doctor.Decide(listing, now).action), int(A::Relist)); // Blind grace first.
        QCOMPARE(int(doctor.Decide(listing, now + doctor.Policy().blindGraceMs).action), int(A::HandBackToDefault));
    }
    void handOverCapEndsWhereThePhoneIsListed()
    {
        // Iteration 4 (A3): the count can end on either side. Here it reaches
        // the cap on the private side (an odd count of the doctor's own
        // rungs, as after the transport's uncounted start beside a hung
        // default). At the cap the default that lists the phone is still
        // handed back to, and no more private servers are started.
        // Changed deliberately (iteration 5, 3): each executed hand-back is
        // noted by the transport (NoteHandOver), as here.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto lost = Blind(PhoneServerKind::BorrowedDefault);
        auto listing = Blind(PhoneServerKind::Owned);
        listing.shadowRunning = true;
        listing.defaultListsPhone = true;
        std::int64_t now = 0;
        const auto next = [&](const PhoneLinkObservation& o) {
            PhoneLinkDecision last;
            for (int i = 0; i < 400; ++i, now += 500) {
                last = doctor.Decide(o, now);
                if (last.action != A::Relist) break;
            }
            return last;
        };
        const auto handedBack = [&] {
            const auto last = next(listing);
            if (last.action == A::HandBackToDefault) doctor.NoteHandOver();
            return last;
        };
        QCOMPARE(int(handedBack().action), int(A::HandBackToDefault)); // 1
        QCOMPARE(int(next(lost).action), int(A::StartShadowServer));    // 2
        QCOMPARE(int(handedBack().action), int(A::HandBackToDefault)); // 3
        QCOMPARE(int(next(lost).action), int(A::StartShadowServer));    // 4: the cap, private side.
        QVERIFY(!doctor.HandOverAllowed());
        QCOMPARE(int(next(listing).action), int(A::HandBackToDefault)); // Still made at the cap.
        const auto named = next(lost);
        QCOMPARE(int(named.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(named.action), int(A::WaitForUser));
    }
    void authorizingGetsAShortGraceFirst()
    {
        // Changed deliberately (iteration 2, B3): authorizing is the Allow
        // handshake. It is never reset; after the grace the user is asked.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        const auto authorizing = With([](auto& o) { o.phoneState = PhoneAdbState::Authorizing; });
        QCOMPARE(int(doctor.Decide(authorizing, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(authorizing, 1000).action), int(A::Relist));
        for (std::int64_t now = 2000; now < 60000; now += 1000) {
            const auto decision = doctor.Decide(authorizing, now);
            QCOMPARE(int(decision.action), int(A::WaitForUser));
            QCOMPARE(int(decision.diagnosis), int(D::Authorizing));
        }
        // `connecting` is not the handshake: it keeps the reconnect ladder.
        PhoneLinkDoctor connectingDoctor;
        connectingDoctor.OnEvent(PhoneLinkEvent::DeviceArrival, 0);
        const auto connecting = With([](auto& o) { o.phoneState = PhoneAdbState::Connecting; });
        QCOMPARE(int(connectingDoctor.Decide(connecting, 0).action), int(A::Relist));
        QCOMPARE(int(connectingDoctor.Decide(connecting, 2000).action), int(A::ReconnectTransport));
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
        // Iteration 2 (A9): after the third restart of the episode the user
        // is asked to re-plug the cable; probing goes on, restarts stop.
        for (std::int64_t now = 81000; now < 300000; now += 1000) {
            const auto decision = doctor.Decide(blind, now);
            QCOMPARE(int(decision.diagnosis), int(D::ReplugCable));
            QCOMPARE(int(decision.action), int(A::WaitForUser));
            QCOMPARE_MS(decision.nextPollMs - now, doctor.Policy().userWaitPollMs);
        }
        // An event renews both grace and budget.
        doctor.OnEvent(PhoneLinkEvent::Resume, 301000);
        QCOMPARE(int(doctor.Decide(blind, 301000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(blind, 306000).action), int(A::RestartOwnedServer));
    }
    void windowsCannotTellNeverStopsAnotherServer()
    {
        // Renamed in iteration 4 (was blindWithoutWindowsInterfaceNeverEscalates).
        // Changed deliberately (iteration 3, A4): Windows cannot tell
        // (Unknown) is treated as present for OkuFlow's own server after a
        // long blind period: restarts and their cap, then the re-plug
        // sentence. Before that period, and for any other server, nothing.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto unknown = Blind();
        unknown.adbInterface = PhoneAdbInterface::Unknown;
        const auto period = doctor.Policy().unknownInterfaceBlindMs;
        int restarts = 0;
        PhoneLinkDecision last;
        for (std::int64_t now = 0; now < period + 300000; now += 1000) {
            last = doctor.Decide(unknown, now);
            if (now < period) QCOMPARE(int(last.action), int(A::Relist));
            restarts += last.action == A::RestartOwnedServer;
        }
        QCOMPARE(restarts, doctor.Policy().blindRestartCap);
        QCOMPARE(int(last.diagnosis), int(D::ReplugCable));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        // Changed deliberately (iteration 4, A4): a server OkuFlow did not
        // start. After the period OkuFlow may start its own private server
        // beside the blind default; that default is never asked to exit or
        // stopped, and the state ends in the re-plug sentence (it read
        // "Restoring" for ever before).
        PhoneLinkDoctor borrowedDoctor;
        borrowedDoctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto borrowed = Blind(PhoneServerKind::BorrowedDefault);
        borrowed.adbInterface = PhoneAdbInterface::Unknown;
        auto beside = Blind(PhoneServerKind::Owned);
        beside.adbInterface = PhoneAdbInterface::Unknown;
        beside.shadowRunning = true;
        std::int64_t now = 0;
        for (; now < period; now += 1000) QCOMPARE(int(borrowedDoctor.Decide(borrowed, now).action), int(A::Relist));
        QCOMPARE(int(borrowedDoctor.Decide(borrowed, now).action), int(A::StartShadowServer));
        for (std::int64_t end = now + 120000; now < end; now += 1000) {
            last = borrowedDoctor.Decide(beside, now);
            QVERIFY(last.action != A::AskDefaultServerToExit && last.action != A::TerminateDefaultServer);
        }
        QCOMPARE(int(last.diagnosis), int(D::ReplugCable));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        // The same for a hung default: never terminated under Unknown, the
        // re-plug sentence once it stayed unresponsive for the period.
        PhoneLinkDoctor hungDoctor;
        hungDoctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.adbInterface = PhoneAdbInterface::Unknown;
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        for (now = 0; now < period + 60000; now += 1000) {
            last = hungDoctor.Decide(hung, now);
            QVERIFY(last.action != A::TerminateDefaultServer);
            if (now < period) QCOMPARE(int(last.action), int(A::Relist));
        }
        QCOMPARE(int(last.diagnosis), int(D::ReplugCable));
        QCOMPARE(int(last.action), int(A::WaitForUser));
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
        // Changed deliberately (iteration 5, 5): such an endpoint may now end
        // in the re-plug sentence (WaitForUser); it is still never taken over.
        for (std::int64_t now = 0; now < 120000; now += 1000) {
            for (const auto& o : {blind, hung}) {
                const auto action = doctor.Decide(o, now).action;
                QVERIFY(action == A::Relist || action == A::WaitForUser);
            }
        }
    }
    void userEndpointThatDoesNotListThePhoneEndsInTheReplugSentence()
    {
        // Iteration 5 (5): responsive, Windows shows the phone, the user's
        // endpoint does not list it: after the blind grace the re-plug
        // sentence, never a takeover; a listing of the phone ends it.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto blind = Blind(PhoneServerKind::Override);
        const auto grace = doctor.Policy().blindGraceMs;
        for (std::int64_t now = 0; now < grace; now += 1000)
            QCOMPARE(int(doctor.Decide(blind, now).action), int(A::Relist));
        for (std::int64_t now = grace; now < 120000; now += 1000) {
            const auto named = doctor.Decide(blind, now);
            QCOMPARE(int(named.diagnosis), int(D::ReplugCable));
            QCOMPARE(int(named.action), int(A::WaitForUser));
            QCOMPARE_MS(named.nextPollMs - now, doctor.Policy().userWaitPollMs);
        }
        auto listed = Ready();
        listed.server = PhoneServerKind::Override;
        QCOMPARE(int(doctor.Decide(listed, 121000).diagnosis), int(D::Connected));
    }
    void hungUserEndpointEndsInTheReplugSentence()
    {
        // Iteration 5 (5): the user's endpoint hangs: never stopped; after
        // userEndpointUnresponsiveMs the re-plug sentence; an event restarts
        // the time.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::Override);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        const auto limit = doctor.Policy().userEndpointUnresponsiveMs;
        for (std::int64_t now = 0; now < limit; now += 1000) {
            const auto waiting = doctor.Decide(hung, now);
            QCOMPARE(int(waiting.diagnosis), int(D::ServerUnresponsive));
            QCOMPARE(int(waiting.action), int(A::Relist));
        }
        for (std::int64_t now = limit; now < limit + 60000; now += 1000) {
            const auto named = doctor.Decide(hung, now);
            QCOMPARE(int(named.diagnosis), int(D::ReplugCable));
            QCOMPARE(int(named.action), int(A::WaitForUser));
        }
        doctor.OnEvent(PhoneLinkEvent::Resume, 200000);
        QCOMPARE(int(doctor.Decide(hung, 200000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(hung, 200000 + limit).diagnosis), int(D::ReplugCable));
    }
    void confirmedHungDefaultIsTerminatedWithBudget()
    {
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        QCOMPARE(int(doctor.Decide(hung, 0).action), int(A::Relist)); // Not confirmed yet.
        hung.defaultUnresponsiveConfirmed = true;
        const auto first = doctor.Decide(hung, 1000);
        QCOMPARE(int(first.action), int(A::TerminateDefaultServer));
        QVERIFY(!first.hostKillAlreadySent); // A hung server never got host:kill: send it first.
        QCOMPARE(int(doctor.Decide(hung, 2000).action), int(A::Relist));
        doctor.NoteDefaultStopRefused();
        // Changed deliberately (iteration 2, A5): the refusal is named while
        // the probe still says unresponsive, and nothing is retried.
        for (std::int64_t now = 20000; now < 120000; now += 1000) {
            const auto refused = doctor.Decide(hung, now);
            QCOMPARE(int(refused.diagnosis), int(D::OtherProgramHoldsPhone));
            QCOMPARE(int(refused.action), int(A::WaitForUser));
        }
    }
    void handBackDeferredByALiveSessionIsNotCounted()
    {
        // Iteration 5 (3): HandBackToDefault decided while a live session
        // keeps the private server is not carried out; it counts toward the
        // hand-over cap only once the transport retires that server.
        PhoneLinkPolicy policy;
        policy.handOverCap = 2;
        PhoneLinkDoctor doctor(policy);
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto listing = Blind(PhoneServerKind::Owned);
        listing.shadowRunning = true;
        listing.defaultListsPhone = true;
        std::int64_t now = 0;
        int decided = 0;
        for (; now < 60000 && decided < 3; now += 500)
            decided += doctor.Decide(listing, now).action == A::HandBackToDefault; // Deferred twice...
        QCOMPARE(decided, 3);
        doctor.NoteHandOver(); // ...then executed once.
        QVERIFY(doctor.HandOverAllowed());
        const auto lost = Blind(PhoneServerKind::BorrowedDefault);
        PhoneLinkDecision last;
        for (int i = 0; i < 200; ++i, now += 500) {
            last = doctor.Decide(lost, now);
            if (last.action != A::Relist) break;
        }
        QCOMPARE(int(last.action), int(A::StartShadowServer));
    }
    void deathWithAHangingListenerKeepsTheImmediateRelaunch()
    {
        // Iteration 6 (6): in one decision the app process is gone (which
        // releases the hold) and the listener still hangs (which holds again
        // without launching). The once-per-episode immediate relaunch is not
        // spent there: the next decision with the app dead launches at once.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto notRunning = With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::Yes, PhoneFact::No, PhoneFact::Yes, PhoneFact::No}; });
        auto closed = notRunning;
        closed.shell.appRunning = PhoneFact::Yes;
        auto deadAndHanging = notRunning;
        deadAndHanging.companion = PhoneCompanionProbe::TimedOut;
        deadAndHanging.adbd = PhoneAdbdHealth::Answers;
        QCOMPARE(int(doctor.Decide(notRunning, 0).action), int(A::WakeAndLaunchApp));
        QCOMPARE(int(doctor.Decide(notRunning, 3000).action), int(A::WakeAndLaunchApp));
        QCOMPARE(int(doctor.Decide(closed, 9000).action), int(A::WaitForUser)); // Held.
        const auto both = doctor.Decide(deadAndHanging, 10000);
        QCOMPARE(int(both.diagnosis), int(D::CompanionNotResponding));
        QCOMPARE(int(both.action), int(A::WaitForUser));
        QCOMPARE(int(doctor.Decide(notRunning, 11000).action), int(A::WakeAndLaunchApp));
        // That launch spent it: a second death in the episode waits the
        // periodic spacing after the last launch.
        QCOMPARE(int(doctor.Decide(closed, 17000).action), int(A::WaitForUser));
        QVERIFY(doctor.Decide(notRunning, 18000).action != A::WakeAndLaunchApp);
        QCOMPARE(int(doctor.Decide(notRunning, 11000 + doctor.Policy().launchPeriodicMs).action), int(A::WakeAndLaunchApp));
    }
    void handOverOfAnEarlierEpisodeIsDropped()
    {
        // Iteration 6 (5): a start beside the default or a hand-back noted
        // after a newer event belongs to the old episode and is dropped.
        PhoneLinkPolicy policy;
        policy.handOverCap = 1;
        PhoneLinkDoctor doctor(policy);
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto earlier = doctor.Episode();
        doctor.OnEvent(PhoneLinkEvent::ManualPick, 1000);
        QVERIFY(!doctor.NoteHandOver(earlier));
        QVERIFY(doctor.HandOverAllowed());
        QVERIFY(doctor.NoteHandOver(doctor.Episode()));
        QVERIFY(!doctor.HandOverAllowed());
    }
    void abortedTerminationDoesNotSpendTheBudget()
    {
        // Iteration 5 (3), changed deliberately in iteration 6 (1): a
        // confirmed hang whose pre-stop listing answers (also with FAIL) is
        // not terminated and does not spend the termination cap. The aborted
        // stops have an end of their own: the other-program sentence after
        // abortedStopCap of them, polled at the user-wait cadence and
        // ended by a server that recovers or goes away.
        PhoneLinkDoctor doctor;
        const auto policy = doctor.Policy();
        QVERIFY(policy.abortedStopCap > 1);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        // Aborts below their own cap leave the whole termination cap.
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        std::int64_t now = 0;
        int aborted = 0, carried = 0;
        PhoneLinkDecision last;
        hung.defaultUnresponsiveConfirmed = true;
        for (; now < 600000 && last.diagnosis != D::OtherProgramHoldsPhone; now += 1000) {
            last = doctor.Decide(hung, now);
            if (last.action != A::TerminateDefaultServer) continue;
            if (aborted < policy.abortedStopCap - 1) {
                ++aborted;
                doctor.NoteTerminationAborted(last.episode, false);
            } else {
                ++carried;
            }
        }
        QCOMPARE(aborted, policy.abortedStopCap - 1);
        QCOMPARE(carried, policy.terminateCap);
        QCOMPARE(int(last.action), int(A::WaitForUser));

        // Every stop aborted (the transport confirms the hang again 5 s after
        // each abort): the sentence comes after abortedStopCap aborts.
        doctor.OnEvent(PhoneLinkEvent::Resume, 1000000);
        now = 1000000;
        aborted = 0;
        std::int64_t confirmedAt = now + 5000;
        last = {};
        for (; now < 1600000; now += 1000) {
            hung.defaultUnresponsiveConfirmed = now >= confirmedAt;
            last = doctor.Decide(hung, now);
            if (last.diagnosis == D::OtherProgramHoldsPhone) break;
            if (last.action == A::TerminateDefaultServer) {
                ++aborted;
                doctor.NoteTerminationAborted(last.episode, false);
                confirmedAt = now + 5000;
            }
        }
        QCOMPARE(aborted, policy.abortedStopCap);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        // It stands whether or not the hang is confirmed at the moment, and
        // probing goes on at the user-wait cadence.
        for (int i = 0; i < 60; ++i) {
            now += policy.userWaitPollMs;
            hung.defaultUnresponsiveConfirmed = i % 2 == 0;
            const auto waiting = doctor.Decide(hung, now);
            QCOMPARE(int(waiting.diagnosis), int(D::OtherProgramHoldsPhone));
            QCOMPARE(int(waiting.action), int(A::WaitForUser));
            QCOMPARE_MS(waiting.nextPollMs, now + policy.userWaitPollMs);
        }
        // A server that recovers ends it...
        const auto recovered = Blind(PhoneServerKind::BorrowedDefault);
        now += policy.userWaitPollMs;
        QCOMPARE(int(doctor.Decide(recovered, now).diagnosis), int(D::NotListedByServer));
        // ...and so does one that goes away.
        auto gone = Blind(PhoneServerKind::BorrowedDefault);
        gone.serverProbe = PhoneServerProbe::NoListener;
        now += policy.userWaitPollMs;
        QCOMPARE(int(doctor.Decide(gone, now).diagnosis), int(D::ServerStarting));

        // An event starts over, and aborts of an earlier episode count for
        // nothing (neither given back nor toward the end).
        const auto earlier = doctor.Episode();
        doctor.OnEvent(PhoneLinkEvent::Resume, 2000000);
        for (int i = 0; i < policy.abortedStopCap; ++i) doctor.NoteTerminationAborted(earlier, false);
        hung.defaultUnresponsiveConfirmed = true;
        QCOMPARE(int(doctor.Decide(hung, 2000000).action), int(A::TerminateDefaultServer));
        // Carried-out stops still end at the termination cap.
        carried = 1;
        for (now = 2001000; now < 2400000; now += 1000) {
            last = doctor.Decide(hung, now);
            carried += last.action == A::TerminateDefaultServer;
        }
        QCOMPARE(carried, policy.terminateCap);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        doctor.NoteTerminationAborted(earlier, false);
        QCOMPARE(int(doctor.Decide(hung, 2401000).diagnosis), int(D::OtherProgramHoldsPhone));
    }
    void abortedStopsOfOneRungDoNotEndTheOther()
    {
        // Iteration 7 (2): the hung branch's aborted terminations and the
        // takeover branch's unsent asks and aborted terminations are counted
        // separately: a default that was slow only in its version probes
        // (hung branch at its cap, the sentence) and then answers them again
        // but stays blind is still asked to exit beside OkuFlow's server, and
        // the reverse.
        PhoneLinkDoctor doctor;
        const auto policy = doctor.Policy();
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        auto beside = Blind(PhoneServerKind::Owned);
        beside.shadowRunning = true;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        std::int64_t now = 0;
        int aborted = 0;
        PhoneLinkDecision last;
        for (; now < 600000 && last.diagnosis != D::OtherProgramHoldsPhone; now += 1000) {
            last = doctor.Decide(hung, now);
            if (last.action != A::TerminateDefaultServer) continue;
            QVERIFY(!last.hostKillAlreadySent);
            ++aborted;
            doctor.NoteTerminationAborted(last.episode, last.hostKillAlreadySent);
        }
        QCOMPARE(aborted, policy.abortedStopCap);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        // It answers its version probes again, still blind: a private server
        // beside it, then (after the takeover grace) an ask, not the sentence.
        QCOMPARE(int(doctor.Decide(Blind(PhoneServerKind::BorrowedDefault), now).action), int(A::Relist));
        now += policy.blindGraceMs;
        QCOMPARE(int(doctor.Decide(Blind(PhoneServerKind::BorrowedDefault), now).action), int(A::StartShadowServer));
        QCOMPARE(int(doctor.Decide(beside, now + 1000).action), int(A::Relist));
        now += 1000 + policy.takeoverGraceMs;
        const auto ask = doctor.Decide(beside, now);
        QCOMPARE(int(ask.diagnosis), int(D::NotListedByServer));
        QCOMPARE(int(ask.action), int(A::AskDefaultServerToExit));
        // Its termination after the ignored ask is the takeover's own (past
        // the termination spacing the hung branch reserved, too). Changed
        // deliberately in iteration 8 (B4): the hung branch's three aborted
        // terminations were given back but keep their spacing, which has
        // grown to the ceiling (it was the short retry before).
        now += std::max(policy.hostKillWaitMs, policy.takeoverCeilingMs);
        const auto takeoverStop = doctor.Decide(beside, now);
        QCOMPARE(int(takeoverStop.action), int(A::TerminateDefaultServer));
        QVERIFY(takeoverStop.hostKillAlreadySent);

        // The reverse: unsent asks end the takeover, and a default that hangs
        // afterwards is still terminated (its first stop is not named).
        doctor.OnEvent(PhoneLinkEvent::Resume, 1000000);
        now = 1000000;
        int asks = 0;
        last = {};
        for (; now < 1600000; now += 500) {
            last = doctor.Decide(beside, now);
            if (last.diagnosis == D::OtherProgramHoldsPhone) break;
            if (last.action == A::AskDefaultServerToExit) {
                ++asks;
                doctor.NoteExitRequestNotSent(last.episode);
            }
        }
        QCOMPARE(asks, policy.abortedStopCap);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        now += 1000;
        const auto hungStop = doctor.Decide(hung, now);
        QCOMPARE(int(hungStop.diagnosis), int(D::ServerUnresponsive));
        QCOMPARE(int(hungStop.action), int(A::TerminateDefaultServer));
    }
    void hungDefaultIsNotTerminatedWithoutTheWindowsInterface()
    {
        // C2: a slow but healthy default with no phone attached is never killed.
        for (auto present : {PhoneAdbInterface::Absent, PhoneAdbInterface::Unknown}) {
            PhoneLinkDoctor doctor;
            doctor.OnEvent(PhoneLinkEvent::Resume, 0);
            auto hung = Blind(PhoneServerKind::BorrowedDefault);
            hung.serverProbe = PhoneServerProbe::Unresponsive;
            hung.defaultUnresponsiveConfirmed = true;
            hung.adbInterface = present;
            for (std::int64_t now = 0; now < 120000; now += 1000)
                QVERIFY(doctor.Decide(hung, now).action != A::TerminateDefaultServer);
        }
    }
    void everyFailedStopOutcomeEndsInTheSentence()
    {
        // A6: identity changed / failed twice, or refused / not adb / unopenable once.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        QCOMPARE(int(doctor.Decide(hung, 0).action), int(A::TerminateDefaultServer));
        doctor.NoteDefaultStopFailed();
        QVERIFY(!doctor.DefaultStopRefused()); // One retry.
        QCOMPARE(int(doctor.Decide(hung, 15000).action), int(A::TerminateDefaultServer));
        doctor.NoteDefaultStopFailed();
        QVERIFY(doctor.DefaultStopRefused());
        const auto named = doctor.Decide(hung, 16000);
        QCOMPARE(int(named.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(named.action), int(A::WaitForUser));
        // Without any note, terminations are capped per episode too.
        PhoneLinkDoctor capped;
        capped.OnEvent(PhoneLinkEvent::AppStart, 0);
        int terminations = 0;
        PhoneLinkDecision last;
        for (std::int64_t now = 0; now < 600000; now += 1000) {
            last = capped.Decide(hung, now);
            terminations += last.action == A::TerminateDefaultServer;
        }
        QCOMPARE(terminations, capped.Policy().terminateCap);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
    }
    void stopRefusalEndsWithItsCause()
    {
        // A7: refusal is cleared once the phone is listed (or the transport
        // clears it when the listener is gone or changed).
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        doctor.NoteDefaultStopRefused();
        QCOMPARE(int(doctor.Decide(Blind(), 0).diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(doctor.Decide(Ready(), 1000).diagnosis), int(D::Connected));
        QVERIFY(!doctor.DefaultStopRefused());
        QCOMPARE(int(doctor.Decide(Blind(), 2000).diagnosis), int(D::NotListedByServer));
        doctor.NoteDefaultStopRefused();
        doctor.ClearDefaultStopRefused();
        QCOMPARE(int(doctor.Decide(Blind(), 3000).diagnosis), int(D::NotListedByServer));
    }
    void shadowedDefaultThatListsThePhoneIsHandedBack()
    {
        // A8: a default that lists the phone (also unauthorized) is used.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto shadow = Blind(PhoneServerKind::Owned);
        shadow.shadowRunning = true;
        QCOMPARE(int(doctor.Decide(shadow, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(shadow, 5000).action), int(A::Relist)); // Takeover grace.
        shadow.defaultListsPhone = true;
        QCOMPARE(int(doctor.Decide(shadow, 6000).action), int(A::HandBackToDefault));
        QCOMPARE(int(doctor.Decide(shadow, 7000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(shadow, 9000).action), int(A::HandBackToDefault));
        // Even a stop refusal from an earlier takeover gives way to the listing.
        doctor.NoteDefaultStopRefused();
        QCOMPARE(int(doctor.Decide(shadow, 12000).action), int(A::HandBackToDefault));
    }
    void defaultThatKeepsComingBackEndsInTheSentence()
    {
        // A2: OkuFlow's server blind beside another adb that comes back after
        // every host:kill (Android Studio restarting its adb): bounded.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto beside = Blind(PhoneServerKind::Owned);
        beside.shadowRunning = true;
        int asks = 0, terminations = 0;
        PhoneLinkDecision last;
        std::int64_t now = 0;
        for (int round = 0; round < 6; ++round) {
            // The other adb is beside us until it is asked to exit...
            for (int i = 0; i < 20; ++i, now += 1000) {
                last = doctor.Decide(beside, now);
                asks += last.action == A::AskDefaultServerToExit;
                terminations += last.action == A::TerminateDefaultServer;
                if (last.action == A::AskDefaultServerToExit) { now += 1000; break; }
            }
            // ...exits, and comes back a few seconds later.
            for (int i = 0; i < 3; ++i, now += 1000) doctor.Decide(Blind(PhoneServerKind::Owned), now);
        }
        for (int i = 0; i < 10; ++i, now += 1000) { // Back again, past the takeover grace.
            last = doctor.Decide(beside, now);
            asks += last.action == A::AskDefaultServerToExit;
        }
        QCOMPARE(asks, doctor.Policy().askExitCap);
        QCOMPARE(terminations, 0);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(last.action), int(A::WaitForUser));
    }
    void aNewListenerBesideIsAskedBeforeItIsTerminated()
    {
        // A2: the asked listener was replaced by another process before the
        // termination step: the new one gets the grace and host:kill first.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto beside = Blind(PhoneServerKind::Owned);
        beside.shadowRunning = true;
        QCOMPARE(int(doctor.Decide(beside, 0).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(beside, 6000).action), int(A::AskDefaultServerToExit));
        doctor.NoteDefaultListenerChanged();
        QCOMPARE(int(doctor.Decide(beside, 8000).action), int(A::Relist)); // Not terminated.
        QCOMPARE(int(doctor.Decide(beside, 14000).action), int(A::AskDefaultServerToExit));
    }
    void askThatSendsNothingIsNotRememberedAsSent()
    {
        // Iteration 6 (2): an AskDefaultServerToExit that sent no host:kill
        // (the pre-ask checks kept the server) is not remembered as sent: no
        // termination hostKillWaitMs later as if the request had been
        // ignored, and the ask is not used up (one ask per episode here).
        PhoneLinkPolicy policy;
        policy.askExitCap = 1;
        PhoneLinkDoctor doctor(policy);
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto beside = Blind(PhoneServerKind::Owned);
        beside.shadowRunning = true;
        QCOMPARE(int(doctor.Decide(beside, 0).action), int(A::Relist));
        const auto kept = doctor.Decide(beside, policy.takeoverGraceMs);
        QCOMPARE(int(kept.action), int(A::AskDefaultServerToExit));
        doctor.NoteExitRequestNotSent(kept.episode);
        std::int64_t now = policy.takeoverGraceMs + 500;
        for (; now < 2 * policy.takeoverGraceMs; now += 500) {
            const auto next = doctor.Decide(beside, now);
            QCOMPARE(int(next.action), int(A::Relist));
            QCOMPARE(int(next.diagnosis), int(D::NotListedByServer));
        }
        // Asked again at the ask spacing; this one is sent and ignored, so
        // termination follows it.
        QCOMPARE(int(doctor.Decide(beside, now).action), int(A::AskDefaultServerToExit));
        QCOMPARE(int(doctor.Decide(beside, now + policy.hostKillWaitMs).action), int(A::TerminateDefaultServer));

        // Asks that never send anything end in the other-program sentence
        // after abortedStopCap of them (an earlier episode's note counts for
        // nothing).
        const auto earlier = doctor.Episode();
        doctor.OnEvent(PhoneLinkEvent::Resume, 100000);
        doctor.NoteExitRequestNotSent(earlier);
        int asks = 0;
        PhoneLinkDecision last;
        for (now = 100000; now < 400000; now += 500) {
            last = doctor.Decide(beside, now);
            QVERIFY(last.action != A::TerminateDefaultServer);
            if (last.diagnosis == D::OtherProgramHoldsPhone) break;
            if (last.action == A::AskDefaultServerToExit) {
                ++asks;
                doctor.NoteExitRequestNotSent(last.episode);
            }
        }
        QCOMPARE(asks, policy.abortedStopCap);
        QCOMPARE(int(last.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(int(last.action), int(A::WaitForUser));
    }
    void askThatFindsTheDefaultGoneIsGivenBackUncounted()
    {
        // Iteration 7 (4): an ask whose default was already gone asked
        // nothing (given back, no termination follows) and kept nothing (not
        // an aborted stop): with one ask and one aborted stop per episode,
        // the next ask still comes, and neither cap ends it.
        PhoneLinkPolicy policy;
        policy.askExitCap = 1;
        policy.abortedStopCap = 1;
        PhoneLinkDoctor doctor(policy);
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto beside = Blind(PhoneServerKind::Owned);
        beside.shadowRunning = true;
        QCOMPARE(int(doctor.Decide(beside, 0).action), int(A::Relist));
        const auto first = doctor.Decide(beside, policy.takeoverGraceMs);
        QCOMPARE(int(first.action), int(A::AskDefaultServerToExit));
        doctor.NoteExitRequestNotNeeded(first.episode);
        std::int64_t now = policy.takeoverGraceMs + policy.hostKillWaitMs;
        QCOMPARE(int(doctor.Decide(beside, now).action), int(A::Relist)); // No termination.
        now = 2 * policy.takeoverGraceMs;
        const auto second = doctor.Decide(beside, now);
        QCOMPARE(int(second.diagnosis), int(D::NotListedByServer));
        QCOMPARE(int(second.action), int(A::AskDefaultServerToExit));
        // An earlier episode's note gives nothing back.
        const auto earlier = doctor.Episode();
        doctor.OnEvent(PhoneLinkEvent::Resume, 100000);
        QCOMPARE(int(doctor.Decide(beside, 100000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(beside, 100000 + policy.takeoverGraceMs).action), int(A::AskDefaultServerToExit));
        doctor.NoteExitRequestNotNeeded(earlier);
        QCOMPARE(int(doctor.Decide(beside, 100000 + 2 * policy.takeoverGraceMs).action), int(A::TerminateDefaultServer));
    }
    void defaultThatIsGoneAtEveryAskEndsInTheSentence()
    {
        // Council round 7: a default that answers listings but that the
        // listener lookup cannot identify (an adb bound to every address)
        // is "already gone" at every ask. Only the first such ask of an
        // episode is free; the following ones count, and the rung ends in
        // the other-program sentence instead of asking for ever.
        PhoneLinkPolicy policy;
        PhoneLinkDoctor doctor(policy);
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto beside = Blind(PhoneServerKind::Owned);
        beside.shadowRunning = true;
        std::int64_t now = 0;
        int asks = 0;
        bool named = false;
        for (; now < 600000 && !named; now += 1000) {
            const auto decision = doctor.Decide(beside, now);
            QVERIFY2(decision.action != A::TerminateDefaultServer, "A default that was never asked must not be terminated.");
            if (decision.action == A::AskDefaultServerToExit) {
                ++asks;
                doctor.NoteExitRequestNotNeeded(decision.episode);
            }
            named = decision.diagnosis == D::OtherProgramHoldsPhone;
            if (named) QCOMPARE(int(decision.action), int(A::WaitForUser));
        }
        QVERIFY2(named, "Repeated asks that find the default gone never reached a sentence.");
        QCOMPARE(asks, policy.abortedStopCap + 1); // One free, then the cap.
        // It stays named, and polls at the user-wait cadence.
        const auto later = doctor.Decide(beside, now + 60000);
        QCOMPARE(int(later.diagnosis), int(D::OtherProgramHoldsPhone));
        QCOMPARE(later.nextPollMs, now + 60000 + policy.userWaitPollMs);
        // A new event gives one free ask again.
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, now + 100000);
        const auto afterEvent = doctor.Decide(beside, now + 100000 + policy.takeoverGraceMs + policy.blindGraceMs);
        QVERIFY(afterEvent.diagnosis != D::OtherProgramHoldsPhone);
    }
    void ownedServerWhoseListingFailsIsRestartedThenNamed()
    {
        // Iteration 8 (A1): OkuFlow's own server answers host:version (its
        // manager keeps it) while its device listing fails. It was relisted
        // for ever; now the restart rung and its cap after the blind grace,
        // then the re-plug sentence, which stands; an event renews it.
        PhoneLinkDoctor doctor;
        const auto policy = doctor.Policy();
        doctor.OnEvent(PhoneLinkEvent::Resume, 0);
        auto silent = Blind();
        silent.serverProbe = PhoneServerProbe::Unresponsive;
        QCOMPARE(int(DiagnosePhoneLink(silent)), int(D::ServerUnresponsive));
        int restarts = 0;
        PhoneLinkDecision last;
        std::int64_t firstRestart = -1;
        for (std::int64_t now = 0; now < 400000; now += 500) {
            last = doctor.Decide(silent, now);
            if (last.action == A::RestartOwnedServer) {
                ++restarts;
                if (firstRestart < 0) firstRestart = now;
            }
        }
        QCOMPARE_MS(firstRestart, policy.blindGraceMs);
        QCOMPARE(restarts, policy.blindRestartCap);
        QCOMPARE(int(last.diagnosis), int(D::ReplugCable));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 500000);
        QCOMPARE(int(doctor.Decide(silent, 500000).action), int(A::Relist));
        QCOMPARE(int(doctor.Decide(silent, 500000 + policy.blindGraceMs).action), int(A::RestartOwnedServer));
    }
    void adbThatCannotStartBesideAnAnsweringDefaultIsNamed()
    {
        // Iteration 8 (A3): another program's adb answers on the default
        // port but is blind; OkuFlow's private server beside it cannot be
        // started. The end is the AdbCannotStart sentence, not the
        // other-program sentence after the hand-over cap.
        for (const int failures : {0, 1, 2}) {
            PhoneLinkDoctor doctor;
            doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
            auto blind = Blind(PhoneServerKind::BorrowedDefault);
            blind.serverLaunchFailures = failures;
            QCOMPARE(int(doctor.Decide(blind, 0).action), int(A::Relist));
            const auto decision = doctor.Decide(blind, doctor.Policy().blindGraceMs);
            if (failures < doctor.Policy().adbLaunchFailuresToName) {
                QCOMPARE(int(decision.action), int(A::StartShadowServer));
                QVERIFY(decision.diagnosis != D::AdbCannotStart);
            } else {
                QCOMPARE(int(decision.diagnosis), int(D::AdbCannotStart));
            }
        }
    }
    void adbThatCannotStartBesideADefaultIsTriedAgain()
    {
        // After council round 8's fixes: nothing but this rung starts a
        // private server beside an answering default. With the sentence up
        // the start is tried again at the ceiling spacing, uncharged, so the
        // sentence ends once adb.exe can start (it stood until OkuFlow was
        // restarted), and it never turns into the other-program sentence.
        PhoneLinkDoctor doctor;
        const auto policy = doctor.Policy();
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto blind = Blind(PhoneServerKind::BorrowedDefault);
        blind.serverLaunchFailures = policy.adbLaunchFailuresToName;
        std::vector<std::int64_t> starts;
        QCOMPARE(int(doctor.Decide(blind, 0).action), int(A::Relist)); // The blind grace runs first.
        const std::int64_t until = policy.blindGraceMs + (policy.handOverCap + 3) * policy.restartCeilingMs;
        for (std::int64_t now = policy.blindGraceMs; now <= until; now += 1000) {
            const auto decision = doctor.Decide(blind, now);
            QCOMPARE(int(decision.diagnosis), int(D::AdbCannotStart));
            QVERIFY(decision.action == A::StartShadowServer || decision.action == A::WaitForUser);
            QCOMPARE_MS(decision.nextPollMs - now, policy.userWaitPollMs);
            if (decision.action == A::StartShadowServer) starts.push_back(now);
        }
        QVERIFY2(int(starts.size()) > policy.handOverCap, "The start beside the default must be tried again past the hand-over cap.");
        for (size_t i = 1; i < starts.size(); ++i) QCOMPARE_MS(starts[i] - starts[i - 1], policy.restartCeilingMs);
        // OkuFlow's adb answers again: the ordinary rung, the sentence gone.
        blind.serverLaunchFailures = 0;
        QVERIFY(doctor.Decide(blind, until + policy.restartCeilingMs).diagnosis != D::AdbCannotStart);
    }
    void silentPrivateServerBesideADefaultThatServesThePhoneIsHandedBack()
    {
        // Iteration 9 (A7): OkuFlow's private server beside the default does
        // not answer (ServerStarting) or its listing fails (an owned
        // ServerUnresponsive) while the default lists the phone. Neither
        // diagnosis looked at the default: OkuFlow stayed on its broken
        // server for good ("cannot start" or the re-plug sentence). It is
        // handed back under the hand-back budget; never to a default whose
        // listing of the phone is stalled.
        for (const auto probe : {PhoneServerProbe::NoListener, PhoneServerProbe::Unresponsive}) {
            PhoneLinkDoctor doctor;
            const auto policy = doctor.Policy();
            doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
            auto silent = Blind();
            silent.serverProbe = probe;
            silent.shadowRunning = true;
            silent.defaultListsPhone = true;
            silent.serverLaunchFailures = policy.adbLaunchFailuresToName; // Also with the sentence due.
            const auto first = doctor.Decide(silent, 0);
            QCOMPARE(int(first.action), int(A::HandBackToDefault));
            // Spaced by the hand-back budget (a live session can defer it).
            QCOMPARE(int(doctor.Decide(silent, 1).action), int(A::Relist));
            QCOMPARE(int(doctor.Decide(silent, policy.reconnectRetryMs).action), int(A::HandBackToDefault));
            auto stalled = silent;
            stalled.defaultPhoneStalled = true;
            PhoneLinkDoctor other;
            other.OnEvent(PhoneLinkEvent::AppStart, 0);
            for (std::int64_t now = 0; now < 120000; now += 1000)
                QVERIFY(other.Decide(stalled, now).action != A::HandBackToDefault);
            auto notListing = silent;
            notListing.defaultListsPhone = false;
            PhoneLinkDoctor third;
            third.OnEvent(PhoneLinkEvent::AppStart, 0);
            for (std::int64_t now = 0; now < 120000; now += 1000)
                QVERIFY(third.Decide(notListing, now).action != A::HandBackToDefault);
        }
    }
    void restartDeferredForALiveSessionIsGivenBack()
    {
        // Iteration 9 (B2): a restart the transport defers because a live
        // session uses the private server restarted nothing. It spent one of
        // the three restarts whose end is the re-plug sentence.
        PhoneLinkDoctor doctor;
        const auto policy = doctor.Policy();
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto blind = Blind();
        int restarts = 0;
        for (std::int64_t now = 0; now < 1000000; now += 500) {
            const auto decision = doctor.Decide(blind, now);
            if (decision.action != A::RestartOwnedServer) continue;
            ++restarts;
            if (restarts <= policy.blindRestartCap) doctor.NoteRestartDeferred(decision.episode);
        }
        // The deferred ones did not count: the cap's restarts still follow.
        QCOMPARE(restarts, 2 * policy.blindRestartCap);
        QCOMPARE(int(doctor.Decide(blind, 1000000).diagnosis), int(D::ReplugCable));
        // A give-back of an earlier episode is dropped.
        PhoneLinkDoctor later;
        later.OnEvent(PhoneLinkEvent::AppStart, 0);
        std::uint64_t old = 0;
        int counted = 0;
        for (std::int64_t now = 0; now < 1000000; now += 500) {
            const auto decision = later.Decide(blind, now);
            if (decision.action != A::RestartOwnedServer) continue;
            ++counted;
            if (counted == 1) { old = decision.episode; later.OnEvent(PhoneLinkEvent::DeviceArrival, now); }
            else later.NoteRestartDeferred(old);
        }
        QCOMPARE(counted, 1 + policy.blindRestartCap);
    }
    void terminationCapKeepsTheSentenceWithoutAConfirmation()
    {
        // Iteration 8 (A6): every termination clears the hang confirmation
        // (the transport forgets the stopped listener). Once the cap is
        // spent, the sentence stands also while a respawned default is not
        // confirmed hung yet; it read "Restoring" until a new confirmation.
        PhoneLinkDoctor doctor;
        const auto policy = doctor.Policy();
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        int terminations = 0;
        std::int64_t now = 0;
        for (; now < 600000 && terminations < policy.terminateCap; now += 1000) {
            hung.defaultUnresponsiveConfirmed = true;
            if (doctor.Decide(hung, now).action == A::TerminateDefaultServer) ++terminations;
        }
        QCOMPARE(terminations, policy.terminateCap);
        hung.defaultUnresponsiveConfirmed = false; // A new process, not confirmed yet.
        for (int i = 0; i < 30; ++i, now += 1000) {
            const auto decision = doctor.Decide(hung, now);
            QCOMPARE(int(decision.diagnosis), int(D::OtherProgramHoldsPhone));
            QCOMPARE(int(decision.action), int(A::WaitForUser));
        }
    }
    void givenBackAttemptsKeepTheSpacingGrowing()
    {
        // Iteration 8 (B4): a give-back lowers the count toward the cap but
        // not the spacing: the third reservation waits the ceiling, as it
        // does without give-backs (it waited the short retry every time).
        PhoneLinkDoctor doctor;
        const auto policy = doctor.Policy();
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        std::vector<std::int64_t> at;
        for (std::int64_t now = 0; now < 600000 && at.size() < 3; now += 500) {
            const auto decision = doctor.Decide(hung, now);
            if (decision.action != A::TerminateDefaultServer) continue;
            at.push_back(now);
            doctor.NoteTerminationAborted(decision.episode, false);
        }
        QCOMPARE(int(at.size()), 3);
        QCOMPARE_MS(at[1] - at[0], policy.restartRetryMs);
        QCOMPARE_MS(at[2] - at[1], policy.takeoverCeilingMs);
    }
    void resumeSignalsOfOneWakeAreOneEvent()
    {
        // Iteration 8 (B5): one wake raises up to four resume signals; each
        // renewed every cap. Within resumeCoalesceMs of the last applied
        // resume they are one event (measured from the applied one).
        PhoneLinkDoctor doctor;
        const auto policy = doctor.Policy();
        QVERIFY(doctor.OnEvent(PhoneLinkEvent::Resume, 0));
        const auto episode = doctor.Episode();
        QVERIFY(doctor.ReserveLaunch(0));
        QVERIFY(!doctor.ReserveLaunch(1));
        QVERIFY(!doctor.OnEvent(PhoneLinkEvent::Resume, 1000));
        QVERIFY(!doctor.OnEvent(PhoneLinkEvent::Resume, policy.resumeCoalesceMs - 1));
        QCOMPARE(doctor.Episode(), episode);
        // The launch budget was not renewed by the coalesced signals.
        QVERIFY(!doctor.ReserveLaunch(2000));
        // Another kind of event is never coalesced; a later resume applies.
        QVERIFY(doctor.OnEvent(PhoneLinkEvent::DeviceArrival, policy.resumeCoalesceMs - 1));
        QVERIFY(doctor.OnEvent(PhoneLinkEvent::Resume, policy.resumeCoalesceMs));
        QVERIFY(doctor.Episode() == episode + 2);
    }
    void aFlappingCableIsNamed()
    {
        // Iteration 8 (B5): a cable or hub that drops more often than the
        // blind grace never let a grace pass. flapRemovals removals within
        // flapWindowMs name the cable until the link holds, a quiet period
        // passes, or a manual pick or resume starts a fresh attempt.
        PhoneLinkDoctor doctor;
        const auto policy = doctor.Policy();
        const auto blind = Blind();
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        // An ordinary quick re-plug (and a USB mode switch) is not flapping.
        doctor.OnEvent(PhoneLinkEvent::DeviceRemoval, 1000);
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 1500);
        doctor.OnEvent(PhoneLinkEvent::DeviceRemoval, 4000);
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 4500);
        QCOMPARE(int(doctor.Decide(blind, 4500).diagnosis), int(D::NotListedByServer));
        // Removals spread wider than the window are not flapping either.
        PhoneLinkDoctor spread;
        for (int i = 0; i < 8; ++i) spread.OnEvent(PhoneLinkEvent::DeviceRemoval, i * (policy.flapWindowMs / 2));
        QCOMPARE(int(spread.Decide(blind, 8 * (policy.flapWindowMs / 2)).diagnosis), int(D::NotListedByServer));

        const auto flap = [&](PhoneLinkDoctor& d, std::int64_t from) {
            for (int i = 0; i < policy.flapRemovals; ++i) {
                d.OnEvent(PhoneLinkEvent::DeviceRemoval, from + i * 3000);
                d.OnEvent(PhoneLinkEvent::DeviceArrival, from + i * 3000 + 1000);
            }
            return from + (policy.flapRemovals - 1) * 3000 + 1000;
        };
        std::int64_t now = flap(doctor, 10000);
        for (std::int64_t t = now; t < now + policy.flapQuietMs - 3000; t += 1000) {
            const auto decision = doctor.Decide(blind, t);
            QCOMPARE(int(decision.diagnosis), int(D::ReplugCable));
            QCOMPARE(int(decision.action), int(A::WaitForUser));
        }
        // A phone the server reaches at this moment keeps its own sentence
        // and ladder: the cable verdict does not hide what the phone waits
        // for (it sent a user who re-plugged a few times back to the cable).
        const auto unauthorized = With([](auto& o) {
            o.phoneState = PhoneAdbState::Unauthorized;
            o.adbd = PhoneAdbdHealth::Unknown; o.companion = PhoneCompanionProbe::Unknown;
        });
        QCOMPARE(int(doctor.Decide(unauthorized, now).diagnosis), int(D::Unauthorized));
        QCOMPARE(int(doctor.Decide(Problem("paused"), now).diagnosis), int(D::CompanionPaused));
        QCOMPARE(int(doctor.Decide(blind, now).diagnosis), int(D::ReplugCable));
        // A quiet period ends it.
        QVERIFY(doctor.Decide(blind, now + policy.flapQuietMs).diagnosis != D::ReplugCable);
        // A manual pick ends it at once.
        now = flap(doctor, now + 100000);
        QCOMPARE(int(doctor.Decide(blind, now).diagnosis), int(D::ReplugCable));
        doctor.OnEvent(PhoneLinkEvent::ManualPick, now + 1);
        QVERIFY(doctor.Decide(blind, now + 1).diagnosis != D::ReplugCable);
        // A resume too.
        now = flap(doctor, now + 100000);
        QCOMPARE(int(doctor.Decide(blind, now).diagnosis), int(D::ReplugCable));
        doctor.OnEvent(PhoneLinkEvent::Resume, now + 1);
        QVERIFY(doctor.Decide(blind, now + 1).diagnosis != D::ReplugCable);
        // A working link is shown, and once it holds the verdict ends.
        now = flap(doctor, now + 100000);
        QCOMPARE(int(doctor.Decide(Ready(), now).diagnosis), int(D::Connected));
        QCOMPARE(int(doctor.Decide(Ready(), now + policy.connectedHoldMs).diagnosis), int(D::Connected));
        QVERIFY(doctor.Decide(blind, now + policy.connectedHoldMs + 1000).diagnosis != D::ReplugCable);
    }
    void replugSentenceOfTheOwnedOfflineLadderStands()
    {
        // Iteration 8 (B10): after the third restart of the owned offline
        // ladder the re-plug sentence flipped back to "Restoring" whenever a
        // reconnect slot came free. Once it stands it stays until an event.
        PhoneLinkDoctor doctor;
        const auto policy = doctor.Policy();
        doctor.OnEvent(PhoneLinkEvent::Resume, 0);
        const auto offline = With([](auto& o) { o.phoneState = PhoneAdbState::Offline; });
        std::int64_t now = 0;
        int restarts = 0;
        for (; now < 1000000; now += 500) {
            const auto decision = doctor.Decide(offline, now);
            restarts += decision.action == A::RestartOwnedServer;
            if (decision.diagnosis == D::ReplugCable) break;
        }
        QCOMPARE(restarts, policy.blindRestartCap);
        for (const auto end = now + 300000; now < end; now += 500) {
            const auto decision = doctor.Decide(offline, now);
            QCOMPARE(int(decision.diagnosis), int(D::ReplugCable));
            QCOMPARE(int(decision.action), int(A::WaitForUser));
        }
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, now);
        QCOMPARE(int(doctor.Decide(offline, now).action), int(A::ReconnectTransport));
    }
    void anotherListedPhoneIsNotBlindness()
    {
        // C4: a second phone listed by the server is not "blind": no restarts.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto other = Blind();
        other.otherUsbListed = true;
        for (std::int64_t now = 0; now < 120000; now += 1000) {
            const auto decision = doctor.Decide(other, now);
            QCOMPARE(int(decision.diagnosis), int(D::WantedPhoneNotAttached));
            QCOMPARE(int(decision.action), int(A::WaitForUser));
        }
    }
    void hangingCompanionIsNamedAfterBoundedLaunches()
    {
        // A15: the listener accepts and never answers.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto hanging = With([](auto& o) {
            o.companion = PhoneCompanionProbe::TimedOut; o.adbd = PhoneAdbdHealth::Answers;
            o.shell = {PhoneFact::Yes, PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes}; });
        int launches = 0;
        PhoneLinkDecision last;
        for (std::int64_t now = 0; now < 300000; now += 1000) {
            last = doctor.Decide(hanging, now);
            launches += last.action == A::WakeAndLaunchApp;
        }
        QCOMPARE(launches, doctor.Policy().hangingCompanionLaunches);
        QCOMPARE(int(last.diagnosis), int(D::CompanionNotResponding));
        QCOMPARE(int(last.action), int(A::WaitForUser));
        doctor.OnEvent(PhoneLinkEvent::ManualPick, 300000);
        QCOMPARE(int(doctor.Decide(hanging, 300000).action), int(A::WakeAndLaunchApp));
    }
    void oldCompanionWhilePausedIsLaunchedOnlyOnEvents()
    {
        // D1: Pause in the companion installed today: no listener, the probe
        // is refused, the app process runs. One launch per event, no loop.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto paused = With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::Yes, PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes}; });
        int launches = 0;
        for (std::int64_t now = 0; now < 300000; now += 1000) {
            const auto decision = doctor.Decide(paused, now);
            launches += decision.action == A::WakeAndLaunchApp;
            QCOMPARE(int(decision.diagnosis), int(D::CompanionListenerClosed));
        }
        QCOMPARE(launches, 1);
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 300000);
        QCOMPARE(int(doctor.Decide(paused, 300000).action), int(A::WakeAndLaunchApp));
    }
    void companionFailureIsNamedAfterAGrace()
    {
        // Iteration 3 (A5): busy camera, failed, no mode: "Restoring" while
        // the companion retries, then the user is told; an event restarts it.
        for (const char* code : {"busy", "failed", "noMode"}) {
            PhoneLinkDoctor doctor;
            doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
            const auto failed = Problem(code);
            const auto grace = doctor.Policy().companionFailedGraceMs;
            for (std::int64_t now = 0; now < grace; now += 1000)
                QCOMPARE(int(doctor.Decide(failed, now).action), int(A::Relist));
            for (std::int64_t now = grace; now < grace + 60000; now += 1000) {
                const auto decision = doctor.Decide(failed, now);
                QCOMPARE(int(decision.diagnosis), int(D::CompanionFailed));
                QCOMPARE(int(decision.action), int(A::WaitForUser));
            }
            doctor.OnEvent(PhoneLinkEvent::ManualPick, grace + 60000);
            QCOMPARE(int(doctor.Decide(failed, grace + 60000).action), int(A::Relist));
        }
    }
    void stopOutcomeOfAnEarlierEpisodeIsDropped()
    {
        // Iteration 3 (C4): a refusal noted after a newer event does not
        // leak into the new episode.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto hung = Blind(PhoneServerKind::BorrowedDefault);
        hung.serverProbe = PhoneServerProbe::Unresponsive;
        hung.defaultUnresponsiveConfirmed = true;
        const auto decision = doctor.Decide(hung, 0);
        QCOMPARE(int(decision.action), int(A::TerminateDefaultServer));
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 100); // While the stop ran.
        QVERIFY(!doctor.NoteDefaultStopRefused(decision.episode));
        QVERIFY(!doctor.NoteDefaultStopFailed(decision.episode));
        QVERIFY(!doctor.DefaultStopRefused());
        QCOMPARE(int(doctor.Decide(hung, 200).action), int(A::TerminateDefaultServer));
        // The current episode's outcome is kept.
        QVERIFY(doctor.NoteDefaultStopRefused(doctor.Episode()));
        QVERIFY(doctor.DefaultStopRefused());
    }
    void sessionStartSharesThePerEventLaunchBudget()
    {
        // C3: the session start path and the ladder draw on one budget.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::ManualPick, 0);
        QVERIFY(doctor.ReserveLaunch(0));   // The start path launches first.
        QVERIFY(!doctor.ReserveLaunch(100)); // Not again at once.
        const auto paused = With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes, PhoneFact::Yes}; });
        // Changed deliberately (iteration 4, A5): just after that launch the
        // refusal is a launch still starting; it is held once the settle
        // window passed.
        QCOMPARE(int(doctor.Decide(paused, 1000).action), int(A::Relist));
        for (std::int64_t now = doctor.Policy().launchSettleMs; now < 120000; now += 1000) {
            QCOMPARE(int(doctor.Decide(paused, now).action), int(A::WaitForUser));
            QVERIFY(!doctor.ReserveLaunch(now)); // Held until the next event.
        }
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 120000);
        QVERIFY(doctor.ReserveLaunch(120000));
        // A killed app (process gone) keeps the ordinary spaced budget.
        PhoneLinkDoctor killed;
        killed.OnEvent(PhoneLinkEvent::AppStart, 0);
        QVERIFY(killed.ReserveLaunch(0));
        QCOMPARE(int(killed.Decide(Refused(PhoneFact::Yes, PhoneFact::No), 500).action), int(A::Relist));
        QCOMPARE(int(killed.Decide(Refused(PhoneFact::Yes, PhoneFact::No), 3000).action), int(A::WakeAndLaunchApp));
        QVERIFY(!killed.ReserveLaunch(3500));
    }
    void launchStillStartingIsNotHeld()
    {
        // Iteration 4 (A5): after OkuFlow's own launch the app process runs
        // before its listener opens. A refusal then is not "listener closed":
        // nothing is held, a start that fails is retried at the ordinary
        // spacing, and it reads as the app being started.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto killed = Refused(PhoneFact::Yes, PhoneFact::No);
        const auto starting = With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::Yes, PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes}; });
        QCOMPARE(int(doctor.Decide(killed, 0).action), int(A::WakeAndLaunchApp));
        const auto settling = doctor.Decide(starting, 1000);
        QCOMPARE(int(settling.action), int(A::Relist));
        QCOMPARE(int(settling.diagnosis), int(D::AppNotRunning));
        QCOMPARE(int(doctor.Decide(killed, 2000).action), int(A::Relist)); // The start failed.
        QCOMPARE(int(doctor.Decide(killed, 3000).action), int(A::WakeAndLaunchApp)); // Ordinary spacing.
        // A refusal that outlasts the window with the process running is held.
        const auto settle = doctor.Policy().launchSettleMs;
        QCOMPARE(int(doctor.Decide(starting, 3000 + settle).diagnosis), int(D::CompanionListenerClosed));
        QCOMPARE(int(doctor.Decide(starting, 3000 + settle).action), int(A::WaitForUser));
        QVERIFY(!doctor.ReserveLaunch(3000 + settle + 20000));
    }
    void pausedCompanionThatDiesIsRelaunched()
    {
        // Iteration 4 (A5): paused, launched once and held; then the app
        // process dies. The same doctor relaunches it instead of waiting for
        // an event.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto paused = With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::Yes, PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes}; });
        QCOMPARE(int(doctor.Decide(paused, 0).action), int(A::WakeAndLaunchApp));
        for (std::int64_t now = 1000; now < 10000; now += 1000)
            QCOMPARE(int(doctor.Decide(paused, now).action), int(A::WaitForUser));
        QVERIFY(!doctor.ReserveLaunch(10000));
        const auto died = doctor.Decide(Refused(PhoneFact::Yes, PhoneFact::No), 11000);
        QCOMPARE(int(died.diagnosis), int(D::AppNotRunning));
        QCOMPARE(int(died.action), int(A::WakeAndLaunchApp));
    }
    void pausedCompanionThatKeepsDyingIsRelaunchedAtTheOrdinarySpacing()
    {
        // Iteration 5 (2): pause, death, relaunch, pause, death. Pause is
        // stored on the phone and survives the kill, so a battery manager
        // that keeps killing the app would pull it to the front every time:
        // only the first release in an episode relaunches at once.
        PhoneLinkPolicy policy;
        PhoneLinkDoctor doctor(policy);
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto paused = With([](auto& o) {
            o.companion = PhoneCompanionProbe::Refused;
            o.shell = {PhoneFact::Yes, PhoneFact::No, PhoneFact::Yes, PhoneFact::Yes}; });
        const auto died = Refused(PhoneFact::Yes, PhoneFact::No);
        QCOMPARE(int(doctor.Decide(paused, 0).action), int(A::WakeAndLaunchApp));
        QCOMPARE(int(doctor.Decide(paused, 9000).action), int(A::WaitForUser));
        const std::int64_t relaunchMs = 11000;
        QCOMPARE(int(doctor.Decide(died, relaunchMs).action), int(A::WakeAndLaunchApp)); // First release: at once.
        for (std::int64_t now = 12000; now < 18000; now += 1000) doctor.Decide(paused, now); // Paused again: held.
        QCOMPARE(int(doctor.Decide(paused, 18000).action), int(A::WaitForUser));
        for (std::int64_t now = 19000; now < relaunchMs + policy.launchPeriodicMs; now += 1000) {
            const auto again = doctor.Decide(died, now);
            QCOMPARE(int(again.diagnosis), int(D::AppNotRunning));
            QVERIFY2(again.action != A::WakeAndLaunchApp, QByteArray::number(now).constData());
        }
        QCOMPARE(int(doctor.Decide(died, relaunchMs + policy.launchPeriodicMs).action), int(A::WakeAndLaunchApp));
        // A new event starts a new episode: the first release is at once again.
        doctor.OnEvent(PhoneLinkEvent::DeviceArrival, 40000);
        QCOMPARE(int(doctor.Decide(paused, 40000).action), int(A::WakeAndLaunchApp));
        doctor.Decide(paused, 49000);
        QCOMPARE(int(doctor.Decide(died, 50000).action), int(A::WakeAndLaunchApp));
    }
    void adbThatCannotStartIsNamed()
    {
        // A16: OkuFlow's own adb.exe cannot be launched (quarantine, missing).
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto starting = With([](auto& o) {
            o.server = PhoneServerKind::Owned; o.serverProbe = PhoneServerProbe::NoListener;
            o.phoneState = PhoneAdbState::NotListed; });
        starting.serverLaunchFailures = 1;
        QCOMPARE(int(doctor.Decide(starting, 0).diagnosis), int(D::ServerStarting));
        starting.serverLaunchFailures = 2;
        const auto named = doctor.Decide(starting, 1000);
        QCOMPARE(int(named.diagnosis), int(D::AdbCannotStart));
        QCOMPARE(int(named.action), int(A::WaitForUser));
        starting.serverLaunchFailures = 0;
        QCOMPARE(int(doctor.Decide(starting, 2000).diagnosis), int(D::ServerStarting));
    }
    void flappingCompanionIsNotRelaunchedOnEveryFlip()
    {
        // C8: ready / refused / ready / refused within seconds.
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        const auto killed = Refused(PhoneFact::Yes, PhoneFact::No);
        int launches = 0;
        for (std::int64_t now = 0; now < 60000; now += 1000)
            launches += doctor.Decide((now / 1000) % 2 ? Ready() : killed, now).action == A::WakeAndLaunchApp;
        // The ordinary launch budget (0, 3, 9, 29, 49 s), not one per flip (30).
        QVERIFY2(launches <= 5, QByteArray::number(launches).constData());
        // A live session renews at once.
        auto streaming = Ready();
        streaming.sessionStreaming = true;
        doctor.Decide(streaming, 60000);
        QCOMPARE(int(doctor.Decide(killed, 60500).action), int(A::WakeAndLaunchApp));
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
        // Changed deliberately (iteration 2, A12): the sibling window is
        // blamed only after the blind grace; before that "Restoring".
        PhoneLinkDoctor doctor;
        doctor.OnEvent(PhoneLinkEvent::AppStart, 0);
        auto held = Blind();
        held.otherInstanceRunning = true;
        for (std::int64_t now = 0; now < 30000; now += 1000) {
            const auto decision = doctor.Decide(held, now);
            QVERIFY(decision.action != A::RestartOwnedServer); // Never restarts against a sibling.
            if (now < doctor.Policy().blindGraceMs) {
                QCOMPARE(int(decision.diagnosis), int(D::NotListedByServer));
                QCOMPARE(int(decision.action), int(A::Relist));
            } else {
                QCOMPARE(int(decision.diagnosis), int(D::OtherInstanceHoldsPhone));
                QCOMPARE(int(decision.action), int(A::WaitForUser));
            }
        }
        // A sibling while the phone is listed is not blamed for anything.
        auto listed = Ready();
        listed.otherInstanceRunning = true;
        QCOMPARE(int(doctor.Decide(listed, 31000).diagnosis), int(D::Connected));
    }
};

QTEST_APPLESS_MAIN(PhoneLinkDoctorTests)
#include "phone_link_doctor_tests.moc"
