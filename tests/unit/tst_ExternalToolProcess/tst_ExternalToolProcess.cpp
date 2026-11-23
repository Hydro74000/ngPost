// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// A preparation tool under its supervisor (plan_queue.md §6.3, lot C5.3):
// nothing runs before it is allowed, a stop is proven only once every
// descendant is gone (one that left the tool's group, one that ignores
// SIGTERM), a crashed controller still gets its proof, a killed supervisor
// leaves none, and a proof that is not this supervision's is never taken.
//
// This binary is its own supervisor (toolsupervisor::kSwitch), as ngPost is,
// and its own crashing controller (--controller-crash-fixture).
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
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <csignal>
#include <cstring>
#include <memory>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <unistd.h>
#endif

namespace
{

#ifdef Q_OS_LINUX
bool writeScript(const QString &path, const QByteArray &body)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write("#!/bin/sh\n" + body) > 0;
}

//! The tool leaves at once, after starting a writer in a session of its own
//! (out of the tool's process group), which the supervisor adopts.
QByteArray writerScript(bool ignoreTerm)
{
    const QByteArray trap = ignoreTerm ? "trap \"\" TERM; " : "";
    return "setsid sh -c '" + trap
        + "echo $$ > \"$2\"; while :; do echo x >> \"$1\"; sleep 0.05; done' w \"$1\" \"$2\" &\n"
          "exit 0\n";
}

qint64 readPid(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll().trimmed().toLongLong() : 0;
}

bool alive(qint64 pid)
{
#ifdef Q_OS_UNIX
    return pid > 0 && (::kill(pid_t(pid), 0) == 0 || errno != ESRCH);
#else
    Q_UNUSED(pid)
    return false;
#endif
}

void killQuietly(qint64 pid)
{
#ifdef Q_OS_UNIX
    if (pid > 0)
        ::kill(pid_t(pid), SIGKILL);
#else
    Q_UNUSED(pid)
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
    QTimer::singleShot(20000, &app, [] { ::_exit(8); });
    if (!tool->start(QStringLiteral("/bin/sh"),
                     { dir + QStringLiteral("/writer.sh"),
                       dir + QStringLiteral("/witness"),
                       dir + QStringLiteral("/grandchild.pid") },
                     dir,
                     dir + QStringLiteral("/proof")))
        return 7;
    return app.exec();
}
#endif // Q_OS_LINUX

} // namespace

class tst_ExternalToolProcess : public QObject
{
    Q_OBJECT

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
        bool start(const QStringList &args, const QString &proof = {})
        {
            return tool.start(QStringLiteral("/bin/sh"),
                              args,
                              dir.path(),
                              proof.isEmpty() ? path(QStringLiteral("proof")) : proof);
        }
        bool verified() const { return stopped.size() == 1 && stopped.at(0).at(0).toBool(); }
    };

private slots:
    void capabilityIsDeclared()
    {
#ifdef Q_OS_LINUX
        QCOMPARE(ExternalToolProcess::capability(), QStringLiteral("linux_subreaper"));
#else
        // No proven supervision here yet: nothing is ever started.
        QCOMPARE(ExternalToolProcess::capability(), QStringLiteral("unavailable"));
        ExternalToolProcess tool;
        QVERIFY(!tool.start(QStringLiteral("tool"), {}, QDir::tempPath(), QStringLiteral("proof")));
        QCOMPARE(ExternalToolProcess::proofState(QStringLiteral("{}")),
                 ExternalToolProcess::State::Unproven);
#endif
    }

    void aToolRunsOnlyOnceAllowed()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        Run r;
        QVERIFY(writeScript(r.path(QStringLiteral("touch.sh")), "touch \"$1\"\n"));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.sh")), r.path(QStringLiteral("ran")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::AwaitingGo);
        QTest::qWait(300);
        QVERIFY(!QFile::exists(r.path(QStringLiteral("ran")))); // not before "go"
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 5000);
        QVERIFY(r.verified());
        QVERIFY(QFile::exists(r.path(QStringLiteral("ran"))));
        QCOMPARE(r.started.size(), 1);
        QCOMPARE(r.tool.toolStatus(), 0);
        QCOMPARE(ExternalToolProcess::proofState(r.tool.identityJson()),
                 ExternalToolProcess::State::StoppedVerified);
#endif
    }

    void aStopBeforeGoRunsNothing()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        Run r;
        QVERIFY(writeScript(r.path(QStringLiteral("touch.sh")), "touch \"$1\"\n"));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.sh")), r.path(QStringLiteral("ran")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 5000);
        QVERIFY(r.verified());
        QCOMPARE(r.tool.toolStatus(), -1);
        QCOMPARE(r.started.size(), 0);
        QTest::qWait(200);
        QVERIFY(!QFile::exists(r.path(QStringLiteral("ran"))));
