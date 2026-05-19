//========================================================================
//
// tst_MockNntp.cpp — self-test of the Python mock NNTP server.
//
// Drives the mock with a raw QTcpSocket so failures in real-server tests
// later on can't be ambiguously blamed on either side. If this file is
// red, look at server.py FIRST; if it's green and the higher-level
// tst_PostFlow is red, the bug is in ngPost.
//
//========================================================================

#include <QtTest>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTcpSocket>

#include "MockNntpServer.h"

using ngpost::tests::MockNntpServer;

namespace
{

bool hasPython3()
{
    return !QStandardPaths::findExecutable("python3").isEmpty()
           || !QStandardPaths::findExecutable("python").isEmpty();
}

//! Read one CRLF-terminated reply line from the socket. Returns "" on timeout.
QByteArray readLine(QTcpSocket &s, int timeoutMs = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!s.canReadLine() && t.elapsed() < timeoutMs) {
        if (!s.waitForReadyRead(timeoutMs - int(t.elapsed())))
            return {};
    }
    return s.readLine();
}

void writeLine(QTcpSocket &s, const QByteArray &line)
{
    s.write(line);
    if (!line.endsWith("\r\n"))
        s.write("\r\n");
    s.flush();
}

} // namespace

class TestMockNntp : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    //! Server greets clients with 200 (POSTING OK) within a reasonable delay.
    void greet_200();

    //! QUIT closes the connection cleanly with 205.
    void quit_205();

    //! POST a tiny article, expect 240 and find the .eml in the dump dir.
    void post_dumps_article_to_disk();

    //! Two clients posting simultaneously each get their own dumped article.
    void two_parallel_posts_dump_two_articles();

    //! --fail-auth makes AUTHINFO PASS reply 481.
    void fail_auth_returns_481();

    //! With withTls=true, sslPort() is set and a QSslSocket can connect and
    //! receive the 200 greeting. Self-signed cert verification is bypassed
    //! the same way ngPost does it in production.
    void tls_greet_200();
};

void TestMockNntp::initTestCase()
{
    if (!hasPython3())
        QSKIP("python3 not on PATH; integration tests need it");
}

void TestMockNntp::greet_200()
{
    MockNntpServer mock;
    QVERIFY(mock.start());

    QTcpSocket s;
    s.connectToHost("127.0.0.1", mock.port());
    QVERIFY(s.waitForConnected(3000));

    const QByteArray banner = readLine(s);
    QVERIFY2(banner.startsWith("200 "),
             qPrintable(QStringLiteral("expected 200 greeting, got: %1").arg(QString::fromLatin1(banner))));
}

void TestMockNntp::quit_205()
{
    MockNntpServer mock;
    QVERIFY(mock.start());

    QTcpSocket s;
    s.connectToHost("127.0.0.1", mock.port());
    QVERIFY(s.waitForConnected(3000));
    QVERIFY(!readLine(s).isEmpty()); // banner

    writeLine(s, "QUIT");
    const QByteArray bye = readLine(s);
    QVERIFY2(bye.startsWith("205"),
             qPrintable(QStringLiteral("expected 205, got: %1").arg(QString::fromLatin1(bye))));
    QVERIFY(s.waitForDisconnected(2000));
}

