#include <QtTest>
#include "okuflow/common/gpu_copy_lease.hpp"

using namespace okuflow;

class GpuCopyLeaseTests : public QObject {
    Q_OBJECT
private slots:
    void producerRemainsOwnedUntilObservedCompletion()
    {
        auto producer = std::make_shared<int>(42);
        std::weak_ptr<int> observable = producer;
        GpuCopyLease copy;
        QVERIFY(copy.Begin(producer, 100));
        producer.reset();
        QVERIFY(!observable.expired());
        QVERIFY(!copy.Poll(GpuCopyCompletion::Pending, 120));
        QVERIFY(!observable.expired());
        QVERIFY(!copy.Begin(std::make_shared<int>(43), 120));
        QVERIFY(copy.Poll(GpuCopyCompletion::Complete, 125));
        QVERIFY(observable.expired());
        QVERIFY(!copy.Pending());
        QVERIFY(copy.Begin(std::make_shared<int>(43), 130));
    }

    void deadlineDoesNotReleaseProducerOrPermitReuse()
    {
        auto producer = std::make_shared<int>(42);
        std::weak_ptr<int> observable = producer;
        GpuCopyLease copy;
        QVERIFY(copy.Begin(producer, 100));
        producer.reset();
        QVERIFY(!copy.Poll(GpuCopyCompletion::Pending, 1100));
        QVERIFY(copy.Failed());
        QVERIFY(copy.Pending());
        QVERIFY(!observable.expired());
        QVERIFY(!copy.Begin(std::make_shared<int>(43), 1101));
        // A fault is sticky, including a late apparent completion.
        QVERIFY(!copy.Poll(GpuCopyCompletion::Complete, 1102));
        QVERIFY(!observable.expired());
    }

    void eventRecordOrQueryFailureRetainsSource()
    {
        auto producer = std::make_shared<int>(42);
        std::weak_ptr<int> observable = producer;
        GpuCopyLease copy;
        QVERIFY(copy.Begin(producer, 100));
        producer.reset();
        QVERIFY(!copy.Poll(GpuCopyCompletion::Failed, 101));
        QVERIFY(copy.Failed());
        QVERIFY(!observable.expired());
    }

    void emptyLeaseCannotAuthorizeCopy()
    {
        GpuCopyLease copy;
        QVERIFY(!copy.Begin({}, 0));
        QVERIFY(!copy.Pending());
        QVERIFY(copy.Poll(GpuCopyCompletion::Complete, 0));
    }
};

QTEST_APPLESS_MAIN(GpuCopyLeaseTests)
#include "gpu_copy_lease_tests.moc"
