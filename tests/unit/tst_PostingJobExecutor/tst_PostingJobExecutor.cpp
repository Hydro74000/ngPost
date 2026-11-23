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
#include "TestMain.h"
#include "history/HistoryQueueStore.h"
#include "history/PostHistoryService.h"
#include "queue/PostQueue.h"
#include "queue/QueueScheduler.h"
#include "tools/ExternalToolProcess.h"
#include "utils/PathHelper.h"

#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QThread>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

// Where a supervisor proves the stop of a tool and all its descendants (lot
// C5.6); macOS runs tools without ever proving anything.
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
#define PROVEN_SUPERVISION 1
#endif

using namespace queue;
using ngpost::tests::HomeSandbox;
using ngpost::tests::MockNntpServer;

namespace
{

// --- the fake tools ---------------------------------------------------------------------
//
// The test binary stands for rar and par2 (lot C5.6), on every platform: run
// with NGPOST_FAKE_TOOLS set, it does what NGPOST_FAKE_RAR (or NGPOST_FAKE_PAR2,
// when given a .par2 file) says, as ';'-separated actions run in order:
//   touch=<path>   pid=<path>   late=<path> (a descendant creates it 1 s later)
//   ignoreterm (POSIX)   sleep=<ms>   write=<bytes> (into its archive or .par2)
//   exit=<code>

constexpr const char *kLateSwitch = "--fake-late";

void createFile(const QString &path)
{
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.close();
}

//! A descendant of the fake tool, outliving it unless its tree is stopped.
void spawnLateWriter(const QString &path)
{
#ifdef Q_OS_WIN
    wchar_t self[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" "
        + QString::fromLatin1(kLateSwitch).toStdWString() + L" \"" + path.toStdWString() + L"\"";
    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION info = {};
    if (::CreateProcessW(nullptr,
                         command.data(),
                         nullptr,
                         nullptr,
                         FALSE,
                         CREATE_NO_WINDOW,
                         nullptr,
                         nullptr,
                         &startup,
                         &info)) {
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
    }
#else
    const QByteArray target = QFile::encodeName(path);
    if (::fork() == 0) {
        ::sleep(1);
        const int fd = ::open(target.constData(), O_CREAT | O_WRONLY, 0644);
        if (fd >= 0)
            ::close(fd);
        ::_exit(0);
    }
#endif
}

int fakeTool(int argc, char *argv[])
{
    QString rarTarget;
    QString par2Target;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (par2Target.isEmpty() && arg.endsWith(QLatin1String(".par2")))
            par2Target = arg;
        else if (rarTarget.isEmpty()
                 && (arg.endsWith(QLatin1String(".rar")) || arg.endsWith(QLatin1String(".7z"))))
            rarTarget = arg;
    }
    const bool par2 = !par2Target.isEmpty();
    const QString script = par2
        ? qEnvironmentVariable("NGPOST_FAKE_PAR2", QStringLiteral("write=4000"))
        : qEnvironmentVariable("NGPOST_FAKE_RAR", QStringLiteral("write=40000"));
    for (const QString &action : script.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QString name = action.section(QLatin1Char('='), 0, 0);
        const QString value = action.section(QLatin1Char('='), 1);
        if (name == QLatin1String("touch")) {
            createFile(value);
        } else if (name == QLatin1String("pid")) {
            QFile f(value);
            if (f.open(QIODevice::WriteOnly))
                f.write(QByteArray::number(QCoreApplication::applicationPid()));
        } else if (name == QLatin1String("late")) {
            spawnLateWriter(value);
        } else if (name == QLatin1String("ignoreterm")) {
#ifndef Q_OS_WIN
            ::signal(SIGTERM, SIG_IGN);
#endif
        } else if (name == QLatin1String("sleep")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(value.toInt()));
        } else if (name == QLatin1String("write")) {
            QFile f(par2 ? par2Target : rarTarget);
            if (!f.open(QIODevice::WriteOnly))
                return 2;
            f.write(QByteArray(value.toInt(), '\0'));
        } else if (name == QLatin1String("exit")) {
            return value.toInt();
        }
    }
    return 0;
}

//! Before the test application: the fake tools and their descendants.
int fakeToolsMain(int argc, char *argv[])
{
    if (argc == 3 && std::strcmp(argv[1], kLateSwitch) == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        createFile(QString::fromLocal8Bit(argv[2]));
        return 0;
    }
    if (qEnvironmentVariableIsSet("NGPOST_FAKE_TOOLS"))
        return fakeTool(argc, argv);
    return -1;
}

void killPid(qint64 pid)
{
    if (pid <= 0)
        return;
#ifdef Q_OS_WIN
    if (HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid))) {
        ::TerminateProcess(process, 1);
        ::CloseHandle(process);
    }
#else
    ::kill(pid_t(pid), SIGKILL);
#endif
}

//! Ends the process whose pid a fake tool wrote in @p pidFile.
void killWrittenPid(const QString &pidFile)
{
    QFile f(pidFile);
    if (f.open(QIODevice::ReadOnly))
        killPid(f.readAll().trimmed().toLongLong());
}

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

