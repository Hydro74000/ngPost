#include <QtTest>
#include "../../../src/vpn/WindowsServiceControl.h"
using namespace WindowsServiceControl;
class TestWindowsServiceControl : public QObject {
    Q_OBJECT
private slots:
    void stopIsNotCompletion() {
        State state = State::Running;
        int requests = 0;
        auto query = [&] { return state; };
        auto request = [&] { ++requests; return true; };
        QCOMPARE(stopStep(query, request), StopResult::Pending);
        QCOMPARE(requests, 1);
        state = State::StopPending;
        QCOMPARE(stopStep(query, request), StopResult::Pending);
        QCOMPARE(requests, 1);
        state = State::Stopped;
        QCOMPARE(stopStep(query, request), StopResult::Stopped);
        QCOMPARE(requests, 1);
    }
    void deniedAndUnknownNeverMeanStopped() {
        QCOMPARE(stopStep([] { return State::Running; }, [] { return false; }), StopResult::Failed);
        QCOMPARE(stopStep([] { return State::Unknown; }, [] { return true; }), StopResult::Failed);
    }
    void waitForStartupBeforeStopping() {
        int requests = 0;
        QCOMPARE(stopStep([] { return State::StartPending; }, [&] { ++requests; return true; }), StopResult::Pending);
        QCOMPARE(requests, 0);
    }
    void missingServiceIsStopped() {
#ifdef Q_OS_WIN
        QString error;
        auto name = "ngPost-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(WindowsServiceControl::query(name, &error), State::Stopped);
        QVERIFY(!startDemand(name, &error));
        QVERIFY(!error.isEmpty());
#else
        QSKIP("Native SCM requires Windows");
#endif
    }
};
QTEST_APPLESS_MAIN(TestWindowsServiceControl)
#include "tst_WindowsServiceControl.moc"
