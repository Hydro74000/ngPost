// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_PostFlow.cpp — end-to-end posting with the mock NNTP server.
//
// Spawns the built ngPost CLI binary against the Python mock NNTP server
// and verifies:
//   * exit code 0
//   * the output .nzb file is valid XML and references the right number
//     of segments
//   * the mock dumped one .eml per segment with the expected headers
//
// This is the foundation that Phase 5 VPN E2E builds on: same flow, but
// the mock is reached via a WireGuard / OpenVPN tunnel instead of
// loopback, and we additionally assert on the peer IP recorded in the
// server log.
//
//========================================================================

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QXmlStreamReader>

#include "history/PostHistoryStore.h"
#include "MockNntpServer.h"
#include "TestEnv.h"

using ngpost::tests::HomeSandbox;
using ngpost::tests::MockNntpServer;
using ngpost::tests::locateNgPostBinary;

namespace
{

bool hasPython3()
{
    return !QStandardPaths::findExecutable("python3").isEmpty()
           || !QStandardPaths::findExecutable("python").isEmpty();
}

//! Run ngPost as a subprocess inside the given sandboxed HOME and return its
//! exit code. stdout/stderr are merged and stored in `output`.
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

//! Walk the .nzb XML and count <segment> elements (one per article).
int countSegmentsInNzb(const QByteArray &nzbBytes, QString *firstSubject = nullptr)
{
    QXmlStreamReader r(nzbBytes);
    int segments = 0;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("segment"))
            ++segments;
        if (firstSubject && r.isStartElement()
            && r.name() == QLatin1String("file")
            && firstSubject->isEmpty()) {
            *firstSubject = r.attributes().value(QLatin1String("subject")).toString();
        }
    }
    return segments;
}

struct NzbFileEntry
{
    QString subject;
    QStringList segments;
};

QList<NzbFileEntry> collectNzbFiles(const QByteArray &nzbBytes)
{
    QList<NzbFileEntry> files;
    QXmlStreamReader r(nzbBytes);
    int current = -1;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name() == QLatin1String("file")) {
            NzbFileEntry entry;
            entry.subject = r.attributes().value(QLatin1String("subject")).toString();
            files << entry;
            current = files.size() - 1;
        } else if (r.isStartElement() && r.name() == QLatin1String("segment")
                   && current >= 0) {
            files[current].segments << r.readElementText();
        }
    }
    return files;
}

} // namespace

class TestPostFlow : public QObject
{
    Q_OBJECT

private:
    QString _bin;

private slots:
    void initTestCase();

    //! Post a 4-byte file through the mock. Exactly one article should be
    //! created and dumped, and the NZB should reference one <segment>.
    void post_tiny_file_one_segment();

    //! Post a file larger than the article size; the segment count must
    //! match ceil(filesize / articleSize) and every article ends up in the
    //! dump dir.
    void post_split_file_matches_segment_count();

    //! Post over SSL using the mock's TLS port. ngPost rejects the
    //! mock's self-signed cert (no `--insecure-ssl` flag exists yet — this
    //! is current production behaviour). The test asserts the cert rejection
    //! is reported in the output, proving the QSslSocket path is wired up
    //! and the failure mode is what users would see if they pointed ngPost
    //! at a mis-configured NNTP provider.
    void ssl_rejects_self_signed_cert();

    //! With `-x`, the article Subject and From headers must NOT include the
    //! plaintext filename. Posts a file whose name is a fingerprint string
    //! we then grep for.
    void obfuscation_hides_filename_in_article_headers();

    //! The mock drops the connection after N bytes; ngPost must not crash or
    //! hang regardless of whether retry eventually succeeds.
    void retry_on_dropped_connection();

    //! Resuming only the second file of a multi-file post must preserve the
    //! original history row/ordinal instead of overwriting file #1.
    void resume_history_post_preserves_original_file_ordinals();

    //! Config values are split on the first '=' only: server passwords,
    //! NZB paths and RAR passwords may legally contain '='.
    void config_values_keep_equals();
};