//! A real SQLite refusal of the tool supervision steps: every one, or only
//! those recording @p state.
bool refuseToolStates(const QString &dbPath, const QString &state = {})
{
    const QString connection = QUuid::createUuid().toString();
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(dbPath);
        QSqlQuery q(db);
        const QString when = state.isEmpty()
            ? QString()
            : QStringLiteral(" WHEN NEW.tool_state='%1'").arg(state);
        ok = db.open()
            && q.exec(QStringLiteral("CREATE TRIGGER refuse_tool_states BEFORE UPDATE OF "
                                     "tool_state ON post_artifact_sets%1 BEGIN SELECT "
                                     "RAISE(ABORT, 'injected'); END")
                          .arg(when));
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
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
    //! The tool posts run as rar and par2: this binary, as fake tools.
    QString rarProgram = QCoreApplication::applicationFilePath();
    QString par2Program = QCoreApplication::applicationFilePath();

    ~Env()
    {
        executor.reset(); // before the fake tools' settings go
        qunsetenv("NGPOST_FAKE_TOOLS");
        qunsetenv("NGPOST_FAKE_RAR");
        qunsetenv("NGPOST_FAKE_PAR2");
    }

    //! @p rarScript and @p par2Script: what the fake tools do, see fakeTool().
    bool start(QString *error,
               const QString &rarScript = {},
               const QStringList &mockArgs = {},
               const QString &par2Script = {})
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
        // Preparation facts are written for a session only, as in the queue.
        ngPost->historyService()->setOwnerId(QStringLiteral("owner-test"));
        // Stand-ins for rar and par2: CI has neither. Read by the tools at
        // their start, inherited through their supervisor.
        qputenv("NGPOST_FAKE_TOOLS", "1");
        qputenv("NGPOST_FAKE_RAR",
                rarScript.isEmpty() ? QByteArray("write=40000") : rarScript.toLocal8Bit());
        qputenv("NGPOST_FAKE_PAR2",
                par2Script.isEmpty() ? QByteArray("write=4000") : par2Script.toLocal8Bit());
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
                               int bytes = 64000,
                               bool obfuscate = false)
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
        o.job.obfuscateFileName = obfuscate;
        o.job.tmpPath = root + QStringLiteral("/tmp");
        o.job.rarName = name + QStringLiteral("-arch");
        o.job.rarTool = QStringLiteral("rar");
        o.job.rarPath = rarProgram;
        o.job.par2Tool = par2::Tool::Par2cmdline;
        o.job.par2Path = par2Program;
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

    //! The artifact set the scheduler opens when a preparation begins.
    QString beginSet(qint64 postId)
    {
        const QueueEntry *entry = queue->find(postId);
        ArtifactSetRef set;
        set.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        set.generation = entry->lastGeneration + 1;
        set.recipeRevision = entry->recipeRevision;
        set.root = root + QStringLiteral("/tmp");
        return queue->engineBeginPreparation(postId, set) ? set.id : QString();
    }

    //! What the history holds of @p setId's tools.
    PostHistoryStore::ArtifactSetRecord set(const QString &setId) const
    {
        return ngPost->historyService()->artifactSet(setId).value_or(
            PostHistoryStore::ArtifactSetRecord());
    }

    //! Waits for @p setId's tool to be recorded as @p state.
    bool waitToolState(const QString &setId, const QString &state, int ms = 15000) const
    {
        return QTest::qWaitFor([&] { return set(setId).toolState == state; }, ms);
    }

    //! A statement on a connection of its own (a trigger).
    bool exec(const QString &sql) const
    {
        const QString name = QUuid::createUuid().toString();
        bool done = false;
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(dbPath());
            QSqlQuery q(db);
            done = db.open() && q.exec(sql);
        }
        QSqlDatabase::removeDatabase(name);
        return done;
    }
    //! The history refuses to record any source as restored, or accepts again.
    bool refuseReturns(bool refuse) const
    {
        return exec(refuse ? QStringLiteral("CREATE TRIGGER refuse_returns BEFORE UPDATE OF state "
                                            "ON post_source_moves WHEN NEW.state='restored' BEGIN "
                                            "SELECT RAISE(ABORT, 'injected'); END")
                           : QStringLiteral("DROP TRIGGER refuse_returns"));
    }

    //! The states of the journaled moves of @p postId, in order (lot C5.7).
    QStringList moveStates(qint64 postId) const
    {
        QStringList states;
        const QString name = QUuid::createUuid().toString();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(dbPath());
            QSqlQuery q(db);
            if (db.open()
                && q.exec(QStringLiteral("SELECT state FROM post_source_moves WHERE post_id=%1 "
                                         "ORDER BY sequence")
                              .arg(postId)))
                while (q.next())
                    states << q.value(0).toString();
        }
        QSqlDatabase::removeDatabase(name);
        return states;
    }

    //! The pid of the supervisor recorded for @p setId.
    qint64 supervisorPid(const QString &setId) const
    {
        const QJsonObject identity = QJsonDocument::fromJson(set(setId).toolIdentityJson.toUtf8())
                                         .object();
        return identity.value(QStringLiteral("pid")).toString().toLongLong();
    }

    QString source(const QString &name) const { return root + QStringLiteral("/%1.bin").arg(name); }
    QString archiveDir(const QString &name) const
    {
        return root + QStringLiteral("/tmp/%1-arch").arg(name);
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
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("prepared"), true, true));
        QVERIFY(id > 0);
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QVERIFY2(env.prepare(id, run, set, &error), qPrintable(error));

        const PreparedManifest manifest = env.executor->manifest(id, run, set);
        QVERIFY(manifest.isValid());
        QVERIFY(manifest.compressed);
        QCOMPARE(manifest.files.size(), 2); // the archive and its parity, in order
        QVERIFY(manifest.files.first().endsWith(QStringLiteral(".par2")));
        QVERIFY(manifest.files.last().endsWith(QStringLiteral(".rar")));
        QCOMPARE(manifest.bytes, qint64(44000));
#ifdef PROVEN_SUPERVISION
        QVERIFY(!env.executor->runtime(id)); // no job while the entry waits
#else
        QVERIFY(env.executor->runtime(id)); // only "exited": it keeps its guard
