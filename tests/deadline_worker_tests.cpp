#include "okuflow/common/deadline_worker.hpp"
#include <QtTest>
#include <future>
#include <atomic>

using namespace std::chrono_literals;
using Worker = okuflow::DeadlineWorker;

class DeadlineWorkerTests : public QObject {
    Q_OBJECT
private slots:
    void completedTasksPublishResultsAndAreSerial()
    {
        Worker worker;
        int value = 0;
        std::thread::id first;
        QCOMPARE(worker.Execute([&] { value = 21; first = std::this_thread::get_id(); }, 1s), Worker::Result::Completed);
        QCOMPARE(worker.Execute([&] { if (first == std::this_thread::get_id()) value *= 2; }, 1s), Worker::Result::Completed);
        QCOMPARE(value, 42);
    }
    void deadlineDoesNotCancelOrReleaseAnOwnedTask()
    {
        auto worker = std::make_unique<Worker>();
        auto owner = std::make_shared<int>(42);
        std::weak_ptr<int> weak = owner;
        auto release = std::make_shared<std::promise<void>>();
        auto wait = release->get_future().share();
        auto entered = std::make_shared<std::promise<void>>();
        auto enteredFuture = entered->get_future();
        std::promise<void> finished;
        auto finishedFuture = finished.get_future();
        const auto start = std::chrono::steady_clock::now();
        const auto result = worker->Execute([owner, wait, entered, &finished] {
            entered->set_value(); wait.wait(); finished.set_value();
        }, 20ms);
        owner.reset();
        const auto retry = worker->Execute([] {}, 20ms);
        worker.reset(); // Must not join a stuck third-party call.
        const bool retained = !weak.expired();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        release->set_value();
        QVERIFY(finishedFuture.wait_for(2s) == std::future_status::ready);
        QCOMPARE(result, Worker::Result::TimedOut);
        QCOMPARE(retry, Worker::Result::Rejected);
        QVERIFY(retained);
        QVERIFY(elapsed < 500ms);
    }
    void exceptionsAreReportedWithoutEscapingTheWorker()
    {
        Worker worker;
        QCOMPARE(worker.Execute([] { throw std::runtime_error("fixture"); }, 1s), Worker::Result::Threw);
    }
};
QTEST_APPLESS_MAIN(DeadlineWorkerTests)
#include "deadline_worker_tests.moc"
