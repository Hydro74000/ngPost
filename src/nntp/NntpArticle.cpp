/*
 * Copyright (c) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
 * Copyright (c) 2024-2026 Hydro74000 <acymap@gmail.com>
 * Licensed under the GNU General Public License v3.0
 */

#include "NntpArticle.h"
#include "NntpConnection.h"
#include "NgPost.h"
#include "nntp/NntpFile.h"
#include "nntp/Nntp.h"
#include "utils/Yenc.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <random>
#include <vector>

ushort NntpArticle::sNbMaxTrySending = 5;

NntpArticle::NntpArticle(NntpFile *file, uint part, qint64 pos, qint64 bytes,
                         const std::string *from, bool obfuscateArticles):
    _nntpFile(file), _part(part),
    _id(QUuid::createUuid()),
    _from(from),
    _subject(nullptr),
    _body(nullptr),
    _filePos(pos), _fileBytes(bytes),
    _bodySize(0),
    _bodyWireSize(0),
    _nbTrySending(0),
    _msgId(),
    _obfuscateArticles(obfuscateArticles)
{
    file->addArticle(this);
    connect(this, &NntpArticle::posted, _nntpFile, &NntpFile::onArticlePosted, Qt::QueuedConnection);
    connect(this, &NntpArticle::failed, _nntpFile, &NntpFile::onArticleFailed, Qt::QueuedConnection);

    if (!obfuscateArticles)
    {
        std::stringstream ss;
        ss << _nntpFile->nameWithQuotes().toStdString() << " (" << part << "/" << _nntpFile->nbArticles() << ")";

        std::string subject = ss.str();
        _subject = new char[subject.size() + 1];
        std::copy(subject.begin(), subject.end(), _subject);
        _subject[subject.size()] = '\0';

    }
}

namespace
{
//! One Mersenne Twister per thread, seeded once from std::random_device.
//!
//! Both generators below used to build their own std::random_device and their
//! own std::mt19937 on every call, so each obfuscated article re-seeded 624
//! words of state twice before it could even be encoded. Article builders run
//! one per Poster thread and never share this engine, so keeping it costs no
//! synchronisation and gives the same quality of draw.
std::mt19937 &articleRandomEngine()
{
    static thread_local std::mt19937 engine{ std::random_device{}() };
    return engine;
}
} // namespace

std::string generateRandomString(int length) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::mt19937 &engine = articleRandomEngine();
    std::uniform_int_distribution<> dist(0, sizeof(alphanum) - 2);

    std::string randomString;
    randomString.reserve(length);
    for (int i = 0; i < length; ++i) {
        randomString += alphanum[dist(engine)];
    }

    return randomString;
}

int generateRandomStringLength(int start, int end) {
    std::uniform_int_distribution<int> dist(start, end);
    return dist(articleRandomEngine());
}

namespace
{
//! Widest decimal rendering of the integers the yEnc lines carry: ten digits
//! for a 32-bit unsigned, twenty for a signed 64-bit with its minus sign.
constexpr size_t kMaxUIntDigits = 10;
constexpr size_t kMaxI64Digits  = 20;
constexpr size_t kEndlineLen    = std::char_traits<char>::length(Nntp::ENDLINE);
constexpr size_t kCrc32Digits   = 8; //!< pcrc32 is written %08x, so always eight

//! Capacities for the three fixed-format pieces of the body: the literal text
//! of each format string -- sizeof() counts its NUL, which is the terminator
//! we need -- plus the worst case of every field it interpolates.
//!
//! Sizing them this way is what rules truncation out. snprintf cannot shorten
//! what always fits, so its return value is exact, and no runtime branch is
//! needed to catch a failure that cannot happen. That matters for the tail in
//! particular: its return value advances the write pointer, and a truncated
//! one would have set _bodyWireSize past the end of the allocation.
constexpr size_t kHeadCapacity = sizeof("=ybegin part= total= line=128 size= name=")
                               + 2 * kMaxUIntDigits + kMaxI64Digits;
constexpr size_t kYpartCapacity = sizeof("=ypart begin= end=")
                                + 2 * kEndlineLen + 2 * kMaxI64Digits;
constexpr size_t kTailCapacity = sizeof("=yend size= pcrc32=.")
                               + 3 * kEndlineLen + kMaxI64Digits + kCrc32Digits;
} // namespace

//! Upper bound on what Yenc::encode writes for \a nbBytes of input, its
//! trailing NUL included.
//!
//! Every input byte can need an escape, so two output bytes, and the encoder
//! breaks the line every 128 output columns for two more. Adversarial fuzzing
//! (an input where every byte escapes) fills this to within 7 bytes, so it is
//! a tight bound, not a guess -- which is what makes it safe to halve the
//! 4 * nbBytes that used to be allocated here.
size_t NntpArticle::yEncWorstCaseSize(qint64 nbBytes)
{
    size_t const n = static_cast<size_t>(nbBytes > 0 ? nbBytes : 0);
    return n * 2 + (n * 2) / 128 * 2 + 8;
}