#endif
        QTest::qWait(50);                     // a released job is gone...
        QVERIFY(QDir(manifest.dir).exists()); // ...and its files stay
        QCOMPARE(env.mock.receivedArticles().size(), 0);
    }

    void activationPostsExactlyThePreparedFiles()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("activated"), true, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QVERIFY2(env.prepare(id, run, set, &error), qPrintable(error));

        QSignalSpy started(env.executor.get(), &QueueExecutor::transferStarted);
        QSignalSpy finished(env.executor.get(), &QueueExecutor::transferFinished);
        QSignalSpy finalized(env.executor.get(), &QueueExecutor::finalized);
        env.executor->activate(id, run, set);
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

    // --- supervised tools (lot C5.6) ----------------------------------------------------

    void aRefusedSupervisionRunsNoTool()
    {
        Env env;
        QString error;
        const QString ran = env.sandbox.rootPath() + QStringLiteral("/rar-ran");
        QVERIFY2(env.start(&error, QStringLiteral("touch=%1;write=40000").arg(ran)),
                 qPrintable(error));
        QVERIFY(refuseToolStates(env.dbPath()));
        const qint64 id = env.submit(env.post(QStringLiteral("refused"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        env.executor->prepareStep(id, run, set, PrepStep::Compress);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        QVERIFY(!steps.first().at(4).toBool());
        const QString stepErr = steps.first().at(5).toString();
        QVERIFY2(stepErrorIs(stepErr, kStepStoreFailed), qPrintable(stepErr));
        QTest::qWait(300);
        QVERIFY(!QFileInfo::exists(ran));                                  // never told to run
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("refused")))); // its name is back
        QVERIFY(!env.executor->runtime(id));
        QCOMPARE(env.set(set).toolState, QStringLiteral("idle"));
    }

    void toolStatesAreRecordedAndTheirProofsStayOutOfThePost()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("recorded"), true, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QVERIFY2(env.prepare(id, run, set, &error), qPrintable(error));
        const PostHistoryStore::ArtifactSetRecord record = env.set(set);
        QCOMPARE(record.supervision, ExternalToolProcess::capability());
        QVERIFY(!record.toolIdentityJson.isEmpty());
#ifdef PROVEN_SUPERVISION
        QCOMPARE(record.toolState, QStringLiteral("stopped_verified"));
        // Once the history holds a proven stop, its proof file is not kept.
        QVERIFY(QDir(PathHelper::configDir() + QStringLiteral("/tool-proofs"))
                    .entryList(QDir::Files)
                    .isEmpty());
#else
        QCOMPARE(record.toolState, QStringLiteral("exited")); // nothing proven, never
#endif
        const PreparedManifest manifest = env.executor->manifest(id, run, set);
        QCOMPARE(manifest.files.size(), 2);
        for (const QString &file : manifest.files)
            QVERIFY2(!file.endsWith(QStringLiteral(".proof")), qPrintable(file));
    }

    void aToolThatCannotStartFailsItsStep()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
#ifdef Q_OS_WIN
        env.rarProgram = env.root + QStringLiteral("/notanexe.exe");
        QVERIFY(writeFakeTool(env.rarProgram, QStringLiteral("not an executable\n")));
#else
        env.rarProgram = env.root + QStringLiteral("/badinterpreter");
        QVERIFY(writeFakeTool(env.rarProgram, QStringLiteral("#!/nonexistent/interpreter\n")));
#endif
        const qint64 id = env.submit(
            env.post(QStringLiteral("unstarted"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        env.executor->prepareStep(id, run, env.beginSet(id), PrepStep::Compress);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        QVERIFY(!steps.first().at(4).toBool());
        const QString stepErr = steps.first().at(5).toString();
        QVERIFY2(stepErr.contains(QStringLiteral("Could not start external tool")),
                 qPrintable(stepErr));
        // An exec that failed proves that nothing ran, wherever nothing else can.
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("unstarted"))));
        QVERIFY(!QDir(env.archiveDir(QStringLiteral("unstarted"))).exists());
        QTRY_VERIFY(!env.executor->runtime(id));
    }

    void aFailingToolFailsItsStep()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, QStringLiteral("exit=3")), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("failing"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        env.executor->prepareStep(id, run, env.beginSet(id), PrepStep::Compress);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        QVERIFY(!steps.first().at(4).toBool());
        const QString stepErr = steps.first().at(5).toString();
        QVERIFY2(stepErr.contains(QStringLiteral("compression: 3")), qPrintable(stepErr));
        QVERIFY(!stepErrorIs(stepErr, kStepToolUnproven)); // a failure, not a stuck tool
#ifdef PROVEN_SUPERVISION
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("failing")))); // name back
        QVERIFY(!QDir(env.archiveDir(QStringLiteral("failing"))).exists());
        QTRY_VERIFY(!env.executor->runtime(id));
#else
        // Ended on its own (Exited), but nothing proves its descendants gone:
        // nothing renamed back or deleted, and the runtime keeps the map back.
        QVERIFY(!QFileInfo::exists(env.source(QStringLiteral("failing"))));
        QVERIFY(QDir(env.archiveDir(QStringLiteral("failing"))).exists());
        QVERIFY(env.executor->runtime(id));
#endif
    }

    void anEmptyPar2FailsItsStep()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error, {}, {}, QStringLiteral("exit=0")), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("noparity"), false, true));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        env.executor->prepareStep(id, run, env.beginSet(id), PrepStep::Par2);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        QVERIFY(!steps.first().at(4).toBool());
        const QString stepErr = steps.first().at(5).toString();
        QVERIFY2(stepErr.contains(QStringLiteral("wrote no par2 file")), qPrintable(stepErr));
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("noparity"))));
#ifdef PROVEN_SUPERVISION
        QVERIFY(!QDir(env.archiveDir(QStringLiteral("noparity"))).exists());
