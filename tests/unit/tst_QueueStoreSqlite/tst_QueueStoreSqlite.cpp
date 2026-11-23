// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Queue tables of the history database (schema v6, plan_queue.md §5.5):
// migration, atomic admissions, run succession, removal, reload, secrets.
//
//========================================================================

#include "QueueTestHelpers.h"
#include "history/HistoryQueueStore.h"
#include "history/PostHistoryStore.h"
#include "queue/OwnerLock.h"
#include "queue/PostQueue.h"

#include <QLockFile>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

using namespace queue;
using queuetest::makeOptions;

namespace
{
//! F1 of a transferred post: (result, run, duration) are what the store holds.
AttemptResult attempt(qint64 postId,
                      const QString &runId,
                      const QString &result = QStringLiteral("success"),
                      qint64 activeMs = 5000)
{
    AttemptResult a;
    a.postId = postId;
    a.runId = runId;
    a.result = result;
    a.avgSpeed = QStringLiteral("1 MB/s");
    a.activeMs = activeMs;
    return a;
}
} // namespace

class tst_QueueStoreSqlite : public QObject
{
    Q_OBJECT

private:
    struct Fixture
    {
        QTemporaryDir dir;
        std::unique_ptr<PostHistoryStore> store;
        std::unique_ptr<HistoryQueueStore> adapter;
        std::unique_ptr<PostQueue> queue;

        explicit Fixture(bool storePasswords = true)
        {
            store = std::make_unique<PostHistoryStore>(dir.filePath(
                                                           QStringLiteral("history.sqlite")),
                                                       storePasswords);
            store->setOwnerId(QStringLiteral("owner-A"));
            adapter = std::make_unique<HistoryQueueStore>(store.get());
            queue = std::make_unique<PostQueue>(adapter.get(), QStringLiteral("queue-A"));
        }
        qint64 add(const QString &name, bool compress = false)
        {
            PersistedQueueOptions o = makeOptions(name, compress);
            o.job.rarPass = QStringLiteral("secret-%1").arg(name);
            o.passwordOrigin = PasswordOrigin::Generated;
            return queue->submit(o, Origin::Quick, 3).postIds.value(0);
        }
        //! One value of the database, through a connection of its own.
        QVariant scalar(const QString &sql, const QVariantList &values = {})
        {
            const QString name = QStringLiteral("probe-%1").arg(++probes);
            QVariant value;
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
                db.setDatabaseName(dir.filePath(QStringLiteral("history.sqlite")));
                if (db.open()) {
                    QSqlQuery q(db);
                    q.prepare(sql);
                    for (const QVariant &v : values)
                        q.addBindValue(v);
                    if (q.exec() && q.next())
                        value = q.value(0);
                }
            }
            QSqlDatabase::removeDatabase(name);
            return value;
        }
        int probes = 0;
    };