#endif
    }

    void aShortToolReapedFirstIsStillStarted()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        // The exec answer read only after the tool was reaped.
        qputenv("NGPOST_TEST_SLOW_EXEC_ANSWER", "1");
        const auto restore = qScopeGuard([] { qunsetenv("NGPOST_TEST_SLOW_EXEC_ANSWER"); });
        Run r;
        QVERIFY(writeScript(r.path(QStringLiteral("touch.sh")), "touch \"$1\"\n"));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.sh")), r.path(QStringLiteral("ran")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 5000);
        QVERIFY(r.verified());
        QCOMPARE(r.started.size(), 1); // it ran: announced, even late
        QCOMPARE(r.tool.toolStatus(), 0);
        QVERIFY(QFile::exists(r.path(QStringLiteral("ran"))));
#endif
    }

    void aLaunchThatCannotPrepareIsSpentOnce()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        // No descriptor left for the exec answer (EMFILE).
        qputenv("NGPOST_TEST_FAIL_EXEC_PIPE", "1");
        const auto restore = qScopeGuard([] { qunsetenv("NGPOST_TEST_FAIL_EXEC_PIPE"); });
        Run r;
        QSignalSpy failed(&r.tool, &ExternalToolProcess::execFailed);
        QVERIFY(writeScript(r.path(QStringLiteral("touch.sh")), "touch \"$1\"\n"));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.sh")), r.path(QStringLiteral("ran")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 5000);
        QVERIFY(r.verified());
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.at(0).at(0).toInt(), EMFILE);
        QCOMPARE(r.started.size(), 0);
        QCOMPARE(r.tool.toolStatus(), 127);
        QTest::qWait(200);
        QVERIFY(!QFile::exists(r.path(QStringLiteral("ran")))); // never tried again
