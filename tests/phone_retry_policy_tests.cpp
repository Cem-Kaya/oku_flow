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
        policy.Failed(5000);
        policy.Observe(1, true, "", 7);
        QVERIFY(!policy.CanAttempt(5001));
        // adb transport IDs restart at one after a new server is launched.
        policy.Observe(1, true, "", 8);
        QVERIFY(policy.CanAttempt(5001));
    }
    void failureBackoffAndRecovery() {
        PhoneRetryPolicy policy;
        policy.Observe(7, true, "");
        policy.Failed(0);
        QVERIFY(!policy.CanAttempt(4999));
        QVERIFY(policy.CanAttempt(5000));
        policy.Observe(7, true, ""); // A continuously ready endpoint does not reset a failure.
        policy.Failed(5000);
        QVERIFY(!policy.CanAttempt(14999));
        QVERIFY(policy.CanAttempt(15000));
        policy.Failed(15000);
        QVERIFY(!policy.CanAttempt(34999));
        policy.Failed(35000);
        QVERIFY(!policy.CanAttempt(94999));
        policy.Observe(8, false, ""); // Replug: transport identities are not persisted.
        QVERIFY(policy.CanAttempt(35001));
        policy.Failed(35001);
        policy.Observe(8, true, "");
        QVERIFY(policy.CanAttempt(35002));
    }
    void busyAndProblemChanges() {
        PhoneRetryPolicy policy;
        policy.Observe(3, false, "busy");
        policy.Failed(100, true);
        QVERIFY(!policy.CanAttempt(30099));
        QVERIFY(policy.CanAttempt(30100));
        policy.Observe(3, false, "");
        QVERIFY(policy.CanAttempt(101));
        policy.Failed(101);
        policy.Succeeded();
        QVERIFY(policy.CanAttempt(102));
    }
};
QTEST_APPLESS_MAIN(PhoneRetryPolicyTests)
#include "phone_retry_policy_tests.moc"