private slots:
    void freshDatabaseHasTheV6Shape()
    {
        Fixture f;
        QString error;
        QVERIFY2(f.store->initialize(&error), qPrintable(error));
        QCOMPARE(f.scalar(QStringLiteral("SELECT value FROM schema_meta WHERE key='version'"))
                     .toString(),
                 QStringLiteral("6"));
        for (const char *table : { "post_queue",
                                   "post_runs",
                                   "post_queue_orders",
                                   "post_queue_intents",
                                   "post_artifact_sets",
                                   "post_run_artifacts",
                                   "post_source_moves",
                                   "post_path_refs" })
            QVERIFY2(f.scalar(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name=?"),
                              { QLatin1String(table) })
                             .toInt()
                         == 1,
                     table);
    }

    void aV5DatabaseIsMigratedWithItsRows()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("old.sqlite"));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("v5"));
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE schema_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO schema_meta VALUES('version','5')")));
            QVERIFY(q.exec(
                QStringLiteral("CREATE TABLE posts(id INTEGER PRIMARY KEY AUTOINCREMENT,"
                               "created_at TEXT NOT NULL, started_at TEXT, finished_at TEXT,"
                               "nzb_name TEXT NOT NULL, nzb_path TEXT, status TEXT NOT NULL)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO posts(created_at, nzb_name, status) "
                                          "VALUES('2026-01-01', 'old', 'done')")));
        }
        QSqlDatabase::removeDatabase(QStringLiteral("v5"));
        PostHistoryStore store(path, true);
        QString error;
        QVERIFY2(store.initialize(&error), qPrintable(error));
        HistoryQueueStore adapter(&store);
        QCOMPARE(adapter.loadEntries(QStringLiteral("any")).size(), 0);
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("check"));
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("SELECT nzb_name, owner_id FROM posts")) && q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("old"));
        QVERIFY(q.value(1).isNull()); // a legacy post has no owner
        q.finish();
        db.close();
    }

    void admissionWritesPostRunAndEntryTogether()
    {
        Fixture f;
        const qint64 id = f.add(QStringLiteral("alpha"));
        QVERIFY(id > 0);
        const QString runId = f.queue->find(id)->runId;
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { id }).toString(),
                 QStringLiteral("queued"));
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT owner_id FROM posts WHERE id=?"), { id }).toString(),
            QStringLiteral("owner-A"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT run_no FROM post_runs WHERE run_id=?"), { runId })
                     .toInt(),
                 1);
        QCOMPARE(f.scalar(QStringLiteral("SELECT current_run_id FROM post_info WHERE post_id=?"),
                          { id })
                     .toString(),
                 runId);
        QCOMPARE(f.scalar(QStringLiteral("SELECT group_name FROM post_groups WHERE post_id=?"),
                          { id })
                     .toString(),
                 QStringLiteral("alt.binaries.test"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT tab_number FROM post_queue WHERE post_id=?"),
                          { id })
                     .toInt(),
                 3);
        QVERIFY(
            !f.scalar(QStringLiteral("SELECT options_json FROM post_queue WHERE post_id=?"), { id })
                 .toString()
                 .contains(QStringLiteral("secret-alpha")));
    }

    void aReloadGivesBackTheWholeQueue()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"), true);
        const qint64 c = f.add(QStringLiteral("c"));
        QVERIFY(f.queue->move({ c }, 0, f.queue->view()).ok());
        QVERIFY(f.queue->setHeld({ a }, true, f.queue->view()).ok());
        QVERIFY(f.queue->engineBlock(b,
                                     { QStringLiteral("resources"),
                                       { { QStringLiteral("needed"), 42 } } },
                                     Lifecycle::Queued));
        QVERIFY(f.queue->engineActivate(c));
        QVERIFY(f.queue->postNow(b, f.queue->view(), false).errorCode == QStringLiteral("blocked"));
        QVERIFY(f.queue->revalidate(b, f.queue->view()).ok());
        QVERIFY(f.queue->postNow(b, f.queue->view()).ok());

        PostQueue reloaded(f.adapter.get(), QStringLiteral("queue-A"));
        QVERIFY(reloaded.load());
        QCOMPARE(reloaded.order(), f.queue->order());
        QVERIFY(reloaded.find(a)->held);
        QCOMPARE(reloaded.find(c)->lifecycle, Lifecycle::Posting);
        QCOMPARE(reloaded.find(b)->recipeRevision, f.queue->find(b)->recipeRevision);
        QCOMPARE(reloaded.find(b)->options.job.rarPass, QStringLiteral("secret-b"));
        QCOMPARE(reloaded.find(b)->options.archiverFamily, QStringLiteral("rar"));
        QCOMPARE(reloaded.find(b)->name, QStringLiteral("b"));
        QCOMPARE(reloaded.intent().targetPostId, b);
        QCOMPARE(reloaded.intent().activeRunId, f.queue->find(c)->runId);
    }

    void anInterruptionRenewsTheRunOnce()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const QString first = f.queue->find(a)->runId;
        QVERIFY(f.queue->engineActivate(a));
        QVERIFY(f.queue->engineInterrupt(a, true, false, true));
        const QString second = f.queue->find(a)->runId;
        QVERIFY(second != first);
        QCOMPARE(f.scalar(QStringLiteral("SELECT result FROM post_runs WHERE run_id=?"), { first })
                     .toString(),
                 QStringLiteral("interrupted"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT run_no FROM post_runs WHERE run_id=?"), { second })
                     .toInt(),
                 2);
        QCOMPARE(f.scalar(QStringLiteral("SELECT run_id FROM post_queue WHERE post_id=?"), { a })
                     .toString(),
                 second);
        QCOMPARE(f.scalar(QStringLiteral("SELECT current_run_id FROM post_info WHERE post_id=?"),
                          { a })
                     .toString(),
                 second);
        QCOMPARE(f.scalar(QStringLiteral(
                              "SELECT transfer_manifest_complete FROM post_info WHERE post_id=?"),
                          { a })
                     .toInt(),
                 1);
    }

    void removingANeverStartedPostCancelsIt()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue->remove({ a }, RemovePolicy::Default, f.queue->view()).ok());
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { a }).toString(),
                 QStringLiteral("cancelled"));
        QVERIFY(f.scalar(QStringLiteral("SELECT owner_id FROM posts WHERE id=?"), { a }).isNull());
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_queue")).toInt(), 0);
        QCOMPARE(f.scalar(QStringLiteral("SELECT result FROM post_runs WHERE post_id=?"), { a })
                     .toString(),
                 QStringLiteral("cancelled"));
    }

    void aResumeQueuesTheSamePostUnderItsNextRun()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        QVERIFY(f.queue->remove({ a, b }, RemovePolicy::Default, f.queue->view()).ok());
        f.scalar(QStringLiteral("UPDATE posts SET status='partial' WHERE id=?"), { a });
        f.scalar(QStringLiteral("UPDATE posts SET owner_id='owner-B' WHERE id=?"), { b });

        PersistedQueueOptions o = makeOptions(QStringLiteral("a"));
        o.job.resumeHistoryPostId = a;
        QVERIFY(f.queue->enqueueExisting(a, o, Origin::Resume).ok());
        const QueueEntry *entry = f.queue->find(a);
        QVERIFY(entry);
        QCOMPARE(entry->runNo, 2);
        QVERIFY(entry->transferManifestEngaged);
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_runs WHERE post_id=?"), { a })
                     .toInt(),
                 2);
        // The earlier outcome stays until the resume really transfers.
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { a }).toString(),
                 QStringLiteral("partial"));
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT owner_id FROM posts WHERE id=?"), { a }).toString(),
            QStringLiteral("owner-A"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT current_run_id FROM post_info WHERE post_id=?"),
                          { a })
                     .toString(),
                 entry->runId);
        QCOMPARE(f.queue->enqueueExisting(a, o, Origin::Resume).errorCode,
                 QStringLiteral("already_queued"));
        // Another live session's post is not taken.
        QVERIFY(
            !f.queue->enqueueExisting(b, makeOptions(QStringLiteral("b")), Origin::Resume).ok());
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_runs WHERE post_id=?"), { b })
                     .toInt(),
                 1);

        PostQueue reloaded(f.adapter.get(), QStringLiteral("queue-A"));
        QVERIFY(reloaded.load());
        QVERIFY(reloaded.find(a));
        QCOMPARE(reloaded.find(a)->runId, entry->runId);
    }

    void aFailingTransactionLeavesNothing()
    {
        Fixture f;
        f.add(QStringLiteral("a"));
        const int posts = f.scalar(QStringLiteral("SELECT COUNT(*) FROM posts")).toInt();
        QueueTransaction tx;
        QueueEntry fresh;
        fresh.postId = -1;
        fresh.queueId = QStringLiteral("queue-A");
        fresh.options = makeOptions(QStringLiteral("x"));
        fresh.name = QStringLiteral("x");
        QueueEntry ghost = fresh;
        ghost.postId = 9999; // not in the queue: the update fails
        tx.upserts << fresh << ghost;
        QVERIFY(!f.adapter->commit(tx).ok());
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM posts")).toInt(), posts);
    }

    void passwordsFollowStorePasswords()
    {
        Fixture f(false);
        const qint64 a = f.add(QStringLiteral("a"));
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT rar_pass FROM posts WHERE id=?"), { a }).toString(),
            QString());
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT has_password FROM posts WHERE id=?"), { a }).toInt(),
            1);
        PostQueue reloaded(f.adapter.get(), QStringLiteral("queue-A"));
        QVERIFY(reloaded.load());
        QVERIFY(reloaded.find(a)->options.job.rarPass.isEmpty()); // to enter again, never invented
    }

    void editsAreMirroredUntilTheTransferStarts()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        PersistedQueueOptions o = f.queue->find(a)->options;
        o.job.nzbFilePath = QStringLiteral("/nonexistent/other.nzb");
        o.job.grpList = { QStringLiteral("alt.binaries.x"), QStringLiteral("alt.binaries.y") };
        QVERIFY(f.queue->edit(a, o, f.queue->view()).ok());
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT nzb_name FROM posts WHERE id=?"), { a }).toString(),
            QStringLiteral("other"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_groups WHERE post_id=?"), { a })
                     .toInt(),
                 2);
        QVERIFY(f.store->markPostStarted(a));
        o.job.nzbFilePath = QStringLiteral("/nonexistent/third.nzb");
        o.job.keepRar = true;
        QVERIFY(f.queue->edit(a, o, f.queue->view()).ok());
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT nzb_name FROM posts WHERE id=?"), { a }).toString(),
            QStringLiteral("other"));
    }

    void aFutureDatabaseIsRefusedUntouched()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("future.sqlite"));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("future"));
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE schema_meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO schema_meta VALUES('version','99')")));
        }
        QSqlDatabase::removeDatabase(QStringLiteral("future"));
        PostHistoryStore store(path, true);
        QString error;
        QVERIFY(!store.initialize(&error));
        QVERIFY(error.contains(QStringLiteral("newer ngPost")));
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("future2"));
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name='posts'"))
                && q.next());
        QCOMPARE(q.value(0).toInt(), 0); // no DDL ran
        q.finish();
        db.close();
    }

    void artifactSetsAreKeptAcrossReloads()
    {
        Fixture f;
        PersistedQueueOptions o = makeOptions(QStringLiteral("k"), true);
        o.job.keepRar = true;
        const qint64 a = f.queue->submit(o, Origin::Auto).postIds.value(0);
        QVERIFY(f.queue->engineBeginPreparation(
            a,
            { QStringLiteral("gen1"), 1, 0, 0, QStringLiteral("/tmp") }));
        QVERIFY(f.queue->engineRecordPrepared(
            a,
            { QStringLiteral("gen1"), 1, 0, 500, QStringLiteral("/tmp") }));
        o.job.rarName = QStringLiteral("again"); // recipe change: gen1 invalidated, kept by keepRar
        QVERIFY(f.queue->edit(a, o, f.queue->view()).ok());
        PostQueue reloaded(f.adapter.get(), QStringLiteral("queue-A"));
        QVERIFY(reloaded.load());
        const QueueEntry *e = reloaded.find(a);
        QVERIFY(e->artifacts.isNull());
        QCOMPARE(e->retainedSets.size(), 1);
        QCOMPARE(e->retainedSets.first().bytes, qint64(500));
        QCOMPARE(e->lastGeneration, 1);
    }

    // --- recoverable preparation (lot C5.1) -------------------------------------------

    void artifactSetsAreRecordedBeforeTheDisk()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        const QString run = f.queue->find(a)->runId;
        PostHistoryStore::ArtifactSetPlan plan{ QStringLiteral("set-1"),
                                                a,
                                                run,
                                                1,
                                                0,
                                                QStringLiteral("/tmp/root"),
                                                QStringLiteral("/tmp/root/a-1/archives"),
                                                QStringLiteral("/tmp/root/a-1/staging") };
        QString error;
        QVERIFY2(f.store->createArtifactSet(plan, &error), qPrintable(error));
        QVERIFY2(f.store->createArtifactSet(plan, &error), qPrintable(error)); // repeated
        const auto set = f.store->artifactSet(QStringLiteral("set-1"));
        QVERIFY(set);
        QCOMPARE(set->state, QStringLiteral("preparing"));
        QCOMPARE(set->toolState, QStringLiteral("idle"));
        QCOMPARE(set->plan.archiveDir, plan.archiveDir);
        QCOMPARE(set->plan.stagingDir, plan.stagingDir);

        PostHistoryStore::ArtifactSetPlan moved = plan;
        moved.archiveDir = QStringLiteral("/elsewhere");
        QVERIFY(!f.store->createArtifactSet(moved, &error)); // its places never change
        QVERIFY(error.startsWith(QStringLiteral("artifact_set_mismatch")));
        PostHistoryStore::ArtifactSetPlan again = plan;
        again.setId = QStringLiteral("set-again");
        QVERIFY(!f.store->createArtifactSet(again, &error)); // generation 1 is taken
        QVERIFY(error.startsWith(QStringLiteral("artifact_set_generation")));
        PostHistoryStore::ArtifactSetPlan stale = plan;
        stale.setId = QStringLiteral("set-stale");
        stale.generation = 2;
        stale.runId = QStringLiteral("not-the-run");
        QVERIFY(!f.store->createArtifactSet(stale, &error));
        QVERIFY(error.startsWith(QStringLiteral("stale_run")));
        f.store->setOwnerId(QStringLiteral("owner-B"));
        PostHistoryStore::ArtifactSetPlan foreign = plan;
        foreign.setId = QStringLiteral("set-foreign");
        foreign.generation = 2;
        QVERIFY(!f.store->createArtifactSet(foreign, &error));
        QVERIFY(error.startsWith(QStringLiteral("not_owner")));
    }

    void aSetIsVerifiedOnlyWithItsCompleteList()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        const QString run = f.queue->find(a)->runId;
        const ArtifactSetRef ref{ QStringLiteral("set-1"), 1, 0, 0, QStringLiteral("/tmp/root") };
        QVERIFY(f.queue->engineBeginPreparation(a, ref));
        QString error;
        QVERIFY2(f.store->createArtifactSet(
                     { ref.id, a, run, 1, 0, ref.root, QStringLiteral("/tmp/root/archives"), {} },
                     &error),
                 qPrintable(error));
        // The model saying "prepared" is not a proof.
        QVERIFY(f.queue->engineRecordPrepared(a, ref));
        QCOMPARE(f.store->artifactSet(ref.id)->state, QStringLiteral("preparing"));

        auto file = [](int sequence, const QString &name, const QString &role, bool publish) {
            return PostHistoryStore::ArtifactRecord{
                sequence,
                QStringLiteral("/tmp/root/archives/") + name,
                1000 + sequence,
                QStringLiteral("2026-11-23T10:00:00Z"),
                QStringLiteral("{\"dev\":1,\"ino\":%1}").arg(sequence + 10),
                role,
                publish,
                QStringLiteral("alt.binaries.test")
            };
        };
        const QList<PostHistoryStore::ArtifactRecord> manifest{
            file(0, QStringLiteral("a.part1.rar"), QStringLiteral("archive"), true),
            file(1, QStringLiteral("a.part2.rar"), QStringLiteral("archive"), true),
            file(2, QStringLiteral("a.par2"), QStringLiteral("par2"), true)
        };
        QVERIFY(!f.store->recordPreparedArtifacts(a, run, ref.id, {}, &error));
        QCOMPARE(error, QStringLiteral("manifest_empty"));
        QVERIFY(!f.store->recordPreparedArtifacts(a,
                                                  run,
                                                  ref.id,
                                                  { manifest[0], manifest[2] },
                                                  &error));
        QCOMPARE(error, QStringLiteral("manifest_sequence")); // a hole: partial
        QVERIFY(!f.store->recordPreparedArtifacts(
            a,
            run,
            ref.id,
            { file(0, QStringLiteral("x"), QStringLiteral("nfo"), false) },
            &error));
        QCOMPARE(error, QStringLiteral("manifest_publishes_nothing"));
        QVERIFY(f.store->recordToolSupervision(a,
                                               run,
                                               ref.id,
                                               QStringLiteral("linux_test"),
                                               QStringLiteral("starting"),
                                               {},
                                               &error));
        QVERIFY(!f.store->recordPreparedArtifacts(a, run, ref.id, manifest, &error));
        QVERIFY(error.startsWith(QStringLiteral("tool_not_stopped"))); // a tool may still write
        QVERIFY(f.store->recordToolSupervision(a,
                                               run,
                                               ref.id,
                                               {},
                                               QStringLiteral("stopped_verified"),
                                               {},
                                               &error));

        QVERIFY2(f.store->recordPreparedArtifacts(a, run, ref.id, manifest, &error),
                 qPrintable(error));
        const auto set = f.store->artifactSet(ref.id);
        QCOMPARE(set->state, QStringLiteral("verified"));
        QCOMPARE(set->bytes, qint64(1000 + 1001 + 1002));
        QCOMPARE(set->artifacts, manifest);
        QCOMPARE(f.scalar(QStringLiteral("SELECT current_artifact_set_id FROM post_queue WHERE "
                                         "post_id=?"),
                          { a })
                     .toString(),
                 ref.id);
        QVERIFY2(f.store->recordPreparedArtifacts(a, run, ref.id, manifest, &error),
                 qPrintable(error)); // repeated after a lost acknowledgement
        QVERIFY(!f.store->recordPreparedArtifacts(a, run, ref.id, { manifest[0] }, &error));
        QVERIFY(error.startsWith(QStringLiteral("artifact_set_verified")));
        // A later write of the entry neither verifies nor unverifies it.
        QVERIFY(f.queue->setHeld({ a }, true, f.queue->view()).ok());
        QCOMPARE(f.store->artifactSet(ref.id)->state, QStringLiteral("verified"));
        QCOMPARE(f.store->artifactSet(ref.id)->bytes, qint64(3003));
    }

    void sourceMovesAreJournaledAndOutliveTheEntry()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const QString run = f.queue->find(a)->runId;
        const QList<PostHistoryStore::SourceMove> moves{ { 0,
                                                           QStringLiteral("/in/a.mkv"),
                                                           QStringLiteral("/stage/x1"),
                                                           QStringLiteral("{\"ino\":1}"),
                                                           {},
                                                           {},
                                                           {} },
                                                         { 1,
                                                           QStringLiteral("/in/a.nfo"),
                                                           QStringLiteral("/stage/x2"),
                                                           QStringLiteral("{\"ino\":2}"),
                                                           {},
                                                           {},
                                                           {} } };
        QString error;
        QVERIFY2(f.store->planSourceMoves(a, run, {}, moves, &error), qPrintable(error));
        QVERIFY2(f.store->planSourceMoves(a, run, {}, moves, &error), qPrintable(error));
        auto conflicting = moves;
        conflicting[1].stagedPath = QStringLiteral("/stage/other");
        QVERIFY(!f.store->planSourceMoves(a, run, {}, conflicting, &error));
        QVERIFY(error.startsWith(QStringLiteral("source_move_conflict")));
        QVERIFY(!f.store->planSourceMoves(a, run, QStringLiteral("no-such-set"), moves, &error));
        QCOMPARE(f.store->unsettledSourceMoves(a).size(), 2);

        QVERIFY(f.store->resolveSourceMoves(a, run, { 0, 1 }, QStringLiteral("moved"), &error));
        QVERIFY(f.store->resolveSourceMoves(a, run, { 1 }, QStringLiteral("restored"), &error));
        QVERIFY(!f.store->resolveSourceMoves(a, run, { 1 }, QStringLiteral("moved"), &error));
        QVERIFY(error.startsWith(QStringLiteral("source_move_state"))); // restored is settled

        // Reopened, as after a crash: the journal is the same.
        PostHistoryStore reopened(f.dir.filePath(QStringLiteral("history.sqlite")), true);
        reopened.setOwnerId(QStringLiteral("owner-A"));
        const auto unsettled = reopened.unsettledSourceMoves(a);
        QCOMPARE(unsettled.size(), 1);
        QCOMPARE(unsettled.first().sequence, 0);
        QCOMPARE(unsettled.first().state, QStringLiteral("moved"));
        QCOMPARE(unsettled.first().stagedPath, QStringLiteral("/stage/x1"));

        // The entry leaves; its post keeps its owner while a source is away.
        QVERIFY(f.queue->remove({ a }, RemovePolicy::Default, f.queue->view()).ok());
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT owner_id FROM posts WHERE id=?"), { a }).toString(),
            QStringLiteral("owner-A"));
        QVERIFY2(f.store->resolveSourceMoves(a, run, { 0 }, QStringLiteral("restored"), &error),
                 qPrintable(error));
        QVERIFY(f.store->unsettledSourceMoves(a).isEmpty());
    }

    void toolSupervisionFollowsItsSteps()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        const QString run = f.queue->find(a)->runId;
        QString error;
        QVERIFY(f.store->createArtifactSet({ QStringLiteral("set-1"),
                                             a,
                                             run,
                                             1,
                                             0,
                                             QStringLiteral("/r"),
                                             QStringLiteral("/r/a"),
                                             {} },
                                           &error));
        auto step = [&](const QString &capability, const QString &state) {
            return f.store->recordToolSupervision(a,
                                                  run,
                                                  QStringLiteral("set-1"),
                                                  capability,
                                                  state,
                                                  QStringLiteral("{\"pid\":42}"),
                                                  &error);
        };
        QVERIFY(!step({}, QStringLiteral("starting"))); // how it stops comes first
        QCOMPARE(error, QStringLiteral("supervision_missing"));
        QVERIFY(step(QStringLiteral("linux_test"), QStringLiteral("starting")));
        QVERIFY(step({}, QStringLiteral("running")));
        QVERIFY(!step({}, QStringLiteral("idle")));
        QVERIFY(step({}, QStringLiteral("quiescing")));
        QVERIFY(step({}, QStringLiteral("unproven")));
        QVERIFY(!step({}, QStringLiteral("starting"))); // nothing new on an unproven stop
        QVERIFY(step({}, QStringLiteral("stopped_verified")));
        QVERIFY(step({}, QStringLiteral("starting")));
        PostHistoryStore reopened(f.dir.filePath(QStringLiteral("history.sqlite")), true);
        const auto set = reopened.artifactSet(QStringLiteral("set-1"));
        QCOMPARE(set->supervision, QStringLiteral("linux_test"));
        QCOMPARE(set->toolState, QStringLiteral("starting"));
        QCOMPARE(set->toolIdentityJson, QStringLiteral("{\"pid\":42}"));
    }

    void anUnsettledJournalKeepsItsPostFromDeletion()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        const QString run = f.queue->find(a)->runId;
        QString error;
        QVERIFY(f.store->createArtifactSet({ QStringLiteral("set-1"),
                                             a,
                                             run,
                                             1,
                                             0,
                                             QStringLiteral("/r"),
                                             QStringLiteral("/r/a"),
                                             {} },
                                           &error));
        QVERIFY(f.store->planSourceMoves(a,
                                         run,
                                         QStringLiteral("set-1"),
                                         { { 0,
                                             QStringLiteral("/in/a"),
                                             QStringLiteral("/stage/a"),
                                             QStringLiteral("{\"ino\":1}"),
                                             {},
                                             {},
                                             {} } },
                                         &error));
        QVERIFY(f.queue->remove({ a }, RemovePolicy::Default, f.queue->view()).ok());
        QVERIFY(!f.store->deletePost(a, &error));
        QVERIFY(error.startsWith(QStringLiteral("source_journal_unsettled")));
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM posts WHERE id=?"), { a }).toInt(),
                 1);
        QVERIFY(f.store->resolveSourceMoves(a, run, { 0 }, QStringLiteral("restored"), &error));
        QVERIFY2(f.store->deletePost(a, &error), qPrintable(error));
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM posts WHERE id=?"), { a }).toInt(),
                 0);
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_artifact_sets")).toInt(), 0);
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_source_moves")).toInt(), 0);
    }

    void preparationFactsNeedTheirSession()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const QString run = f.queue->find(a)->runId;
        QString error;
        QVERIFY(f.store->planSourceMoves(a,
                                         run,
                                         {},
                                         { { 0,
                                             QStringLiteral("/in/a"),
                                             QStringLiteral("/stage/a"),
                                             QStringLiteral("{\"ino\":1}"),
                                             {},
                                             {},
                                             {} } },
                                         &error));
        f.store->setOwnerId(QString()); // speaks for no session
        QVERIFY(!f.store->resolveSourceMoves(a, run, { 0 }, QStringLiteral("restored"), &error));
        QVERIFY(error.startsWith(QStringLiteral("not_owner")));
        f.store->setOwnerId(QStringLiteral("owner-B")); // another one
        QVERIFY(!f.store->resolveSourceMoves(a, run, { 0 }, QStringLiteral("restored"), &error));
        QVERIFY(error.startsWith(QStringLiteral("not_owner")));
        QCOMPARE(f.scalar(QStringLiteral("SELECT state FROM post_source_moves")).toString(),
                 QStringLiteral("planned"));
    }

    void aVerifiedSizeOutlivesItsInvalidation()
    {
        Fixture f;
        PersistedQueueOptions o = makeOptions(QStringLiteral("k"), true);
        o.job.keepRar = true;
        const qint64 a = f.queue->submit(o, Origin::Auto).postIds.value(0);
        const QString run = f.queue->find(a)->runId;
        const ArtifactSetRef ref{ QStringLiteral("set-1"), 1, 0, 0, QStringLiteral("/tmp/root") };
        QVERIFY(f.queue->engineBeginPreparation(a, ref));
        QString error;
        QVERIFY(f.store->createArtifactSet(
            { ref.id, a, run, 1, 0, ref.root, QStringLiteral("/tmp/root/archives"), {} },
            &error));
        QVERIFY2(f.store->recordPreparedArtifacts(a,
                                                  run,
                                                  ref.id,
                                                  { { 0,
                                                      QStringLiteral("/tmp/root/archives/k.rar"),
                                                      3003,
                                                      QStringLiteral("2026-11-23T10:00:00Z"),
                                                      QStringLiteral("{\"ino\":7}"),
                                                      QStringLiteral("archive"),
                                                      true,
                                                      {} } },
                                                  &error),
                 qPrintable(error));
        QVERIFY(f.queue->engineRecordPrepared(a, ref)); // the model knows no size
        o.job.rarName = QStringLiteral("again");        // invalidated, kept by keepRar
        QVERIFY(f.queue->edit(a, o, f.queue->view()).ok());
        QCOMPARE(f.store->artifactSet(ref.id)->state, QStringLiteral("invalidated"));
        QCOMPARE(f.store->artifactSet(ref.id)->bytes, qint64(3003));
        QVERIFY(f.queue->setHeld({ a }, true, f.queue->view()).ok()); // written again
        QCOMPARE(f.store->artifactSet(ref.id)->bytes, qint64(3003));
    }

    void aStaleRecipeIsNeitherPreparedNorVerified()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        const QString run = f.queue->find(a)->runId;
        const ArtifactSetRef ref{ QStringLiteral("set-1"), 1, 0, 0, QStringLiteral("/r") };
        QVERIFY(f.queue->engineBeginPreparation(a, ref));
        QString error;
        QVERIFY(!f.store->createArtifactSet(
            { ref.id, a, run, 1, 5, ref.root, QStringLiteral("/r/a"), {} },
            &error));
        QVERIFY(error.startsWith(QStringLiteral("recipe_revision")));
        QVERIFY2(f.store->createArtifactSet(
                     { ref.id, a, run, 1, 0, ref.root, QStringLiteral("/r/a"), {} },
                     &error),
                 qPrintable(error));
        // The recipe changes while the set is prepared.
        f.scalar(QStringLiteral("UPDATE post_queue SET recipe_revision=recipe_revision+1"));
        QVERIFY(!f.store->recordPreparedArtifacts(a,
                                                  run,
                                                  ref.id,
                                                  { { 0,
                                                      QStringLiteral("/r/a/a.rar"),
                                                      10,
                                                      QStringLiteral("2026-11-23T10:00:00Z"),
                                                      QStringLiteral("{\"ino\":7}"),
                                                      QStringLiteral("archive"),
                                                      true,
                                                      {} } },
                                                  &error));
        QVERIFY(error.startsWith(QStringLiteral("recipe_revision")));
        QCOMPARE(f.store->artifactSet(ref.id)->state, QStringLiteral("preparing"));
    }

    void aReplayedMoveKeepsItsSet()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        const QString run = f.queue->find(a)->runId;
        QString error;
        for (int generation : { 1, 2 })
            QVERIFY(f.store->createArtifactSet({ QStringLiteral("set-%1").arg(generation),
                                                 a,
                                                 run,
                                                 generation,
                                                 0,
                                                 QStringLiteral("/r"),
                                                 QStringLiteral("/r/%1").arg(generation),
                                                 {} },
                                               &error));
        const QList<PostHistoryStore::SourceMove> move{ { 0,
                                                          QStringLiteral("/in/a"),
                                                          QStringLiteral("/stage/a"),
                                                          QStringLiteral("{\"ino\":1}"),
                                                          {},
                                                          {},
                                                          {} } };
        QVERIFY(f.store->planSourceMoves(a, run, QStringLiteral("set-1"), move, &error));
        QVERIFY(f.store->planSourceMoves(a, run, QStringLiteral("set-1"), move, &error));
        QVERIFY(!f.store->planSourceMoves(a, run, QStringLiteral("set-2"), move, &error));
        QVERIFY(error.startsWith(QStringLiteral("source_move_conflict")));
        QVERIFY(!f.store->planSourceMoves(a, run, {}, move, &error));
        QVERIFY(error.startsWith(QStringLiteral("source_move_conflict")));
    }

    void anEarlierV6DatabaseGainsTheSupervisionColumns()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("history.sqlite"));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                        QStringLiteral("early-v6"));
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE schema_meta(key TEXT PRIMARY KEY, value "
                                          "TEXT)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO schema_meta VALUES('version', '6')")));
            QVERIFY(q.exec(QStringLiteral(
                "CREATE TABLE post_artifact_sets (artifact_set_id TEXT PRIMARY KEY NOT NULL, "
                "post_id INTEGER NOT NULL, created_by_run TEXT NOT NULL, generation INTEGER NOT "
                "NULL, recipe_revision INTEGER NOT NULL, state TEXT NOT NULL, retained INTEGER NOT "
                "NULL DEFAULT 0, root TEXT NOT NULL, archive_dir TEXT, staging_dir TEXT, "
                "marker_identity_json TEXT, bytes INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT "
                "NULL, UNIQUE(post_id, generation), UNIQUE(post_id, artifact_set_id))")));
            q.finish();
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("early-v6"));
        PostHistoryStore store(path, true);
        QString error;
        QVERIFY2(store.initialize(&error), qPrintable(error));
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                    QStringLiteral("early-v6-check"));
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QStringList columns;
        {
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("PRAGMA table_info(post_artifact_sets)")));
            while (q.next())
                columns << q.value(1).toString();
        }
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(QStringLiteral("early-v6-check"));
        for (const char *column : { "supervision", "tool_state", "tool_identity_json" })
            QVERIFY2(columns.contains(QLatin1String(column)), column);
    }

    void normalizationSparesLiveOwners()
    {
        Fixture f;
        const qint64 mine = f.add(QStringLiteral("mine"));
        QVERIFY(f.store->markPostStarted(mine));
        f.store->setOwnerId(QStringLiteral("owner-B"));
        const qint64 other = f.add(QStringLiteral("other"));
        QVERIFY(f.store->markPostStarted(other));
        QString error;
        QVERIFY2(f.store->normalizeInterruptedPosts({ QStringLiteral("owner-B") }, &error),
                 qPrintable(error));
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { other }).toString(),
            QStringLiteral("posting")); // a live owner's post is untouched
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { mine }).toString(),
            QStringLiteral("posting")); // still in a queue: never declared cancelled
    }

    void legacyCleanupSparesLockedOwners()
    {
        Fixture f;
        auto other = std::make_unique<OwnerLock>(f.dir.filePath(QStringLiteral("history.sqlite")));
        QVERIFY(other->acquire());
        f.store->setOwnerId(other->ownerId());
        const qint64 theirs = f.add(QStringLiteral("theirs"));
        QVERIFY(f.store->markPostStarted(theirs));
        // Out of any queue: only its owner's lock protects it now.
        f.scalar(QStringLiteral("DELETE FROM post_queue WHERE post_id=?"), { theirs });
        f.store->setOwnerId({}); // a second session, through the historical entry point
        QString error;
        QVERIFY2(f.store->markPostCrashedArticlesUnknown(&error), qPrintable(error));
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { theirs }).toString(),
            QStringLiteral("posting"));
        other.reset(); // that session ended without cleaning up
        QVERIFY2(f.store->markPostCrashedArticlesUnknown(&error), qPrintable(error));
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { theirs }).toString(),
            QStringLiteral("cancelled"));
    }

    void aTransferredRunEndsWithItsPostOutcome()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue->engineActivate(a));
        QCOMPARE(f.scalar(QStringLiteral("SELECT state FROM post_runs WHERE post_id=?"), { a })
                     .toString(),
                 QStringLiteral("posting"));
        QVERIFY(!f.scalar(QStringLiteral("SELECT started_at FROM post_runs WHERE post_id=?"), { a })
                     .isNull());
        f.scalar(QStringLiteral("UPDATE posts SET status='success' WHERE id=?"), { a });
        QVERIFY(f.queue->engineRetire(a));
        QCOMPARE(f.scalar(QStringLiteral("SELECT result FROM post_runs WHERE post_id=?"), { a })
                     .toString(),
                 QStringLiteral("success"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT state FROM post_runs WHERE post_id=?"), { a })
                     .toString(),
                 QStringLiteral("terminal"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT end_cause FROM post_runs WHERE post_id=?"), { a })
                     .toString(),
                 QStringLiteral("normal"));
    }

    void aFailedPreparationEndsFailedButAResumeKeepsItsStatus()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        QVERIFY(f.queue->engineBeginPreparation(
            a,
            { QStringLiteral("g1"), 1, 0, 0, QStringLiteral("/tmp") }));
        QVERIFY(f.queue->engineFailPreparation(a, PrepStep::Compress, QStringLiteral("exit 2")));
        QVERIFY(!f.queue->find(a));
        QCOMPARE(f.scalar(QStringLiteral("SELECT result FROM post_runs WHERE post_id=?"), { a })
                     .toString(),
                 QStringLiteral("failed"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT end_cause FROM post_runs WHERE post_id=?"), { a })
                     .toString(),
                 QStringLiteral("technical"));
        QVERIFY(f.scalar(QStringLiteral("SELECT supervision_json FROM post_runs WHERE post_id=?"),
                         { a })
                    .toString()
                    .contains(QStringLiteral("exit 2")));
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { a }).toString(),
                 QStringLiteral("failed"));

        // A resume that fails before transferring keeps what it can resume.
        f.scalar(QStringLiteral("UPDATE posts SET status='partial' WHERE id=?"), { a });
        PersistedQueueOptions o = makeOptions(QStringLiteral("a"));
        o.job.resumeHistoryPostId = a;
        QVERIFY(f.queue->enqueueExisting(a, o, Origin::Resume).ok());
        QVERIFY(f.queue->engineFailPreparation(a, PrepStep::Collect, QStringLiteral("gone")));
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { a }).toString(),
                 QStringLiteral("partial"));
    }

    void aStoppedPreparationIsCancelled()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"), true);
        QVERIFY(f.queue->engineBeginPreparation(
            a,
            { QStringLiteral("g1"), 1, 0, 0, QStringLiteral("/tmp") }));
        QVERIFY(f.queue->engineSetLifecycle(a, Lifecycle::Quiescing, Slot::Preparation));
        QCOMPARE(f.scalar(QStringLiteral("SELECT state FROM post_runs WHERE post_id=?"), { a })
                     .toString(),
                 QStringLiteral("preparing")); // nothing was transferred
        QVERIFY(f.queue->engineRetire(a));
        QCOMPARE(f.scalar(QStringLiteral("SELECT result FROM post_runs WHERE post_id=?"), { a })
                     .toString(),
                 QStringLiteral("cancelled"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { a }).toString(),
                 QStringLiteral("cancelled"));
    }

    void f1IsRecordedOnceAndKeepsTheEntry()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue->engineActivate(a));
        const QString run = f.queue->find(a)->runId;
        QVERIFY(f.queue->engineRecordResult(attempt(a, run)));
        QCOMPARE(f.queue->find(a)->lifecycle, Lifecycle::Finalizing);
        const auto runValue = [&f, &run](const char *column) {
            return f.scalar(QStringLiteral("SELECT %1 FROM post_runs WHERE run_id=?")
                                .arg(QLatin1String(column)),
                            { run });
        };
        QCOMPARE(runValue("result").toString(), QStringLiteral("success"));
        QCOMPARE(runValue("finalization_state").toString(), QStringLiteral("nzb_pending"));
        QCOMPARE(runValue("state").toString(), QStringLiteral("settling"));
        QCOMPARE(runValue("active_ms").toLongLong(), qint64(5000));
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { a }).toString(),
                 QStringLiteral("success"));
        const auto seconds = [&f, a] {
            return f
                .scalar(QStringLiteral("SELECT active_seconds FROM post_info WHERE post_id=?"),
                        { a })
                .toLongLong();
        };
        QCOMPARE(seconds(), qint64(5));
        // The same acknowledgement again (its answer was lost): nothing doubles.
        QVERIFY(f.queue->engineRecordResult(attempt(a, run)));
        QCOMPARE(seconds(), qint64(5));
        QCOMPARE(runValue("active_ms").toLongLong(), qint64(5000));
        // A contradicting one is refused.
        QVERIFY(!f.queue->engineRecordResult(attempt(a, run, QStringLiteral("failed"))));
        QCOMPARE(runValue("result").toString(), QStringLiteral("success"));
    }

    void f3RetiresButKeepsTheOwnerUntilReleased()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue->engineActivate(a));
        const QString run = f.queue->find(a)->runId;
        QVERIFY(!f.queue->engineFinishFinalization(a, run, FinalizationStep::Kind::Complete));
        QVERIFY(f.queue->find(a)); // no F3 without F1
        QVERIFY(f.queue->engineRecordResult(attempt(a, run)));
        QVERIFY(!f.queue->engineReleaseOwnership(a)); // still queued
        QVERIFY(f.queue->engineFinishFinalization(a, run, FinalizationStep::Kind::Complete));
        QVERIFY(!f.queue->find(a));
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_queue")).toInt(), 0);
        QCOMPARE(f.scalar(QStringLiteral("SELECT finalization_state FROM post_runs WHERE run_id=?"),
                          { run })
                     .toString(),
                 QStringLiteral("complete"));
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT owner_id FROM posts WHERE id=?"), { a }).toString(),
            QStringLiteral("owner-A")); // its post actions are not over
        QVERIFY(f.queue->engineReleaseOwnership(a));
        QVERIFY(f.scalar(QStringLiteral("SELECT owner_id FROM posts WHERE id=?"), { a }).isNull());
    }

    void aReplacedRunOrAForeignOwnerIsRefused()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue->engineActivate(a));
        const QString run = f.queue->find(a)->runId;
        QVERIFY(!f.queue->engineRecordResult(attempt(a, QStringLiteral("an-older-run"))));
        f.scalar(QStringLiteral("UPDATE posts SET owner_id='owner-B' WHERE id=?"), { a });
        QVERIFY(!f.queue->engineRecordResult(attempt(a, run)));
        QVERIFY(f.scalar(QStringLiteral("SELECT result FROM post_runs WHERE run_id=?"), { run })
                    .isNull());
    }

    void anAbandonKeepsTheFactsAndLeaves()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue->engineActivate(a));
        const QString run = f.queue->find(a)->runId;
        f.scalar(QStringLiteral("UPDATE posts SET status='partial' WHERE id=?"), { a });
        QVERIFY(f.queue->engineFinishFinalization(a, run, FinalizationStep::Kind::Abandon));
        QVERIFY(!f.queue->find(a));
        QCOMPARE(f.scalar(QStringLiteral("SELECT finalization_state FROM post_runs WHERE run_id=?"),
                          { run })
                     .toString(),
                 QStringLiteral("abandoned"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { a }).toString(),
                 QStringLiteral("partial")); // facts kept, still resumable
    }

    void aKeptRunAddsItsTimeOnce()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue->engineActivate(a));
        const QString run = f.queue->find(a)->runId;
        QVERIFY(f.queue->engineKeepForRestart(a, run, 3000, true, false, true));
        const QueueEntry *entry = f.queue->find(a);
        QCOMPARE(entry->lifecycle, Lifecycle::Queued);
        QVERIFY(entry->transferManifestEngaged);
        QVERIFY(entry->runId != run);
        QCOMPARE(f.scalar(QStringLiteral("SELECT active_ms FROM post_runs WHERE run_id=?"), { run })
                     .toLongLong(),
                 qint64(3000));
        QCOMPARE(f.scalar(QStringLiteral("SELECT state FROM post_runs WHERE run_id=?"), { run })
                     .toString(),
                 QStringLiteral("interrupted"));
        // The history total counts it, then the next attempt on top.
        const auto seconds = [&f, a] {
            return f
                .scalar(QStringLiteral("SELECT active_seconds FROM post_info WHERE post_id=?"),
                        { a })
                .toLongLong();
        };
        QCOMPARE(seconds(), qint64(3));
        QVERIFY(f.queue->engineActivate(a));
        QVERIFY(f.queue->engineRecordResult(attempt(a, f.queue->find(a)->runId)));
        QCOMPARE(seconds(), qint64(8));
        // The same keep, for the run that is gone: refused, nothing added.
        QVERIFY(!f.queue->engineKeepForRestart(a, run, 3000, true, false, true));
        QCOMPARE(f.scalar(QStringLiteral("SELECT active_ms FROM post_runs WHERE run_id=?"), { run })
                     .toLongLong(),
                 qint64(3000));
    }

    void aKeepAfterF1IsRefused()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue->engineActivate(a));
        const QString run = f.queue->find(a)->runId;
        QVERIFY(f.queue->engineRecordResult(attempt(a, run)));
        QVERIFY(!f.queue->engineKeepForRestart(a, run, 1000, true, false, true));
        QCOMPARE(f.queue->find(a)->runId, run); // still finalizing that run
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_runs WHERE post_id=?"), { a })
                     .toInt(),
                 1);
    }

    void eachOperationChecksItsOwnRun()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue->engineActivate(a));
        const QString first = f.queue->find(a)->runId;
        QVERIFY(f.queue->engineKeepForRestart(a, first, 0, false, false, true));
        const QString second = f.queue->find(a)->runId;
        const quint64 revision = f.queue->find(a)->recipeRevision;
        // Declaring the current run while finalizing the old one fails.
        QueueTransaction tx;
        tx.expectedRuns.insert(a, second);
        tx.expectedRevisions.insert(a, revision);
        tx.finalizations << FinalizationStep{ a, first, FinalizationStep::Kind::Abandon };
        QCOMPARE(f.adapter->commit(tx).error.left(9), QStringLiteral("stale_run"));
        QVERIFY(f.queue->find(a));
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_queue")).toInt(), 1);
        // A renewal without the run it was issued for fails too...
        QueueTransaction renewal;
        renewal.renewRuns << a;
        renewal.expectedRevisions.insert(a, revision);
        QCOMPARE(f.adapter->commit(renewal).error, QStringLiteral("missing_precondition"));
        // ...and so does an operation that does not say which revision it saw.
        QueueTransaction unrevised;
        unrevised.finalizations << FinalizationStep{ a, second, FinalizationStep::Kind::Abandon };
        QCOMPARE(f.adapter->commit(unrevised).error, QStringLiteral("missing_precondition"));
        // A renewal and the finalization of the same post never go together.
        QueueTransaction both;
        both.expectedRuns.insert(a, second);
        both.expectedRevisions.insert(a, revision);
        both.renewRuns << a;
        both.finalizations << FinalizationStep{ a, second, FinalizationStep::Kind::Abandon };
        QCOMPARE(f.adapter->commit(both).error, QStringLiteral("conflicting_operations"));
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_runs WHERE post_id=?"), { a })
                     .toInt(),
                 2);
        // So does a command that saw another recipe revision.
        QueueTransaction revised;
        revised.expectedRevisions.insert(a, f.queue->find(a)->recipeRevision + 1);
        revised.finalizations << FinalizationStep{ a, second, FinalizationStep::Kind::Abandon };
        QVERIFY(!f.adapter->commit(revised).ok());
        QCOMPARE(f.scalar(QStringLiteral("SELECT COUNT(*) FROM post_queue")).toInt(), 1);
    }

    void aFailingFinalizationLeavesNoResult()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        QVERIFY(f.queue->engineActivate(a));
        const QString run = f.queue->find(a)->runId;
        QueueTransaction tx;
        tx.results << attempt(a, run);
        tx.finalizations << FinalizationStep{ 4242,
                                              QStringLiteral("nowhere"),
                                              FinalizationStep::Kind::Complete };
        QVERIFY(!f.adapter->commit(tx).ok());
        QVERIFY(f.scalar(QStringLiteral("SELECT result FROM post_runs WHERE run_id=?"), { run })
                    .isNull());
        QCOMPARE(f.scalar(QStringLiteral("SELECT status FROM posts WHERE id=?"), { a }).toString(),
                 QStringLiteral("queued"));
    }

    void aDeadOwnersQueueIsClaimedInOrder()
    {
        Fixture f;
        const qint64 a = f.add(QStringLiteral("a"));
        const qint64 b = f.add(QStringLiteral("b"));
        QVERIFY(f.queue->move({ b }, 0, f.queue->view()).ok());
        QVERIFY(f.queue->engineActivate(a));
        QVERIFY(f.queue->postNow(b, f.queue->view()).ok());
        f.store->setOwnerId(QStringLiteral("owner-C"));
        PostQueue session(f.adapter.get(), QStringLiteral("owner-C"));
        const qint64 c = session.submit(makeOptions(QStringLiteral("c")), Origin::Quick)
                             .postIds.value(0);
        QString error;
        QCOMPARE(f.store->claimQueue(QStringLiteral("queue-A"),
                                     QStringLiteral("owner-A"),
                                     QStringLiteral("owner-C"),
                                     QStringLiteral("owner-C"),
                                     Domain::Session,
                                     &error),
                 2);
        QVERIFY(session.load());
        QCOMPARE(session.order(), (QList<qint64>{ c, a, b }));
        QVERIFY(session.intent().isNull()); // a restored "post now" is abandoned
        QCOMPARE(
            f.scalar(QStringLiteral("SELECT owner_id FROM posts WHERE id=?"), { a }).toString(),
            QStringLiteral("owner-C"));
        QCOMPARE(f.scalar(
                      QStringLiteral("SELECT COUNT(*) FROM post_queue WHERE queue_id='queue-A'"))
                     .toInt(),
                 0);
    }

    void ownerLocksProveVitality()
    {
        QTemporaryDir dir;
        const QString db = dir.filePath(QStringLiteral("h.sqlite"));
        OwnerLock owner(db);
        QVERIFY(owner.acquire());
        QVERIFY(OwnerLock::isValidOwnerId(owner.ownerId()));
        QVERIFY(!OwnerLock::isValidOwnerId(QStringLiteral("../../etc/passwd")));
        QVERIFY(OwnerLock::ownersOnDisk(db).contains(owner.ownerId()));
        QCOMPARE(OwnerLock::probe(db, owner.ownerId()), OwnerLock::Vitality::Alive);
        const QString gone = QStringLiteral("0123abcd-0000-4000-8000-000000000000");
        std::unique_ptr<QLockFile> guard;
        QCOMPARE(OwnerLock::probe(db, gone, &guard), OwnerLock::Vitality::Dead);
        QVERIFY(guard && guard->isLocked()); // held until the claim is committed
        QCOMPARE(OwnerLock::probe(db, QStringLiteral("not-an-id")), OwnerLock::Vitality::Unknown);
    }
};

QTEST_GUILESS_MAIN(tst_QueueStoreSqlite)
#include "tst_QueueStoreSqlite.moc"
