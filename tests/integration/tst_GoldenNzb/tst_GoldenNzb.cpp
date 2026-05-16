//========================================================================
//
// tst_GoldenNzb.cpp — non-regression diff against committed "golden" NZB
// outputs.
//
// For each scenario:
//   1. Post a deterministic input file via the mock NNTP.
//   2. Read back the NZB ngPost wrote.
//   3. Normalize (strip random/timestamp fields).
//   4. Diff against tests/fixtures/nzb/<name>.golden.nzb.
//
// To regenerate goldens after an intentional NZB format change:
//   NGPOST_REGEN_GOLDENS=1 ./tst_GoldenNzb
//
// The check-in diff is the explicit audit trail for the format change.
//
//========================================================================

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>

#include "Fixtures.h"
#include "MockNntpServer.h"
#include "TestEnv.h"

using ngpost::tests::HomeSandbox;
using ngpost::tests::MockNntpServer;
using ngpost::tests::fixturesDir;
using ngpost::tests::locateNgPostBinary;

namespace
{

bool hasPython3()
{
    return !QStandardPaths::findExecutable("python3").isEmpty()
           || !QStandardPaths::findExecutable("python").isEmpty();
}

int runNgPost(const QString &bin, const QStringList &args,
              const QString &sandboxHome, QString &output, int timeoutMs = 30000)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("HOME", sandboxHome);
    env.insert("XDG_CONFIG_HOME", sandboxHome + QStringLiteral("/.config"));
    env.insert("APPDATA", sandboxHome);
    env.insert("USERPROFILE", sandboxHome);
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(bin, args);
    if (!p.waitForStarted(5000))
        return -1;
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        output = QString::fromLocal8Bit(p.readAll());
        return -2;
    }
    output = QString::fromLocal8Bit(p.readAll());
    return p.exitCode();
}

