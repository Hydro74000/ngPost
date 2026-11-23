// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// QueueScheduler on a FakeQueueExecutor: strict order, preparation and
// transfer slots, budget, the full "post now" rule, pause, stops and
// stale acknowledgements (plan_queue.md §3.4, §3.5).
//
//========================================================================

#include "QueueTestHelpers.h"
#include "queue/FakeQueueExecutor.h"
#include "queue/InMemoryQueueStore.h"
#include "queue/QueueScheduler.h"

#include <QSignalSpy>
#include <QtTest>

#include <limits>
#include <memory>

using namespace queue;
using queuetest::makeOptions;
using queuetest::settle;
using Kind = FakeQueueExecutor::Command::Kind;

class tst_QueueScheduler : public QObject
{
    Q_OBJECT

private:
    struct Fixture
    {
        InMemoryQueueStore store;
        PostQueue queue{ &store, QStringLiteral("session") };
        FakeQueueExecutor executor;
        std::unique_ptr<QueueScheduler> scheduler;
        qint64 freeBytes = -1; //!< < 0: unlimited

        explicit Fixture(int preparedMax = 2)
        {
            QueueScheduler::Budget budget;
            budget.preparedMax = preparedMax;
            budget.marginBytes = 100;
            budget.freeBytes = [this](const QString &) {
                return freeBytes < 0 ? std::numeric_limits<qint64>::max() : freeBytes;
            };
            scheduler = std::make_unique<QueueScheduler>(&queue, &executor, budget);
        }
        qint64 add(const QString &name, bool compress = false, bool par2 = false)
        {
            return queue.submit(makeOptions(name, compress, par2), Origin::Auto).postIds.value(0);
        }
        Lifecycle state(qint64 id) const { return queue.find(id)->lifecycle; }
        //! Runs every step of the preparation of @p id to success.
        void finishPreparation(qint64 id)
        {
            for (int i = 0; i < 4 && state(id) == Lifecycle::Preparing; ++i) {
                QVERIFY(executor.ackStep(id, true, 100));
                settle();
            }
        }
        void finishTransfer(qint64 id)
        {
            QVERIFY(executor.ackTransferFinished(id));
            settle();
            QVERIFY(executor.ackFinalized(id));
            settle();
        }
    };

private slots:
    void readyHeadStartsAtOnce()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        settle();
        QCOMPARE(f.state(a), Lifecycle::Posting);
        QCOMPARE(f.executor.count(Kind::Activate, a), 1);
        f.finishTransfer(a);
        QVERIFY(!f.queue.find(a)); // retired after F3
    }

    void strictOrderWaitsForTheHeadPreparation()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true, true);
        const qint64 b = f.add(QStringLiteral("b")); // ready, but behind a
        settle();
        QCOMPARE(f.state(a), Lifecycle::Preparing);
        QVERIFY(f.executor.last(Kind::Prepare, a));
        QCOMPARE(f.executor.last(Kind::Prepare, a)->step, PrepStep::Compress);
        QCOMPARE(f.state(b), Lifecycle::Queued);
        QCOMPARE(f.executor.count(Kind::Activate), 0);
        QVERIFY(f.executor.ackStep(a, true, 300));
        settle();
        QVERIFY(f.executor.last(Kind::Prepare, a));
        QCOMPARE(f.executor.last(Kind::Prepare, a)->step, PrepStep::Par2);
        QVERIFY(f.executor.ackStep(a, true, 30));
        settle();
        QVERIFY(f.executor.last(Kind::Prepare, a));
        QCOMPARE(f.executor.last(Kind::Prepare, a)->step, PrepStep::Collect);
        QVERIFY(f.executor.ackStep(a));
        settle();
        QCOMPARE(f.state(a), Lifecycle::Posting);
        QCOMPARE(f.queue.find(a)->artifacts.bytes, qint64(330));
        QCOMPARE(f.queue.find(a)->artifacts.generation, 1);
        QCOMPARE(f.state(b), Lifecycle::Queued);
    }

    void pauseFallsBetweenTwoSteps()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true, true);
        settle();
        f.queue.setPaused(true);
        QVERIFY(f.executor.ackStep(a, true, 300)); // rar ends while paused
        settle();
        QCOMPARE(f.executor.count(Kind::Prepare, a), 1); // par2 does not start
        f.queue.setPaused(false);
        settle();
        QVERIFY(f.executor.last(Kind::Prepare, a));
        QCOMPARE(f.executor.last(Kind::Prepare, a)->step, PrepStep::Par2);
    }

    void pauseHoldsTheActiveTransfer()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        settle();
        f.queue.setPaused(true);
        settle();
        QCOMPARE(f.executor.count(Kind::Pause, a), 1);
        f.finishTransfer(a);
        QCOMPARE(f.state(b), Lifecycle::Queued); // nothing new while paused
        f.queue.setPaused(false);
        settle();
        QCOMPARE(f.state(b), Lifecycle::Posting);
    }

    void nextEntryIsPreparedWhileTheActivePosts()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"), true);
        settle();
        QCOMPARE(f.state(a), Lifecycle::Posting);
        QCOMPARE(f.state(b), Lifecycle::Queued); // anticipation waits for the real start
        QVERIFY(f.executor.ackTransferStarted(a));
        settle();
        QCOMPARE(f.state(b), Lifecycle::Preparing);
        f.finishPreparation(b);
        QCOMPARE(f.state(b), Lifecycle::Prepared);
        f.finishTransfer(a);
        QCOMPARE(f.state(b), Lifecycle::Posting);
    }

    void speculativePreparationRespectsTheCount()
    {
        Fixture f(0);
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"), true);
        settle();
        QCOMPARE(f.state(b), Lifecycle::Queued); // no speculative preparation at 0
        f.finishTransfer(a);
        QCOMPARE(f.state(b), Lifecycle::Preparing); // required once it is the head
    }

    void requiredPreparationNeedsSpaceAndNeverDeletes()
    {
        Fixture f;
        f.freeBytes = 500; // 1000 bytes of sources + margin do not fit
        const qint64 a = f.add(QStringLiteral("a"), true);
        settle();
        QCOMPARE(f.queue.find(a)->blocked.code, QStringLiteral("resources"));
        QCOMPARE(f.executor.count(Kind::Prepare), 0);
        f.freeBytes = 10000;
        f.queue.setRevalidateFunction([](const QueueEntry &) { return BlockReason{}; });
        QVERIFY(f.queue.revalidate(a, f.queue.view()).ok());
        settle();
        QCOMPARE(f.state(a), Lifecycle::Preparing);
    }

    void overtakenPreparationFinishesAndWaits()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"), true);
        const qint64 c = f.add(QStringLiteral("c"), true);
        settle();
        QVERIFY(f.executor.ackTransferStarted(a));
        settle();
        QCOMPARE(f.state(b), Lifecycle::Preparing);
        QVERIFY(f.queue.postNext({ c }, f.queue.view()).ok()); // c overtakes b
        settle();
        QCOMPARE(f.state(b), Lifecycle::Preparing); // not killed by the move
        f.finishPreparation(b);
        QCOMPARE(f.state(b), Lifecycle::Prepared);
        QCOMPARE(f.state(c), Lifecycle::Preparing); // the head's preparation is required
        f.finishTransfer(a);
        QCOMPARE(f.state(b), Lifecycle::Prepared); // ready, but behind c: it waits
        QCOMPARE(f.executor.count(Kind::Activate, b), 0);
        f.finishPreparation(c);
        QCOMPARE(f.state(c), Lifecycle::Posting);
    }

    void postNowPreparesTheTargetWhileTheActiveKeepsPosting()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"), true);
        const qint64 c = f.add(QStringLiteral("c"), true);
        settle();
        QVERIFY(f.executor.ackTransferStarted(a));
        settle();
        QCOMPARE(f.state(b), Lifecycle::Preparing); // speculative
        QVERIFY(f.queue.postNow(c, f.queue.view()).ok());
        settle();
        QVERIFY(f.executor.last(Kind::Interrupt, b));
        QCOMPARE(f.executor.last(Kind::Interrupt, b)->disposition, Disposition::Keep);
        QCOMPARE(f.state(b), Lifecycle::Quiescing);
        QCOMPARE(f.executor.count(Kind::Interrupt, a), 0); // the active keeps posting
        const QString bRun = f.queue.find(b)->runId;
        QVERIFY(f.executor.ackQuiesced(b));
        settle();
        QCOMPARE(f.state(b), Lifecycle::Queued);
        QVERIFY(f.queue.find(b)->runId != bRun); // successor run
        QCOMPARE(f.state(c), Lifecycle::Preparing);
        f.finishPreparation(c);
        QCOMPARE(f.state(c), Lifecycle::Prepared);
        QVERIFY(f.executor.last(Kind::Interrupt, a));
        QCOMPARE(f.executor.last(Kind::Interrupt, a)->disposition, Disposition::Keep);
        QCOMPARE(f.queue.intent().state, PostNowIntent::State::Switching);
        QVERIFY(f.executor.ackQuiesced(a, true, true));
        settle();
        QVERIFY(f.queue.intent().isNull());
        QCOMPARE(f.queue.order(), (QList<qint64>{ c, a, b }));
        QCOMPARE(f.state(c), Lifecycle::Posting);
        QVERIFY(f.queue.find(a)->transferManifestEngaged);
        QCOMPARE(f.queue.find(a)->runNo, 2);
        f.finishTransfer(c);
        QCOMPARE(f.state(a), Lifecycle::Posting); // resumes right after
    }

    void postNowOfAReadyTargetSwitchesAtOnce()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        settle();
        QVERIFY(f.queue.postNow(b, f.queue.view()).ok());
        settle();
        QCOMPARE(f.state(a), Lifecycle::Quiescing);
        QVERIFY(f.executor.ackQuiesced(a, true, true));
        settle();
        QCOMPARE(f.state(b), Lifecycle::Posting);
    }

    void postNowIsDroppedWhenTheActiveEndsFirst()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"), true);
        settle();
        QVERIFY(f.queue.postNow(b, f.queue.view()).ok());
        settle();
        QSignalSpy dropped(&f.queue, &PostQueue::intentDropped);
        f.finishTransfer(a); // ends naturally while b prepares
        QCOMPARE(dropped.count(), 1);
        QCOMPARE(dropped.at(0).at(1).toString(), QStringLiteral("active_changed"));
        f.finishPreparation(b);
        QCOMPARE(f.state(b), Lifecycle::Posting); // simply next, no resurrection of a
    }

    void switchKeepsTheOldActiveWhenTheTargetFails()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        settle();
        QVERIFY(f.queue.postNow(b, f.queue.view()).ok());
        settle();
        QCOMPARE(f.queue.intent().state, PostNowIntent::State::Switching);
        QVERIFY(
            f.queue.engineBlock(b, { QStringLiteral("sources_missing"), {} }, Lifecycle::Queued));
        QVERIFY(f.executor.ackQuiesced(a, true, true));
        settle();
        QCOMPARE(f.queue.order().first(), a);
        QCOMPARE(f.state(a), Lifecycle::Posting); // repart with its successor run
        QCOMPARE(f.queue.find(a)->runNo, 2);
        QVERIFY(f.executor.last(Kind::Activate, a));
        QCOMPARE(f.executor.last(Kind::Activate, a)->runId, f.queue.find(a)->runId);
    }

    void vpnPreflightBlocksTheSwitch()
    {
        Fixture f;
        f.scheduler->setSwitchPreflight([](const QueueEntry &, const QueueEntry &) {
            return QueueScheduler::Admission::Blocked;
        });
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        settle();
        QVERIFY(f.queue.postNow(b, f.queue.view()).ok());
        settle();
        QVERIFY(f.queue.intent().isNull());
        QCOMPARE(f.executor.count(Kind::Interrupt, a), 0); // never interrupted for nothing
    }

    void holdingTheActiveKeepsItForLater()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        settle();
        QCOMPARE(f.queue.setHeld({ a }, true, f.queue.view()).status,
                 QueueCommandResult::Status::InProgress);
        settle();
        QCOMPARE(f.state(a), Lifecycle::Quiescing);
        QVERIFY(f.executor.ackQuiesced(a, true, true));
        settle();
        QVERIFY(f.queue.find(a)->held);
        QVERIFY(f.queue.find(a)->transferManifestEngaged);
        QCOMPARE(f.state(b), Lifecycle::Posting);
    }

    void aCloseKeepsTheTransferAndThePreparation()
    {
        Fixture f(0);
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"), true);
        const qint64 c = f.add(QStringLiteral("c"));
        settle();
        QVERIFY(f.executor.ackTransferStarted(a));
        QVERIFY(f.queue.postNow(b, f.queue.view()).ok()); // b must be prepared first
        settle();
        QCOMPARE(f.state(b), Lifecycle::Preparing);
        QSignalSpy dropped(&f.queue, &PostQueue::intentDropped);
        f.scheduler->keepRunningForClose();
        QCOMPARE(dropped.count(), 1);
        QCOMPARE(dropped.at(0).at(1).toString(), QStringLiteral("closing"));
        QCOMPARE(f.executor.last(Kind::Interrupt, a)->disposition, Disposition::Keep);
        QCOMPARE(f.executor.last(Kind::Interrupt, b)->disposition, Disposition::Keep);
        QVERIFY(f.executor.ackQuiesced(a, true, true));
        QVERIFY(f.executor.ackQuiesced(b));
        settle();
        QCOMPARE(f.state(a), Lifecycle::Queued);
        QVERIFY(f.queue.find(a)->transferManifestEngaged);
        QVERIFY(!f.queue.find(a)->held); // kept, not suspended
        QCOMPARE(f.queue.find(a)->runNo, 2);
        QCOMPARE(f.state(b), Lifecycle::Queued);
        QCOMPARE(f.state(c), Lifecycle::Queued);
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, b, c }));
        QCOMPARE(f.executor.count(Kind::Activate, a), 1); // nothing started again
        QCOMPARE(f.executor.count(Kind::Activate, c), 0);
    }

    void aCloseCanLetTheTransferEnd()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"), true);
        settle();
        QVERIFY(f.executor.ackTransferStarted(a));
        settle();
        QCOMPARE(f.state(b), Lifecycle::Preparing); // speculative
        f.scheduler->keepRunningForClose(true);
        QCOMPARE(f.executor.count(Kind::Interrupt, a), 0);
        QCOMPARE(f.executor.count(Kind::Pause, a), 0); // not held either
        QVERIFY(f.executor.ackQuiesced(b));
        settle();
        f.finishTransfer(a);
        QVERIFY(!f.queue.find(a));
        QCOMPARE(f.state(b), Lifecycle::Queued); // nothing starts after it
    }

    void aCloseLetsASwitchUnderWayEnd()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        settle();
        QVERIFY(f.queue.postNow(b, f.queue.view()).ok());
        settle();
        QCOMPARE(f.queue.intent().state, PostNowIntent::State::Switching);
        f.scheduler->keepRunningForClose();
        QCOMPARE(f.executor.count(Kind::Interrupt, a), 1); // asked once, by the switch
        QVERIFY(f.executor.ackQuiesced(a, true, true));
        settle();
        QVERIFY(f.queue.intent().isNull());
        QVERIFY(!f.scheduler->isTechnicallyPaused());
        QCOMPARE(f.queue.order(), (QList<qint64>{ b, a }));
        QCOMPARE(f.state(b), Lifecycle::Queued); // nothing starts while closing
        f.scheduler->cancelClose();              // the close failed elsewhere
        settle();
        QCOMPARE(f.state(b), Lifecycle::Posting);
    }

    void aCloseStopsNothingWhileAWriteWaits()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        settle();
        QVERIFY(f.executor.ackTransferStarted(a));
        f.store.failNextCommits(2); // F1, then its retry
        QVERIFY(f.executor.ackTransferFinished(a));
        settle();
        QVERIFY(f.scheduler->isTechnicallyPaused());
        f.scheduler->retryAfterTechnicalPause();
        QVERIFY(f.scheduler->isTechnicallyPaused());
        QCOMPARE(f.state(a), Lifecycle::Posting); // F1 not written yet
        f.scheduler->keepRunningForClose();
        QCOMPARE(f.executor.count(Kind::Interrupt, a), 0); // its runtime already finalizes
        f.scheduler->retryAfterTechnicalPause();
        settle();
        QCOMPARE(f.state(a), Lifecycle::Finalizing);
        QVERIFY(f.executor.ackFinalized(a));
        settle();
        QVERIFY(!f.queue.find(a));
    }

    void aCloseAsksAStalledSwitchAgainAsASwitch()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        settle();
        // The switch is written, its stop is not.
        f.store.failCommitsWhere([a](const QueueTransaction &tx) {
            for (const QueueEntry &entry : tx.upserts)
                if (entry.postId == a && entry.lifecycle == Lifecycle::Quiescing)
                    return true;
            return false;
        });
        QVERIFY(f.queue.postNow(b, f.queue.view()).ok());
        settle();
        QCOMPARE(f.queue.intent().state, PostNowIntent::State::Switching);
        QCOMPARE(f.state(a), Lifecycle::Posting);
        QVERIFY(f.scheduler->isTechnicallyPaused());
        f.store.failCommitsWhere({});
        f.scheduler->retryAfterTechnicalPause();
        f.scheduler->keepRunningForClose();
        QCOMPARE(f.executor.count(Kind::Interrupt, a), 1);
        QVERIFY(f.executor.ackQuiesced(a, true, true));
        settle();
        QVERIFY(f.queue.intent().isNull()); // committed as a switch
        QCOMPARE(f.queue.order(), (QList<qint64>{ b, a }));
        QCOMPARE(f.state(b), Lifecycle::Queued);
    }

    void aCloseFromTheAdmissionStartsNoPreparation()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"), true);
        // A VPN refused while the head is admitted: its handler closes the queue.
        f.scheduler->setAdmission([&f, a](const QueueEntry &entry) {
            if (entry.postId != a)
                return QueueScheduler::Admission::Proceed;
            f.scheduler->keepRunningForClose();
            return QueueScheduler::Admission::Blocked;
        });
        settle();
        QVERIFY(f.queue.find(a)->blocked.isBlocked());
        QCOMPARE(f.state(b), Lifecycle::Queued);
        QCOMPARE(f.executor.count(Kind::Prepare, b), 0);
        QCOMPARE(f.executor.count(Kind::Activate, a), 0);
    }

    void removingTheActiveIsATerminalStop()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        settle();
        QVERIFY(f.queue.remove({ a }, RemovePolicy::Default, f.queue.view()).ok());
        settle();
        QVERIFY(f.executor.last(Kind::Interrupt, a));
        QCOMPARE(f.executor.last(Kind::Interrupt, a)->disposition, Disposition::Stop);
        f.finishTransfer(a);
        QVERIFY(!f.queue.find(a));
    }

    void cancellingAPreparationRetiresTheEntry()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        settle();
        QVERIFY(f.queue.remove({ a }, RemovePolicy::Default, f.queue.view()).ok());
        settle();
        QVERIFY(f.executor.ackStep(a)); // a step that was running ends meanwhile: ignored
        QVERIFY(f.executor.ackQuiesced(a));
        settle();
        QVERIFY(!f.queue.find(a));
    }

    void finalizationFailureBlocksUntilRetried()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        settle();
        QVERIFY(f.executor.ackTransferFinished(a));
        settle();
        QVERIFY(f.executor.ackFinalized(a, false, QStringLiteral("disk full")));
        settle();
        QCOMPARE(f.queue.find(a)->blocked.code, QStringLiteral("finalization_failed"));
        QCOMPARE(f.state(b), Lifecycle::Posting); // the blocked finalization frees the slot
        QVERIFY(f.queue.retryFinalization(a, f.queue.view()).ok());
        QCOMPARE(f.executor.count(Kind::RetryFinalization, a), 1);
        QVERIFY(f.executor.ackFinalized(a));
        settle();
        QVERIFY(!f.queue.find(a));
    }

    void unprovenStopBlocksWithoutCleanup()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        const qint64 b = f.add(QStringLiteral("b"), true); // needs the slot a holds
        settle();
        QVERIFY(f.queue.setHeld({ a }, true, f.queue.view()).ok());
        settle();
        QVERIFY(f.executor.ackQuiesced(a, false));
        settle();
        QCOMPARE(f.queue.find(a)->blocked.code, QStringLiteral("quiesce_failed"));
        QCOMPARE(f.state(a), Lifecycle::Quiescing);
        QCOMPARE(f.queue.find(a)->slot, Slot::Preparation); // the runtime keeps it
        QVERIFY(!f.queue.find(a)->artifacts.isNull());      // nothing released
        QVERIFY(f.scheduler->isTechnicallyPaused());
        f.scheduler->retryAfterTechnicalPause();
        settle();
        QVERIFY(f.scheduler->isTechnicallyPaused()); // the stop is still not proven
        QCOMPARE(f.executor.count(Kind::Prepare, b), 0);

        QVERIFY(f.executor.ackQuiesced(a, true)); // proven at last
        settle();
        QVERIFY(!f.scheduler->isTechnicallyPaused());
        QVERIFY(!f.queue.find(a)->blocked.isBlocked());
        QVERIFY(f.queue.find(a)->held);
        QCOMPARE(f.state(b), Lifecycle::Preparing);
    }

    void retirePolicyEndsAFailedPreparation()
    {
        Fixture f;
        f.scheduler->setPreparationFailurePolicy(QueueScheduler::PreparationFailure::Retire);
        const qint64 a = f.add(QStringLiteral("a"), true);
        const qint64 b = f.add(QStringLiteral("b"));
        settle();
        QVERIFY(f.executor.ackStep(a, false, 0, QStringLiteral("rar exit 2")));
        settle();
        QVERIFY(!f.queue.find(a));
        QCOMPARE(f.store.outcomeOf(a).result, QStringLiteral("failed"));
        QCOMPARE(f.store.outcomeOf(a).endCause, QStringLiteral("technical"));
        QVERIFY(f.store.outcomeOf(a).diagnosticJson.contains(QStringLiteral("rar exit 2")));
        QCOMPARE(f.state(b), Lifecycle::Posting); // the next one goes on
    }

    void retireThatCannotBeWrittenPauses()
    {
        Fixture f;
        f.scheduler->setPreparationFailurePolicy(QueueScheduler::PreparationFailure::Retire);
        const qint64 a = f.add(QStringLiteral("a"), true);
        settle();
        f.store.failNextCommits(1);
        QVERIFY(f.executor.ackStep(a, false));
        settle();
        QVERIFY(f.queue.find(a)); // nothing half done
        QVERIFY(f.scheduler->isTechnicallyPaused());
        f.scheduler->retryAfterTechnicalPause();
        settle();
        QVERIFY(!f.queue.find(a));
        QCOMPARE(f.store.outcomeOf(a).result, QStringLiteral("failed"));
    }

    void retirePolicyKeepsWhatOnlyItsRuntimeCanSettle()
    {
        Fixture f;
        f.scheduler->setPreparationFailurePolicy(QueueScheduler::PreparationFailure::Retire);
        const qint64 a = f.add(QStringLiteral("a"), true);
        settle();
        f.executor.setUnsettled(a); // sources still renamed
        QVERIFY(f.executor.ackStep(a, false));
        settle();
        QVERIFY(f.queue.find(a));
        QCOMPARE(f.queue.find(a)->blocked.code, QStringLiteral("preparation_failed"));
    }

    void aToolEndedUnprovenKeepsItsSlotAndPausesTheQueue()
    {
        Fixture f;
        f.scheduler->setPreparationFailurePolicy(QueueScheduler::PreparationFailure::Retire);
        const qint64 a = f.add(QStringLiteral("a"), true);
        const qint64 b = f.add(QStringLiteral("b"), true); // needs the slot a holds
        settle();
        const QString run = f.queue.find(a)->runId;
        QVERIFY(
            f.executor.ackStep(a, false, 0, stepError(kStepToolUnproven, QStringLiteral("rar"))));
        settle();
        // Not retired, not blocked as a failure: the tool may still write.
        QVERIFY(f.queue.find(a));
        QCOMPARE(f.queue.find(a)->blocked.code, QLatin1String(kStepToolUnproven));
        QCOMPARE(f.state(a), Lifecycle::Preparing);
        QCOMPARE(f.queue.find(a)->slot, Slot::Preparation);
        QCOMPARE(f.queue.find(a)->runId, run);
        QVERIFY(!f.queue.find(a)->artifacts.isNull());
        QVERIFY(f.scheduler->isTechnicallyPaused());
        QCOMPARE(f.scheduler->technicalPauseReason(), QLatin1String(kStepToolUnproven));
        f.scheduler->retryAfterTechnicalPause();
        settle();
        QVERIFY(f.scheduler->isTechnicallyPaused()); // nothing proves it stopped since
        QCOMPARE(f.executor.count(Kind::Prepare, b), 0);
        QCOMPARE(f.executor.count(Kind::Prepare, a), 1); // no next step either
    }

    void aRefusedSupervisionKeepsThePostForANewAttempt()
    {
        Fixture f;
        f.scheduler->setPreparationFailurePolicy(QueueScheduler::PreparationFailure::Retire);
        const qint64 a = f.add(QStringLiteral("a"), true);
        settle();
        const QString run = f.queue.find(a)->runId;
        QVERIFY(f.executor.ackStep(a, false, 0, stepError(kStepStoreFailed, QStringLiteral("db"))));
        settle();
        // The history refused, not the post: kept, unblocked, under a new run.
        QVERIFY(f.queue.find(a));
        QVERIFY(!f.queue.find(a)->blocked.isBlocked());
        QCOMPARE(f.state(a), Lifecycle::Queued);
        QVERIFY(f.queue.find(a)->runId != run);
        QVERIFY(f.scheduler->isTechnicallyPaused());
        QCOMPARE(f.scheduler->technicalPauseReason(), QLatin1String(kStepStoreFailed));
        f.scheduler->retryAfterTechnicalPause();
        settle();
        QVERIFY(!f.scheduler->isTechnicallyPaused());
        QCOMPARE(f.state(a), Lifecycle::Preparing); // prepared again
        QCOMPARE(f.executor.count(Kind::Prepare, a), 2);
    }

    void aRefusedSupervisionWithWorkStillHeldRenewsNothing()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        settle();
        const QString run = f.queue.find(a)->runId;
        f.executor.setUnsettled(a); // sources its runtime could not rename back
        QVERIFY(f.executor.ackStep(a, false, 0, stepError(kStepStoreFailed, QStringLiteral("db"))));
        settle();
        // A new run would be refused by the runtime still there: none starts.
        QCOMPARE(f.queue.find(a)->runId, run);
        QCOMPARE(f.state(a), Lifecycle::Preparing);
        QCOMPARE(f.queue.find(a)->slot, Slot::Preparation);
        QCOMPARE(f.queue.find(a)->blocked.code, QLatin1String(kStepStoreFailed));
        QVERIFY(f.scheduler->isTechnicallyPaused());
        f.scheduler->retryAfterTechnicalPause();
        settle();
        QVERIFY(f.scheduler->isTechnicallyPaused());
        QCOMPARE(f.executor.count(Kind::Prepare, a), 1);
    }

    void finalizationFollowsF1ThenF3()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        settle();
        const QString run = f.queue.find(a)->runId;
        QVERIFY(f.executor.ackTransferFinished(a));
        settle();
        // F1 recorded with Finalizing, then only F2 is asked.
        QCOMPARE(f.state(a), Lifecycle::Finalizing);
        QVERIFY(f.store.resultOf(run));
        QCOMPARE(f.store.resultOf(run)->result, QStringLiteral("success"));
        QCOMPARE(f.executor.count(Kind::Finalize, a), 1);
        QCOMPARE(f.executor.count(Kind::CompleteLocalWork, a), 0);
        QVERIFY(f.executor.ackFinalized(a));
        settle();
        // F3, then the local work: never before the entry left durably.
        QVERIFY(!f.queue.find(a));
        QCOMPARE(f.store.finalizationOf(run), FinalizationStep::Kind::Complete);
        QCOMPARE(f.executor.count(Kind::CompleteLocalWork, a), 1);
    }

    void aFailedF1AsksForNoF2()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        settle();
        f.store.failNextCommits(1);
        QVERIFY(f.executor.ackTransferFinished(a));
        settle();
        QCOMPARE(f.executor.count(Kind::Finalize, a), 0);
        QVERIFY(f.scheduler->isTechnicallyPaused());
        f.scheduler->retryAfterTechnicalPause();
        settle();
        QCOMPARE(f.executor.count(Kind::Finalize, a), 1); // asked once F1 holds
    }

    void aFailedF2BlocksThenRetryCompletes()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        settle();
        QVERIFY(f.executor.ackTransferFinished(a));
        settle();
        QVERIFY(f.executor.ackFinalized(a, false, QStringLiteral("disk full")));
        settle();
        QCOMPARE(f.queue.find(a)->blocked.code, QStringLiteral("finalization_failed"));
        QCOMPARE(f.executor.count(Kind::CompleteLocalWork, a), 0);
        QVERIFY(f.queue.retryFinalization(a, f.queue.view()).ok());
        settle();
        QCOMPARE(f.executor.count(Kind::RetryFinalization, a), 1);
        QVERIFY(f.executor.ackFinalized(a));
        settle();
        QVERIFY(!f.queue.find(a));
        QCOMPARE(f.executor.count(Kind::CompleteLocalWork, a), 1);
    }

    void failedWriteHoldsTheQueueUntilRetried()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        QSignalSpy pauses(f.scheduler.get(), &QueueScheduler::technicalPauseChanged);
        settle();
        QVERIFY(f.executor.ackStep(a, true, 300)); // compression
        settle();
        f.store.failNextCommits(1);
        QVERIFY(f.executor.ackStep(a, true, 10)); // collection: "prepared" is not written
        settle();
        QVERIFY(f.scheduler->isTechnicallyPaused());
        QCOMPARE(pauses.size(), 1);
        QCOMPARE(f.state(a), Lifecycle::Preparing);
        QCOMPARE(f.executor.count(Kind::Activate), 0);
        f.scheduler->reevaluate();
        QCOMPARE(f.executor.count(Kind::Activate), 0); // nothing starts meanwhile

        f.scheduler->retryAfterTechnicalPause();
        settle();
        QVERIFY(!f.scheduler->isTechnicallyPaused());
        QCOMPARE(pauses.size(), 2);
        QCOMPARE(f.state(a), Lifecycle::Posting);
        QCOMPARE(f.queue.find(a)->artifacts.bytes, qint64(310)); // recorded once
    }

    void acknowledgementsWaitBehindAFailedOne()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        settle();
        f.store.failNextCommits(1);
        QVERIFY(f.executor.ackTransferFinished(a)); // "finalizing" is not written
        settle();
        QVERIFY(f.executor.ackFinalized(a)); // must not retire before it
        settle();
        QVERIFY(f.queue.find(a));
        QCOMPARE(f.state(a), Lifecycle::Posting);
        f.scheduler->retryAfterTechnicalPause();
        settle();
        QVERIFY(!f.queue.find(a)); // both applied, in order
        QVERIFY(!f.scheduler->isTechnicallyPaused());
    }

    void stopIsAskedOnlyOnceQuiescingIsWritten()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        settle();
        QCOMPARE(f.state(a), Lifecycle::Posting);
        f.store.failNextCommits(1);
        QVERIFY(f.queue.remove({ a }, RemovePolicy::Default, f.queue.view()).ok());
        settle();
        QCOMPARE(f.executor.count(Kind::Interrupt, a), 0);
        QCOMPARE(f.state(a), Lifecycle::Posting);
        QVERIFY(f.scheduler->isTechnicallyPaused());
    }

    void failedActivationStartsNoTransfer()
    {
        Fixture f;
        f.store.failNextCommits(1);
        const qint64 a = f.add(QStringLiteral("a")); // admission itself fails
        QCOMPARE(a, qint64(0));
        const qint64 b = f.add(QStringLiteral("b"));
        f.store.failNextCommits(1); // the activation of b
        settle();
        QCOMPARE(f.executor.count(Kind::Activate), 0);
        QCOMPARE(f.state(b), Lifecycle::Queued);
        QVERIFY(f.scheduler->isTechnicallyPaused());
        f.scheduler->retryAfterTechnicalPause();
        settle();
        QCOMPARE(f.state(b), Lifecycle::Posting);
    }

    void vpnAdmissionWaitsOrBlocks()
    {
        Fixture f;
        QueueScheduler::Admission answer = QueueScheduler::Admission::Wait;
        f.scheduler->setAdmission([&answer](const QueueEntry &) { return answer; });
        const qint64 a = f.add(QStringLiteral("a"));
        settle();
        QCOMPARE(f.state(a), Lifecycle::Queued);
        answer = QueueScheduler::Admission::Blocked;
        f.scheduler->reevaluate();
        QCOMPARE(f.queue.find(a)->blocked.code, QStringLiteral("vpn_unavailable"));
    }

    void staleAcknowledgementsAreIgnored()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        settle();
        const QString oldRun = f.queue.find(a)->runId;
        QVERIFY(f.queue.setHeld({ a }, true, f.queue.view()).ok());
        settle();
        QVERIFY(f.executor.ackQuiesced(a, true, true));
        settle();
        QVERIFY(f.queue.find(a)->runId != oldRun);
        AttemptResult late;
        late.result = QStringLiteral("success");
        emit f.executor.transferFinished(a, oldRun, late); // late, from the old run
        emit f.executor.finalized(a, oldRun, true, {});
        settle();
        QVERIFY(f.queue.find(a));
        QCOMPARE(f.state(a), Lifecycle::Queued);
    }
};

QTEST_GUILESS_MAIN(tst_QueueScheduler)
#include "tst_QueueScheduler.moc"
