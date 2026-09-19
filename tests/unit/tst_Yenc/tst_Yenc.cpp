// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// tst_Yenc.cpp — coverage of Yenc::encode (yEnc body encoding).
//
//========================================================================

#include <QtTest>

#include <vector>

#include "nntp/NntpArticle.h"
#include "utils/Yenc.h"

class TestYenc : public QObject
{
    Q_OBJECT

private slots:
    //! Encoding an empty buffer should still write the trailing NUL and
    //! return a length of 1 (the NUL itself). CRC32 of nothing is 0.
    void encode_empty_input();

    //! Each byte of 0x00, 0x0A (LF), 0x0D (CR), 0x3D ('=') becomes an escape
    //! pair "= <byte+64+42>" so the wire format never carries these literal
    //! bytes. Verify that escape pairs are emitted at the right offsets.
    void encode_critical_bytes_are_escaped();

    //! After 128 raw output columns the encoder must emit CRLF and reset the
    //! column counter. Feed 256 zero bytes (each becomes an escape pair, so
    //! 2 output columns per input byte → 256 output cols → 2 CRLFs).
    void encode_wraps_at_128_columns();

    //! Yenc::encode also computes a zlib-compatible CRC32 (Ethernet/PNG poly,
    //! reflected). For ASCII "Hello, ngPost!" the canonical value is fixed.
    //! Computed once with `python3 -c "import zlib; print(hex(zlib.crc32(b'Hello, ngPost!')))"`.
    void encode_crc32_matches_zlib();

    //! Stress test on 1 MiB of random data — must not write past the buffer
    //! (`dataSize*2 + 2` extra bytes for worst-case escape + CRLF + NUL).
    void encode_large_buffer_no_overflow();

    //! NntpArticle::yEncWorstCaseSize() is what sizes the single allocation
    //! yEncBody() builds the whole body in, so the encoder must never write
    //! more than it promises — including on inputs chosen to escape every
    //! single byte, which is where the old 4x over-allocation was hiding.
    void worst_case_bound_is_never_exceeded();
    void worst_case_bound_is_never_exceeded_data();

    //! Three bytes escape depending on WHERE in the line they land, which is
    //! the half of the escape rules the cases above cannot reach:
    //!   - SPACE and TAB at either end of a line, because anything that trims
    //!     trailing whitespace in transit drops the byte and the decoded file
    //!     is a byte short with a failing CRC32;
    //!   - '.' at column 0 only, which is NNTP dot-stuffing.
    //! The end-of-line half of the whitespace rule was dead code
    //! (`column - 1 == maxwidth`, needing an unreachable column == 129), so
    //! ~44 raw line-final blanks went out per 700 KB article.
    void column_sensitive_bytes_are_escaped();
    void column_sensitive_bytes_are_escaped_data();

    //! The last byte of an article ends a line wherever its column falls: the
    //! body is followed by CRLF and the =yend trailer. A SPACE or TAB there is
    //! as exposed to trailing-whitespace trimming as one on the last column,
    //! and it went out raw on ~2 articles in 256.
    void last_input_byte_blank_is_escaped();
    void last_input_byte_blank_is_escaped_data();
};

