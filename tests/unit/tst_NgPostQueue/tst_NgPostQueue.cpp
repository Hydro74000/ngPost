// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// NgPost with its queue engine on (NGPOST_QUEUE_ENGINE=1, plan_queue.md lot
// C3), in process, on the history database and the mock NNTP server: what
// happens when the queue cannot write. A monitored file is admitted later, a
// global cancel that was not written starts nothing, and Resume ends a
// technical pause.
//
//========================================================================

#include "MockNntpServer.h"
#include "NgPost.h"
#include "PostingJobOptionsCodec.h"
#include "queue/QueueEntry.h"
#include "TestEnv.h"
#include "history/PostHistoryService.h"
#include "queue/PostQueue.h"
#include "queue/QueueScheduler.h"
#include "utils/PathHelper.h"

#include <QDir>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include <memory>

using namespace queue;
using ngpost::tests::HomeSandbox;
using ngpost::tests::MockNntpServer;

namespace
{

struct Env
{
    HomeSandbox sandbox;
    MockNntpServer mock;
    QString root;
    QByteArray previousSwitch = qgetenv("NGPOST_QUEUE_ENGINE");
    int argc = 2;
    QByteArray arg0 = QByteArrayLiteral("tst_NgPostQueue");
    QByteArray arg1 = QByteArrayLiteral("--queue-test");
    char *argv[3] = { arg0.data(), arg1.data(), nullptr };
    std::unique_ptr<NgPost> ngPost;

    ~Env()
    {
        ngPost.reset();
        if (previousSwitch.isNull())
            qunsetenv("NGPOST_QUEUE_ENGINE");
        else
            qputenv("NGPOST_QUEUE_ENGINE", previousSwitch);
    }

    bool start(QString *error, const QStringList &mockArgs = {}, const QString &extraConfig = {})
    {
        if (!mock.start(mockArgs)) {
            *error = QStringLiteral("the mock NNTP server did not start");
            return false;
        }
        root = sandbox.rootPath();
        qputenv("NGPOST_QUEUE_ENGINE", "1"); // read once, when NgPost is built
        ngPost = std::make_unique<NgPost>(argc, argv);
        QFile conf(PathHelper::configFilePath());
        if (!conf.open(QIODevice::WriteOnly | QIODevice::Text)) {
            *error = QStringLiteral("could not write the test configuration");
            return false;
        }
        QTextStream(&conf) << "GROUPS = alt.binaries.test\nthread = 1\nnzbPath = " << root
                           << "\nPOST_DB = " << dbPath() << "\n[server]\nhost = 127.0.0.1\n"
                           << "port = " << mock.port() << "\nssl = false\nconnection = 1\n";
        if (!extraConfig.isEmpty()) {
            // Global keys must precede the [server] section: written again, first.
            conf.close();
            QFile again(PathHelper::configFilePath());
            if (!again.open(QIODevice::ReadOnly | QIODevice::Text)) {
                *error = QStringLiteral("could not read the test configuration back");
                return false;
            }
            const QByteArray body = again.readAll();
            again.close();
            if (!again.open(QIODevice::WriteOnly | QIODevice::Text)) {
                *error = QStringLiteral("could not rewrite the test configuration");
                return false;
            }
            again.write(extraConfig.toUtf8() + body);
            again.close();
        }
        conf.close();
        *error = ngPost->parseDefaultConfig();
        if (!error->isEmpty())
            return false;
        if (!ngPost->queueEngineEnabled() || !ngPost->ensureHistoryStoreForTest()) {
            *error = QStringLiteral("no queue engine or no history database");
            return false;
        }
        return true;
    }

    QString dbPath() const { return root + QStringLiteral("/history.sqlite"); }

    QFileInfo source(const QString &name, int bytes = 20000) const
    {
        const QString path = root + QStringLiteral("/%1.bin").arg(name);
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
            file.write(QByteArray(bytes, 'n'));
        file.close();
        return QFileInfo(path);
    }

