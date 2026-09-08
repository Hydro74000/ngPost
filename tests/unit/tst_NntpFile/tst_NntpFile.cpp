// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_NntpFile.cpp — coverage of NntpFile::articleCount, the arithmetic that
// decides how many articles every posted file is cut into.
//
// It ran only inside a full posting run until now. An off-by-one here does
// not fail loudly: it silently posts one article too few (truncating the
// file on Usenet) or one too many (a trailing empty article), and the nzb
// records whichever it did as if it were correct.
//
//========================================================================

#include <QtTest>

#include "nntp/NntpFile.h"

class TestNntpFile : public QObject
{
    Q_OBJECT

private slots:
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

QTEST_APPLESS_MAIN(TestNntpFile)
#include "tst_NntpFile.moc"
