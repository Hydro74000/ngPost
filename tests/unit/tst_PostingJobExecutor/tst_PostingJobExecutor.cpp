// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// PostingJobExecutor on real PostingJobs, an in-process NgPost, the history
// database and the mock NNTP server (plan_queue.md lot C3): a preparation
// ends without any transfer, an activation posts exactly what was prepared,
// acknowledgements are tied to their run, a run ends once, and a failed
// queue write starts nothing.
//
//========================================================================

#include "MockNntpServer.h"
#include "NgPost.h"
#include "PostingJob.h"
#include "PostingJobExecutor.h"
#include "TestEnv.h"
#include "history/HistoryQueueStore.h"
#include "history/PostHistoryService.h"
#include "queue/PostQueue.h"
#include "queue/QueueScheduler.h"
#include "utils/PathHelper.h"

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QThread>
#include <QElapsedTimer>
#include <QtTest>

#include <memory>

using namespace queue;
using ngpost::tests::HomeSandbox;
using ngpost::tests::MockNntpServer;

namespace
{

bool writeFakeTool(const QString &path, const QString &script)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(script.toUtf8());
    f.close();
    return f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                            | QFileDevice::ExeOwner);
}

//! One NgPost with a server on the mock, its history, a queue on that history
//! and the executor under test.
struct Env
{
    HomeSandbox sandbox;
    MockNntpServer mock;
    QString root;
    int argc = 2;
    QByteArray arg0 = QByteArrayLiteral("tst_PostingJobExecutor");
    QByteArray arg1 = QByteArrayLiteral("--queue-test");
    char *argv[3] = { arg0.data(), arg1.data(), nullptr };
    std::unique_ptr<NgPost> ngPost;
    std::unique_ptr<ServiceQueueStore> store;
    std::unique_ptr<PostQueue> queue;
    std::unique_ptr<PostingJobExecutor> executor;

    bool start(QString *error, const QString &rarScript = {}, const QStringList &mockArgs = {})
    {
        if (!mock.start(mockArgs)) {
            *error = QStringLiteral("the mock NNTP server did not start");
            return false;
        }
        root = sandbox.rootPath();
        ngPost = std::make_unique<NgPost>(argc, argv);
        QFile conf(PathHelper::configFilePath());
        if (!conf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            *error = QStringLiteral("could not write the test configuration");
            return false;
        }
        QTextStream(&conf) << "GROUPS = alt.binaries.test\nthread = 1\nnzbPath = " << root
                           << "\nPOST_DB = " << dbPath() << "\n[server]\nhost = 127.0.0.1\n"
                           << "port = " << mock.port() << "\nssl = false\nconnection = 1\n";
        conf.close();
        *error = ngPost->parseDefaultConfig();
        if (!error->isEmpty())
            return false;
        if (!ngPost->ensureHistoryStoreForTest()) {
            *error = QStringLiteral("the history database did not open");
            return false;
        }
#ifdef Q_OS_UNIX
        // Stand-ins for rar and par2, as tst_PostFlow: CI has neither.
        const QString rar = rarScript.isEmpty()
            ? QStringLiteral("#!/bin/sh\nfor a in \"$@\"; do case \"$a\" in *.rar) t=\"$a\";; "
                             "esac; done\nhead -c 40000 /dev/zero > \"$t\"\nexit 0\n")
            : rarScript;
        if (!writeFakeTool(root + QStringLiteral("/fakerar"), rar)
            || !writeFakeTool(root + QStringLiteral("/fakepar2"),
                              QStringLiteral("#!/bin/sh\nfor a in \"$@\"; do case \"$a\" in "
                                             "*.par2) t=\"$a\";; esac; done\n"
                                             "head -c 4000 /dev/zero > \"$t\"\nexit 0\n"))) {
            *error = QStringLiteral("could not write the fake tools");
            return false;
        }
#else
        Q_UNUSED(rarScript)
#endif
        store = std::make_unique<ServiceQueueStore>(ngPost->historyService());
        queue = std::make_unique<PostQueue>(store.get(), QStringLiteral("session-test"));
        executor = std::make_unique<PostingJobExecutor>(queue.get(),
                                                        [this](const QueueEntry &entry,
                                                               const JobExecutionContext &context) {
            return new PostingJob(ngPost.get(), entry.options.job, context);
        },
                                                        QStringLiteral("owner-test"));
        return true;
    }

