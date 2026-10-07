// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_NntpFile.cpp — coverage of NntpFile::articleCount, the arithmetic that
// decides how many articles every posted file is cut into, and of Nntp's
// command serialiser, which decides what may be put on the wire at all.
//
// It ran only inside a full posting run until now. An off-by-one here does
// not fail loudly: it silently posts one article too few (truncating the
// file on Usenet) or one too many (a trailing empty article), and the nzb
// records whichever it did as if it were correct.
//
//========================================================================

#include <QtTest>

#include "nntp/Nntp.h"
#include "nntp/NntpFile.h"

class TestNntpFile : public QObject
{
    Q_OBJECT

private slots:
    // ---- Nntp command serialisation ----------------------------------
    //
    // An nzb is an untrusted document, and a message-id used to be pasted
    // into "stat <id>" as it came. An XML entity encoding a CR/LF then added
    // a whole second command to an already authenticated session.

    //! What a well-formed message-id looks like, and everything that would
    //! end the command line early or split its argument.
    void message_id_validation_data();
    void message_id_validation();

    //! The serialiser is the gate: a refused id yields nothing to write,
    //! rather than a line the caller might send anyway.
    void stat_command_is_one_line_or_nothing();

    //! Credentials are configured by hand, so they get the same treatment at
    //! their own point of emission.
    void authinfo_refuses_credentials_carrying_a_line_break();

    //! Header values come from file names, which on Unix may hold a CR or an
    //! LF; one of those would forge the rest of the header block.
    void header_values_lose_their_control_characters();

    // ---- NntpFile::articleCount --------------------------------------

    //! A file that divides evenly must not gain a spare empty article.
    void article_count_exact_multiple();

    //! A remainder of any size is one more article, never a fraction.
    void article_count_rounds_the_remainder_up();

    //! Smaller than one article is still one article, not zero: the file has
    //! to be posted.
    void article_count_smaller_than_one_article();

    //! Zero, negative and nonsensical inputs yield zero articles rather than
    //! one empty one. An empty or vanished file must not be posted as a
    //! zero-length article that no client can reassemble.
    void article_count_rejects_non_positive_input();

    //! The count is a uint but the size is a qint64: a large file must not
    //! wrap around. 8 TiB in 700 KiB articles is beyond 32 bits of *bytes*,
    //! which is exactly where a qint32 intermediate would break.
    void article_count_survives_a_very_large_file();
};

void TestNntpFile::article_count_exact_multiple()
{
    QCOMPARE(NntpFile::articleCount(716800, 716800), 1u);
    QCOMPARE(NntpFile::articleCount(716800 * 4, 716800), 4u);
    QCOMPARE(NntpFile::articleCount(1000, 10), 100u);
}

void TestNntpFile::article_count_rounds_the_remainder_up()
{
    QCOMPARE(NntpFile::articleCount(716801, 716800), 2u);
    QCOMPARE(NntpFile::articleCount(716800 * 4 + 1, 716800), 5u);
    // One byte short of the next boundary is still the same article count.
    QCOMPARE(NntpFile::articleCount(716800 * 5 - 1, 716800), 5u);
}

void TestNntpFile::article_count_smaller_than_one_article()
{
    QCOMPARE(NntpFile::articleCount(1, 716800), 1u);
    QCOMPARE(NntpFile::articleCount(716799, 716800), 1u);
}

void TestNntpFile::article_count_rejects_non_positive_input()
{
    QCOMPARE(NntpFile::articleCount(0, 716800), 0u);
    QCOMPARE(NntpFile::articleCount(-1, 716800), 0u);
    // An article size of zero would otherwise divide by zero.
    QCOMPARE(NntpFile::articleCount(716800, 0), 0u);
    QCOMPARE(NntpFile::articleCount(716800, -1), 0u);
    QCOMPARE(NntpFile::articleCount(0, 0), 0u);
}

void TestNntpFile::article_count_survives_a_very_large_file()
{
    const qint64 articleSize = 716800;               // the shipped default
    const qint64 eightTiB    = Q_INT64_C(8) * 1024 * 1024 * 1024 * 1024;
    QVERIFY(eightTiB > Q_INT64_C(0xFFFFFFFF));       // past 32 bits of bytes

    const uint expected = static_cast<uint>(eightTiB / articleSize
                                            + (eightTiB % articleSize ? 1 : 0));
    QCOMPARE(NntpFile::articleCount(eightTiB, articleSize), expected);
    QVERIFY2(NntpFile::articleCount(eightTiB, articleSize) > 12000000u,
             "a truncating intermediate would report far too few articles");
}

// ---- Nntp command serialisation --------------------------------------
//
// An nzb is an untrusted document, and a message-id used to be pasted into
// "stat <id>" as it came. An XML entity encoding a CR/LF then added a whole
// second command to an already authenticated session.