    PostingJobOptions post(const QString &name, int bytes = 20000) const
    {
        PostingJobOptions o;
        const QFileInfo file = source(name, bytes);
        o.files = { file };
        o.inputPaths = { file.absoluteFilePath() };
        o.nzbFilePath = root + QStringLiteral("/%1.nzb").arg(name);
        o.grpList = { QStringLiteral("alt.binaries.test") };
        o.from = "poster@example.invalid";
        o.articleSizeBytes = 16384;
        return o;
    }

    //! Runs @p sql on a connection of its own, as another program would.
    bool sql(const QString &statement)
    {
        const QString name = QUuid::createUuid().toString();
        bool ok = false;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(dbPath());
            QSqlQuery q(db);
            ok = db.open() && q.exec(statement);
            if (!ok)
                qWarning() << q.lastError().text();
        }
        QSqlDatabase::removeDatabase(name);
        return ok;
    }

    QVariant scalar(const QString &statement)
    {
        const QString name = QUuid::createUuid().toString();
        QVariant value;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(dbPath());
            QSqlQuery q(db);
            if (db.open() && q.exec(statement) && q.next())
                value = q.value(0);
        }
        QSqlDatabase::removeDatabase(name);
        return value;
    }
};

} // namespace

class tst_NgPostQueue : public QObject
{
    Q_OBJECT

private slots:
    void aRefusedMonitoredFileIsAdmittedLater()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        QVERIFY(env.sql(QStringLiteral("CREATE TRIGGER refuse_posts BEFORE INSERT ON posts "
                                       "BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        env.ngPost->setAdmissionRetryBaseMsForTest(100);
        env.ngPost->postMonitoredForTest(env.source(QStringLiteral("watched")));
        QCOMPARE(env.ngPost->pendingAdmissionRetriesForTest(), 1);
        QVERIFY(env.sql(QStringLiteral("DROP TRIGGER refuse_posts")));
        // Without any new event from the watcher.
        QTRY_COMPARE_WITH_TIMEOUT(env.scalar(QStringLiteral("SELECT status FROM posts")).toString(),
                                  QStringLiteral("success"),
                                  20000);
        QCOMPARE(env.ngPost->pendingAdmissionRetriesForTest(), 0);
        QCOMPARE(env.scalar(QStringLiteral("SELECT COUNT(*) FROM posts")).toInt(), 1);
    }

    void aCancelThatCannotBeWrittenStartsNothing()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const NgPost::QueueAdmission first = env.ngPost->admitPost(env.post(QStringLiteral("one")),
                                                                   Origin::Cli);
        QVERIFY(first.accepted);
        env.ngPost->postQueue()->setPaused(true); // before the scheduler looks at it
        QVERIFY(env.ngPost->admitPost(env.post(QStringLiteral("two")), Origin::Cli).accepted);
        QVERIFY(env.sql(QStringLiteral("CREATE TRIGGER refuse_removal BEFORE DELETE ON post_queue "
                                       "BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        env.ngPost->cancelAllPostingJobs();
        QTest::qWait(300);
        QCOMPARE(env.ngPost->postQueue()->order().size(), 2);
        QVERIFY(env.ngPost->postQueue()->isPaused()); // nothing started against the request
        QVERIFY(env.ngPost->isPaused());
        QVERIFY(!env.ngPost->cancelingAllForTest()); // it may be asked again
        QCOMPARE(env.mock.receivedArticles().size(), 0);

        QVERIFY(env.sql(QStringLiteral("DROP TRIGGER refuse_removal")));
        env.ngPost->cancelAllPostingJobs();
        QTRY_VERIFY_WITH_TIMEOUT(env.ngPost->postQueue()->order().isEmpty(), 5000);
        QVERIFY(!env.ngPost->postQueue()->isPaused());
        QCOMPARE(env.scalar(QStringLiteral("SELECT COUNT(*) FROM posts WHERE status='cancelled'"))
                     .toInt(),
                 2);
        QCOMPARE(env.mock.receivedArticles().size(), 0);
    }

    void aCancelWhoseStopIsNotWrittenCanBeAskedAgain()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, { QStringLiteral("--slow-mode-ms"), QStringLiteral("30") }),
                 qPrintable(error));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(
            env.post(QStringLiteral("long"), 2 * 1024 * 1024),
            Origin::Cli);
        QVERIFY(admission.accepted);
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 0, 15000);
        QVERIFY(env.sql(
            QStringLiteral("CREATE TRIGGER refuse_stop BEFORE UPDATE OF stage ON post_queue WHEN "
                           "NEW.stage='quiescing' BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        env.ngPost->cancelAllPostingJobs();
        QTRY_VERIFY_WITH_TIMEOUT(env.ngPost->queueSchedulerForTest()->isTechnicallyPaused(), 5000);
        QVERIFY(!env.ngPost->cancelingAllForTest()); // Cancel is not stuck
        QVERIFY(env.ngPost->postQueue()->find(admission.postId));
        env.ngPost->cancelAllPostingJobs(); // refused a second time, before any repair
        QTest::qWait(100);
        QVERIFY(!env.ngPost->cancelingAllForTest());
        QVERIFY(env.ngPost->isPaused()); // the transfer is held meanwhile

        QVERIFY(env.sql(QStringLiteral("DROP TRIGGER refuse_stop")));
        env.ngPost->cancelAllPostingJobs(); // asked again: written this time
        QTRY_VERIFY_WITH_TIMEOUT(!env.ngPost->postQueue()->find(admission.postId), 20000);
        QVERIFY(env.scalar(QStringLiteral("SELECT status FROM posts")).toString()
                != QLatin1String("success"));
    }

    void aMonitorStopThatCannotBeWrittenHoldsAutoPosts()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(env.post(
                                                                           QStringLiteral("auto")),
                                                                       Origin::Auto);
        QVERIFY(admission.accepted);
        env.ngPost->postQueue()->setPaused(true); // before the scheduler looks at it
        QVERIFY(env.sql(QStringLiteral("CREATE TRIGGER refuse_removal BEFORE DELETE ON post_queue "
                                       "BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        env.ngPost->closeAllMonitoringJobs();
        env.ngPost->postQueue()->setPaused(false); // what an unrelated resume would do...
        env.ngPost->closeAllMonitoringJobs();      // ...is caught by the next refusal
        QTest::qWait(500);
        QVERIFY(env.ngPost->postQueue()->find(admission.postId));
        QVERIFY(env.ngPost->postQueue()->isPaused());
        QVERIFY(env.ngPost->isPaused());
        QCOMPARE(env.mock.receivedArticles().size(), 0);
    }

    void aMonitorStopHoldsAnAutoTransferItCouldNotStop()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, { QStringLiteral("--slow-mode-ms"), QStringLiteral("30") }),
                 qPrintable(error));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(
            env.post(QStringLiteral("watched"), 2 * 1024 * 1024),
            Origin::Auto);
        QVERIFY(admission.accepted);
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 0, 15000);
        QVERIFY(env.sql(
            QStringLiteral("CREATE TRIGGER refuse_stop BEFORE UPDATE OF stage ON post_queue WHEN "
                           "NEW.stage='quiescing' BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        env.ngPost->closeAllMonitoringJobs();
        QVERIFY(env.ngPost->postQueue()->isPaused());
        QTest::qWait(500); // what was in flight lands
        const int received = env.mock.receivedArticles().size();
        QTest::qWait(1000);
        QCOMPARE(env.mock.receivedArticles().size(), received); // held, not going on

        QVERIFY(env.sql(QStringLiteral("DROP TRIGGER refuse_stop")));
        env.ngPost->closeAllMonitoringJobs();
        QTRY_VERIFY_WITH_TIMEOUT(!env.ngPost->postQueue()->find(admission.postId), 20000);
    }

    void aGlobalCancelDropsRefusedAdmissions()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        QVERIFY(env.sql(QStringLiteral("CREATE TRIGGER refuse_posts BEFORE INSERT ON posts "
                                       "BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        env.ngPost->setAdmissionRetryBaseMsForTest(200);
        env.ngPost->postMonitoredForTest(env.source(QStringLiteral("refused")));
        QCOMPARE(env.ngPost->pendingAdmissionRetriesForTest(), 1);
        QVERIFY(env.ngPost->hasPostingJobs());
        env.ngPost->cancelAllPostingJobs();
        QCOMPARE(env.ngPost->pendingAdmissionRetriesForTest(), 0);
        QVERIFY(!env.ngPost->cancelingAllForTest());
        QVERIFY(!env.ngPost->hasPostingJobs());
        QVERIFY(!env.ngPost->postQueue()->isPaused());
        QVERIFY(env.sql(QStringLiteral("DROP TRIGGER refuse_posts")));
        QTest::qWait(800); // past the retry it would have made
        QCOMPARE(env.scalar(QStringLiteral("SELECT COUNT(*) FROM posts")).toInt(), 0);
    }

    void aGlobalCancelDropsRetriesOfAnEngineNeverBuilt()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        env.ngPost->setQueueEngineUnavailableForTest(true);
        env.ngPost->setAdmissionRetryBaseMsForTest(200);
        env.ngPost->postMonitoredForTest(env.source(QStringLiteral("early")));
        QVERIFY(!env.ngPost->postQueue());
        QCOMPARE(env.ngPost->pendingAdmissionRetriesForTest(), 1);
        env.ngPost->cancelAllPostingJobs();
        QCOMPARE(env.ngPost->pendingAdmissionRetriesForTest(), 0);
        QVERIFY(!env.ngPost->cancelingAllForTest());
        QVERIFY(!env.ngPost->hasPostingJobs());
        env.ngPost->setQueueEngineUnavailableForTest(false); // repaired
        QTest::qWait(800);                                   // past the retry it would have made
        QVERIFY(!env.ngPost->postQueue());
        QCOMPARE(env.scalar(QStringLiteral("SELECT COUNT(*) FROM posts")).toInt(), 0);
        QCOMPARE(env.mock.receivedArticles().size(), 0);
    }

    void anOwnerReleaseIsRetriedUntilWritten()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        QVERIFY(env.sql(QStringLiteral(
            "CREATE TRIGGER refuse_release BEFORE UPDATE OF owner_id ON posts WHEN NEW.owner_id "
            "IS NULL BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(env.post(
                                                                           QStringLiteral("owned")),
                                                                       Origin::Cli);
        QVERIFY(admission.accepted);
        QTRY_VERIFY_WITH_TIMEOUT(!env.ngPost->postQueue()->find(admission.postId), 20000);
        QTest::qWait(300);
        QVERIFY(!env.scalar(QStringLiteral("SELECT owner_id FROM posts")).isNull());
        QVERIFY(env.ngPost->hasPendingReleasesForTest()); // the end waits for it
        QVERIFY(env.sql(QStringLiteral("DROP TRIGGER refuse_release")));
        QTRY_VERIFY_WITH_TIMEOUT(env.scalar(QStringLiteral("SELECT owner_id FROM posts")).isNull(),
                                 10000);
        QVERIFY(!env.ngPost->hasPendingReleasesForTest());
    }

    void aHeldTransferResumesWithoutResending()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, { QStringLiteral("--slow-mode-ms"), QStringLiteral("20") }),
                 qPrintable(error));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(
            env.post(QStringLiteral("held"), 2 * 1024 * 1024),
            Origin::Cli);
        QVERIFY(admission.accepted);
        const qint64 id = admission.postId;
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 20, 20000);
        queue::PostQueue *queue = env.ngPost->postQueue();
        QVERIFY(queue->setHeld({ id }, true, queue->view()).ok());
        QTRY_VERIFY_WITH_TIMEOUT(queue->find(id) && queue->find(id)->lifecycle == Lifecycle::Queued,
                                 10000);
        QVERIFY(queue->find(id)->held);
        QVERIFY(queue->find(id)->transferManifestEngaged);
        const int beforeResume = env.mock.receivedArticles().size();
        QTest::qWait(300);
        QCOMPARE(env.mock.receivedArticles().size(), beforeResume); // held: nothing goes
        QCOMPARE(
            env.scalar(QStringLiteral("SELECT state FROM post_runs WHERE run_no=1")).toString(),
            QStringLiteral("interrupted"));

        QVERIFY(queue->setHeld({ id }, false, queue->view()).ok());
        QTRY_VERIFY_WITH_TIMEOUT(!queue->find(id), 30000);
        QCOMPARE(env.scalar(QStringLiteral("SELECT status FROM posts")).toString(),
                 QStringLiteral("success"));
        const int total = env.scalar(QStringLiteral("SELECT SUM(total_articles) FROM post_files"))
                              .toInt();
        QVERIFY(total > 20);
        // What was confirmed before the hold is not sent again: at most the
        // article in flight at that moment is.
        QVERIFY2(env.mock.receivedArticles().size() <= total + 1,
                 qPrintable(QStringLiteral("%1 received for %2 articles")
                                .arg(env.mock.receivedArticles().size())
                                .arg(total)));
        QCOMPARE(env.scalar(QStringLiteral("SELECT COUNT(*) FROM post_runs")).toInt(), 2);
    }

    void aHeldPostResumesWithAPasswordTheHistoryDoesNotKeep()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error,
                           { QStringLiteral("--slow-mode-ms"), QStringLiteral("20") },
                           QStringLiteral("HISTORY_STORE_PASSWORDS = false\n")),
                 qPrintable(error));
        PostingJobOptions options = env.post(QStringLiteral("secret"), 1024 * 1024);
        options.declaredPassword = QStringLiteral("only-in-memory-77");
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(options, Origin::Cli);
        QVERIFY(admission.accepted);
        const qint64 id = admission.postId;
        QVERIFY(env.scalar(QStringLiteral("SELECT rar_pass FROM posts")).toString().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 5, 20000);
        queue::PostQueue *queue = env.ngPost->postQueue();
        QVERIFY(queue->setHeld({ id }, true, queue->view()).ok());
        QTRY_VERIFY_WITH_TIMEOUT(queue->find(id) && queue->find(id)->lifecycle == Lifecycle::Queued,
                                 10000);
        QVERIFY(queue->setHeld({ id }, false, queue->view()).ok());
        QTRY_VERIFY_WITH_TIMEOUT(!queue->find(id), 30000);
        QCOMPARE(env.scalar(QStringLiteral("SELECT status FROM posts")).toString(),
                 QStringLiteral("success"));
    }

    void aCloseKeepsTheQueueForARestart()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, { QStringLiteral("--slow-mode-ms"), QStringLiteral("20") }),
                 qPrintable(error));
        const NgPost::QueueAdmission first = env.ngPost->admitPost(env.post(QStringLiteral("first"),
                                                                            2 * 1024 * 1024),
                                                                   Origin::Cli);
        QVERIFY(first.accepted);
        const NgPost::QueueAdmission second = env.ngPost->admitPost(env.post(
                                                                        QStringLiteral("second")),
                                                                    Origin::Cli);
        QVERIFY(second.accepted);
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 5, 20000);
        QSignalSpy closed(env.ngPost.get(), &NgPost::queueClosed);
        env.ngPost->closeQueueKeeping();
        // Nothing is admitted from now on, not even before the close is over.
        QVERIFY(!env.ngPost->admitPost(env.post(QStringLiteral("late")), Origin::Cli).accepted);
        env.ngPost->resume(); // nor started again
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 20000);
        QVERIFY(closed.at(0).at(0).toBool());

        PostQueue *queue = env.ngPost->postQueue();
        QCOMPARE(queue->order(), (QList<qint64>{ first.postId, second.postId }));
        QCOMPARE(queue->find(first.postId)->lifecycle, Lifecycle::Queued);
        QVERIFY(queue->find(first.postId)->transferManifestEngaged);
        QVERIFY(!queue->find(first.postId)->held);
        QCOMPARE(env.scalar(QStringLiteral("SELECT state FROM post_runs WHERE post_id=%1 AND "
                                           "run_no=1")
                                .arg(first.postId))
                     .toString(),
                 QStringLiteral("interrupted"));
        QCOMPARE(queue->find(second.postId)->lifecycle, Lifecycle::Queued);
        const int received = env.mock.receivedArticles().size();
        QTest::qWait(500);
        QCOMPARE(env.mock.receivedArticles().size(), received); // nothing goes on or starts
        QCOMPARE(env.scalar(QStringLiteral("SELECT COUNT(*) FROM posts")).toInt(), 2);
    }

    void aCloseBeforeTheTransferStartsSendsNothing()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(env.post(QStringLiteral(
                                                                           "unsent")),
                                                                       Origin::Cli);
        QVERIFY(admission.accepted);
        env.ngPost->startQueuedWork(); // activated; its start waits for the event loop
        QCOMPARE(env.ngPost->postQueue()->find(admission.postId)->lifecycle, Lifecycle::Posting);
        QSignalSpy closed(env.ngPost.get(), &NgPost::queueClosed);
        env.ngPost->closeQueueKeeping(); // before that turn
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 10000);
        QVERIFY(closed.at(0).at(0).toBool());
        QTest::qWait(500);
        QCOMPARE(env.mock.receivedArticles().size(), 0);
        const QueueEntry *entry = env.ngPost->postQueue()->find(admission.postId);
        QVERIFY(entry);
        QCOMPARE(entry->lifecycle, Lifecycle::Queued);
        QVERIFY(!entry->transferManifestEngaged);
        // The kept runtime never started: no file was registered for a transfer.
        QCOMPARE(env.scalar(QStringLiteral("SELECT COUNT(*) FROM post_files")).toInt(), 0);
        QCOMPARE(
            env.scalar(QStringLiteral("SELECT transfer_manifest_complete FROM post_info")).toInt(),
            0);
    }

    void aCloseThatCannotBeWrittenSaysSo()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, { QStringLiteral("--slow-mode-ms"), QStringLiteral("30") }),
                 qPrintable(error));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(
            env.post(QStringLiteral("kept"), 2 * 1024 * 1024),
            Origin::Cli);
        QVERIFY(admission.accepted);
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 0, 15000);
        QVERIFY(env.sql(
            QStringLiteral("CREATE TRIGGER refuse_stop BEFORE UPDATE OF stage ON post_queue WHEN "
                           "NEW.stage='quiescing' BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        QSignalSpy closed(env.ngPost.get(), &NgPost::queueClosed);
        env.ngPost->closeQueueKeeping();
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QVERIFY(!closed.at(0).at(0).toBool()); // never announced as saved
        QVERIFY(env.ngPost->postQueue()->find(admission.postId));
        QVERIFY(env.ngPost->isPaused()); // held; Resume is the way out
        QVERIFY(!env.ngPost->isClosingQueue());

        QVERIFY(env.sql(QStringLiteral("DROP TRIGGER refuse_stop")));
        env.ngPost->closeQueueKeeping(); // asked again: written this time
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 2, 20000);
        QVERIFY(closed.at(1).at(0).toBool());
        QCOMPARE(env.ngPost->postQueue()->find(admission.postId)->lifecycle, Lifecycle::Queued);
    }

    void aCloseNeverStopsAPostWhoseResultWaits()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        QVERIFY(env.sql(QStringLiteral(
            "CREATE TRIGGER refuse_f1 BEFORE UPDATE OF finalization_state ON post_runs WHEN "
            "NEW.finalization_state='nzb_pending' BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(env.post(
                                                                           QStringLiteral("ended")),
                                                                       Origin::Cli);
        QVERIFY(admission.accepted);
        QTRY_VERIFY_WITH_TIMEOUT(env.ngPost->queueSchedulerForTest()->isTechnicallyPaused(), 15000);
        QSignalSpy closed(env.ngPost.get(), &NgPost::queueClosed);
        env.ngPost->closeQueueKeeping(); // the retry of F1 fails again
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QVERIFY(!closed.at(0).at(0).toBool());
        QVERIFY(env.ngPost->postQueue()->find(admission.postId));
        QCOMPARE(env.scalar(QStringLiteral("SELECT COUNT(*) FROM post_runs")).toInt(), 1);

        QVERIFY(env.sql(QStringLiteral("DROP TRIGGER refuse_f1")));
        env.ngPost->closeQueueKeeping(); // F1 written: the post ends, it is not kept
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 2, 20000);
        QVERIFY(closed.at(1).at(0).toBool());
        QVERIFY(!env.ngPost->postQueue()->find(admission.postId));
        QCOMPARE(env.scalar(QStringLiteral("SELECT status FROM posts")).toString(),
                 QStringLiteral("success"));
        QCOMPARE(env.scalar(QStringLiteral("SELECT COUNT(*) FROM post_runs")).toInt(), 1);
    }

    void aFatalExitKeepsTheTransferForARestart()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, { QStringLiteral("--slow-mode-ms"), QStringLiteral("20") }),
                 qPrintable(error));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(
            env.post(QStringLiteral("cut"), 2 * 1024 * 1024),
            Origin::Cli);
        QVERIFY(admission.accepted);
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 5, 20000);
        QSignalSpy closed(env.ngPost.get(), &NgPost::queueClosed);
        env.ngPost->requestExitForTest(false);
        QVERIFY(env.ngPost->exitPendingForTest());
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 20000);
        QVERIFY(closed.at(0).at(0).toBool());
        const QueueEntry *entry = env.ngPost->postQueue()->find(admission.postId);
        QVERIFY(entry);
        QCOMPARE(entry->lifecycle, Lifecycle::Queued);
        QVERIFY(entry->transferManifestEngaged);
        QVERIFY(env.scalar(QStringLiteral("SELECT status FROM posts")).toString()
                != QLatin1String("success"));
    }

    void aFatalExitWhoseCloseFailsStillStopsTheTransfer()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, { QStringLiteral("--slow-mode-ms"), QStringLiteral("20") }),
                 qPrintable(error));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(
            env.post(QStringLiteral("cut"), 2 * 1024 * 1024),
            Origin::Cli);
        QVERIFY(admission.accepted);
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 5, 20000);
        QVERIFY(env.sql(
            QStringLiteral("CREATE TRIGGER refuse_stop BEFORE UPDATE OF stage ON post_queue WHEN "
                           "NEW.stage='quiescing' BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        QSignalSpy closed(env.ngPost.get(), &NgPost::queueClosed);
        env.ngPost->requestExitForTest(false);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 5000);
        QVERIFY(!closed.at(0).at(0).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(env.ngPost->exitRequestCountForTest(), 1, 5000);
        // What the exit tears down: the transfer stops first, its confirmed
        // articles recorded, and nothing is sent after.
        env.ngPost.reset();
        const int received = env.mock.receivedArticles().size();
        QTest::qWait(500);
        QCOMPARE(env.mock.receivedArticles().size(), received);
        const int posted = env
                               .scalar(QStringLiteral(
                                   "SELECT COUNT(*) FROM post_articles WHERE status='posted'"))
                               .toInt();
        QVERIFY2(posted >= received - 1 && posted <= received,
                 qPrintable(
                     QStringLiteral("%1 recorded for %2 received").arg(posted).arg(received)));
    }

    void aFatalExitLetsTheTransferItWaitsForEnd()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, { QStringLiteral("--slow-mode-ms"), QStringLiteral("20") }),
                 qPrintable(error));
        const NgPost::QueueAdmission first = env.ngPost->admitPost(
            env.post(QStringLiteral("direct"), 512 * 1024),
            Origin::Cli);
        QVERIFY(first.accepted);
        const NgPost::QueueAdmission second = env.ngPost->admitPost(env.post(
                                                                        QStringLiteral("next")),
                                                                    Origin::Cli);
        QVERIFY(second.accepted);
        QTRY_VERIFY_WITH_TIMEOUT(env.mock.receivedArticles().size() > 0, 20000);
        QSignalSpy closed(env.ngPost.get(), &NgPost::queueClosed);
        env.ngPost->requestExitForTest(true);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 20000);
        QVERIFY(closed.at(0).at(0).toBool());
        QVERIFY(!env.ngPost->postQueue()->find(first.postId)); // ended on its own
        QCOMPARE(env.scalar(
                        QStringLiteral("SELECT status FROM posts WHERE id=%1").arg(first.postId))
                     .toString(),
                 QStringLiteral("success"));
        QCOMPARE(env.ngPost->postQueue()->find(second.postId)->lifecycle, Lifecycle::Queued);
    }

    void aPreparationRootIsChosenBeforeItsSet()
    {
        Env env;
        QString error;
        const QString ram = env.sandbox.rootPath() + QStringLiteral("/ram");
        const QString disk = env.sandbox.rootPath() + QStringLiteral("/disk");
        QVERIFY(QDir().mkpath(ram) && QDir().mkpath(disk));
        QVERIFY2(env.start(&error,
                           {},
                           QStringLiteral("TMP_RAM = %1\nTMP_RAM_RATIO = 1.1\n").arg(ram)),
                 qPrintable(error));
        QueueEntry entry;
        entry.options.job = env.post(QStringLiteral("rooted"));
        entry.options.job.tmpPath = disk;
        entry.options.job.doCompress = true;
        // It fits in TMP_RAM: there, canonical, before any set records it (lot C5.8).
        QCOMPARE(env.ngPost->preparationRoot(entry), QFileInfo(ram).canonicalFilePath());
        PersistedSource huge;
        huge.sizeBytes = Q_INT64_C(1) << 60;
        entry.options.sources = { huge };
        QCOMPARE(env.ngPost->preparationRoot(entry), QFileInfo(disk).canonicalFilePath());
        entry.options.sources.clear();
        entry.options.job.doCompress = false; // nothing prepared: TMP_DIR as before
        QCOMPARE(env.ngPost->preparationRoot(entry), QFileInfo(disk).canonicalFilePath());
    }

    void resumeEndsATechnicalPause()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        QVERIFY(env.sql(QStringLiteral(
            "CREATE TRIGGER refuse_stage BEFORE UPDATE OF stage ON post_queue WHEN NEW.stage <> "
            "OLD.stage BEGIN SELECT RAISE(ABORT, 'injected'); END")));
        const NgPost::QueueAdmission admission = env.ngPost->admitPost(env.post(QStringLiteral(
                                                                           "paused")),
                                                                       Origin::Cli);
        QVERIFY(admission.accepted);
        QTRY_VERIFY_WITH_TIMEOUT(env.ngPost->queueSchedulerForTest()->isTechnicallyPaused(), 5000);
        QVERIFY(env.ngPost->isPaused()); // shown as paused: Resume is the way out
        QCOMPARE(env.mock.receivedArticles().size(), 0);

        QVERIFY(env.sql(QStringLiteral("DROP TRIGGER refuse_stage")));
        env.ngPost->resume();
        QTRY_VERIFY_WITH_TIMEOUT(!env.ngPost->postQueue()->find(admission.postId), 20000);
        QVERIFY(!env.ngPost->isPaused());
        QCOMPARE(env.scalar(QStringLiteral("SELECT status FROM posts")).toString(),
                 QStringLiteral("success"));
        QVERIFY(env.mock.receivedArticles().size() > 0);
    }
};

QTEST_MAIN(tst_NgPostQueue)
#include "tst_NgPostQueue.moc"