#else
        QVERIFY(QDir(env.archiveDir(QStringLiteral("noparity"))).exists());
#endif
    }

    void stoppingBeforeTheToolRunsProvesItNeverRan()
    {
        Env env;
        QString error;
        const QString ran = env.sandbox.rootPath() + QStringLiteral("/rar-ran");
        QVERIFY2(env.start(&error, QStringLiteral("touch=%1;write=40000").arg(ran)),
                 qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("early"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        QSignalSpy quiesced(env.executor.get(), &QueueExecutor::quiesced);
        env.executor->prepareStep(id, run, set, PrepStep::Compress);
        // Before its supervisor could even say it is ready: never told to run.
        env.executor->interrupt(id, run, Disposition::Keep);
        QTRY_COMPARE_WITH_TIMEOUT(quiesced.size(), 1, 10000);
        QVERIFY(quiesced.first().at(2).toBool()); // proven everywhere: nothing ran
        QVERIFY(!env.executor->runtime(id));
        QTest::qWait(300);
        QCOMPARE(steps.size(), 0);
        QVERIFY(!QFileInfo::exists(ran));
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("early"))));
        QCOMPARE(env.set(set).toolState, QStringLiteral("idle"));
    }

    void stoppingARunningToolStopsItsDescendants()
    {
        Env env;
        QString error;
        const QString late = env.sandbox.rootPath() + QStringLiteral("/late-write");
        QVERIFY2(env.start(&error, QStringLiteral("late=%1;sleep=30000").arg(late)),
                 qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("stopped"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        QSignalSpy quiesced(env.executor.get(), &QueueExecutor::quiesced);
        env.executor->prepareStep(id, run, set, PrepStep::Compress);
        QVERIFY(env.waitToolState(set, QStringLiteral("running")));
        env.executor->interrupt(id, run, Disposition::Keep);
        env.executor->interrupt(id, run, Disposition::Keep); // the same stop, under way
        QTRY_COMPARE_WITH_TIMEOUT(quiesced.size(), 1, 10000);
#ifdef PROVEN_SUPERVISION
        QVERIFY(quiesced.first().at(2).toBool());
        QVERIFY(!env.executor->runtime(id));
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("stopped")))); // name back
        QCOMPARE(env.set(set).toolState, QStringLiteral("stopped_verified"));
#else
        // Stopped, but never proven: everything stays where it is.
        QVERIFY(!quiesced.first().at(2).toBool());
        QVERIFY(env.executor->runtime(id));
        QVERIFY(!QFileInfo::exists(env.source(QStringLiteral("stopped"))));
        QCOMPARE(env.set(set).toolState, QStringLiteral("unproven"));
#endif
        QTest::qWait(1500);
        QCOMPARE(quiesced.size(), 1);      // one answer for the stop asked twice
        QCOMPARE(steps.size(), 0);         // the killed tool's end is not a step end
        QVERIFY(!QFileInfo::exists(late)); // its descendant went with it
        QCOMPARE(env.historyStatus(id), QStringLiteral("queued")); // nothing was finalized
    }

    void aStopTheHistoryCannotHoldKeepsEverything()
    {
#ifndef PROVEN_SUPERVISION
        QSKIP("nothing is ever proven here: there is no proven stop to record");
#else
        Env env;
        QString error;
        QVERIFY2(env.start(&error, QStringLiteral("sleep=30000")), qPrintable(error));
        QVERIFY(refuseToolStates(env.dbPath(), QStringLiteral("stopped_verified")));
        const qint64 id = env.submit(env.post(QStringLiteral("unheld"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QSignalSpy quiesced(env.executor.get(), &QueueExecutor::quiesced);
        env.executor->prepareStep(id, run, set, PrepStep::Compress);
        QVERIFY(env.waitToolState(set, QStringLiteral("running")));
        env.executor->interrupt(id, run, Disposition::Keep);
        QTRY_COMPARE_WITH_TIMEOUT(quiesced.size(), 1, 10000);
        // Proven, but not recorded: nothing goes on from a proof only in memory.
        QVERIFY(!quiesced.first().at(2).toBool());
        QVERIFY(env.executor->runtime(id));
        QVERIFY(!QFileInfo::exists(env.source(QStringLiteral("unheld"))));
        QCOMPARE(env.set(set).toolState, QStringLiteral("quiescing"));
        // Its proof stays for the recovery.
        QVERIFY(!QDir(PathHelper::configDir() + QStringLiteral("/tool-proofs"))
                     .entryList(QDir::Files)
                     .isEmpty());
#endif
    }

    void anEndTheHistoryCannotHoldRenewsNothing()
    {
#ifndef PROVEN_SUPERVISION
        QSKIP("nothing is ever proven here: there is no proven stop to record");
#else
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        QVERIFY(refuseToolStates(env.dbPath(), QStringLiteral("stopped_verified")));
        QueueScheduler::Budget budget;
        budget.rootFor = [&env](const QueueEntry &) { return env.root + QStringLiteral("/tmp"); };
        QueueScheduler scheduler(env.queue.get(), env.executor.get(), budget);
        const qint64 id = env.submit(env.post(QStringLiteral("unended"), true, false, 64000, true));
        QTRY_VERIFY_WITH_TIMEOUT(scheduler.isTechnicallyPaused(), 15000);
        QCOMPARE(scheduler.technicalPauseReason(), QLatin1String(kStepStoreFailed));
        // Its sources are still renamed: the runtime keeps them, under its run.
        const QueueEntry *entry = env.queue->find(id);
        QVERIFY(entry);
        QCOMPARE(entry->lifecycle, Lifecycle::Preparing);
        QCOMPARE(entry->blocked.code, QLatin1String(kStepStoreFailed));
        QVERIFY(env.executor->runtime(id));
        QCOMPARE(env.executor->runtime(id)->executionContext().runId, entry->runId);
        QVERIFY(!QFileInfo::exists(env.source(QStringLiteral("unended"))));
#endif
    }

    void aPreparedPostIsReleasedOnlyOnceItsToolsAreProven()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("held"), true, true, 64000, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
#ifdef PROVEN_SUPERVISION
        QVERIFY2(env.prepare(id, run, set, &error), qPrintable(error));
#else
        // Its sources never come back: the collection refuses to go on.
        QVERIFY(!env.prepare(id, run, set, &error));
        QVERIFY2(error.contains(QLatin1String(kStepSourcesPending)), qPrintable(error));
#endif
        QSignalSpy quiesced(env.executor.get(), &QueueExecutor::quiesced);
#ifdef PROVEN_SUPERVISION
        QTRY_VERIFY(!env.executor->runtime(id));
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("held")))); // name back
        env.executor->interrupt(id, run, Disposition::Keep);
        QTRY_COMPARE(quiesced.size(), 1);
        QVERIFY(quiesced.first().at(2).toBool());
