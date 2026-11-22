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
#include "TestEnv.h"
#include "history/PostHistoryService.h"
#include "queue/PostQueue.h"
#include "queue/QueueScheduler.h"
#include "utils/PathHelper.h"

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

    bool start(QString *error, const QStringList &mockArgs = {})
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
