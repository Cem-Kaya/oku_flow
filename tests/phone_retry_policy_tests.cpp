#include "okuflow/capture/phone_retry_policy.hpp"
#include <QtTest>
using okuflow::PhoneRetryPolicy;
class PhoneRetryPolicyTests : public QObject {
    Q_OBJECT
private slots:
    void serverReplacementResetsReusedTransportBackoff() {
        PhoneRetryPolicy policy;
        policy.Observe(1, true, "", 7);
        policy.Failed(0);
        policy.Failed(1000);
        policy.Observe(1, true, "", 7);
        QVERIFY(!policy.CanAttempt(1001));
        // adb transport IDs restart at one after a new server is launched.
        policy.Observe(1, true, "", 8);
        QVERIFY(policy.CanAttempt(1001));
    }
    // Deliberately changed with the reconnect ladder: the old 5/10/20/60 s
    // stack (30 s while busy) is the owner's complaint. Now 1/2/3/5 s with a
    // 10 s ceiling, 5 s while the phone reports busy, and events reset it.
    void failureBackoffAndRecovery() {
        PhoneRetryPolicy policy;
        policy.Observe(7, true, "");
        policy.Failed(0);
        QVERIFY(!policy.CanAttempt(999));
        QVERIFY(policy.CanAttempt(1000));
        policy.Observe(7, true, ""); // A continuously ready endpoint does not reset a failure.
        policy.Failed(1000);
        QVERIFY(!policy.CanAttempt(2999));
        QVERIFY(policy.CanAttempt(3000));
        policy.Failed(3000);
        QVERIFY(!policy.CanAttempt(5999));
        policy.Failed(6000);
        QVERIFY(!policy.CanAttempt(10999));
        QVERIFY(policy.CanAttempt(11000));
        policy.Failed(11000);
        policy.Failed(21000);
        QVERIFY(!policy.CanAttempt(30999));
        QVERIFY(policy.CanAttempt(31000)); // Ceiling.
        policy.Observe(8, false, ""); // Replug: transport identities are not persisted.
        QVERIFY(policy.CanAttempt(21001));
        policy.Failed(21001);
        policy.Observe(8, true, "");
        QVERIFY(policy.CanAttempt(21002));
    }
    void busyAndProblemChanges() {
        PhoneRetryPolicy policy;
        policy.Observe(3, false, "busy");
        policy.Failed(100, true);
        QVERIFY(!policy.CanAttempt(5099));
        QVERIFY(policy.CanAttempt(5100));
        policy.Observe(3, false, "");
        QVERIFY(policy.CanAttempt(101));
        policy.Failed(101);
        policy.Succeeded();
        QVERIFY(policy.CanAttempt(102));
    }
    void eventResetStartsAFreshLadder() {
        PhoneRetryPolicy policy;
        policy.Observe(3, true, "");
        for (int i = 0; i < 6; ++i) policy.Failed(i * 10000);
        QVERIFY(!policy.CanAttempt(50001));
        policy.Reset();
        QVERIFY(policy.CanAttempt(50001));
    }
};
QTEST_APPLESS_MAIN(PhoneRetryPolicyTests)
#include "phone_retry_policy_tests.moc"