void TestPostFlow::initTestCase()
{
    if (!hasPython3())
        QSKIP("python3 not on PATH; integration tests need it");
    _bin = locateNgPostBinary();
    if (_bin.isEmpty())
        QSKIP("ngPost binary not found. Build it first or set NGPOST_BIN.");
    qInfo() << "Using binary:" << _bin;
}

void TestPostFlow::post_tiny_file_one_segment()
{
    HomeSandbox sandbox;
    MockNntpServer mock;
    QVERIFY(mock.start());

    // Tiny input — 4 bytes, well under the default article size, so exactly
    // one segment is expected.
    const QString inPath = sandbox.rootPath() + QStringLiteral("/tiny.bin");
    {
        QFile in(inPath);
        QVERIFY(in.open(QIODevice::WriteOnly));
        in.write("hey!");
    }
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/tiny.nzb");

    const QString srv = QStringLiteral("u:p@@@127.0.0.1:%1:1:nossl").arg(mock.port());
    const QStringList args = {
        "-S", srv,
        "-i", inPath,
        "-o", nzbPath,
        "-g", "alt.binaries.test",
        "--quiet",
        "--disp_progress", "none",
    };

    QString out;
    const int exitCode = runNgPost(_bin, args, sandbox.rootPath(), out);
    QVERIFY2(exitCode == 0,
             qPrintable(QStringLiteral("ngPost exit=%1, output:\n%2").arg(exitCode).arg(out)));

    QVERIFY2(QFile::exists(nzbPath),
             qPrintable(QStringLiteral("NZB not written; output:\n%1").arg(out)));

    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::ReadOnly));
    const QByteArray nzbContent = nzb.readAll();
    QString subject;
    const int segs = countSegmentsInNzb(nzbContent, &subject);
    QCOMPARE(segs, 1);
    QVERIFY2(subject.contains("tiny.bin"),
             qPrintable(QStringLiteral("NZB <file subject=...> did not mention the input file. Subject was: %1").arg(subject)));

    const QStringList arts = mock.receivedArticles();
    QCOMPARE(arts.size(), 1);
    const QByteArray article = mock.readArticle(arts.first());
    QVERIFY(article.contains("Newsgroups: alt.binaries.test"));
    QVERIFY2(article.contains("ybegin"),
             "article body should contain a yEnc =ybegin header line");
}

void TestPostFlow::post_split_file_matches_segment_count()
{
    HomeSandbox sandbox;
    MockNntpServer mock;
    QVERIFY(mock.start());

    // 8 KiB input + 1 KiB article size → 8 segments.
    const QString inPath = sandbox.rootPath() + QStringLiteral("/split.bin");
    {
        QFile in(inPath);
        QVERIFY(in.open(QIODevice::WriteOnly));
        QByteArray buf(8192, 'x');
        in.write(buf);
    }
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/split.nzb");

    const QString srv = QStringLiteral("u:p@@@127.0.0.1:%1:2:nossl").arg(mock.port());
    const QStringList args = {
        "-S", srv,
        "-i", inPath,
        "-o", nzbPath,
        "-g", "alt.binaries.test",
        "-a", "1024",
        "--quiet",
        "--disp_progress", "none",
    };

    QString out;
    const int exitCode = runNgPost(_bin, args, sandbox.rootPath(), out);
    QVERIFY2(exitCode == 0,
             qPrintable(QStringLiteral("ngPost exit=%1, output:\n%2").arg(exitCode).arg(out)));

    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::ReadOnly));
    const int segs = countSegmentsInNzb(nzb.readAll());
    QCOMPARE(segs, 8);

    const QStringList arts = mock.receivedArticles();
    QCOMPARE(arts.size(), 8);
}

