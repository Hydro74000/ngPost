// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// A preparation tool under its supervisor (plan_queue.md §6.3, lots C5.3
// Linux and C5.4 Windows): nothing runs before it is allowed, a stop is
// proven only once every descendant is gone (one that left the tool's group,
// one that ignores SIGTERM), a crashed controller still gets its proof, a
// killed supervisor leaves none, and a proof that is not this supervision's
// is never taken. On Windows, nothing breaks away from the job, nested jobs
// included.
//
// The tools are small Python scripts. This binary is its own supervisor
// (toolsupervisor::kSwitch), as ngPost is, and its own crashing controller
// (--controller-crash-fixture).
//
//========================================================================

#include "tools/ExternalToolProcess.h"
#include "tools/ToolSupervisor.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <csignal>
#include <cstring>
#include <memory>

#if defined(Q_OS_LINUX) || defined(Q_OS_WIN)
#define PROVEN_SUPERVISION 1
#endif
#if defined(PROVEN_SUPERVISION) || defined(Q_OS_MACOS)
#define TOOLS_RUN 1 // tools run here, proven or not (macOS: never proven)
#endif

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{

#ifdef TOOLS_RUN
QString python()
{
#ifdef Q_OS_WIN
    const QString found = QStandardPaths::findExecutable(QStringLiteral("python"));
#else
    const QString found = QStandardPaths::findExecutable(QStringLiteral("python3"));
#endif
    return found;
}

bool writeScript(const QString &path, const QByteArray &body)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(body) > 0;
}

const QByteArray kTouch = "import pathlib, sys\npathlib.Path(sys.argv[1]).touch()\n";
const QByteArray kTrue = "pass\n";

//! The tool leaves at once, after starting a writer of its own that is out of
//! its process group (a new session; detached on Windows), which the
//! supervisor adopts (subreaper) or keeps (job). Arguments: witness file, pid
//! file, "1" to ignore SIGTERM.
const QByteArray kWriter = R"(import os, subprocess, sys
child = r'''
import os, signal, sys, time
if sys.argv[3] == "1":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
with open(sys.argv[2] + ".tmp", "w") as f:
    f.write(str(os.getpid()))
os.replace(sys.argv[2] + ".tmp", sys.argv[2])
while True:
    with open(sys.argv[1], "a") as f:
        f.write("x\n")
    time.sleep(0.05)
'''
options = {}
if os.name == "nt":
    options["creationflags"] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
else:
    options["start_new_session"] = True
subprocess.Popen([sys.executable, "-c", child] + sys.argv[1:4], stdin=subprocess.DEVNULL,
                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True, **options)
)";

QStringList writerArgs(const QString &dir, bool ignoreTerm)
{
    return { dir + QStringLiteral("/writer.py"),
             dir + QStringLiteral("/witness"),
             dir + QStringLiteral("/grandchild.pid"),
             ignoreTerm ? QStringLiteral("1") : QStringLiteral("0") };
}

qint64 readPid(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed().toLongLong() : 0;
}

bool alive(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!process)
        return false;
    DWORD code = 0;
    const bool running = ::GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    ::CloseHandle(process);
    return running;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno != ESRCH;
#endif
}

void killQuietly(qint64 pid)
{
    if (pid <= 0)
        return;
#ifdef Q_OS_WIN
    if (HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid))) {
        ::TerminateProcess(process, 1);
        ::CloseHandle(process);
    }
#else
    ::kill(pid_t(pid), SIGKILL);
#endif
}

#endif // TOOLS_RUN

#ifdef PROVEN_SUPERVISION
//! The start time a supervision records for the process @p pid.
QString startTimeOfSelf()
{
#ifdef Q_OS_WIN
    FILETIME created = {};
    FILETIME exited = {};
    FILETIME kernel = {};
    FILETIME user = {};
    ::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user);
    ULARGE_INTEGER value = {};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return QString::number(value.QuadPart);
