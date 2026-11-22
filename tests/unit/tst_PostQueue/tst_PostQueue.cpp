// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// PostQueue: capabilities from facts, durable-first mutations, stale views,
// strict ordering rules and the "post now" intent (plan_queue.md §3).
//
//========================================================================

#include "QueueTestHelpers.h"
#include "queue/InMemoryQueueStore.h"
#include "queue/PostQueue.h"

#include <QSignalSpy>
#include <QtTest>

using namespace queue;
using queuetest::makeOptions;

class tst_PostQueue : public QObject
{
    Q_OBJECT

private:
    struct Fixture
    {
        InMemoryQueueStore store;
        PostQueue queue{ &store, QStringLiteral("session-A") };

        qint64 add(const QString &name, bool compress = false)
        {
            const QueueCommandResult r = queue.submit(makeOptions(name, compress), Origin::Quick);
            return r.ok() ? r.postIds.value(0) : 0;
        }
        QueueView v() const { return queue.view(); }
    };

    static QueueEntry entry(Lifecycle lifecycle, Slot slot = Slot::None)
    {
        QueueEntry e;
        e.postId = 1;
        e.lifecycle = lifecycle;
        e.slot = slot;
        e.options = makeOptions(QStringLiteral("e"));
        return e;
    }

private slots:
    // --- capabilities ---------------------------------------------------------------
    void capabilitiesOfWaitingEntries()
    {
        QueueEntry e = entry(Lifecycle::Queued);
        QVERIFY(capability(Action::Edit, e).allowed);
        QVERIFY(capability(Action::Edit, e).reason.isEmpty());
        QVERIFY(!capability(Action::Remove, e).needsConfirmation);
        e.transferManifestEngaged = true; // a resume, back in Queued
        QCOMPARE(capability(Action::Edit, e).reason, QStringLiteral("manifest_engaged_restricted"));
        QVERIFY(capability(Action::Remove, e).needsConfirmation);
        e = entry(Lifecycle::Prepared);
        QCOMPARE(capability(Action::Edit, e).reason,
                 QStringLiteral("recipe_change_invalidates_artifacts"));
        QVERIFY(capability(Action::PostNow, e).allowed);
    }

    void flagsOnlyRestrict()
    {
        QueueEntry e = entry(Lifecycle::Queued);
        e.held = true;
        QVERIFY(!capability(Action::PostNow, e).allowed);
        QVERIFY(capability(Action::PostNext, e).allowed); // still skipped once moved
        QVERIFY(capability(Action::Release, e).allowed);
        QVERIFY(!capability(Action::Hold, e).allowed);
        e = entry(Lifecycle::Posting, Slot::Transfer);
        e.blocked = { QStringLiteral("x"), {} };
        QVERIFY(!capability(Action::Move, e).allowed); // blocked never widens Posting
        QVERIFY(!capability(Action::PostNow, e).allowed);
    }

    void capabilitiesOfBusyEntries()
    {
        QueueEntry e = entry(Lifecycle::Posting, Slot::Transfer);
        QVERIFY(!capability(Action::Move, e).allowed);
        QCOMPARE(capability(Action::Hold, e).reason, QStringLiteral("interruption_kept"));
        QVERIFY(capability(Action::Remove, e).needsConfirmation);
        for (Lifecycle l : { Lifecycle::Quiescing, Lifecycle::Recovering }) {
            e = entry(l, Slot::Transfer);
            for (Action a :
                 { Action::Move, Action::Hold, Action::Remove, Action::Duplicate, Action::Edit })
                QVERIFY(!capability(a, e).allowed);
            QVERIFY(!capability(Action::Revalidate, e).allowed);
            e.blocked = { QStringLiteral("quiesce_failed"), {} };
            QVERIFY(capability(Action::Revalidate, e).allowed);
        }
        e = entry(Lifecycle::Finalizing);
        QVERIFY(!capability(Action::Remove, e).allowed);
        QVERIFY(!capability(Action::AbandonPlanning, e).allowed);
        e.blocked = { QStringLiteral("finalization_failed"), {} };
        QVERIFY(capability(Action::RetryFinalization, e).allowed);
        QVERIFY(capability(Action::AbandonPlanning, e).needsConfirmation);
    }