void TestPostFlow::ssl_rejects_self_signed_cert()
{
    HomeSandbox sandbox;
    MockNntpServer mock;
    QVERIFY(mock.start({}, /*withTls=*/true));

    const QString inPath = sandbox.rootPath() + QStringLiteral("/tls.bin");
    {
        QFile in(inPath);
        QVERIFY(in.open(QIODevice::WriteOnly));
        in.write("over-tls!");
    }
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/tls.nzb");

    // ":ssl" suffix in the combined server spec.
    const QString srv = QStringLiteral("u:p@@@127.0.0.1:%1:1:ssl").arg(mock.sslPort());
    const QStringList args = {
        "-S", srv,
        "-i", inPath,
        "-o", nzbPath,
        "-g", "alt.binaries.test",
        "--disp_progress", "none",
    };

    QString out;
    // Cap the run at 8 seconds. ngPost will keep retrying with --no-quiet
    // so we'll see the SSL error within a second or two, then loop.
    const int exitCode = runNgPost(_bin, args, sandbox.rootPath(), out, /*timeoutMs=*/8000);

    // ngPost may either exit non-zero on its own or be killed by our
    // timeout (-2). Both are fine — we just need the SSL error message in
    // the captured output.
    QVERIFY2(out.contains("self-signed", Qt::CaseInsensitive)
                 || out.contains("certificate", Qt::CaseInsensitive),
             qPrintable(QStringLiteral("expected an SSL/cert error in output (exit=%1):\n%2").arg(exitCode).arg(out)));
}

void TestPostFlow::obfuscation_hides_filename_in_article_headers()
{
    HomeSandbox sandbox;
    MockNntpServer mock;
    QVERIFY(mock.start());

    // Distinctive filename we can grep for.
    const QString secret = QStringLiteral("MY-SECRET-FILE-1234.bin");
    const QString inPath = sandbox.rootPath() + "/" + secret;
    {
        QFile in(inPath);
        QVERIFY(in.open(QIODevice::WriteOnly));
        in.write("don't-leak-me");
    }
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/obf.nzb");

    const QString srv = QStringLiteral("u:p@@@127.0.0.1:%1:1:nossl").arg(mock.port());
    const QStringList args = {
        "-S", srv,
        "-i", inPath,
        "-o", nzbPath,
        "-g", "alt.binaries.test",
        "-x",                // obfuscate article headers
        "--quiet",
        "--disp_progress", "none",
    };

    QString out;
    const int exitCode = runNgPost(_bin, args, sandbox.rootPath(), out);
    QVERIFY2(exitCode == 0,
             qPrintable(QStringLiteral("ngPost exit=%1, output:\n%2").arg(exitCode).arg(out)));

    const QStringList arts = mock.receivedArticles();
    QCOMPARE(arts.size(), 1);
    const QByteArray article = mock.readArticle(arts.first());

    // Look at the headers — the body's =ybegin line legitimately contains the
    // (obfuscated) yEnc name; we care that the original plaintext filename
    // doesn't leak into Subject/From.
    const int blank = article.indexOf("\r\n\r\n");
    const QByteArray headers = blank >= 0 ? article.left(blank) : article;
    QVERIFY2(!headers.contains(secret.toUtf8()),
             qPrintable(QStringLiteral("obfuscation leaked filename into article headers:\n%1")
                            .arg(QString::fromLatin1(headers))));
}

void TestPostFlow::retry_on_dropped_connection()
{
    HomeSandbox sandbox;
    MockNntpServer mock;
    // Drop the first connection after ~80 bytes (enough to greet+auth and
    // start an article transfer, then break). ngPost should reconnect and
    // succeed on retry.
    QVERIFY(mock.start({ "--drop-after-bytes", "80" }));

    const QString inPath = sandbox.rootPath() + QStringLiteral("/retry.bin");
    {
        QFile in(inPath);
        QVERIFY(in.open(QIODevice::WriteOnly));
        in.write("retry-me");
    }
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/retry.nzb");

    const QString srv = QStringLiteral("u:p@@@127.0.0.1:%1:1:nossl").arg(mock.port());
    QString out;
    const int code = runNgPost(_bin, {
        "-S", srv, "-i", inPath, "-o", nzbPath, "-g", "alt.binaries.test",
        "-r", "5",
        "--quiet", "--disp_progress", "none",
    }, sandbox.rootPath(), out, /*timeoutMs=*/60000);
    // We don't require success — the goal is to demonstrate that ngPost
    // doesn't hang or crash. Acceptable outcomes: (a) ngPost retried enough
    // times to complete (exit 0); (b) it gave up after -r retries with a
    // non-zero exit. Either way we want a NON-hanging finish.
    QVERIFY2(code != -1 && code != -2,
             qPrintable(QStringLiteral("ngPost crashed or timed out (code=%1):\n%2").arg(code).arg(out)));
}