void TestYenc::encode_empty_input()
{
    const char src[1] = { '\0' };
    uchar      dst[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    quint32    crc    = 0xDEADBEEF;

    qint64 n = Yenc::encode(src, /*dataSize*/ 0, dst, crc);

    QCOMPARE(n, qint64(1));   // just the terminating NUL
    QCOMPARE(dst[0], uchar(0));
    QCOMPARE(crc, quint32(0)); // CRC32 of empty is 0
}

void TestYenc::encode_critical_bytes_are_escaped()
{
    // After + 42 each input byte produces:
    //   0x00 (NUL) → 0x2A ('*') — but +42 = 0x2A is not in escape set
    //   0xD6 (some byte where +42 wraps to 0x00) is the actual NUL we need to escape.
    // Easiest: feed an input whose +42 mod 256 equals each of {0x00, 0x0A, 0x0D, 0x3D}
    //   target 0x00 → input 0xD6  (256 - 42 = 214 = 0xD6)
    //   target 0x0A → input 0xE0  (256 + 10 - 42 = 224 = 0xE0)
    //   target 0x0D → input 0xE3
    //   target 0x3D → input 0x13 ('=' is 61 = 0x3D, so input 61 - 42 = 19 = 0x13)
    const char src[]  = { '\xD6', '\xE0', '\xE3', '\x13' };
    uchar      dst[32] = {};
    quint32    crc    = 0;

    qint64 n = Yenc::encode(src, /*dataSize*/ 4, dst, crc);

    // Each of the four inputs should produce a 2-byte escape pair: '=' then
    // (target + 64). So output is 8 bytes + trailing NUL = 9.
    QCOMPARE(n, qint64(9));
    QCOMPARE(dst[0], uchar('='));
    QCOMPARE(dst[1], uchar(0x00 + 64));
    QCOMPARE(dst[2], uchar('='));
    QCOMPARE(dst[3], uchar(0x0A + 64));
    QCOMPARE(dst[4], uchar('='));
    QCOMPARE(dst[5], uchar(0x0D + 64));
    QCOMPARE(dst[6], uchar('='));
    QCOMPARE(dst[7], uchar(0x3D + 64));
    QCOMPARE(dst[8], uchar(0)); // NUL terminator
}

void TestYenc::encode_wraps_at_128_columns()
{
    // 0xD6 + 42 = 0x00 → must escape, so each input byte produces 2 output
    // bytes. We need 64 input bytes to hit 128 output columns and trigger one
    // CRLF.
    constexpr qint64 N = 64;
    std::vector<char> src(N, '\xD6');
    std::vector<uchar> dst(8 * N, 0);
    quint32 crc = 0;

    qint64 n = Yenc::encode(src.data(), N, dst.data(), crc);

    // Each input: '=' + escaped-byte = 2 bytes. After 128 column count we get
    // a CRLF (2 bytes). Then the trailing NUL = 1.
    // Total = 64*2 + 2 + 1 = 131
    QCOMPARE(n, qint64(131));
    QCOMPARE(dst[128], uchar('\r'));
    QCOMPARE(dst[129], uchar('\n'));
    QCOMPARE(dst[130], uchar(0));
}

void TestYenc::encode_crc32_matches_zlib()
{
    const QByteArray input("Hello, ngPost!");
    std::vector<uchar> dst(input.size() * 4 + 16, 0);
    quint32 crc = 0;

    Yenc::encode(input.constData(), input.size(), dst.data(), crc);

    // zlib.crc32(b"Hello, ngPost!") == 0xBEEDCA46
    // (computed with `python3 -c "import zlib; print(hex(zlib.crc32(b'Hello, ngPost!')))"`)
    QCOMPARE(crc, quint32(0xBEEDCA46));
}

void TestYenc::encode_large_buffer_no_overflow()
{
    constexpr qint64 N = 1024 * 1024; // 1 MiB
    std::vector<char> src(N);
    // Mix of values so we hit some escape branches and some plain ones.
    for (qint64 i = 0; i < N; ++i)
        src[static_cast<size_t>(i)] = static_cast<char>((i * 7) & 0xFF);

    // Worst case: every byte escapes (2 output bytes), plus a CRLF every 128
    // columns. Allocate dataSize*4 + 16 to be safe.
    std::vector<uchar> dst(N * 4 + 16, 0xAB);
    quint32 crc = 0;

    qint64 n = Yenc::encode(src.data(), N, dst.data(), crc);

    QVERIFY2(n > 0, "encode returned non-positive length");
    QVERIFY2(n < qint64(N * 4 + 16), "encode wrote past allocated buffer");
    QCOMPARE(dst[static_cast<size_t>(n) - 1], uchar(0)); // trailing NUL
    QVERIFY(crc != 0);
}

void TestYenc::worst_case_bound_is_never_exceeded_data()
{
    QTest::addColumn<QByteArray>("input");

    // Each of these bytes becomes an escape pair after the +42 shift, so a
    // buffer full of them is the encoder's absolute worst case.
    const struct { const char *name; char byte; } escaping[] = {
        { "all NUL escapes", '\xD6' },
        { "all LF escapes",  '\xE0' },
        { "all CR escapes",  '\xE3' },
        { "all '=' escapes", '\x13' },
    };
    for (const auto &e : escaping)
        QTest::newRow(e.name) << QByteArray(300000, e.byte);

    // Line-start-only escapes, and a mix that keeps hitting column 0.
    QTest::newRow("all spaces") << QByteArray(300000, char(' ' - 42));
    QTest::newRow("all dots") << QByteArray(300000, char('.' - 42));

    QByteArray alternating(300000, '\0');
    for (int i = 0; i < alternating.size(); ++i)
        alternating[i] = (i & 1) ? '\x13' : char(' ' - 42);
    QTest::newRow("alternating escape/space") << alternating;

    QByteArray random(300000, '\0');
    // Unsigned: i * 7919 overflows an int past i = 271 181.
    for (int i = 0; i < random.size(); ++i)
        random[i] = static_cast<char>((static_cast<unsigned>(i) * 7919u) & 0xFFu);
    QTest::newRow("pseudo random") << random;

    QTest::newRow("empty") << QByteArray();
    QTest::newRow("single byte") << QByteArray(1, '\x13');
}

void TestYenc::worst_case_bound_is_never_exceeded()
{
    QFETCH(QByteArray, input);

    const size_t bound = NntpArticle::yEncWorstCaseSize(input.size());

    // A canary right past the bound catches a write that overruns it.
    std::vector<uchar> dst(bound + 64, 0xAB);
    quint32 crc = 0;

    const qint64 written = Yenc::encode(input.constData(), input.size(), dst.data(), crc);

    QVERIFY2(written > 0, "encode returned a non-positive length");
    QVERIFY2(written <= qint64(bound),
             qPrintable(QString("encode wrote %1 bytes for a bound of %2")
                                .arg(written).arg(bound)));
    QCOMPARE(dst[static_cast<size_t>(written) - 1], uchar(0)); // trailing NUL
    for (size_t i = bound; i < dst.size(); ++i)
        QCOMPARE(dst[i], uchar(0xAB)); // nothing written past the bound
}

void TestYenc::column_sensitive_bytes_are_escaped_data()
{
    QTest::addColumn<uchar>("rawInput");   //!< byte handed to the encoder
    QTest::addColumn<int>("index");        //!< where it sits in the input
    QTest::addColumn<bool>("mustEscape");

    // After + 42: 0xF6 → ' ' (0x20), 0xDF → '\t' (0x09), 0x04 → '.' (0x2E).
    // maxwidth is 128, so the last column of a line is index 127.
    QTest::newRow("space at column 0")       << uchar(0xF6) << 0   << true;
    QTest::newRow("space mid line")          << uchar(0xF6) << 60  << false;
    QTest::newRow("space at last column")    << uchar(0xF6) << 127 << true;
    QTest::newRow("tab at column 0")         << uchar(0xDF) << 0   << true;
    QTest::newRow("tab mid line")            << uchar(0xDF) << 60  << false;
    QTest::newRow("tab at last column")      << uchar(0xDF) << 127 << true;
    // Dot-stuffing guards the start of a line only: a '.' anywhere else is
    // ordinary data and escaping it would be a needless byte on every line.
    QTest::newRow("dot at column 0")         << uchar(0x04) << 0   << true;
    QTest::newRow("dot mid line")            << uchar(0x04) << 60  << false;
    QTest::newRow("dot at last column")      << uchar(0x04) << 127 << false;
}

void TestYenc::column_sensitive_bytes_are_escaped()
{
    QFETCH(uchar, rawInput);
    QFETCH(int, index);
    QFETCH(bool, mustEscape);

    // 0x17 + 42 == 'A', a filler that never escapes. That is what keeps input
    // index and output column equal for every byte ahead of `index`, so the
    // assertions below can address the output directly.
    constexpr qint64  N = 160;
    std::vector<char> src(static_cast<size_t>(N), char(0x17));
    src[static_cast<size_t>(index)] = static_cast<char>(rawInput);

    std::vector<uchar> dst(NntpArticle::yEncWorstCaseSize(N) + 16, 0xAA);
    quint32            crc = 0;

    Yenc::encode(src.data(), N, dst.data(), crc);

    const size_t at      = static_cast<size_t>(index);
    const uchar  encoded = static_cast<uchar>((rawInput + 42) & 0xFF);

    if (mustEscape) {
        QCOMPARE(dst[at],     uchar('='));
        QCOMPARE(dst[at + 1], uchar(encoded + 64));
    } else {
        QCOMPARE(dst[at], encoded);
        if (at > 0)
            QVERIFY2(dst[at - 1] != uchar('='), "escaped a byte that needs no escape");
    }
}

void TestYenc::last_input_byte_blank_is_escaped_data()
{
    QTest::addColumn<uchar>("rawInput"); //!< last byte handed to the encoder
    QTest::addColumn<int>("size");       //!< input length, so its last column
    QTest::addColumn<bool>("mustEscape");

    // Same byte mapping as above. A '.' only matters at column 0, so it is the
    // control row: ending an article is no reason to escape it.
    QTest::newRow("space ends a short article") << uchar(0xF6) << 10 << true;
    QTest::newRow("tab ends a short article") << uchar(0xDF) << 10 << true;
    QTest::newRow("space ends the second line") << uchar(0xF6) << 140 << true;
    QTest::newRow("dot ends a short article") << uchar(0x04) << 10 << false;
}

void TestYenc::last_input_byte_blank_is_escaped()
{
    QFETCH(uchar, rawInput);
    QFETCH(int, size);
    QFETCH(bool, mustEscape);

    // 0x17 fills with 'A', which never escapes, so the encoded body is the
    // input shifted by one CRLF per full line.
    const qint64 N = size;
    std::vector<char> src(static_cast<size_t>(N), char(0x17));
    src.back() = static_cast<char>(rawInput);

    std::vector<uchar> dst(NntpArticle::yEncWorstCaseSize(N) + 16, 0xAA);
    quint32 crc = 0;

    // The returned size counts the trailing NUL.
    const qint64 n = Yenc::encode(src.data(), N, dst.data(), crc);
    QVERIFY(n >= 3);
    const size_t end = static_cast<size_t>(n - 1);
    const uchar encoded = static_cast<uchar>((rawInput + 42) & 0xFF);

    QCOMPARE(dst[end], uchar(0));
    if (mustEscape) {
        QCOMPARE(dst[end - 2], uchar('='));
        QCOMPARE(dst[end - 1], uchar(encoded + 64));
    } else {
        QCOMPARE(dst[end - 1], encoded);
        QVERIFY2(dst[end - 2] != uchar('='), "escaped a byte that needs no escape");
    }
}

QTEST_APPLESS_MAIN(TestYenc)
#include "tst_Yenc.moc"