void NntpArticle::yEncBody(const char data[])
{
    std::string const filename = _obfuscateArticles
                                         ? generateRandomString(generateRandomStringLength(32, 62))
                                         : _nntpFile->fileName();

    // The body is built in place, in a single allocation: the =ybegin/=ypart
    // lines first, then Yenc::encode writing straight behind them, then the
    // =yend line. Going through a std::stringstream and its str() copy meant
    // four passes over ~700 KB and 15 allocations for exactly these bytes.
    char      head[kHeadCapacity];
    int const headLen = std::snprintf(head,
                                      sizeof head,
                                      "=ybegin part=%u total=%u line=128 size=%lld name=",
                                      _part,
                                      _nntpFile->nbArticles(),
                                      static_cast<long long>(_nntpFile->fileSize()));
    char      ypart[kYpartCapacity];
    int const ypartLen = std::snprintf(ypart,
                                       sizeof ypart,
                                       "%s=ypart begin=%lld end=%lld%s",
                                       Nntp::ENDLINE,
                                       static_cast<long long>(_filePos + 1),
                                       static_cast<long long>(_filePos + _fileBytes),
                                       Nntp::ENDLINE);

    size_t const capacity = static_cast<size_t>(headLen) + filename.size()
                          + static_cast<size_t>(ypartLen) + yEncWorstCaseSize(_fileBytes)
                          + kTailCapacity;

    _body     = new char[capacity];
    char *ptr = _body;
    std::memcpy(ptr, head, static_cast<size_t>(headLen));
    ptr += headLen;
    std::memcpy(ptr, filename.data(), filename.size());
    ptr += filename.size();
    std::memcpy(ptr, ypart, static_cast<size_t>(ypartLen));
    ptr += ypartLen;

    quint32      crc32   = 0xFFFFFFFF;
    qint64 const encoded = Yenc::encode(data, _fileBytes, reinterpret_cast<uchar *>(ptr), crc32);
    ptr += encoded - 1; // Yenc::encode counts its own NUL, which the tail overwrites

    // %08x, not %x: the yEnc format calls for eight hex digits, and a CRC
    // whose top nibble is zero -- one article in sixteen -- used to be written
    // a digit short. Lenient decoders never minded; ones that compare the
    // field as a string do.
    int const tailLen = std::snprintf(ptr,
                                      kTailCapacity,
                                      "%s=yend size=%lld pcrc32=%08x%s.%s",
                                      Nntp::ENDLINE,
                                      static_cast<long long>(_fileBytes),
                                      crc32,
                                      Nntp::ENDLINE,
                                      Nntp::ENDLINE);
    // Exact by construction, see kTailCapacity: this advances the pointer that
    // sets _bodyWireSize, so a truncated count would run the socket write off
    // the end of the buffer.
    Q_ASSERT(tailLen > 0 && static_cast<size_t>(tailLen) < kTailCapacity);
    ptr += tailLen;

    size_t const bodySize = static_cast<size_t>(ptr - _body);
    _bodyWireSize         = static_cast<qint64>(bodySize);

    // What goes in the nzb is the article as the server stores it, so drop the
    // trailing "." ENDLINE: that is the NNTP end-of-body marker written on the
    // wire, not part of the article.
    constexpr size_t kDotTerminator = 1 + kEndlineLen;
    _bodySize = bodySize > kDotTerminator ? static_cast<qint64>(bodySize - kDotTerminator)
                                          : static_cast<qint64>(bodySize);
}

NntpArticle::~NntpArticle()
{
    freeMemory();
}

QString NntpArticle::str() const
{
    if (_msgId.isEmpty())
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
        _msgId = _id.toString(sMsgIdFormat);
#else
        _msgId = _id.toString();
#endif
    return QString("%5 - Article #%1/%2 <id: %3, nbTrySend: %4>").arg(
                _part).arg(_nntpFile->nbArticles()).arg(_msgId).arg(
                _nbTrySending).arg(_nntpFile->name());
}

bool NntpArticle::tryResend()
{
    if (_nbTrySending < sNbMaxTrySending)
    {
        _id = QUuid::createUuid();
        return true;
    }
    else
        return false;
}

void NntpArticle::write(NntpConnection *con, const std::string &idSignature)
{
    ++_nbTrySending;
    const std::string articleHeader = header(idSignature);
    _nntpFile->onArticlePostingStarted(this, _nbTrySending);
    con->write(articleHeader.data(), static_cast<qint64>(articleHeader.size()));
    con->write(_body, _bodyWireSize);
}

std::string NntpArticle::header(const std::string &idSignature) const
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
    QByteArray msgId = _id.toByteArray(sMsgIdFormat);
#else
    QByteArray msgId = _id.toByteArray();
#endif
    std::stringstream ss;
    ss << "From: "        << (_from == nullptr ? NgPost::randomStdFrom() : *_from)    << Nntp::ENDLINE
       << "Newsgroups: "  << _nntpFile->groups()  << Nntp::ENDLINE
       << "Subject: "     << (_subject == nullptr ? msgId.constData() : _subject) << Nntp::ENDLINE
       << "Message-ID: <" << msgId.constData() << "@" << idSignature << ">" << Nntp::ENDLINE
       << Nntp::ENDLINE;
    _msgId = QString("%1@%2").arg(QString::fromUtf8(msgId.constData()), QString::fromStdString(idSignature));
    return ss.str();
}

void NntpArticle::dumpToFile(const QString &path, const std::string &articleIdSignature)
{
    QString fileName = QString("%1/%2_%3.yenc").arg(path, QString::fromStdString(_nntpFile->fileName()), QString::number(_part));
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly))
    {
        qDebug() << "[NntpArticle::dumpToFile] error creating file " << fileName;
        return;
    }

    std::string const articleHeader = header(articleIdSignature);
    file.write(articleHeader.data(), static_cast<qint64>(articleHeader.size()));
    file.write(_body, _bodyWireSize);
    file.close();
}
