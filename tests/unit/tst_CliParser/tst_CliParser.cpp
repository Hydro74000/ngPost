//========================================================================
//
// tst_CliParser.cpp — CLI argument parsing surface, exercised by spawning
// the real ngPost binary as a subprocess.
//
// We use QProcess rather than constructing NgPost in-process because
// `NgPost::parseCommandLine` reads `QCoreApplication::arguments()`, which is
// fixed at the QCoreApplication's construction. Varying argv across test
// methods would otherwise require recreating QCoreApplication, which Qt
// doesn't reliably support.
//
// The ngPost binary path is resolved from $NGPOST_BIN, falling back to the
// canonical build location at <repo>/src/ngPost.
//
//========================================================================

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

#include "TestEnv.h"

using ngpost::tests::HomeSandbox;
using ngpost::tests::locateNgPostBinary;

namespace
{

struct RunResult
{
    int     exitCode;
    QString stdoutText;
    QString stderrText;
    bool    timedOut;
};

RunResult run(const QString &bin, const QStringList &args, const QString &sandboxHome)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("HOME", sandboxHome);
    env.insert("XDG_CONFIG_HOME", sandboxHome + QStringLiteral("/.config"));
    env.insert("APPDATA", sandboxHome);
    env.insert("USERPROFILE", sandboxHome);
    p.setProcessEnvironment(env);
    p.start(bin, args);

    RunResult r{ -1, QString(), QString(), false };
    if (!p.waitForFinished(10000)) {
        r.timedOut = true;
        p.kill();
        p.waitForFinished(2000);
        return r;
    }
    r.exitCode   = p.exitCode();
    r.stdoutText = QString::fromLocal8Bit(p.readAllStandardOutput());
    r.stderrText = QString::fromLocal8Bit(p.readAllStandardError());
    return r;
}

} // namespace

class TestCliParser : public QObject
{
    Q_OBJECT

private:
    QString _bin;

private slots:
    void initTestCase();

    //! `--version` prints the version banner and exits 0.
    void version_flag_succeeds();

    //! `--help` lists the user-facing flags (sanity-check a few well-known ones).
    void help_lists_major_flags();

    //! An unknown flag fails with a non-zero exit code. Currently
    //! ERR_WRONG_ARG = 3 but tests assert "non-zero" for resilience to enum
    //! reordering.
    void unknown_flag_rejected();

    //! `--vpn --no_vpn` together is contradictory and must be rejected with a
    //! "mutually exclusive" message.
    void vpn_and_no_vpn_mutually_exclusive();

    //! `--vpn_profile <name>` referring to a profile that does not exist in
    //! the config is rejected with a clear "does not match any profile"
    //! message.
    void vpn_profile_unknown_rejected();

    //! `--auto <dir>` without `--compress` must error with ERR_AUTO_NO_COMPRESS.
    void auto_dir_without_compress_rejected();
};

void TestCliParser::initTestCase()
{
    _bin = locateNgPostBinary();
    if (_bin.isEmpty()) {
        QSKIP("ngPost binary not found. Build it first or set NGPOST_BIN.");
    }
    qInfo() << "Using binary:" << _bin;
}

void TestCliParser::version_flag_succeeds()
{
    HomeSandbox sandbox;
    const RunResult r = run(_bin, { "--version" }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "ngPost --version timed out");
    QCOMPARE(r.exitCode, 0);
    QVERIFY2(r.stdoutText.contains("5.2.2"),
             qPrintable(QStringLiteral("stdout did not mention version: %1").arg(r.stdoutText)));
}

void TestCliParser::help_lists_major_flags()
{
    HomeSandbox sandbox;
    const RunResult r = run(_bin, { "--help" }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "ngPost --help timed out");
    QCOMPARE(r.exitCode, 0);

    const QString out = r.stdoutText + r.stderrText;
    for (const char *flag : { "--vpn", "--vpn_profile", "--auto", "--monitor" }) {
        QVERIFY2(out.contains(QString::fromLatin1(flag)),
                 qPrintable(QStringLiteral("help output did not mention '%1'").arg(QString::fromLatin1(flag))));
    }
}

void TestCliParser::unknown_flag_rejected()
{
    HomeSandbox sandbox;
    const RunResult r = run(_bin, { "--bogus-flag-does-not-exist" }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0,
             qPrintable(QStringLiteral("unknown flag should fail; got exit=%1").arg(r.exitCode)));
    // Error path uses _error() which writes to stderr.
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains("syntax", Qt::CaseInsensitive)
                 || out.contains("Unknown option", Qt::CaseInsensitive),
             qPrintable(QStringLiteral("expected a syntax error message, got: %1").arg(out)));
}

void TestCliParser::vpn_and_no_vpn_mutually_exclusive()
{
    HomeSandbox sandbox;

    // Need at least one input source to bypass the earlier ERR_NO_INPUT check
    // and reach the VPN mutex check.
    const QString stub = sandbox.rootPath() + QStringLiteral("/in.bin");
    QFile f(stub);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("hello");
    f.close();

    const RunResult r = run(_bin, { "--vpn", "--no_vpn", "-i", stub }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, "expected non-zero exit for --vpn + --no_vpn");
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains("mutually exclusive", Qt::CaseInsensitive),
             qPrintable(QStringLiteral("expected 'mutually exclusive' in output, got: %1").arg(out)));
}

void TestCliParser::vpn_profile_unknown_rejected()
{
    HomeSandbox sandbox;

    const QString stub = sandbox.rootPath() + QStringLiteral("/in.bin");
    QFile f(stub);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("hello");
    f.close();

    const RunResult r = run(_bin,
                            { "--vpn_profile", "NotInTheConfig", "-i", stub },
                            sandbox.rootPath());

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, "expected non-zero exit for unknown vpn profile");
    const QString out = r.stdoutText + r.stderrText;
    QVERIFY2(out.contains("does not match any profile", Qt::CaseInsensitive),
             qPrintable(QStringLiteral("expected profile-mismatch error, got: %1").arg(out)));
}

void TestCliParser::auto_dir_without_compress_rejected()
{
    HomeSandbox sandbox;
    const QString autoDir = sandbox.rootPath() + QStringLiteral("/autoDir");
    QDir().mkpath(autoDir);

    const RunResult r = run(_bin, { "--auto", autoDir }, sandbox.rootPath());

    QVERIFY2(!r.timedOut, "process timed out");
    QVERIFY2(r.exitCode != 0, "expected non-zero exit for --auto without --compress");
}

QTEST_MAIN(TestCliParser)
#include "tst_CliParser.moc"