    QString dbPath() const { return root + QStringLiteral("/history.sqlite"); }

    PersistedQueueOptions post(const QString &name,
                               bool compress = false,
                               bool par2 = false,
                               int bytes = 64000)
    {
        const QString source = root + QStringLiteral("/%1.bin").arg(name);
        QFile file(source);
        if (file.open(QIODevice::WriteOnly))
            file.write(QByteArray(bytes, 'a'));
        file.close();
        QDir().mkpath(root + QStringLiteral("/tmp"));
        PersistedQueueOptions o;
        o.job.files = { QFileInfo(source) };
        o.job.inputPaths = { source };
        o.job.nzbFilePath = root + QStringLiteral("/%1.nzb").arg(name);
        o.job.grpList = { QStringLiteral("alt.binaries.test") };
        o.job.from = "poster@example.invalid";
        o.job.articleSizeBytes = 16384;
        o.job.doCompress = compress;
        o.job.doPar2 = par2;
        o.job.par2Pct = 10;
        o.job.keepRar = false;
        o.job.tmpPath = root + QStringLiteral("/tmp");
        o.job.rarName = name + QStringLiteral("-arch");
        o.job.rarTool = QStringLiteral("rar");
        o.job.rarPath = root + QStringLiteral("/fakerar");
        o.job.par2Tool = par2::Tool::Par2cmdline;
        o.job.par2Path = root + QStringLiteral("/fakepar2");
        o.archiverFamily = compress ? QStringLiteral("rar") : QString();
        o.par2Family = par2 ? par2::toolName(par2::Tool::Par2cmdline) : QString();
        PersistedSource persisted;
        persisted.path = source;
        persisted.sizeBytes = bytes;
        o.sources << persisted;
        return o;
    }

    qint64 submit(const PersistedQueueOptions &options)
    {
        return queue->submit(options, Origin::Cli).postIds.value(0);
    }

    //! Runs every step of a preparation through the executor alone.
    bool prepare(qint64 postId, const QString &runId, const QString &setId, QString *error)
    {
        QSignalSpy steps(executor.get(), &QueueExecutor::stepFinished);
        const QList<PrepStep> all = { PrepStep::Compress, PrepStep::Par2, PrepStep::Collect };
        for (int i = 0; i < all.size(); ++i) {
            executor->prepareStep(postId, runId, setId, all[i]);
            if (!QTest::qWaitFor([&] { return steps.size() > i; }, 15000)) {
                *error = QStringLiteral("step %1 never ended").arg(i);
                return false;
            }
            if (!steps.at(i).at(4).toBool()) {
                *error =
                    QStringLiteral("step %1 failed: %2").arg(i).arg(steps.at(i).at(5).toString());
                return false;
            }
        }
        return true;
    }

    QString historyStatus(qint64 postId) const
    {
        PostHistoryStore::PostDetails details;
        if (!ngPost->historyService()->loadPostDetails(postId, &details))
            return QStringLiteral("<unreadable>");
        return details.post.status;
    }
};

} // namespace

class tst_PostingJobExecutor : public QObject
{
    Q_OBJECT

private slots:
    void preparationEndsWithoutAnyTransfer()
    {
#ifndef Q_OS_UNIX
        QSKIP("the fake rar/par2 tools are shell scripts");
#else
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("prepared"), true, true));
        QVERIFY(id > 0);
        const QString run = env.queue->find(id)->runId;
        QVERIFY2(env.prepare(id, run, QStringLiteral("set-1"), &error), qPrintable(error));

        const PreparedManifest manifest = env.executor->manifest(id, run, QStringLiteral("set-1"));
        QVERIFY(manifest.isValid());
        QVERIFY(manifest.compressed);
        QCOMPARE(manifest.files.size(), 2); // the archive and its parity, in order
        QVERIFY(manifest.files.first().endsWith(QStringLiteral(".par2")));
        QVERIFY(manifest.files.last().endsWith(QStringLiteral(".rar")));
        QCOMPARE(manifest.bytes, qint64(44000));
        QVERIFY(!env.executor->runtime(id));  // no job while the entry waits
        QTest::qWait(50);                     // the released job is gone...
        QVERIFY(QDir(manifest.dir).exists()); // ...and its files stay
        QCOMPARE(env.mock.receivedArticles().size(), 0);