void TestPostFlow::resume_history_post_preserves_original_file_ordinals()
{
    HomeSandbox sandbox;
    MockNntpServer mock;
    QVERIFY(mock.start());

    const QString firstPath = sandbox.rootPath() + QStringLiteral("/first.bin");
    const QString secondPath = sandbox.rootPath() + QStringLiteral("/second.bin");
    for (const QString &path : { firstPath, secondPath }) {
        QFile source(path);
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(QByteArray(8, 'x'));
    }

    const QString dbPath = sandbox.rootPath() + QStringLiteral("/history.sqlite");
    const QString nzbPath = sandbox.rootPath() + QStringLiteral("/resume.nzb");
    PostHistoryStore store(dbPath, true);
    QString err;
    QVERIFY2(store.initialize(&err), qPrintable(err));

    PostHistoryStore::PostRecord post;
    post.nzbName = QStringLiteral("resume.nzb");
    post.nzbPath = nzbPath;
    post.from = QStringLiteral("poster@example.invalid");
    post.groups = { QStringLiteral("alt.binaries.test") };
    const qint64 postId = store.createPost(post, &err);
    QVERIFY2(postId > 0, qPrintable(err));

    auto addFile = [&](int ordinal, const QString &path) {
        const QFileInfo sourceInfo(path);
        PostHistoryStore::FileRecord file;
        file.postId = postId;
        file.ordinal = ordinal;
        file.originalPath = sourceInfo.absoluteFilePath();
        file.postedName = sourceInfo.fileName();
        file.sizeBytes = sourceInfo.size();
        file.mtimeEpoch = sourceInfo.lastModified().toSecsSinceEpoch();
        file.totalArticles = 2;
        file.groups = post.groups;
        return store.upsertFile(file, &err);
    };
    const qint64 firstId = addFile(1, firstPath);
    const qint64 secondId = addFile(2, secondPath);
    QVERIFY2(firstId > 0, qPrintable(err));
    QVERIFY2(secondId > 0, qPrintable(err));

    auto addArticle = [&](qint64 fileId, int part, qint64 pos, const QString &msgId,
                          bool posted) {
        PostHistoryStore::ArticleRecord article;
        article.fileId = fileId;
        article.part = part;
        article.pos = pos;
        article.bytes = 4;
        QVERIFY2(store.upsertArticle(article, &err), qPrintable(err));
        QVERIFY2(store.markArticlePosting(fileId, part, msgId, 1, &err), qPrintable(err));
        if (posted)
            QVERIFY2(store.markArticlePosted(fileId, part, msgId, &err), qPrintable(err));
        else
            QVERIFY2(store.markArticleFailed(fileId, part, msgId,
                                             QStringLiteral("server rejected"), &err),
                     qPrintable(err));
    };
    addArticle(firstId, 1, 0, QStringLiteral("first-1@ngpost"), true);
    addArticle(firstId, 2, 4, QStringLiteral("first-2@ngpost"), true);
    addArticle(secondId, 1, 0, QStringLiteral("second-1@ngpost"), true);
    addArticle(secondId, 2, 4, QStringLiteral("second-old@ngpost"), false);
    QVERIFY2(store.updateFileStatus(firstId, QStringLiteral("posted"), &err), qPrintable(err));
    QVERIFY2(store.updateFileStatus(secondId, QStringLiteral("partial"), &err), qPrintable(err));
    QVERIFY2(store.updatePostStatus(postId, QStringLiteral("partial"), 2, 4, 1, 16,
                                    QStringLiteral("1 KB/s"), &err),
             qPrintable(err));

    const QString srv = QStringLiteral("u:p@@@127.0.0.1:%1:1:nossl").arg(mock.port());
    QString out;
    const int code = runNgPost(_bin, {
        "-S", srv,
        "--resume-post", QString::number(postId),
        "--yes",
        "--post_db", dbPath,
        "-a", "4",
        "--quiet",
        "--disp_progress", "none",
    }, sandbox.rootPath(), out, /*timeoutMs=*/60000);
    QVERIFY2(code == 0,
             qPrintable(QStringLiteral("ngPost exit=%1, output:\n%2").arg(code).arg(out)));

    QCOMPARE(mock.receivedArticles().size(), 1);

    QFile nzb(nzbPath);
    QVERIFY2(nzb.open(QIODevice::ReadOnly),
             qPrintable(QStringLiteral("NZB was not regenerated at %1").arg(nzbPath)));
    const QList<NzbFileEntry> files = collectNzbFiles(nzb.readAll());
    QCOMPARE(files.size(), 2);
    QVERIFY2(files.at(0).subject.contains(QStringLiteral("[1/2]")),
             qPrintable(files.at(0).subject));
    QVERIFY2(files.at(1).subject.contains(QStringLiteral("[2/2]")),
             qPrintable(files.at(1).subject));
    QCOMPARE(files.at(0).segments.size(), 2);
    QCOMPARE(files.at(1).segments.size(), 2);
    QVERIFY(files.at(0).segments.contains(QStringLiteral("first-1@ngpost")));
    QVERIFY(files.at(0).segments.contains(QStringLiteral("first-2@ngpost")));
    QVERIFY(files.at(1).segments.contains(QStringLiteral("second-1@ngpost")));
    QVERIFY(!files.at(1).segments.contains(QStringLiteral("second-old@ngpost")));
}

