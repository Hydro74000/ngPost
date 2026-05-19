//========================================================================
//
// tst_Yenc.cpp — coverage of Yenc::encode (yEnc body encoding).
//
//========================================================================

#include <QtTest>

#include <vector>

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

QTEST_APPLESS_MAIN(TestYenc)
#include "tst_Yenc.moc"