#else
        // Only "exited": its sources keep their names, the runtime its guard,
        // and a stop of the prepared post is never taken as proven.
        QTest::qWait(200);
        QVERIFY(env.executor->runtime(id));
        QVERIFY(!QFileInfo::exists(env.source(QStringLiteral("held"))));
        env.executor->interrupt(id, run, Disposition::Keep);
        QTRY_COMPARE(quiesced.size(), 1);
        QVERIFY(!quiesced.first().at(2).toBool());
        QVERIFY(env.executor->runtime(id));
#endif
    }

    void aLostSupervisorLeavesEverythingInPlace()
    {
        Env env;
        QString error;
        const QString pidFile = env.sandbox.rootPath() + QStringLiteral("/rar-pid");
        QVERIFY2(env.start(&error, QStringLiteral("pid=%1;sleep=30000").arg(pidFile)),
                 qPrintable(error));
        QueueScheduler::Budget budget;
        budget.rootFor = [&env](const QueueEntry &) { return env.root + QStringLiteral("/tmp"); };
        QueueScheduler scheduler(env.queue.get(), env.executor.get(), budget);
        const qint64 id = env.submit(env.post(QStringLiteral("lost"), true, false, 64000, true));
        QTRY_VERIFY_WITH_TIMEOUT(!env.queue->find(id)->artifacts.isNull(), 10000);
        const QString set = env.queue->find(id)->artifacts.id;
        QVERIFY(env.waitToolState(set, QStringLiteral("running")));
        const qint64 supervisor = env.supervisorPid(set);
        QVERIFY(supervisor > 0);
        killPid(supervisor); // its proof can never come now
        QTRY_VERIFY_WITH_TIMEOUT(scheduler.isTechnicallyPaused(), 10000);
        QCOMPARE(scheduler.technicalPauseReason(), QLatin1String(kStepToolUnproven));
        const QueueEntry *entry = env.queue->find(id);
        QVERIFY(entry);
        QCOMPARE(entry->blocked.code, QLatin1String(kStepToolUnproven));
        QCOMPARE(entry->lifecycle, Lifecycle::Preparing);
        QCOMPARE(entry->slot, Slot::Preparation);
        QVERIFY(env.executor->runtime(id));                              // and its owner with it
        QVERIFY(!QFileInfo::exists(env.source(QStringLiteral("lost")))); // still renamed
        QVERIFY(QDir(env.archiveDir(QStringLiteral("lost"))).exists());
        QCOMPARE(env.set(set).toolState, QStringLiteral("unproven"));
        killWrittenPid(pidFile);
    }

    void aStopThatCannotBeProvenKeepsEverything()
    {
#ifdef Q_OS_WIN
        QSKIP("a job object ends its tree at once: no stop can be lost on its way");
#else
        Env env;
        QString error;
        const QString pidFile = env.sandbox.rootPath() + QStringLiteral("/rar-pid");
        // TERM ignored: its supervisor waits before KILL, and is lost meanwhile.
        QVERIFY2(env.start(&error, QStringLiteral("ignoreterm;pid=%1;sleep=30000").arg(pidFile)),
                 qPrintable(error));
        const qint64 id = env.submit(
            env.post(QStringLiteral("unproven"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QSignalSpy quiesced(env.executor.get(), &QueueExecutor::quiesced);
        env.executor->prepareStep(id, run, set, PrepStep::Compress);
        QVERIFY(env.waitToolState(set, QStringLiteral("running")));
        const qint64 supervisor = env.supervisorPid(set);
        QVERIFY(supervisor > 0);
        env.executor->interrupt(id, run, Disposition::Keep);
        killPid(supervisor);
        QTRY_COMPARE_WITH_TIMEOUT(quiesced.size(), 1, 10000);
        QVERIFY(!quiesced.first().at(2).toBool());
        QVERIFY(env.executor->runtime(id));
        QVERIFY(!QFileInfo::exists(env.source(QStringLiteral("unproven"))));
        QVERIFY(QDir(env.archiveDir(QStringLiteral("unproven"))).exists());
        QCOMPARE(env.set(set).toolState, QStringLiteral("unproven"));
        // Asked again, it answers again: still not proven.
        env.executor->interrupt(id, run, Disposition::Keep);
        QTRY_COMPARE_WITH_TIMEOUT(quiesced.size(), 2, 5000);
        QVERIFY(!quiesced.at(1).at(2).toBool());
        killWrittenPid(pidFile);
#endif
    }

    void destroyingAPreparingRuntimeKeepsItsFiles()
    {
        Env env;
        QString error;
        const QString late = env.sandbox.rootPath() + QStringLiteral("/late-write");
        QVERIFY2(env.start(&error, QStringLiteral("late=%1;sleep=30000").arg(late)),
                 qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("dropped"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        env.executor->prepareStep(id, run, set, PrepStep::Compress);
        QVERIFY(env.waitToolState(set, QStringLiteral("running")));
        env.executor.reset(); // its job goes while the tool runs
        // Nothing proves it stopped yet: names and files stay.
        QVERIFY(!QFileInfo::exists(env.source(QStringLiteral("dropped"))));
        QVERIFY(QDir(env.archiveDir(QStringLiteral("dropped"))).exists());
#ifdef PROVEN_SUPERVISION
        // The end of its lifeline: the supervisor stops the tree, then proves it.
        const QDir proofs(PathHelper::configDir() + QStringLiteral("/tool-proofs"));
        QTRY_VERIFY_WITH_TIMEOUT(!proofs.entryList(QDir::Files).isEmpty(), 10000);
#endif
        QTest::qWait(1500);
        QVERIFY(!QFileInfo::exists(late));
    }

    // --- journaled obfuscation (lot C5.7) -------------------------------------------------

    void obfuscatedSourcesAreJournaled()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        const qint64 id = env.submit(
            env.post(QStringLiteral("journaled"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        env.executor->prepareStep(id, run, env.beginSet(id), PrepStep::Compress);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        QVERIFY(steps.first().at(4).toBool());
#ifdef PROVEN_SUPERVISION
        QCOMPARE(env.moveStates(id), QStringList{ QStringLiteral("restored") });
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("journaled"))));
#else
        // Only "exited": still aside, and the journal says where.
        QCOMPARE(env.moveStates(id), QStringList{ QStringLiteral("moved") });
        QVERIFY(!QFileInfo::exists(env.source(QStringLiteral("journaled"))));
#endif
    }

    void aRefusedJournalMovesNoSource()
    {
        Env env;
        QString error;
        const QString ran = env.sandbox.rootPath() + QStringLiteral("/rar-ran");
        QVERIFY2(env.start(&error, QStringLiteral("touch=%1;write=40000").arg(ran)),
                 qPrintable(error));
        const QString connection = QUuid::createUuid().toString();
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(env.dbPath());
            QSqlQuery q(db);
            QVERIFY(db.open()
                    && q.exec(QStringLiteral("CREATE TRIGGER refuse_moves BEFORE INSERT ON "
                                             "post_source_moves BEGIN SELECT RAISE(ABORT, "
                                             "'injected'); END")));
        }
        QSqlDatabase::removeDatabase(connection);
        const qint64 id = env.submit(
            env.post(QStringLiteral("unjournaled"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        env.executor->prepareStep(id, run, env.beginSet(id), PrepStep::Compress);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        QVERIFY(!steps.first().at(4).toBool());
        const QString stepErr = steps.first().at(5).toString();
        QVERIFY2(stepErrorIs(stepErr, kStepStoreFailed), qPrintable(stepErr));
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("unjournaled")))); // never moved
        QTest::qWait(300);
        QVERIFY(!QFileInfo::exists(ran)); // nor any tool started
        QTRY_VERIFY(!env.executor->runtime(id));
    }

    void withoutAnAtomicRenameNoSourceIsPostedUnderItsName()
    {
        Env env;
        QString error;
        const QString ran = env.sandbox.rootPath() + QStringLiteral("/rar-ran");
        QVERIFY2(env.start(&error, QStringLiteral("touch=%1;write=40000").arg(ran)),
                 qPrintable(error));
        const qint64 id = env.submit(
            env.post(QStringLiteral("unrenamed"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        qputenv("NGPOST_TEST_NO_ATOMIC_RENAME", "1");
        env.executor->prepareStep(id, run, env.beginSet(id), PrepStep::Compress);
        qunsetenv("NGPOST_TEST_NO_ATOMIC_RENAME");
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        QVERIFY(!steps.first().at(4).toBool());
        QVERIFY2(steps.first().at(5).toString().contains(QStringLiteral("Couldn't rename file")),
                 qPrintable(steps.first().at(5).toString()));
        // The staging folder, then in place: each attempt with its own intent.
        QCOMPARE(env.moveStates(id),
                 (QStringList{ QStringLiteral("restored"), QStringLiteral("restored") }));
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("unrenamed"))));
        QTest::qWait(300);
        QVERIFY(!QFileInfo::exists(ran));
    }

    void aRefusedReturnRecordFailsTheStep()
    {
#ifndef PROVEN_SUPERVISION
        QSKIP("nothing is ever proven here: sources are never put back");
#else
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        QVERIFY(env.refuseReturns(true));
        const qint64 id = env.submit(env.post(QStringLiteral("unheld"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        env.executor->prepareStep(id, run, env.beginSet(id), PrepStep::Compress);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        // Its name is back, the history does not hold it: the step does not go on.
        QVERIFY(!steps.first().at(4).toBool());
        const QString stepErr = steps.first().at(5).toString();
        QVERIFY2(stepErrorIs(stepErr, kStepStoreFailed), qPrintable(stepErr));
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("unheld"))));
        QCOMPARE(env.moveStates(id), QStringList{ QStringLiteral("moved") });
        QVERIFY(env.executor->runtime(id)); // the work stays held
        // Once the history accepts again, the next stop settles it.
        QVERIFY(env.refuseReturns(false));
        QSignalSpy quiesced(env.executor.get(), &QueueExecutor::quiesced);
        env.executor->interrupt(id, run, Disposition::Keep);
        QTRY_COMPARE_WITH_TIMEOUT(quiesced.size(), 1, 10000);
        QVERIFY(quiesced.first().at(2).toBool());
        QCOMPARE(env.moveStates(id), QStringList{ QStringLiteral("restored") });
        QVERIFY(!QFileInfo::exists(env.root + QStringLiteral("/tmp/.ngPost_src_unheld-arch")));
#endif
    }

    void anUnrecordedReturnComesBeforeAFailedTool()
    {
#ifndef PROVEN_SUPERVISION
        QSKIP("nothing is ever proven here: sources are never put back");
#else
        Env env;
        QString error;
        QVERIFY2(env.start(&error, QStringLiteral("write=40000;exit=3")), qPrintable(error));
        QVERIFY(env.refuseReturns(true));
        const qint64 id = env.submit(env.post(QStringLiteral("twice"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        env.executor->prepareStep(id, run, env.beginSet(id), PrepStep::Compress);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        const QString stepErr = steps.first().at(5).toString();
        QVERIFY2(stepErrorIs(stepErr, kStepStoreFailed), qPrintable(stepErr));
        QVERIFY(QDir(env.archiveDir(QStringLiteral("twice"))).exists()); // nothing deleted
        QVERIFY(env.executor->runtime(id));
#endif
    }

    void anUnrecordedRollbackComesBeforeAFailedObfuscation()
    {
#ifdef Q_OS_WIN
        QSKIP("a source that cannot be moved aside is a symbolic link here");
#else
        Env env;
        QString error;
        const QString ran = env.sandbox.rootPath() + QStringLiteral("/rar-ran");
        QVERIFY2(env.start(&error, QStringLiteral("touch=%1;write=40000").arg(ran)),
                 qPrintable(error));
        PersistedQueueOptions options = env.post(QStringLiteral("rolled"),
                                                 true,
                                                 false,
                                                 64000,
                                                 true);
        // A second source that is a link: never moved aside, so the post stops
        // and the first one comes back, whose return the history refuses.
        const QString link = env.root + QStringLiteral("/rolled-link.bin");
        QVERIFY(QFile::link(env.source(QStringLiteral("rolled")), link));
        options.job.files << QFileInfo(link);
        options.job.inputPaths << link;
        QVERIFY(env.refuseReturns(true));
        const qint64 id = env.submit(options);
        const QString run = env.queue->find(id)->runId;
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        env.executor->prepareStep(id, run, env.beginSet(id), PrepStep::Compress);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        const QString stepErr = steps.first().at(5).toString();
        QVERIFY2(stepErrorIs(stepErr, kStepStoreFailed), qPrintable(stepErr));
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("rolled")))); // back, unrecorded
        QCOMPARE(env.moveStates(id).value(0), QStringLiteral("moved"));
        QVERIFY(env.executor->runtime(id));
        QVERIFY(!QFileInfo::exists(ran));
#endif
    }

    void aBlockedReturnIsLookedAtAgain()
    {
#ifndef PROVEN_SUPERVISION
        QSKIP("nothing is ever proven here: sources are never put back");
#else
        Env env;
        QString error;
        QVERIFY2(env.start(&error, QStringLiteral("sleep=1500;write=40000")), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("taken"), true, false, 64000, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        QSignalSpy quiesced(env.executor.get(), &QueueExecutor::quiesced);
        env.executor->prepareStep(id, run, set, PrepStep::Compress);
        QVERIFY(env.waitToolState(set, QStringLiteral("running")));
        // While it runs, another file takes the real name of its source.
        QFile intruder(env.source(QStringLiteral("taken")));
        QVERIFY(intruder.open(QIODevice::WriteOnly));
        intruder.write("not ours");
        intruder.close();
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        QVERIFY(steps.first().at(4).toBool()); // the archive is fine
        QCOMPARE(env.moveStates(id), QStringList{ QStringLiteral("blocked") });
        QVERIFY(env.executor->runtime(id)); // still holding the way back
        QVERIFY(QFile::remove(env.source(QStringLiteral("taken"))));
        env.executor->interrupt(id, run, Disposition::Keep);
        QTRY_COMPARE_WITH_TIMEOUT(quiesced.size(), 1, 10000);
        QVERIFY(quiesced.first().at(2).toBool());
        QCOMPARE(env.moveStates(id), QStringList{ QStringLiteral("restored") });
        QFile back(env.source(QStringLiteral("taken")));
        QVERIFY(back.open(QIODevice::ReadOnly));
        QCOMPARE(back.readAll(), QByteArray(64000, 'a'));
#endif
    }

    //! Another file takes the real name of a source while its tool runs.
    static bool intrudeWhileRunning(Env &env, const QString &set, const QString &name)
    {
        if (!env.waitToolState(set, QStringLiteral("running")))
            return false;
        QFile intruder(env.source(name));
        if (!intruder.open(QIODevice::WriteOnly))
            return false;
        intruder.write("not ours");
        return true;
    }

    void aBlockedReturnHoldsThePostBeforeItsTransfer()
    {
#ifndef PROVEN_SUPERVISION
        QSKIP("nothing is ever proven here: sources are never put back");
#else
        Env env;
        QString error;
        QVERIFY2(env.start(&error, QStringLiteral("sleep=1500;write=40000")), qPrintable(error));
        const qint64 id = env.submit(env.post(QStringLiteral("carried"), true, true, 64000, true));
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QSignalSpy steps(env.executor.get(), &QueueExecutor::stepFinished);
        env.executor->prepareStep(id, run, set, PrepStep::Compress);
        QVERIFY(intrudeWhileRunning(env, set, QStringLiteral("carried")));
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 1, 15000);
        QVERIFY(steps.first().at(4).toBool()); // the archive is fine
        env.executor->prepareStep(id, run, set, PrepStep::Par2);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 2, 15000);
        env.executor->prepareStep(id, run, set, PrepStep::Collect);
        QTRY_COMPARE_WITH_TIMEOUT(steps.size(), 3, 15000);
        // Nothing is published while its source is aside: no transfer follows.
        QVERIFY(!steps.last().at(4).toBool());
        const QString stepErr = steps.last().at(5).toString();
        QVERIFY2(stepErrorIs(stepErr, kStepSourcesPending), qPrintable(stepErr));
        QVERIFY(env.executor->runtime(id));
        QCOMPARE(env.moveStates(id), QStringList{ QStringLiteral("blocked") });
        QVERIFY(!env.executor->manifest(id, run, set).isValid());
#endif
    }

    void aPostWaitingForItsSourcesStaysInTheQueue_data()
    {
        QTest::addColumn<QString>("rar");
        QTest::newRow("the collection refuses") << QStringLiteral("sleep=1500;write=40000");
        QTest::newRow("the compressor fails") << QStringLiteral("sleep=1500;exit=3");
    }

    void aPostWaitingForItsSourcesStaysInTheQueue()
    {
#ifndef PROVEN_SUPERVISION
        QSKIP("nothing is ever proven here: sources are never put back");
#else
        QFETCH(QString, rar);
        Env env;
        QString error;
        QVERIFY2(env.start(&error, rar), qPrintable(error));
        QueueScheduler::Budget budget;
        budget.rootFor = [&env](const QueueEntry &) { return env.root + QStringLiteral("/tmp"); };
        QueueScheduler scheduler(env.queue.get(), env.executor.get(), budget);
        // Even where a failed preparation retires its post (the command line).
        scheduler.setPreparationFailurePolicy(QueueScheduler::PreparationFailure::Retire);
        const qint64 id = env.submit(env.post(QStringLiteral("carried"), true, false, 64000, true));
        QTRY_VERIFY_WITH_TIMEOUT(!env.queue->find(id)->artifacts.isNull(), 10000);
        const QString set = env.queue->find(id)->artifacts.id;
        QVERIFY(intrudeWhileRunning(env, set, QStringLiteral("carried")));
        QTRY_VERIFY_WITH_TIMEOUT(scheduler.isTechnicallyPaused(), 20000);
        QCOMPARE(scheduler.technicalPauseReason(), QStringLiteral("sources_pending_recovery"));
        const QueueEntry *entry = env.queue->find(id);
        QVERIFY(entry);
        QCOMPARE(entry->blocked.code, QStringLiteral("sources_pending_recovery"));
        // Held as a stop not proven: its slot, its set and its runtime stay.
        QCOMPARE(entry->lifecycle, Lifecycle::Preparing);
        QCOMPARE(entry->slot, Slot::Preparation);
        QCOMPARE(entry->artifacts.id, set);
        QVERIFY(env.executor->runtime(id));
        // Neither a removal nor a close lets go of it.
        env.queue->remove({ id }, RemovePolicy::Default, env.queue->view());
        scheduler.keepRunningForClose(false);
        QTest::qWait(500);
        QVERIFY(env.queue->find(id));
        QVERIFY(env.executor->runtime(id));
        QCOMPARE(env.mock.receivedArticles().size(), 0);
        // At the exit its last return is refused: the journal still says so,
        // and the post stays this session's (lot C6 takes it from there).
        QVERIFY(QFile::remove(env.source(QStringLiteral("carried"))));
        QVERIFY(env.refuseReturns(true));
        env.executor.reset();
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("carried")))); // back on disk
        QCOMPARE(env.moveStates(id), QStringList{ QStringLiteral("blocked") });
        QString owner;
        {
            const QString name = QUuid::createUuid().toString();
            {
                QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
                db.setDatabaseName(env.dbPath());
                QSqlQuery q(db);
                if (db.open()
                    && q.exec(QStringLiteral("SELECT owner_id FROM posts WHERE id=%1").arg(id))
                    && q.next())
                    owner = q.value(0).toString();
            }
            QSqlDatabase::removeDatabase(name);
        }
        QCOMPARE(owner, QStringLiteral("owner-test"));
        QVERIFY(env.refuseReturns(false));