#endif
    }

    void activationPostsExactlyThePreparedFiles()
    {
#ifndef Q_OS_UNIX
        QSKIP("the fake rar/par2 tools are shell scripts");
#else
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("activated"), true, true));
        const QString run = env.queue->find(id)->runId;
        QVERIFY2(env.prepare(id, run, QStringLiteral("set-1"), &error), qPrintable(error));

        QSignalSpy started(env.executor.get(), &QueueExecutor::transferStarted);
        QSignalSpy finished(env.executor.get(), &QueueExecutor::transferFinished);
        QSignalSpy finalized(env.executor.get(), &QueueExecutor::finalized);
        env.executor->activate(id, run, QStringLiteral("set-1"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QCOMPARE(started.size(), 1);
        QCOMPARE(finished.first().at(2).value<AttemptResult>().result, QStringLiteral("success"));
        QCOMPARE(finished.first().at(1).toString(), run);
        QVERIFY(!QFileInfo::exists(env.root + QStringLiteral("/activated.nzb"))); // not before F2
        env.executor->finalize(id, run);
        QTRY_COMPARE_WITH_TIMEOUT(finalized.size(), 1, 10000);
        QVERIFY(finalized.first().at(2).toBool());

        QFile nzb(env.root + QStringLiteral("/activated.nzb"));
        QVERIFY(nzb.open(QIODevice::ReadOnly));
        const QByteArray content = nzb.readAll();
        QVERIFY(content.contains("activated-arch.rar"));
        QVERIFY(content.contains("activated-arch.par2"));
        QVERIFY(!content.contains("activated.bin")); // never the original
        // The streamed copy is gone once the deliverable exists.
        QVERIFY(!QDir(env.root + QStringLiteral("/.ngpost-work")).exists());
#endif
    }

    void postWithoutPackingIsActivatedDirectly()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("plain")));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy finished(env.executor.get(), &QueueExecutor::transferFinished);
        env.executor->activate(id, run, {});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QCOMPARE(finished.first().at(2).value<AttemptResult>().result, QStringLiteral("success"));
        QVERIFY(env.mock.receivedArticles().size() > 0);
        // The runtime waits for its finalization, then goes with its local work.
        QVERIFY(env.executor->runtime(id));
        QSignalSpy local(env.executor.get(), &PostingJobExecutor::localWorkQueued);
        env.executor->completeLocalWork(id, run);
        QCOMPARE(local.size(), 1);
        QTRY_VERIFY(!env.executor->runtime(id));
    }

    void missingManifestPublishesNothing()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("unprepared"), true));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy finished(env.executor.get(), &QueueExecutor::transferFinished);
        env.executor->activate(id, run, QStringLiteral("never-prepared"));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QCOMPARE(finished.first().at(2).value<AttemptResult>().result, QStringLiteral("failed"));
        QTest::qWait(200);
        QCOMPARE(env.mock.receivedArticles().size(), 0); // the original is not posted
        // No NZB was produced: no post action follows.
        QSignalSpy finalized(env.executor.get(), &QueueExecutor::finalized);
        env.executor->finalize(id, run);
        QTRY_COMPARE(finalized.size(), 1);
        QVERIFY(finalized.first().at(2).toBool()); // nothing to publish is not a failure
        QSignalSpy local(env.executor.get(), &PostingJobExecutor::localWorkQueued);
        env.executor->completeLocalWork(id, run);
        QCOMPARE(local.size(), 1);
        QVERIFY(!local.first().at(2).toBool());
    }

    void aRunEndsOnceAndStaleCallsAreRefused()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("once")));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy finished(env.executor.get(), &QueueExecutor::transferFinished);
        env.executor->activate(id, run, {});
        PostingJob *job = env.executor->runtime(id);
        QVERIFY(job);
        // PostingJob may say both: the second must not end the run again.
        connect(job, &PostingJob::postingFinished, job, [job] {
            emit job->noMoreConnection();
        }, Qt::DirectConnection);
        // A second activation of the same post, under another run, is refused.
        env.executor->activate(id, QStringLiteral("another-run"), {});
        QTRY_VERIFY_WITH_TIMEOUT(finished.size() >= 2, 20000);
        QTest::qWait(200);
        int ofRun = 0;
        for (const QList<QVariant> &end : finished)
            if (end.at(1).toString() == run)
                ++ofRun;
        QCOMPARE(ofRun, 1);
        QCOMPARE(finished.size(), 2); // the refused one says so, under its own run
    }

    void stoppingAPreparationProvesTheToolStopped()
    {
#ifndef Q_OS_UNIX
        QSKIP("the fake rar tool is a shell script");
#else
        Env env;
        QString error;
        QVERIFY2(env.start(&error, QStringLiteral("#!/bin/sh\nexec sleep 30\n")),
                 qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("stopped"), true));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        QSignalSpy quiesced(env.executor.get(), &QueueExecutor::quiesced);
        env.executor->prepareStep(id, run, QStringLiteral("set-1"), PrepStep::Compress);
        PostingJob *job = env.executor->runtime(id);
        QVERIFY(job);
        QTest::qWait(200); // the tool is running
        env.executor->interrupt(id, run, Disposition::Keep);
        QTRY_COMPARE_WITH_TIMEOUT(quiesced.size(), 1, 10000);
        QVERIFY(quiesced.first().at(2).toBool());
        QVERIFY(!env.executor->runtime(id));
        QTest::qWait(200);
        QCOMPARE(steps.size(), 0); // the killed tool's end is not a step end
        QVERIFY(QFileInfo::exists(env.root + QStringLiteral("/stopped.bin")));
        QCOMPARE(env.historyStatus(id), QStringLiteral("queued")); // nothing was finalized