void TestMockNntp::post_dumps_article_to_disk()
{
    MockNntpServer mock;
    QVERIFY(mock.start());

    QTcpSocket s;
    s.connectToHost("127.0.0.1", mock.port());
    QVERIFY(s.waitForConnected(3000));
    QVERIFY(!readLine(s).isEmpty()); // banner

    writeLine(s, "POST");
    const QByteArray ready = readLine(s);
    QVERIFY2(ready.startsWith("340"),
             qPrintable(QStringLiteral("expected 340 POST ready, got: %1").arg(QString::fromLatin1(ready))));

    // Minimal article: headers, blank line, body, '.' terminator.
    writeLine(s, "From: tester@example.invalid");
    writeLine(s, "Newsgroups: alt.binaries.test");
    writeLine(s, "Subject: smoke");
    writeLine(s, "Message-ID: <smoke-1@mock>");
    writeLine(s, "");
    writeLine(s, "hello world");
    writeLine(s, ".");

    const QByteArray ok = readLine(s);
    QVERIFY2(ok.startsWith("240"),
             qPrintable(QStringLiteral("expected 240 POST ok, got: %1").arg(QString::fromLatin1(ok))));

    writeLine(s, "QUIT");
    s.waitForDisconnected(2000);

    const QStringList arts = mock.receivedArticles();
    QVERIFY2(!arts.isEmpty(), "no article dumped");
    QCOMPARE(arts.size(), 1);
    const QString basename = arts.first();
    QVERIFY2(basename.contains("smoke-1_mock"),
             qPrintable(QStringLiteral("dumped article name does not encode the Message-ID: %1").arg(basename)));

    const QByteArray body = mock.readArticle(basename);
    QVERIFY(body.contains("Subject: smoke"));
    QVERIFY(body.contains("hello world"));
}

void TestMockNntp::two_parallel_posts_dump_two_articles()
{
    MockNntpServer mock;
    QVERIFY(mock.start());

    auto postOne = [&](const QByteArray &msgid, const QByteArray &payload) {
        QTcpSocket s;
        s.connectToHost("127.0.0.1", mock.port());
        QVERIFY(s.waitForConnected(3000));
        QVERIFY(!readLine(s).isEmpty()); // banner
        writeLine(s, "POST");
        const QByteArray ready = readLine(s);
        QVERIFY(ready.startsWith("340"));
        writeLine(s, "From: a@b");
        writeLine(s, "Subject: par");
        writeLine(s, "Message-ID: <" + msgid + ">");
        writeLine(s, "");
        writeLine(s, payload);
        writeLine(s, ".");
        const QByteArray ok = readLine(s);
        QVERIFY(ok.startsWith("240"));
        writeLine(s, "QUIT");
        s.waitForDisconnected(2000);
    };
    postOne("p1@mock", "alpha");
    postOne("p2@mock", "beta");

    const QStringList arts = mock.receivedArticles();
    QCOMPARE(arts.size(), 2);
}

void TestMockNntp::fail_auth_returns_481()
{
    MockNntpServer mock;
    QVERIFY(mock.start({ "--fail-auth" }));

    QTcpSocket s;
    s.connectToHost("127.0.0.1", mock.port());
    QVERIFY(s.waitForConnected(3000));
    QVERIFY(!readLine(s).isEmpty()); // banner

    writeLine(s, "AUTHINFO USER alice");
    const QByteArray r1 = readLine(s);
    QVERIFY(r1.startsWith("381"));

    writeLine(s, "AUTHINFO PASS secret");
    const QByteArray r2 = readLine(s);
    QVERIFY2(r2.startsWith("481"),
             qPrintable(QStringLiteral("expected 481 with --fail-auth, got: %1").arg(QString::fromLatin1(r2))));
}

void TestMockNntp::tls_greet_200()
{
    MockNntpServer mock;
    QVERIFY(mock.start({}, /*withTls=*/true));
    QVERIFY2(mock.sslPort() != 0, "TLS port not set");

    QSslSocket s;
    s.setPeerVerifyMode(QSslSocket::VerifyNone);
    s.connectToHostEncrypted("127.0.0.1", mock.sslPort());
    QVERIFY2(s.waitForEncrypted(5000),
             qPrintable(QStringLiteral("TLS handshake failed: %1").arg(s.errorString())));

    const QByteArray banner = readLine(s);
    QVERIFY2(banner.startsWith("200 "),
             qPrintable(QStringLiteral("expected 200 greeting over TLS, got: %1").arg(QString::fromLatin1(banner))));
}

QTEST_MAIN(TestMockNntp)
#include "tst_MockNntp.moc"