void TestPostFlow::config_values_keep_equals()
{
    HomeSandbox sandbox;
    MockNntpServer mock;
    QVERIFY(mock.start({ "--require-auth", "cfg-user:p=a=s=s" }));

    const QString inPath = sandbox.rootPath() + QStringLiteral("/cfg.bin");
    {
        QFile in(inPath);
        QVERIFY(in.open(QIODevice::WriteOnly));
        in.write("config-equals");
    }

    const QString nzbDir = sandbox.rootPath() + QStringLiteral("/nzb=out");
    QVERIFY(QDir().mkpath(nzbDir));
    const QString confPath = sandbox.rootPath() + QStringLiteral("/ngpost=cfg.conf");
    {
        QFile conf(confPath);
        QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream s(&conf);
        s << "nzbPath = " << nzbDir << "\n"
          << "RAR_PATH = " << sandbox.rootPath() << "/rar=tool\n"
          << "RAR_PASS = pass=with=equals\n"
          << "PACK = compress,gen_pass\n"
          << "LENGTH_PASS = 19\n"
          << "[server]\n"
          << "host = 127.0.0.1\n"
          << "port = " << mock.port() << "\n"
          << "ssl = false\n"
          << "user = cfg-user\n"
          << "pass = p=a=s=s\n"
          << "connection = 1\n"
          << "enabled = true\n"
          << "nzbcheck = false\n";
    }

    QString out;
    const int code = runNgPost(_bin, {
        "-c", confPath,
        "-i", inPath,
        "-g", "alt.binaries.test",
        "--quiet",
        "--disp_progress", "none",
    }, sandbox.rootPath(), out);
    QVERIFY2(code == 0,
             qPrintable(QStringLiteral("ngPost exit=%1, output:\n%2").arg(code).arg(out)));

    const QString nzbPath = nzbDir + QStringLiteral("/cfg.nzb");
    QVERIFY2(QFile::exists(nzbPath),
             qPrintable(QStringLiteral("NZB not written at expected config path: %1\n%2").arg(nzbPath, out)));

    QFile nzb(nzbPath);
    QVERIFY(nzb.open(QIODevice::ReadOnly));
    const QByteArray nzbContent = nzb.readAll();
    QVERIFY2(nzbContent.contains("<meta type=\"password\">pass=with=equals</meta>"),
             qPrintable(QStringLiteral("RAR_PASS with '=' was not preserved in NZB:\n%1")
                            .arg(QString::fromUtf8(nzbContent))));
    QCOMPARE(mock.receivedArticles().size(), 1);
}

QTEST_MAIN(TestPostFlow)
#include "tst_PostFlow.moc"