#endif
    }

    void aStopWhileTheExecHangsIsProven()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        // The supervisor of this test binary stalls its child just before exec.
        qputenv("NGPOST_TEST_STALL_BEFORE_EXEC", "1");
        const auto restore = qScopeGuard([] { qunsetenv("NGPOST_TEST_STALL_BEFORE_EXEC"); });
        Run r;
        QVERIFY(writeScript(r.path(QStringLiteral("touch.sh")), "touch \"$1\"\n"));
        QVERIFY(r.start({ r.path(QStringLiteral("touch.sh")), r.path(QStringLiteral("ran")) }));
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
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        Run r;
        QVERIFY(writeScript(r.path(QStringLiteral("writer.sh")), writerScript(false)));
        QVERIFY(r.start({ r.path(QStringLiteral("writer.sh")),
                          r.path(QStringLiteral("witness")),
                          r.path(QStringLiteral("grandchild.pid")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.exited.size(), 1, 5000); // the tool itself is done...
        QTRY_VERIFY_WITH_TIMEOUT(readPid(r.path(QStringLiteral("grandchild.pid"))) > 0, 5000);
        const qint64 grandchild = readPid(r.path(QStringLiteral("grandchild.pid")));
        QTest::qWait(500);
        QCOMPARE(r.stopped.size(), 0); // ...but what it left still writes: no proof
        QVERIFY(alive(grandchild));
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 8000);
        QVERIFY(r.verified());
        QVERIFY(!alive(grandchild)); // proven only once it is gone
        const qint64 size = QFileInfo(r.path(QStringLiteral("witness"))).size();
        QTest::qWait(300);
        QCOMPARE(QFileInfo(r.path(QStringLiteral("witness"))).size(), size);
#endif
    }

    void aDescendantIgnoringTermIsKilled()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        Run r;
        QVERIFY(writeScript(r.path(QStringLiteral("writer.sh")), writerScript(true)));
        QVERIFY(r.start({ r.path(QStringLiteral("writer.sh")),
                          r.path(QStringLiteral("witness")),
                          r.path(QStringLiteral("grandchild.pid")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(readPid(r.path(QStringLiteral("grandchild.pid"))) > 0, 5000);
        const qint64 grandchild = readPid(r.path(QStringLiteral("grandchild.pid")));
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 10000);
        QVERIFY(r.verified());
        QVERIFY(!alive(grandchild));
#endif
    }

    void aCrashedControllerStillGetsItsProof()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        QTemporaryDir dir;
        QVERIFY(writeScript(dir.filePath(QStringLiteral("writer.sh")), writerScript(false)));
        QProcess controller;
        controller.start(QCoreApplication::applicationFilePath(),
                         { QStringLiteral("--controller-crash-fixture"), dir.path() });
        QVERIFY(controller.waitForFinished(20000));
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
#endif
    }

    void aControllerDestroyedEarlyLeavesTheSupervisorItsWork()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        QTemporaryDir dir;
        QVERIFY(writeScript(dir.filePath(QStringLiteral("writer.sh")), writerScript(true)));
        auto tool = std::make_unique<ExternalToolProcess>();
        QSignalSpy ready(tool.get(), &ExternalToolProcess::ready);
        QVERIFY(tool->start(QStringLiteral("/bin/sh"),
                            { dir.filePath(QStringLiteral("writer.sh")),
                              dir.filePath(QStringLiteral("witness")),
                              dir.filePath(QStringLiteral("grandchild.pid")) },
                            dir.path(),
                            dir.filePath(QStringLiteral("proof"))));
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
        tool->allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(readPid(dir.filePath(QStringLiteral("grandchild.pid"))) > 0, 5000);
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
#endif
    }

    void aKilledSupervisorLeavesNoProof()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        Run r;
        QVERIFY(writeScript(r.path(QStringLiteral("writer.sh")), writerScript(false)));
        QVERIFY(r.start({ r.path(QStringLiteral("writer.sh")),
                          r.path(QStringLiteral("witness")),
                          r.path(QStringLiteral("grandchild.pid")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(readPid(r.path(QStringLiteral("grandchild.pid"))) > 0, 5000);
        const qint64 grandchild = readPid(r.path(QStringLiteral("grandchild.pid")));
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
        killQuietly(grandchild); // what the test leaves behind
        QTRY_VERIFY_WITH_TIMEOUT(!alive(grandchild), 5000);
#endif
    }

    void aProofThatIsNotThisSupervisionsIsNotTaken()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        Run r;
        QVERIFY(writeScript(r.path(QStringLiteral("true.sh")), "exit 0\n"));
        QVERIFY(r.start({ r.path(QStringLiteral("true.sh")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 5000);
        QVERIFY(r.verified());
        QJsonObject identity = QJsonDocument::fromJson(r.tool.identityJson().toUtf8()).object();
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
        QFile stat(QStringLiteral("/proc/self/stat"));
        QVERIFY(stat.open(QIODevice::ReadOnly));
        const QString text = QString::fromLatin1(stat.readAll());
        const QString start =
            text.mid(text.lastIndexOf(QLatin1Char(')')) + 2).split(QLatin1Char(' ')).value(19);
        QJsonObject live = identity;
        live.insert(QStringLiteral("pid"), QString::number(QCoreApplication::applicationPid()));
        live.insert(QStringLiteral("starttime"), start);
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
#endif
    }

    void anUnexecutableToolEndsProven()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
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
        QCOMPARE(failed.at(0).at(0).toInt(), ENOENT);
        QCOMPARE(started.size(), 0); // exec failed: never "started"
        QVERIFY(stopped.at(0).at(0).toBool());
        QCOMPARE(tool.toolStatus(), 127);
#endif
    }

    void aProofThatCannotBeWrittenIsNoProof()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        Run r;
        QVERIFY(writeScript(r.path(QStringLiteral("true.sh")), "exit 0\n"));
        QVERIFY(
            r.start({ r.path(QStringLiteral("true.sh")) }, r.path(QStringLiteral("absent/proof"))));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 5000);
        QVERIFY(!r.verified());
        QCOMPARE(r.tool.state(), ExternalToolProcess::State::Unproven);
#endif
    }

    void aLostProtocolChannelStillEndsProven()
    {
#ifndef Q_OS_LINUX
        QSKIP("proven supervision: Linux (lot C5.3)");
#else
        Run r;
        QVERIFY(writeScript(r.path(QStringLiteral("writer.sh")), writerScript(false)));
        QVERIFY(r.start({ r.path(QStringLiteral("writer.sh")),
                          r.path(QStringLiteral("witness")),
                          r.path(QStringLiteral("grandchild.pid")) }));
        QTRY_COMPARE_WITH_TIMEOUT(r.ready.size(), 1, 5000);
        r.tool.allowExecution();
        QTRY_VERIFY_WITH_TIMEOUT(readPid(r.path(QStringLiteral("grandchild.pid"))) > 0, 5000);
        const qint64 grandchild = readPid(r.path(QStringLiteral("grandchild.pid")));
        r.tool.dropProtocolForTest(); // its messages now go nowhere (EPIPE)
        r.tool.stop();
        QTRY_COMPARE_WITH_TIMEOUT(r.stopped.size(), 1, 8000);
        QVERIFY(r.verified()); // read from the proof itself
        QVERIFY(!alive(grandchild));
#endif
    }
};

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], toolsupervisor::kSwitch) == 0)
        return toolsupervisor::run(argc, argv);
#ifdef Q_OS_LINUX
    if (argc > 2 && std::strcmp(argv[1], "--controller-crash-fixture") == 0)
        return crashFixture(argc, argv);
#endif
    QCoreApplication app(argc, argv);
    tst_ExternalToolProcess test;
    return QTest::qExec(&test, argc, argv);
}

#include "tst_ExternalToolProcess.moc"