void TestNntpFile::message_id_validation_data()
{
    QTest::addColumn<QString>("messageId");
    QTest::addColumn<bool>("valid");

    QTest::newRow("ordinary")        << QStringLiteral("<abc123@ngpost.test>")   << true;
    QTest::newRow("no at sign")      << QStringLiteral("<justanid>")             << true;
    QTest::newRow("punctuation")     << QStringLiteral("<a.b-c_d+e%f@x.y>")      << true;
    QTest::newRow("empty")           << QString()                               << false;
    QTest::newRow("brackets only")   << QStringLiteral("<>")                    << false;
    QTest::newRow("unbracketed")     << QStringLiteral("abc@ngpost.test")        << false;
    QTest::newRow("no closing")      << QStringLiteral("<abc@ngpost.test")       << false;
    QTest::newRow("CR LF injection") << QStringLiteral("<a@b\r\nSTAT <c@d>>")    << false;
    QTest::newRow("bare LF")         << QStringLiteral("<a@b\nc>")              << false;
    QTest::newRow("bare CR")         << QStringLiteral("<a@b\rc>")              << false;
    QTest::newRow("NUL")             << QStringLiteral("<a@b\0c>")              << false;
    QTest::newRow("space")           << QStringLiteral("<a@b c>")               << false;
    QTest::newRow("tab")             << QStringLiteral("<a@b\tc>")              << false;
    QTest::newRow("inner open")      << QStringLiteral("<a<b@c>")               << false;
    QTest::newRow("inner close")     << QStringLiteral("<a>b@c>")               << false;
    QTest::newRow("DEL")
            << (QStringLiteral("<a@b") + QChar(0x7F) + QStringLiteral("c>")) << false;
    QTest::newRow("non ascii")
            << (QStringLiteral("<a@b") + QChar(0x00E9) + QStringLiteral("c>")) << false;
    QTest::newRow("too long")
            << (QStringLiteral("<") + QString(300, QLatin1Char('a')) + QStringLiteral(">"))
            << false;
    QTest::newRow("at the cap")
            << (QStringLiteral("<") + QString(Nntp::MAX_MSG_ID_LEN - 2, QLatin1Char('a'))
                + QStringLiteral(">"))
            << true;
}

void TestNntpFile::message_id_validation()
{
    QFETCH(QString, messageId);
    QFETCH(bool, valid);

    QCOMPARE(Nntp::isValidMessageId(messageId), valid);
}

void TestNntpFile::stat_command_is_one_line_or_nothing()
{
    QByteArray const good = Nntp::statCommand(QStringLiteral("<abc@ngpost.test>"));
    QCOMPARE(good, QByteArray("stat <abc@ngpost.test>\r\n"));
    QCOMPARE(good.count('\n'), 1);

    // The exact shape the hostile nzb fixture carries.
    QVERIFY(Nntp::statCommand(QStringLiteral("<evil@x\r\nSTAT <injected@y>>")).isEmpty());
    QVERIFY(Nntp::statCommand(QString()).isEmpty());
}

void TestNntpFile::authinfo_refuses_credentials_carrying_a_line_break()
{
    QCOMPARE(Nntp::authInfoUser("alice"), QByteArray("authinfo user alice\r\n"));
    QCOMPARE(Nntp::authInfoPass("s3cret"), QByteArray("authinfo pass s3cret\r\n"));

    QVERIFY(Nntp::authInfoUser("alice\r\nmode reader").isEmpty());
    QVERIFY(Nntp::authInfoPass("s3cret\npost").isEmpty());
    QVERIFY(Nntp::authInfoUser(std::string("ali\0ce", 6)).isEmpty());

    // An empty credential is not a refusal: it is a command with no argument,
    // and the server is the one that decides what to make of it.
    QCOMPARE(Nntp::authInfoUser(""), QByteArray("authinfo user \r\n"));
}

void TestNntpFile::header_values_lose_their_control_characters()
{
    QCOMPARE(Nntp::sanitizedHeaderValue("holiday.mkv"), std::string("holiday.mkv"));
    QCOMPARE(Nntp::sanitizedHeaderValue("evil\r\nSubject: forged"),
             std::string("evil__Subject: forged"));
    QCOMPARE(Nntp::sanitizedHeaderValue(std::string("a\0b", 3)), std::string("a_b"));

    // UTF-8 is what the headers already carry: folding it would rename files.
    QCOMPARE(Nntp::sanitizedHeaderValue("\xC3\xA9" "t" "\xC3\xA9" ".mkv"),
             std::string("\xC3\xA9" "t" "\xC3\xA9" ".mkv"));
}

QTEST_APPLESS_MAIN(TestNntpFile)
#include "tst_NntpFile.moc"