//! Normalize an NZB so two valid runs of the same scenario produce the same
//! string. We replace fields that legitimately vary between runs (timestamps,
//! random message-IDs, random poster email) with stable placeholders.
QByteArray normalizeNzb(const QByteArray &raw)
{
    QString s = QString::fromUtf8(raw);

    // date="<digits>" → date="DATE"
    s.replace(QRegularExpression(R"(date="\d+")"), R"(date="DATE")");

    // <file ... subject="..." ...> — ngPost embeds a date string and a (yEnc)
    // article counter ("(1/8)") in the subject. Strip the leading date and
    // keep just the (n/N) counter.
    s.replace(QRegularExpression(R"((<file [^>]*?subject="\[\d+/\d+\] - "?))"),
              R"(\1)"); // no-op; placeholder for additional rules if needed

    // poster="..." → poster="POSTER"
    s.replace(QRegularExpression(R"(poster="[^"]*")"), R"(poster="POSTER")");

    // Subject attribute commonly contains an ngPost version + timestamp +
    // random filename for obfuscation. Replace the whole subject content
    // with SUBJECT for stable comparison.
    s.replace(QRegularExpression(R"(subject="[^"]*")"), R"(subject="SUBJECT")");

    // <segment ...>random@msgid</segment> → <segment ...>MSGID-<N></segment>.
    // Renumber message-ids deterministically per file so we still spot
    // segment count regressions.
    QRegularExpression seg(R"(<segment([^>]*)>([^<]*)</segment>)");
    QRegularExpressionMatchIterator it = seg.globalMatch(s);
    QString out;
    out.reserve(s.size());
    int last = 0;
    int n = 0;
    while (it.hasNext()) {
        auto m = it.next();
        out += QStringView{ s }.mid(last, m.capturedStart() - last);
        out += QStringLiteral("<segment");
        out += m.captured(1);
        out += QStringLiteral(">MSGID-") + QString::number(n) + QStringLiteral("</segment>");
        last = m.capturedEnd();
        ++n;
    }
    out += QStringView{ s }.mid(last);

    return out.toUtf8();
}

bool regenMode()
{
    return qgetenv("NGPOST_REGEN_GOLDENS") == "1";
}

} // namespace

class TestGoldenNzb : public QObject
{
    Q_OBJECT

private:
    QString _bin;

    //! Run the scenario, compare against `<fixtures>/nzb/<goldenName>`.
    void runAndCompare(const QString &goldenName,
                       const QByteArray &input,
                       const QStringList &extraArgs);

private slots:
    void initTestCase();

    //! 4 bytes → 1 segment golden.
    void golden_tiny_1seg();

    //! 4 KiB with -a 1024 → 4 segments golden.
    void golden_4k_4seg();
};

void TestGoldenNzb::initTestCase()
{
    if (!hasPython3())
        QSKIP("python3 not on PATH; integration tests need it");
    _bin = locateNgPostBinary();
    if (_bin.isEmpty())
        QSKIP("ngPost binary not found. Build it first or set NGPOST_BIN.");

    QDir().mkpath(fixturesDir() + QStringLiteral("/nzb"));
}

void TestGoldenNzb::runAndCompare(const QString &goldenName,
                                  const QByteArray &input,
                                  const QStringList &extraArgs)
{
    HomeSandbox sandbox;
    MockNntpServer mock;
    QVERIFY(mock.start());

    const QString inPath = sandbox.rootPath() + QStringLiteral("/in.bin");
    {
        QFile f(inPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(input);
    }
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/out.nzb");

    QStringList args = {
        "-S", QStringLiteral("u:p@@@127.0.0.1:%1:1:nossl").arg(mock.port()),
        "-i", inPath,
        "-o", nzbPath,
        "-g", "alt.binaries.test",
        "--quiet",
        "--disp_progress", "none",
    };
    args += extraArgs;

    QString out;
    const int code = runNgPost(_bin, args, sandbox.rootPath(), out);
    QVERIFY2(code == 0, qPrintable(QStringLiteral("ngPost exit=%1\n%2").arg(code).arg(out)));

    QFile produced(nzbPath);
    QVERIFY(produced.open(QIODevice::ReadOnly));
    const QByteArray normalized = normalizeNzb(produced.readAll());

    const QString goldenPath = fixturesDir() + QStringLiteral("/nzb/") + goldenName;
    if (regenMode() || !QFile::exists(goldenPath)) {
        QFile g(goldenPath);
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write(normalized);
        qWarning() << "Regenerated golden:" << goldenPath;
        return;
    }

    QFile g(goldenPath);
    QVERIFY(g.open(QIODevice::ReadOnly));
    const QByteArray expected = g.readAll();

    if (normalized != expected) {
        // Dump a side-by-side hint to help diagnose.
        const QString actualPath = sandbox.rootPath() + QStringLiteral("/actual.nzb");
        QFile a(actualPath);
        if (a.open(QIODevice::WriteOnly))
            a.write(normalized);
        QFAIL(qPrintable(QStringLiteral(
            "Golden mismatch.\nExpected (%1) vs actual (%2).\n"
            "Re-run with NGPOST_REGEN_GOLDENS=1 if the change is intentional.\n"
            "First 400 bytes of normalized actual:\n%3")
                             .arg(goldenPath)
                             .arg(actualPath)
                             .arg(QString::fromUtf8(normalized.left(400)))));
    }
}

void TestGoldenNzb::golden_tiny_1seg()
{
    runAndCompare(QStringLiteral("tiny_1seg.golden.nzb"),
                  QByteArray("ngPost-golden-tiny\n"),
                  {});
}

void TestGoldenNzb::golden_4k_4seg()
{
    QByteArray buf(4096, 'A');
    runAndCompare(QStringLiteral("4k_4seg.golden.nzb"), buf,
                  { "-a", "1024" });
}

QTEST_MAIN(TestGoldenNzb)
#include "tst_GoldenNzb.moc"