#else
    QFile stat(QStringLiteral("/proc/self/stat"));
    if (!stat.open(QIODevice::ReadOnly))
        return {};
    const QString text = QString::fromLatin1(stat.readAll());
    return text.mid(text.lastIndexOf(QLatin1Char(')')) + 2).split(QLatin1Char(' ')).value(19);
#endif
}

//! The controller dies (no destructor) while a descendant of its tool writes.
int crashFixture(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString dir = QString::fromLocal8Bit(argv[2]);
    auto *tool = new ExternalToolProcess(&app);
    QObject::connect(tool,
                     &ExternalToolProcess::ready,
                     &app,
                     [tool, dir](const QString &, const QString &id) {
        QFile out(dir + QStringLiteral("/identity.json"));
        if (out.open(QIODevice::WriteOnly))
            out.write(id.toUtf8());
        out.close();
        tool->allowExecution();
    });
    auto *wait = new QTimer(&app);
    QObject::connect(wait, &QTimer::timeout, &app, [dir] {
        if (QFile::exists(dir + QStringLiteral("/grandchild.pid")))
            ::_exit(9); // crash: no destructor, the lifeline just ends
    });
    wait->start(20);
    QTimer::singleShot(30000, &app, [] { ::_exit(8); });
    if (!tool->start(python(), writerArgs(dir, false), dir, dir + QStringLiteral("/proof")))
        return 7;
    return app.exec();
}
#endif // PROVEN_SUPERVISION

} // namespace

class tst_ExternalToolProcess : public QObject
{
    Q_OBJECT

#ifdef TOOLS_RUN
private:
    struct Run
    {
        QTemporaryDir dir;
        ExternalToolProcess tool;
        QSignalSpy ready{ &tool, &ExternalToolProcess::ready };
        QSignalSpy started{ &tool, &ExternalToolProcess::started };
        QSignalSpy exited{ &tool, &ExternalToolProcess::toolExited };
        QSignalSpy stopped{ &tool, &ExternalToolProcess::stopped };
        QString path(const QString &name) const { return dir.filePath(name); }
        bool script(const QString &name, const QByteArray &body) const
        {
            return writeScript(path(name), body);
        }
        bool start(const QStringList &args, const QString &proof = {})
        {
            return tool.start(python(),
                              args,
                              dir.path(),
                              proof.isEmpty() ? path(QStringLiteral("proof")) : proof);
        }
        bool verified() const { return stopped.size() == 1 && stopped.at(0).at(0).toBool(); }
        qint64 grandchild() const { return readPid(path(QStringLiteral("grandchild.pid"))); }
    };
#endif

private slots:
    void initTestCase()
    {
#ifdef TOOLS_RUN
        if (python().isEmpty())
            QSKIP("the fixture tools are Python scripts: no python on PATH");
#endif
    }

