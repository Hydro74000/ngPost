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

std::string generateRandomString(int length) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::random_device rd;
    std::mt19937 engine(rd());
    std::uniform_int_distribution<> dist(0, sizeof(alphanum) - 2);

    std::string randomString;
    randomString.reserve(length);
    for (int i = 0; i < length; ++i) {
        randomString += alphanum[dist(engine)];
    }

    return randomString;
}

int generateRandomStringLength(int start, int end) {
    std::random_device rd;
    std::mt19937 engine(rd());
    std::uniform_int_distribution<int> dist(start, end);
    return dist(engine);
}

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
    char       head[192];
    int const  headLen  = std::snprintf(head,
                                       sizeof head,
                                       "=ybegin part=%u total=%u line=128 size=%lld name=",
                                       _part,
                                       _nntpFile->nbArticles(),
                                       static_cast<long long>(_nntpFile->fileSize()));
    char       ypart[160];
    int const  ypartLen = std::snprintf(ypart,
                                        sizeof ypart,
                                        "%s=ypart begin=%lld end=%lld%s",
                                        Nntp::ENDLINE,
                                        static_cast<long long>(_filePos + 1),
                                        static_cast<long long>(_filePos + _fileBytes),
                                        Nntp::ENDLINE);
    // Both are bounded by the width of their integer types and cannot truncate
    // in the sizes above; bail out rather than post a malformed article if a
    // platform ever proves otherwise.
    if (headLen < 0 || headLen >= static_cast<int>(sizeof head) || ypartLen < 0
        || ypartLen >= static_cast<int>(sizeof ypart))
        return;

    static constexpr size_t kTailCapacity = 96; //!< "=yend size=... pcrc32=..." + "." + ENDLINE

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

    int const tailLen = std::snprintf(ptr,
                                      kTailCapacity,
                                      "%s=yend size=%lld pcrc32=%x%s.%s",
                                      Nntp::ENDLINE,
                                      static_cast<long long>(_fileBytes),
                                      crc32,
                                      Nntp::ENDLINE,
                                      Nntp::ENDLINE);
    ptr += tailLen;

    size_t const bodySize = static_cast<size_t>(ptr - _body);
    _bodyWireSize         = static_cast<qint64>(bodySize);

    // What goes in the nzb is the article as the server stores it, so drop the
    // trailing "." ENDLINE: that is the NNTP end-of-body marker written on the
    // wire, not part of the article.
    static const size_t kDotTerminator = 1 + std::char_traits<char>::length(Nntp::ENDLINE);
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