    void capabilitiesOutsideTheSession()
    {
        QueueEntry e = entry(Lifecycle::Queued);
        e.domain = Domain::Deferred;
        QCOMPARE(capability(Action::Move, e).reason, QStringLiteral("maintenance_only"));
        QVERIFY(capability(Action::Remove, e).allowed);
        QVERIFY(!capability(Action::PostNow, e).allowed);
        e.domain = Domain::Foreign;
        QVERIFY(!capability(Action::Remove, e).allowed);
        e.domain = Domain::Session;
        QueueFacts switching;
        switching.intentSwitching = true;
        QVERIFY(!capability(Action::Hold, e, switching).allowed);
    }

    // --- durable first --------------------------------------------------------------
    void submitAppendsAndAssignsIdentities()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        QVERIFY(a > 0 && b > 0 && a != b);
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, b }));
        QCOMPARE(f.queue.find(a)->runNo, 1);
        QVERIFY(!f.queue.find(a)->runId.isEmpty());
        QCOMPARE(f.queue.find(b)->name, QStringLiteral("b"));
        QCOMPARE(f.queue.find(b)->sourceBytes, qint64(1000));
    }

    void storeFailureLeavesMemoryUntouched()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        QSignalSpy changed(&f.queue, &PostQueue::changed);
        f.store.failNextCommits(4);
        QCOMPARE(f.queue.submit(makeOptions(QStringLiteral("c")), Origin::Auto).errorCode,
                 QStringLiteral("store_failed"));
        QCOMPARE(f.queue.move({ b }, 0, f.v()).errorCode, QStringLiteral("store_failed"));
        QCOMPARE(f.queue.setHeld({ a }, true, f.v()).errorCode, QStringLiteral("store_failed"));
        QCOMPARE(f.queue.remove({ a }, RemovePolicy::Default, f.v()).errorCode,
                 QStringLiteral("store_failed"));
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, b }));
        QVERIFY(!f.queue.find(a)->held);
        QCOMPARE(changed.count(), 0);
    }

    void staleViewsAreRejected()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        const QueueView seen = f.v();
        QVERIFY(f.queue.move({ b }, 0, seen).ok());
        QCOMPARE(f.queue.move({ a }, 0, seen).errorCode,
                 QStringLiteral("stale_view")); // order moved on
        const QueueView fresh = f.v();
        QVERIFY(f.queue.setHeld({ a }, true, fresh).ok());
        QCOMPARE(f.queue.setHeld({ a }, false, fresh).errorCode,
                 QStringLiteral("stale_view")); // recipe
        QCOMPARE(f.queue.setHeld({ a }, false, f.v()).status, QueueCommandResult::Status::Done);
    }

    // --- ordering -----------------------------------------------------------------------
    void moveKeepsTheRelativeOrderOfASelection()
    {
        Fixture f;
        QList<qint64> ids;
        for (const char *n : { "a", "b", "c", "d", "e" })
            ids << f.add(QLatin1String(n));
        QVERIFY(f.queue.move({ ids[3], ids[1] }, 0, f.v()).ok());
        QCOMPARE(f.queue.order(), (QList<qint64>{ ids[1], ids[3], ids[0], ids[2], ids[4] }));
        QVERIFY(f.queue.moveToEdge({ ids[1] }, Edge::Bottom, f.v()).ok());
        QCOMPARE(f.queue.order().last(), ids[1]);
    }

    void theActiveEntryKeepsTheFirstPlace()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        const qint64 c = f.add(QStringLiteral("c"));
        QVERIFY(f.queue.engineActivate(b));
        QCOMPARE(f.queue.order(), (QList<qint64>{ b, a, c }));
        QCOMPARE(f.queue.move({ c }, 0, f.v()).errorCode, QStringLiteral("before_active"));
        QCOMPARE(f.queue.move({ b }, 2, f.v()).errorCode, QStringLiteral("active"));
        QVERIFY(f.queue.postNext({ c }, f.v()).ok());
        QCOMPARE(f.queue.order(), (QList<qint64>{ b, c, a }));
    }

    // --- post now -----------------------------------------------------------------------
    void postNowWithoutActiveIsAMoveToTheTop()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        QCOMPARE(f.queue.postNow(b, f.v()).status, QueueCommandResult::Status::Done);
        QCOMPARE(f.queue.order(), (QList<qint64>{ b, a }));
        QVERIFY(f.queue.intent().isNull());
    }

    void postNowReservesTheSecondPlace()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        const qint64 c = f.add(QStringLiteral("c"));
        const qint64 d = f.add(QStringLiteral("d"));
        QVERIFY(f.queue.engineActivate(a));
        QCOMPARE(f.queue.postNow(d, f.v()).status, QueueCommandResult::Status::InProgress);
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, d, b, c }));
        QCOMPARE(f.queue.intent().targetPostId, d);
        QCOMPARE(f.queue.intent().activeRunId, f.queue.find(a)->runId);
        QVERIFY(f.queue.postNext({ c }, f.v()).ok());
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, d, c, b })); // after the target
        QCOMPARE(f.queue.move({ b }, 1, f.v()).errorCode, QStringLiteral("intent_pending"));
        QCOMPARE(f.queue.postNow(c, f.v()).errorCode, QStringLiteral("intent_exists"));
        QSignalSpy dropped(&f.queue, &PostQueue::intentDropped);
        QVERIFY(f.queue.postNow(c, f.v(), true).ok()); // explicit replacement
        QCOMPARE(f.queue.intent().targetPostId, c);
        QCOMPARE(dropped.count(), 0);
        QVERIFY(f.queue.moveToEdge({ c }, Edge::Bottom, f.v()).ok()); // moving the target cancels
        QVERIFY(f.queue.intent().isNull());
        QCOMPARE(dropped.count(), 1);
    }

    void intentIsDroppedByHoldEditRemoveAndCancel()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        QVERIFY(f.queue.engineActivate(a));
        auto arm = [&] { QVERIFY(f.queue.postNow(b, f.v()).ok()); };
        arm();
        QVERIFY(f.queue.setHeld({ b }, true, f.v()).ok());
        QVERIFY(f.queue.intent().isNull());
        QVERIFY(f.queue.setHeld({ b }, false, f.v()).ok());
        arm();
        PersistedQueueOptions edited = f.queue.find(b)->options;
        edited.job.grpList << QStringLiteral("alt.binaries.other");
        QVERIFY(f.queue.edit(b, edited, f.v()).ok());
        QVERIFY(f.queue.intent().isNull());
        arm();
        const QString id = f.queue.intent().intentId;
        QCOMPARE(f.queue.cancelPostNow(QStringLiteral("other")).errorCode,
                 QStringLiteral("stale_view"));
        QVERIFY(f.queue.cancelPostNow(id).ok());
        QCOMPARE(f.queue.order(), (QList<qint64>{ a, b })); // cancelling keeps the order
        arm();
        QVERIFY(f.queue.remove({ b }, RemovePolicy::Default, f.v()).ok());
        QVERIFY(f.queue.intent().isNull());
    }

    void commitSwitchPutsTheTargetFirstOnlyIfStillValid()
    {
        for (bool valid : { true, false }) {
            Fixture f;
            const qint64 a = f.add(QStringLiteral("a"));
            const qint64 b = f.add(QStringLiteral("b"));
            const qint64 c = f.add(QStringLiteral("c"));
            QVERIFY(f.queue.engineActivate(a));
            QVERIFY(f.queue.postNow(c, f.v()).ok());
            QVERIFY(f.queue.engineSetIntentSwitching());
            QVERIFY(!f.queue.capabilityOf(Action::Hold, c).allowed); // switching locks the target
            if (!valid)
                QVERIFY(f.queue.engineBlock(c,
                                            { QStringLiteral("sources_missing"), {} },
                                            Lifecycle::Queued));
            const QString oldRun = f.queue.find(a)->runId;
            QVERIFY(f.queue.engineCommitSwitch(true));
            QVERIFY(f.queue.intent().isNull());
            QCOMPARE(f.queue.order(),
                     valid ? (QList<qint64>{ c, a, b }) : (QList<qint64>{ a, c, b }));
            const QueueEntry *old = f.queue.find(a);
            QCOMPARE(old->lifecycle, Lifecycle::Queued);
            QVERIFY(old->transferManifestEngaged);
            QVERIFY(old->runId != oldRun);
            QCOMPARE(old->runNo, 2);
            QCOMPARE(f.store.interruptionsRecorded(oldRun), 1);
        }
    }

    // --- edit, duplicate, remove ------------------------------------------------------------
    void editRulesFollowTheFacts()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        PersistedQueueOptions changed = f.queue.find(a)->options;
        changed.job.rarName = QStringLiteral("other");
        QVERIFY(f.queue.engineRecordPrepared(a, { QStringLiteral("set1"), 1, 0, 500, QString() }));
        QVERIFY(f.queue.edit(a, changed, f.v()).ok()); // recipe change invalidates artifacts
        QCOMPARE(f.queue.find(a)->lifecycle, Lifecycle::Queued);
        QVERIFY(f.queue.find(a)->artifacts.isNull());
        QVERIFY(f.queue.find(a)->retainedSets.isEmpty()); // keepRar off: nothing kept

        QVERIFY(f.queue.engineInterrupt(a, true, false, true)); // now a resume
        PersistedQueueOptions keep = f.queue.find(a)->options;
        keep.job.keepRar = true;
        QVERIFY(f.queue.edit(a, keep, f.v()).ok());
        PersistedQueueOptions groups = f.queue.find(a)->options;
        groups.job.grpList << QStringLiteral("g2");
        QCOMPARE(f.queue.edit(a, groups, f.v()).errorCode, QStringLiteral("manifest_engaged"));
        PersistedQueueOptions del = f.queue.find(a)->options;
        del.job.delFilesAfterPost = true;
        QCOMPARE(f.queue.edit(a, del, f.v()).errorCode, QStringLiteral("manifest_engaged"));
    }

    void keepRarRetainsInvalidatedArtifacts()
    {
        Fixture f;
        PersistedQueueOptions o = makeOptions(QStringLiteral("k"), true);
        o.job.keepRar = true;
        const qint64 a = f.queue.submit(o, Origin::Auto).postIds.value(0);
        QVERIFY(f.queue.engineRecordPrepared(a, { QStringLiteral("set1"), 1, 0, 500, QString() }));
        o.job.rarName = QStringLiteral("again");
        QVERIFY(f.queue.edit(a, o, f.v()).ok());
        QCOMPARE(f.queue.find(a)->retainedSets.size(), 1);
        QCOMPARE(f.queue.find(a)->retainedBytes(), qint64(500));
    }

    void duplicateGetsItsOwnPostAndPath()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        bool rerolled = false;
        f.queue.setRerollFunction([&rerolled](PersistedQueueOptions &) { rerolled = true; });
        const QueueCommandResult r = f.queue.duplicate(a, f.v());
        QVERIFY(r.ok());
        const qint64 copy = r.postIds.value(0);
        QVERIFY(copy != a);
        QVERIFY(rerolled);
        QVERIFY(f.queue.find(copy)->options.job.nzbFilePath.endsWith(QStringLiteral("a (2).nzb")));
        QCOMPARE(f.queue.order().last(), copy);
        QVERIFY(!f.queue.find(copy)->blocked.isBlocked());
    }

    void duplicateWaitsForSourcesStillRenamed()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue.engineSetSourceJournal(a, true));
        const QueueCommandResult r = f.queue.duplicate(a, f.v());
        QVERIFY(r.ok());
        QCOMPARE(f.queue.find(r.postIds.value(0))->blocked.code,
                 QStringLiteral("sources_pending_recovery"));
        QVERIFY(f.queue.engineSetSourceJournal(a, false));
        QVERIFY(!f.queue.find(f.queue.duplicate(a, f.v()).postIds.value(0))->blocked.isBlocked());
    }

    void removingARuntimeEntryAsksItsRuntime()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"), true);
        QVERIFY(f.queue.engineActivate(a));
        QVERIFY(f.queue.engineBeginPreparation(b, { QStringLiteral("s"), 1, 0, 0, QString() }));
        QSignalSpy requests(&f.queue, &PostQueue::runtimeActionRequested);
        const QueueCommandResult r = f.queue.remove({ a, b }, RemovePolicy::Default, f.v());
        QCOMPARE(r.status, QueueCommandResult::Status::InProgress);
        QCOMPARE(requests.count(), 2);
        QCOMPARE(requests.at(0).at(1).value<RuntimeRequest>(), RuntimeRequest::StopTransfer);
        QCOMPARE(requests.at(1).at(1).value<RuntimeRequest>(), RuntimeRequest::CancelPreparation);
        QCOMPARE(f.queue.order().size(), 2); // they leave only once their runtime confirmed
    }

    void resumeEntersUnderANewRun()
    {
        Fixture f;
        const PersistedQueueOptions o = makeOptions(QStringLiteral("r"), true, true);
        const QueueCommandResult admitted = f.queue.enqueueExisting(77, o, Origin::Resume);
        QVERIFY(admitted.ok());
        QCOMPARE(admitted.postIds, QList<qint64>{ 77 });
        const QueueEntry *entry = f.queue.find(77);
        QVERIFY(entry);
        QCOMPARE(entry->runNo, 1);
        QVERIFY(entry->transferManifestEngaged);
        QVERIFY(entry->isReady()); // a resume never packs again
        QCOMPARE(f.queue.order(), QList<qint64>{ 77 });
        QCOMPARE(f.queue.enqueueExisting(77, o, Origin::Resume).errorCode,
                 QStringLiteral("already_queued"));
    }

    void finalizationFollowsItsRun()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue.engineActivate(a));
        const QString run = f.queue.find(a)->runId;
        AttemptResult result;
        result.postId = a;
        result.runId = QStringLiteral("not-this-one");
        result.result = QStringLiteral("success");
        QVERIFY(!f.queue.engineRecordResult(result)); // a replaced run
        QVERIFY(!f.queue.engineFinishFinalization(a, run, FinalizationStep::Kind::Complete));
        result.runId = run;
        result.activeMs = 1200;
        QVERIFY(f.queue.engineRecordResult(result));
        QCOMPARE(f.queue.find(a)->lifecycle, Lifecycle::Finalizing);
        QVERIFY(f.store.resultOf(run));
        result.result = QStringLiteral("failed");
        QVERIFY(!f.queue.engineRecordResult(result)); // contradicts F1
        QSignalSpy removed(&f.queue, &PostQueue::entryRemoved);
        QVERIFY(f.queue.engineFinishFinalization(a, run, FinalizationStep::Kind::Complete));
        QCOMPARE(removed.size(), 1);
        QVERIFY(!f.queue.find(a));
        QVERIFY(f.queue.order().isEmpty());
        QCOMPARE(f.store.finalizationOf(run), FinalizationStep::Kind::Complete);
        QVERIFY(!f.store.isReleased(a));
        QVERIFY(f.queue.engineReleaseOwnership(a));
        QVERIFY(f.store.isReleased(a));
    }

    void abandonNeedsNoResult()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue.engineActivate(a));
        const QString run = f.queue.find(a)->runId;
        QVERIFY(f.queue.engineFinishFinalization(a, run, FinalizationStep::Kind::Abandon));
        QVERIFY(!f.queue.find(a));
        QCOMPARE(f.store.finalizationOf(run), FinalizationStep::Kind::Abandon);
    }

    void keepForRestartRecordsTheTimeOfItsRun()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue.engineActivate(a));
        const QString run = f.queue.find(a)->runId;
        QVERIFY(f.queue.engineKeepForRestart(a, run, 2500, false, false, false));
        QCOMPARE(f.store.keptActiveMs(run), qint64(2500));
        QVERIFY(f.queue.find(a)->runId != run);
        QVERIFY(!f.queue.engineKeepForRestart(a, run, 2500, false, false, false));
        QCOMPARE(f.store.keptActiveMs(run), qint64(2500));
    }

    void aResultIsKeptAsFirstRecorded()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue.engineActivate(a));
        const QString run = f.queue.find(a)->runId;
        AttemptResult first;
        first.postId = a;
        first.runId = run;
        first.result = QStringLiteral("success");
        first.activeMs = 1000;
        QVERIFY(f.queue.engineRecordResult(first));
        AttemptResult again = first;
        again.activeMs = 9000; // a repeat after a lost answer
        QVERIFY(f.queue.engineRecordResult(again));
        QCOMPARE(f.store.resultOf(run)->activeMs, qint64(1000));
        QVERIFY(!f.queue.engineKeepForRestart(a, run, 0, false, false, true)); // F1 engaged
        // Two contradicting results in one transaction are refused.
        const qint64 b = f.add(QStringLiteral("b"));
        QueueTransaction tx;
        AttemptResult ok;
        ok.postId = b;
        ok.runId = f.queue.find(b)->runId;
        ok.result = QStringLiteral("success");
        AttemptResult ko = ok;
        ko.result = QStringLiteral("failed");
        tx.results << ok << ko;
        tx.expectedRevisions.insert(b, f.queue.find(b)->recipeRevision);
        QCOMPARE(f.store.commit(tx).error, QStringLiteral("contradictory_result"));
        QVERIFY(!f.store.resultOf(ok.runId));
        // Nor a result and a restart of the same run.
        QueueTransaction restart;
        restart.results << ok;
        restart.renewRuns << b;
        restart.expectedRuns.insert(b, ok.runId);
        restart.expectedRevisions.insert(b, f.queue.find(b)->recipeRevision);
        QCOMPARE(f.store.commit(restart).error, QStringLiteral("conflicting_operations"));
        QVERIFY(!f.store.resultOf(ok.runId));
    }

    void interruptionRenewsTheRunOnce()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const QString first = f.queue.find(a)->runId;
        QVERIFY(f.queue.engineActivate(a));
        QVERIFY(f.queue.engineInterrupt(a, true, true, true));
        const QueueEntry *e = f.queue.find(a);
        QCOMPARE(e->lifecycle, Lifecycle::Queued);
        QVERIFY(e->held);
        QVERIFY(e->transferManifestEngaged);
        QCOMPARE(e->runNo, 2);
        QVERIFY(e->runId != first);
        QCOMPARE(f.store.interruptionsRecorded(first), 1);
        QCOMPARE(f.store.interruptionsRecorded(e->runId), 0);
    }

    void revalidateClearsOrKeepsTheBlock()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(
            f.queue.engineBlock(a, { QStringLiteral("password_required"), {} }, Lifecycle::Queued));
        bool fixed = false;
        f.queue.setRevalidateFunction([&fixed](const QueueEntry &) {
            return fixed ? BlockReason{} : BlockReason{ QStringLiteral("password_required"), {} };
        });
        QCOMPARE(f.queue.revalidate(a, f.v()).errorCode, QStringLiteral("still_blocked"));
        QVERIFY(f.queue.find(a)->blocked.isBlocked());
        fixed = true;
        QVERIFY(f.queue.revalidate(a, f.v()).ok());
        QVERIFY(!f.queue.find(a)->blocked.isBlocked());
    }
};

QTEST_GUILESS_MAIN(tst_PostQueue)
#include "tst_PostQueue.moc"