    void capabilityIsDeclared()
    {
#if defined(Q_OS_LINUX)
        QCOMPARE(ExternalToolProcess::capability(), QStringLiteral("linux_subreaper"));
#elif defined(Q_OS_WIN)
        QCOMPARE(ExternalToolProcess::capability(), QStringLiteral("windows_job"));
#else
        // Tools run, nothing is ever proven (lot C5.5).
        QCOMPARE(ExternalToolProcess::capability(), QStringLiteral("unavailable"));
        QCOMPARE(ExternalToolProcess::proofState(QStringLiteral("{}")),
                 ExternalToolProcess::State::Unproven);
#endif
    }

#ifdef PROVEN_SUPERVISION
    void aToolRunsOnlyOnceAllowed()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("touch.py"), kTouch));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.py")), r.path(QStringLiteral("ran")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::AwaitingGo);
        QTest::qWait(300);
        QVERIFY(!QFile::exists(r.path(QStringLiteral("ran")))); // not before "go"
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(r.verified());
        QVERIFY(QFile::exists(r.path(QStringLiteral("ran"))));
        QCOMPARE(r.started.size(), 1);
        QCOMPARE(r.tool.toolStatus(), 0);
        QCOMPARE(ExternalToolProcess::proofState(r.tool.identityJson()),
                 ExternalToolProcess::State::StoppedVerified);
        r.tool.stop(); // over already: changes nothing
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::StoppedVerified);
    }

    void aStopBeforeGoRunsNothing()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("touch.py"), kTouch));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.py")), r.path(QStringLiteral("ran")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 5000);
        QVERIFY(r.verified());
        QCOMPARE(r.tool.toolStatus(), -1);
        QCOMPARE(r.started.size(), 0);
        QTest::qWait(300);
        QVERIFY(!QFile::exists(r.path(QStringLiteral("ran"))));
    }

    void aStopRightAfterStartIsKept()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("touch.py"), kTouch));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.py")), r.path(QStringLiteral("ran")) }));
        r.tool.stop(); // before the supervisor even connected or said "ready"
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(r.verified());
        QCOMPARE(r.tool.toolStatus(), -1);
        QCOMPARE(r.started.size(), 0);
        QTest::qWait(300);
        QVERIFY(!QFile::exists(r.path(QStringLiteral("ran"))));
    }

    void aShortToolReapedFirstIsStillStarted()
    {
#ifndef Q_OS_LINUX
        QSKIP("a hook of the Linux supervisor (its exec answer)");
#else
        // The exec answer read only after the tool was reaped.
        qputenv("NGPOST_TEST_SLOW_EXEC_ANSWER", "1");
        const auto restore = qScopeGuard([] { qunsetenv("NGPOST_TEST_SLOW_EXEC_ANSWER"); });
        Run r;
        QVERIFY(r.script(QStringLiteral("touch.py"), kTouch));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.py")), r.path(QStringLiteral("ran")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(r.verified());
        QCOMPARE(r.started.size(), 1); // it ran: announced, even late
        QCOMPARE(r.tool.toolStatus(), 0);
        QVERIFY(QFile::exists(r.path(QStringLiteral("ran"))));
#endif
    }

    void aLaunchThatCannotPrepareIsSpentOnce()
    {
#ifndef Q_OS_LINUX
        QSKIP("a hook of the Linux supervisor (its exec answer pipe)");
#else
        // No descriptor left for the exec answer (EMFILE).
        qputenv("NGPOST_TEST_FAIL_EXEC_PIPE", "1");
        const auto restore = qScopeGuard([] { qunsetenv("NGPOST_TEST_FAIL_EXEC_PIPE"); });
        Run r;
        QSignalSpy failed(&r.tool, &ExternalToolProcess::execFailed);
        QVERIFY(r.script(QStringLiteral("touch.py"), kTouch));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.py")), r.path(QStringLiteral("ran")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 5000);
        QVERIFY(r.verified());
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.at(0).at(0).toInt(), EMFILE);
        QCOMPARE(r.started.size(), 0);
        QCOMPARE(r.tool.toolStatus(), 127);
        QTest::qWait(300);
        QVERIFY(!QFile::exists(r.path(QStringLiteral("ran")))); // never tried again
#endif
    }

    void aStopWhileTheExecHangsIsProven()
    {
#ifndef Q_OS_LINUX
        QSKIP("a hook of the Linux supervisor (before exec)");
#else
        // The supervisor of this test binary stalls its child just before exec.
        qputenv("NGPOST_TEST_STALL_BEFORE_EXEC", "1");
        const auto restore = qScopeGuard([] { qunsetenv("NGPOST_TEST_STALL_BEFORE_EXEC"); });
        Run r;
        QVERIFY(r.script(QStringLiteral("touch.py"), kTouch));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.py")), r.path(QStringLiteral("ran")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTest::qWait(300);
        QCOMPARE(r.started.size(), 0); // exec not confirmed: still the stalled child
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 5000);
        QVERIFY(r.verified());
        QCOMPARE(r.started.size(), 0);
        QCOMPARE(r.tool.toolStatus(), 128 + SIGTERM);
        QVERIFY(!QFile::exists(r.path(QStringLiteral("ran"))));