#endif
    }

    void archivesAndSourcesGoOnlyOnceTheirToolIsProvenStopped()
    {
        Env env;
        QString error;
        QVERIFY2(env.start(&error), qPrintable(error));
        PersistedQueueOptions options = env.post(QStringLiteral("cleaned"), true, true);
        options.job.delFilesAfterPost = true; // --rm_posted
        const qint64 id = env.submit(options);
        const QString run = env.queue->find(id)->runId;
        const QString set = env.beginSet(id);
        QVERIFY2(env.prepare(id, run, set, &error), qPrintable(error));
        QSignalSpy finished(env.executor.get(), &QueueExecutor::transferFinished);
        QSignalSpy finalized(env.executor.get(), &QueueExecutor::finalized);
        env.executor->activate(id, run, set);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 20000);
        env.executor->finalize(id, run);
        QTRY_COMPARE_WITH_TIMEOUT(finalized.size(), 1, 10000);
        env.executor->completeLocalWork(id, run);
        QTRY_VERIFY(!env.executor->runtime(id));
#ifdef PROVEN_SUPERVISION
        QTRY_VERIFY(!QDir(env.archiveDir(QStringLiteral("cleaned"))).exists());
        QVERIFY(!QFileInfo::exists(env.source(QStringLiteral("cleaned")))); // posted, then gone
#else
        // Only "exited": nothing proves no descendant still uses them.
        QTest::qWait(500);
        QVERIFY(QDir(env.archiveDir(QStringLiteral("cleaned"))).exists());
        QVERIFY(QFileInfo::exists(env.source(QStringLiteral("cleaned"))));
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

NGPOST_TEST_MAIN_WITH(tst_PostingJobExecutor, fakeToolsMain)
#include "tst_PostingJobExecutor.moc"
