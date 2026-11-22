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