#endif
    }

    void aConfirmedArticleIsRecordedBeforeTheUiHearsOfIt()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, {}, { QStringLiteral("--slow-mode-ms"), QStringLiteral("20") }),
                 qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("captured"), false, false, 3000000));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy started(env.executor.get(), &QueueExecutor::transferStarted);
        env.executor->activate(id, run, {});
        QTRY_COMPARE_WITH_TIMEOUT(started.size(), 1, 10000);
        const int before = env.mock.receivedArticles().size();
        // From here on the UI thread runs no event: only the connections work.
        QElapsedTimer wait;
        wait.start();
        while (wait.elapsed() < 2000)
            QThread::msleep(20);
        const int received = env.mock.receivedArticles().size();
        QVERIFY2(received > before + 5, "the connections did not post on their own");
        QVERIFY2(env.ngPost->historyService()->flush(&error), qPrintable(error));
        const QString name = QUuid::createUuid().toString();
        int recorded = 0;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(env.dbPath());
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT COUNT(*) FROM post_articles a JOIN post_files f ON "
                                     "f.id = a.file_id WHERE f.post_id=? AND a.status='posted'"));
            q.addBindValue(id);
            QVERIFY(q.exec() && q.next());
            recorded = q.value(0).toInt();
        }
        QSqlDatabase::removeDatabase(name);
        // At most the article whose reply was in flight when the count was taken.
        QVERIFY2(recorded >= received - 1,
                 qPrintable(QStringLiteral("%1 confirmed by the server, %2 recorded")
                                .arg(received)
                                .arg(recorded)));
        QSignalSpy finished(env.executor.get(), &QueueExecutor::transferFinished);
        env.executor->interrupt(id, run, Disposition::Stop);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        QCOMPARE(finished.first().at(2).value<AttemptResult>().result, QStringLiteral("failed"));
    }

    void finishingATransferRecordsNoResult()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, {}, { QStringLiteral("--slow-mode-ms"), QStringLiteral("20") }),
                 qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("finished"), false, false, 2000000));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy started(env.executor.get(), &QueueExecutor::transferStarted);
        env.executor->activate(id, run, {});
        QTRY_COMPARE_WITH_TIMEOUT(started.size(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 2, 10000);
        PostingJob *job = env.executor->runtime(id);
        QVERIFY(job);
        const PostingJob::TransferOutcome first = job->finishTransfer();
        QCOMPARE(first.result, QStringLiteral("failed")); // stopped before its end
        QVERIFY(first.activeMs > 0);
        const int received = env.mock.receivedArticles().size();
        QTest::qWait(300);
        QCOMPARE(env.mock.receivedArticles().size(), received); // nothing produces any more
        const PostingJob::TransferOutcome again = job->finishTransfer();
        QCOMPARE(again.activeMs, first.activeMs); // frozen, once
        // The result belongs to the controller (F1): nothing was recorded.
        QCOMPARE(env.historyStatus(id), QStringLiteral("posting"));
    }

    void keepingATransferStopsItWithoutAResult()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, {}, { QStringLiteral("--slow-mode-ms"), QStringLiteral("20") }),
                 qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("kept"), false, false, 2000000));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy started(env.executor.get(), &QueueExecutor::transferStarted);
        QSignalSpy quiesced(env.executor.get(), &QueueExecutor::quiesced);
        QSignalSpy finished(env.executor.get(), &QueueExecutor::transferFinished);
        env.executor->activate(id, run, {});
        QTRY_COMPARE_WITH_TIMEOUT(started.size(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 2, 10000);
        env.executor->interrupt(id, run, Disposition::Keep);
        QTRY_COMPARE_WITH_TIMEOUT(quiesced.size(), 1, 10000);
        QVERIFY(quiesced.first().at(2).toBool()); // stop proven
        QVERIFY(quiesced.first().at(3).toBool()); // its transfer manifest is engaged
        QVERIFY(quiesced.first().at(4).toLongLong() > 0);
        QVERIFY(!env.executor->runtime(id));
        const int received = env.mock.receivedArticles().size();
        QTest::qWait(300);
        QCOMPARE(env.mock.receivedArticles().size(), received);
        QCOMPARE(finished.size(), 0);                               // no result...
        QCOMPARE(env.historyStatus(id), QStringLiteral("posting")); // ...recorded
        const QString name = QUuid::createUuid().toString();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(env.dbPath());
            QVERIFY(db.open());
            QSqlQuery q(db);
            q.prepare(
                QStringLiteral("SELECT transfer_manifest_complete FROM post_info WHERE post_id=?"));
            q.addBindValue(id);
            QVERIFY(q.exec() && q.next());
            QCOMPARE(q.value(0).toInt(), 1);
        }
        QSqlDatabase::removeDatabase(name);
    }

    void anUnrecordedFileSendsNothing()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("unrecorded")));
        const QString run = env.queue->find(id)->runId;
        const QString name = QUuid::createUuid().toString();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(env.dbPath());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("CREATE TRIGGER refuse_files BEFORE INSERT ON post_files "
                                          "BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        }
        QSqlDatabase::removeDatabase(name);
        QSignalSpy finished(env.executor.get(), &QueueExecutor::transferFinished);
        env.executor->activate(id, run, {});
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY(!finished.first().at(2).value<AttemptResult>().transferred);
        QTest::qWait(300);
        QCOMPARE(env.mock.receivedArticles().size(), 0); // a manifest missing a file: nothing goes
    }

    void failedQueueWriteStartsNoTransfer()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        // A real SQLite refusal: no queue row may change stage, so the
        // activation cannot be written (the admission itself goes through).
        const QString connection = QUuid::createUuid().toString();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(env.dbPath());
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY2(q.exec(QStringLiteral("CREATE TRIGGER refuse_queue_updates BEFORE UPDATE OF "
                                           "stage ON post_queue WHEN NEW.stage <> OLD.stage "
                                           "BEGIN SELECT RAISE(ABORT, 'injected'); END")),
                     qPrintable(q.lastError().text()));
        }
        QueueScheduler scheduler(env.queue.get(), env.executor.get(), {});
        const qint64 id = env.submit(env.post(QStringLiteral("frozen")));
        QVERIFY(id > 0); // an admission is an insert
        QTRY_VERIFY_WITH_TIMEOUT(scheduler.isTechnicallyPaused(), 5000);
        QTest::qWait(300);
        QVERIFY(!env.executor->runtime(id));
        QCOMPARE(env.mock.receivedArticles().size(), 0);
        QCOMPARE(env.queue->find(id)->lifecycle, Lifecycle::Queued);

        {
            QSqlDatabase db = QSqlDatabase::database(connection);
            QSqlQuery q(db);
            QVERIFY(q.exec(QStringLiteral("DROP TRIGGER refuse_queue_updates")));
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        scheduler.retryAfterTechnicalPause();
        QTRY_VERIFY_WITH_TIMEOUT(!env.queue->find(id), 20000); // posted, then retired
        QVERIFY(env.mock.receivedArticles().size() > 0);
        QCOMPARE(env.historyStatus(id), QStringLiteral("success"));
    }
};

QTEST_MAIN(tst_PostingJobExecutor)
#include "tst_PostingJobExecutor.moc"