#endif
    }

    void aDescendantOutOfTheGroupIsStoppedFirst()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("writer.py"), kWriter));
        QVERIFY(r.start(writerArgs(r.dir.path(), false)));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.exited.size(), 1, 10000); // the tool itself is done...
        QTRY_VERIFY_WITH_TIMEOUT(r.grandchild() > 0, 10000);
        const qint64 grandchild = r.grandchild();
        QTest::qWait(500);
        QCOMPARE(r.stopped.size(), 0); // ...but what it left still writes: no proof
        QVERIFY(alive(grandchild));
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(r.verified());
        QVERIFY(!alive(grandchild)); // proven only once it is gone
        const qint64 size = QFileInfo(r.path(QStringLiteral("witness"))).size();
        QTest::qWait(300);
        QCOMPARE(QFileInfo(r.path(QStringLiteral("witness"))).size(), size);
    }

    void aDescendantIgnoringTermIsKilled()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("writer.py"), kWriter));
        QVERIFY(r.start(writerArgs(r.dir.path(), true)));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(r.grandchild() > 0, 10000);
        const qint64 grandchild = r.grandchild();
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(r.verified());
        QVERIFY(!alive(grandchild));
    }

    void aCrashedControllerStillGetsItsProof()
    {
        QTemporaryDir dir;
        QVERIFY(writeScript(dir.filePath(QStringLiteral("writer.py")), kWriter));
        QProcess controller;
        controller.start(QCoreApplication::applicationFilePath(),
                         { QStringLiteral("--controller-crash-fixture"), dir.path() });
        QVERIFY(controller.waitForFinished(30000));
        QCOMPARE(controller.exitCode(), 9); // it crashed while the tool's descendant wrote
        QFile identityFile(dir.filePath(QStringLiteral("identity.json")));
        QVERIFY(identityFile.open(QIODevice::ReadOnly));
        const QString identity = QString::fromUtf8(identityFile.readAll());
        const qint64 grandchild = readPid(dir.filePath(QStringLiteral("grandchild.pid")));
        QVERIFY(grandchild > 0);
        // Its supervisor saw the lifeline end, stopped the tree, wrote the proof.
        QTRY_COMPARE_WITH_TIMEOUT(ExternalToolProcess::proofState(identity),
                                  ExternalToolProcess::State::StoppedVerified,
                                  10000);
        QVERIFY(!alive(grandchild));
    }

    void aControllerDestroyedEarlyLeavesTheSupervisorItsWork()
    {
        QTemporaryDir dir;
        QVERIFY(writeScript(dir.filePath(QStringLiteral("writer.py")), kWriter));
        auto tool = std::make_unique<ExternalToolProcess>();
        QSignalSpy ready(tool.get(), &ExternalToolProcess::ready);
        QVERIFY(tool->start(python(),
                            writerArgs(dir.path(), true),
                            dir.path(),
                            dir.filePath(QStringLiteral("proof"))));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        tool->allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(readPid(dir.filePath(QStringLiteral("grandchild.pid"))) > 0,
                                 10000);
        const qint64 grandchild = readPid(dir.filePath(QStringLiteral("grandchild.pid")));
        const QString identity = tool->identityJson();
        QElapsedTimer clock;
        clock.start();
        tool.reset(); // returns at once: its supervisor is neither waited for nor killed
        QVERIFY(clock.elapsed() < 1000);
        // It stops the tree (TERM ignored: KILL after its grace) and proves it.
        QTRY_COMPARE_WITH_TIMEOUT(ExternalToolProcess::proofState(identity),
                                  ExternalToolProcess::State::StoppedVerified,
                                  10000);
        QVERIFY(!alive(grandchild));
    }

    void aKilledSupervisorLeavesNoProof()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("writer.py"), kWriter));
        QVERIFY(r.start(writerArgs(r.dir.path(), false)));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(r.grandchild() > 0, 10000);
        const qint64 grandchild = r.grandchild();
        const qint64 supervisor = QJsonDocument::fromJson(r.tool.identityJson().toUtf8())
                                      .object()
                                      .value(QStringLiteral("pid"))
                                      .toString()
                                      .toLongLong();
        killQuietly(supervisor);
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 5000);
        QVERIFY(!r.verified()); // its writer may live on: nothing may be touched
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::Unproven);
        QCOMPARE(ExternalToolProcess::proofState(r.tool.identityJson()),
                 ExternalToolProcess::State::Unproven);
        killQuietly(grandchild); // what the test leaves behind (Linux; the job does it on Windows)
        QTRY_VERIFY_WITH_TIMEOUT(!alive(grandchild), 5000);
    }

    void aProofThatIsNotThisSupervisionsIsNotTaken()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("true.py"), kTrue));
        QVERIFY(r.start({ r.path(QStringLiteral("true.py")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(r.verified());
        const QJsonObject identity = QJsonDocument::fromJson(r.tool.identityJson().toUtf8())
                                         .object();
        auto stateWith = [&identity](const char *key, const QString &value) {
            QJsonObject changed = identity;
            changed.insert(QLatin1String(key), value);
            return ExternalToolProcess::proofState(
                QString::fromUtf8(QJsonDocument(changed).toJson()));
        };
        QCOMPARE(stateWith("nonce", QStringLiteral("another")),
                 ExternalToolProcess::State::Unproven);
        QCOMPARE(stateWith("starttime", QStringLiteral("1")), ExternalToolProcess::State::Unproven);
        QCOMPARE(stateWith("boot", QStringLiteral("other-boot")),
                 ExternalToolProcess::State::Unproven);
        // A supervisor still there (this very process stands for one) is running.
        QJsonObject live = identity;
        live.insert(QStringLiteral("pid"), QString::number(QCoreApplication::applicationPid()));
        live.insert(QStringLiteral("starttime"), startTimeOfSelf());
        live.insert(QStringLiteral("proof"), r.path(QStringLiteral("no-proof")));
        QCOMPARE(ExternalToolProcess::proofState(QString::fromUtf8(QJsonDocument(live).toJson())),
                 ExternalToolProcess::State::Running);
        // The proof file tampered with.
        QFile proof(r.path(QStringLiteral("proof")));
        QVERIFY(proof.open(QIODevice::ReadWrite));
        const QByteArray body = proof.readAll().replace("tool_status 0", "tool_status x");
        proof.resize(0);
        proof.write(body);
        proof.close();
        QCOMPARE(ExternalToolProcess::proofState(r.tool.identityJson()),
                 ExternalToolProcess::State::Unproven);
    }

    void anUnexecutableToolEndsProven()
    {
        QTemporaryDir dir;
        ExternalToolProcess tool;
        QSignalSpy ready(&tool, &ExternalToolProcess::ready);
        QSignalSpy started(&tool, &ExternalToolProcess::started);
        QSignalSpy failed(&tool, &ExternalToolProcess::execFailed);
        QSignalSpy stopped(&tool, &ExternalToolProcess::stopped);
        QVERIFY(tool.start(dir.filePath(QStringLiteral("no-such-tool")),
                           {},
                           dir.path(),
                           dir.filePath(QStringLiteral("proof"))));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.at(0).at(0).toInt(), 2); // ENOENT, ERROR_FILE_NOT_FOUND
        QCOMPARE(started.size(), 0);             // never "started"
        QVERIFY(stopped.at(0).at(0).toBool());
        QCOMPARE(tool.toolStatus(), 127);
    }

    void aProofThatCannotBeWrittenIsNoProof()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("true.py"), kTrue));
        QVERIFY(
            r.start({ r.path(QStringLiteral("true.py")) }, r.path(QStringLiteral("absent/proof"))));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(!r.verified());
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::Unproven);
    }

    void aLostProtocolChannelStillEndsProven()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("writer.py"), kWriter));
        QVERIFY(r.start(writerArgs(r.dir.path(), false)));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(r.grandchild() > 0, 10000);
        const qint64 grandchild = r.grandchild();
        r.tool.dropProtocolForTest(); // its messages now go nowhere
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(r.verified()); // read from the proof itself
        QVERIFY(!alive(grandchild));
    }

    void nothingBreaksAwayFromTheJob()
    {
#ifndef Q_OS_WIN
        QSKIP("a Windows job");
#else
        Run r;
        // The tool asks for a child out of its job; the job does not allow it.
        QVERIFY(r.script(QStringLiteral("breakaway.py"),
                         "import subprocess, sys\n"
                         "try:\n"
                         "    subprocess.Popen([sys.executable, '-c', 'import time; "
                         "time.sleep(30)'], creationflags=subprocess.CREATE_BREAKAWAY_FROM_JOB)\n"
                         "    open(sys.argv[1], 'w').write('escaped')\n"
                         "except OSError:\n"
                         "    open(sys.argv[1], 'w').write('refused')\n"));
        QVERIFY(
            r.start({ r.path(QStringLiteral("breakaway.py")), r.path(QStringLiteral("outcome")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(r.verified());
        QFile outcome(r.path(QStringLiteral("outcome")));
        QVERIFY(outcome.open(QIODevice::ReadOnly));
        QCOMPARE(outcome.readAll(), QByteArray("refused"));
#endif
    }

    //! Last: it puts this very process in a job, as a CI runner or a service
    //! manager may do with ngPost. The tool's job is then nested in it.
    void aSupervisionInsideAJobStillProvesItsStop()
    {
#ifndef Q_OS_WIN
        QSKIP("a Windows job");
#else
        HANDLE outer = ::CreateJobObjectW(nullptr, nullptr);
        QVERIFY(outer);
        if (!::AssignProcessToJobObject(outer, ::GetCurrentProcess()))
            QSKIP("this process cannot join a job here");
        Run r;
        QVERIFY(r.script(QStringLiteral("writer.py"), kWriter));
        QVERIFY(r.start(writerArgs(r.dir.path(), false)));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(r.grandchild() > 0, 10000);
        const qint64 grandchild = r.grandchild();
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(r.verified());
        QVERIFY(!alive(grandchild));
#endif
    }
#endif // PROVEN_SUPERVISION

#ifdef Q_OS_MACOS
    // --- capability "unavailable" (lot C5.5): tools run, nothing is proven ------------

    void macToolEndedOnItsOwnHasExited()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("touch.py"), kTouch));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.py")), r.path(QStringLiteral("ran")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        QTest::qWait(300);
        QVERIFY(!QFile::exists(r.path(QStringLiteral("ran")))); // not before "go" either
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(!r.verified());
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::Exited); // outputs usable
        QCOMPARE(r.tool.toolStatus(), 0);
        QVERIFY(QFile::exists(r.path(QStringLiteral("ran"))));
        QVERIFY(!QFile::exists(r.path(QStringLiteral("proof")))); // never a proof
        QVERIFY(ExternalToolProcess::proofState(r.tool.identityJson())
                != ExternalToolProcess::State::StoppedVerified);
        r.tool.stop(); // over already: still Exited
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::Exited);
        QTest::qWait(200);
        QCOMPARE(r.stopped.size(), 1);
    }

    void macFailedToolHasExitedWithItsStatus()
    {
        // Ended on its own: no more running, whatever its status (lot C5.6).
        // Its status, not its state, says its outputs are not to be used.
        Run r;
        QVERIFY(r.script(QStringLiteral("fail.py"), "import sys\nsys.exit(3)\n"));
        QVERIFY(r.start({ r.path(QStringLiteral("fail.py")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(!r.verified());
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::Exited);
        QCOMPARE(r.tool.toolStatus(), 3);
    }

    void macUnexecutableToolIsNeverExited()
    {
        // Its exec failed: it never ran, so it never "ended on its own".
        QTemporaryDir dir;
        ExternalToolProcess tool;
        QSignalSpy ready(&tool, &ExternalToolProcess::ready);
        QSignalSpy failed(&tool, &ExternalToolProcess::execFailed);
        QSignalSpy stopped(&tool, &ExternalToolProcess::stopped);
        QVERIFY(tool.start(dir.filePath(QStringLiteral("no-such-tool")),
                           {},
                           dir.path(),
                           dir.filePath(QStringLiteral("proof"))));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(stopped.size(), 1, 5000);
        QCOMPARE(failed.size(), 1);
        QVERIFY(!stopped.at(0).at(0).toBool());
        QCOMPARE(tool.state(), ExternalToolProcess::State::Unproven);
        QCOMPARE(tool.toolStatus(), 127);
    }

    void macLostSupervisorIsUnprovenEvenAfterTheToolEnded()
    {
        // The tool ended, then its supervisor was killed: not as planned.
        Run r;
        QVERIFY(r.script(QStringLiteral("fail.py"), "import sys\nsys.exit(3)\n"));
        qputenv("NGPOST_TEST_STALL_AFTER_TOOL_EXIT", "1");
        const bool started = r.start({ r.path(QStringLiteral("fail.py")) });
        qunsetenv("NGPOST_TEST_STALL_AFTER_TOOL_EXIT");
        QVERIFY(started);
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        QSignalSpy exited(&r.tool, &ExternalToolProcess::toolExited);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 10000);
        const QJsonObject identity = QJsonDocument::fromJson(r.tool.identityJson().toUtf8())
                                         .object();
        ::kill(pid_t(identity.value(QStringLiteral("pid")).toString().toLongLong()), SIGKILL);
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::Unproven);
    }

    void macStoppedToolIsUnprovenAndItsGroupKilled()
    {
        Run r;
        // Stays in its group and ignores TERM: KILL after the grace.
        QVERIFY(r.script(QStringLiteral("stubborn.py"),
                         "import os, signal, sys, time\n"
                         "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
                         "open(sys.argv[1], 'w').write(str(os.getpid()))\n"
                         "time.sleep(60)\n"));
        QVERIFY(
            r.start({ r.path(QStringLiteral("stubborn.py")), r.path(QStringLiteral("tool.pid")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(readPid(r.path(QStringLiteral("tool.pid"))) > 0, 10000);
        const qint64 toolPid = readPid(r.path(QStringLiteral("tool.pid")));
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::Unproven); // never Exited after a stop
        QVERIFY(!alive(toolPid));
        QCOMPARE(r.tool.toolStatus(), 128 + SIGKILL);
    }

    void macDescendantOutOfTheGroupIsNeverAProof()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("writer.py"), kWriter));
        QVERIFY(r.start(writerArgs(r.dir.path(), false)));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(r.grandchild() > 0, 10000);
        const qint64 grandchild = r.grandchild();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        // The tool itself ended well: usable, but nothing says its writer is gone.
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::Exited);
        QVERIFY(!QFile::exists(r.path(QStringLiteral("proof"))));
        killQuietly(grandchild); // out of our sight: the test ends it
        QTRY_VERIFY_WITH_TIMEOUT(!alive(grandchild), 5000);
    }

    void macForgedProofIsNeverTaken()
    {
        Run r;
        QVERIFY(r.script(QStringLiteral("true.py"), kTrue));
        QVERIFY(r.start({ r.path(QStringLiteral("true.py")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        const QJsonObject identity = QJsonDocument::fromJson(r.tool.identityJson().toUtf8())
                                         .object();
        QFile proof(r.path(QStringLiteral("proof")));
        QVERIFY(proof.open(QIODevice::WriteOnly));
        proof.write("ngpost-tool-proof 1\nnonce " + identity.value("nonce").toString().toUtf8()
                    + "\nboot " + identity.value("boot").toString().toUtf8() + "\npid "
                    + identity.value("pid").toString().toUtf8() + "\nstarttime "
                    + identity.value("starttime").toString().toUtf8() + "\ntool_status 0\n");
        proof.close();
        QCOMPARE(ExternalToolProcess::proofState(r.tool.identityJson()),
                 ExternalToolProcess::State::Unproven);
    }
#endif // Q_OS_MACOS
};

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], toolsupervisor::kSwitch) == 0)
        return toolsupervisor::run(argc, argv);
#ifdef PROVEN_SUPERVISION
    if (argc > 2 && std::strcmp(argv[1], "--controller-crash-fixture") == 0)
        return crashFixture(argc, argv);
#endif
    QCoreApplication app(argc, argv);
    tst_ExternalToolProcess test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ExternalToolProcess.moc"
