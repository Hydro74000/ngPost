//========================================================================
//
// Copyright (C) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
// This file is a part of ngPost : https://github.com/Hydro74000/ngPost
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 3..
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>
//
//========================================================================

#include "NzbCheck.h"
#include "NntpCheckCon.h"
#include "nntp/NntpServerParams.h"
#include <cmath>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTime>
#include <QXmlStreamReader>

const QRegularExpression NzbCheck::sNntpArticleYencSubjectRegExp = QRegularExpression(
    sNntpArticleYencSubjectStrRegExp);

namespace
{
//! ".par2" immediately followed by the closing quote or a space, so a data file
//! called something.par2.rar is not mistaken for one.
const QRegularExpression sPar2FileRegExp(QStringLiteral("\\.par2\"?\\s"),
                                         QRegularExpression::CaseInsensitiveOption);

//! ".volNN+MM.par2" -> MM recovery blocks. par2cmdline, ParPar and MultiPar all
//! use this naming; the base .par2 has no vol part and carries no blocks.
const QRegularExpression sPar2BlocksRegExp(QStringLiteral("\\.vol\\d+\\+(\\d+)\\.par2"),
                                           QRegularExpression::CaseInsensitiveOption);

//! ngPost, and most posters, close the subject with the size of the file.
const QRegularExpression sSubjectSizeRegExp(
    QStringLiteral("yEnc\\s+\\(\\d+/\\d+\\)\\s+(\\d+)\\s*$"));

qint64 ceilDiv(qint64 numerator, qint64 denominator)
{
    return denominator > 0 ? (numerator + denominator - 1) / denominator : 0;
}
} // namespace

int Par2Volume::usableBlocks() const
{
    if (blocks <= 0)
        return 0;
    if (nbMissingArticles <= 0)
        return blocks;
    if (nbExpectedArticles <= 0)
        return 0; // no idea how much of it survived: assume nothing
    int const present = nbExpectedArticles - nbMissingArticles;
    if (present <= 0)
        return 0;
    return static_cast<int>((static_cast<qint64>(blocks) * present) / nbExpectedArticles);
}

void NzbCheck::onDisconnected(NntpCheckCon *con)
{
    _connections.remove(con);
    if (_connections.isEmpty()) {
        if (_dispProgressBar) {
            disconnect(&_progressbarTimer,
                       &QTimer::timeout,
                       this,
                       &NzbCheck::onRefreshprogressbarBar);
            onRefreshprogressbarBar();
            _cout << "\n" << MB_FLUSH;
        }

        qint64 const duration = _timeStart.elapsed();

        // A check that never reached the server used to print "0 missing" --
        // i.e. "all good" -- which is the most dangerous answer it could give.
        // Tell the three outcomes apart: nothing verified, partially verified,
        // fully verified.
        bool const nothingVerified = (_nbListedArticles > 0 && _nbCheckedArticles == 0);
        if (nothingVerified) {
            // The denominator here is what the nzb actually lists, not the
            // expected total the summary reports: an article the nzb does not
            // list has no Message-ID and was never verifiable in the first
            // place. Saying "listed" keeps the two figures from reading as a
            // contradiction.
            _cerr << tr("ERROR: check FAILED - not one of the %1 article(s) listed in the nzb "
                        "could be verified. Every connection was refused or dropped: check the "
                        "credentials, and whether another program is already using all the "
                        "connections allowed on the server(s).")
                         .arg(_nbListedArticles)
                  << "\n"
                  << MB_FLUSH;
        } else if (_nbCheckedArticles < _nbListedArticles) {
            _cerr << tr("ERROR: check INCOMPLETE - only %1 of the %2 article(s) listed in the "
                        "nzb were verified. Some connections failed (the server's connection "
                        "limit may have been reached). The missing-article count below is NOT "
                        "reliable.")
                         .arg(_nbCheckedArticles)
                         .arg(_nbListedArticles)
                  << "\n"
                  << MB_FLUSH;
        }

        // Printing "0 missing" right under "not a single article could be
        // verified" is the very confusion this block exists to remove.
        if (!_quietMode && !nothingVerified) {
            _cout << tr("Nb Missing Article(s): %1/%2 (check done in %3 (%4 sec) using %5 "
                        "connections on %6 server(s))")
                         .arg(_nbMissingArticles)
                         .arg(_nbTotalArticles)
                         .arg(QTime::fromMSecsSinceStartOfDay(static_cast<int>(duration))
                                  .toString("hh:mm:ss.zzz"))
                         .arg(std::round(1. * duration / 1000))
                         .arg(_nbCons)
                         .arg(nbCheckingServers())
                  << "\n"
                  << MB_FLUSH;
        }

        if (!_quietMode && !nothingVerified)
            _printRecoveryAnalysis();

        if (_jsonOutput)
            _printJsonReport(duration);

        qApp->quit();
    }
}

void NzbCheck::onRefreshprogressbarBar()
{
    float progressbar = static_cast<float>(_nbCheckedArticles);
    progressbar /= _nbListedArticles;

    _cout << "\r[";
    int pos = static_cast<int>(std::floor(progressbar * sprogressbarBarWidth));
    for (int i = 0; i < sprogressbarBarWidth; ++i) {
        if (i < pos)
            _cout << "=";
        else if (i == pos)
            _cout << ">";
        else
            _cout << " ";
    }
    _cout << "] " << int(progressbar * 100) << " %"
          << " (" << _nbCheckedArticles << " / " << _nbListedArticles << ")" << tr(" missing: ")
          << _nbMissingArticles;
    _cout.flush();

    if (_nbCheckedArticles < _nbListedArticles)
        _progressbarTimer.start(_refreshRate);
}

NzbCheck::NzbCheck()
    : QObject()
    , _nzbPath()
    , _articles()
    , _cout(stdout)
    , _cerr(stderr)
    , _nbTotalArticles(0)
    , _nbListedArticles(0)
    , _nbMissingArticles(0)
    , _nbCheckedArticles(0)
    , _nbMissingArticlesInNzb(0)
    , _nntpServers()
    , _debug(0)
    , _connections()
    , _dispProgressBar(false)
    , _progressbarTimer()
    , _refreshRate(sDefaultRefreshRate)
    , _quietMode(false)
    , _jsonOutput(false)
    , _unusable(false)
    , _nbCons(0) // only assigned by checkPost(), which a failed check never reaches
    , _socketTimeOut(30000)
    , _maxRetries(5)
    , _par2Volumes()
    , _articleVolume()
    , _nbPar2Articles(0)
    , _nbDataArticles(0)
    , _nbMissingPar2Articles(0)
    , _nbMissingDataArticles(0)
    , _par2BlockSize(0)
    , _blockSizeSource()
    , _articleSize(0)
    , _articleSizeFromSubjects(0)
    , _dataSizeBytes(0)
{}

NzbCheck::~NzbCheck()
{
    if (_dispProgressBar)
        _progressbarTimer.stop();
}

int NzbCheck::parseNzb()
{
    QFile file(_nzbPath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QXmlStreamReader xmlReader(&file);
        while (!xmlReader.atEnd()) {
            QXmlStreamReader::TokenType type = xmlReader.readNext();
            if (type == QXmlStreamReader::TokenType::StartElement
                && xmlReader.name().compare(QLatin1String("file")) == 0) {
                QString subject = xmlReader.attributes().value("subject").toString();
                QRegularExpressionMatch match = sNntpArticleYencSubjectRegExp.match(subject);
                int nbArticles = 0, nbExpectedArticles = 0;
                if (match.hasMatch())
                    nbExpectedArticles = match.captured(1).toInt();

                bool const isPar2 = sPar2FileRegExp.match(subject).hasMatch();
                int volumeIdx = -1;
                if (isPar2) {
                    Par2Volume vol;
                    vol.subject            = subject;
                    vol.nbExpectedArticles = nbExpectedArticles;
                    QRegularExpressionMatch blocks = sPar2BlocksRegExp.match(subject);
                    if (blocks.hasMatch()) {
                        vol.blocks   = blocks.captured(1).toInt();
                        vol.isVolume = true;
                    }
                    volumeIdx = _par2Volumes.size();
                    _par2Volumes.append(vol);
                } else {
                    // The announced size is what lets us derive the payload of
                    // one article, and with it how far a lost article reaches
                    // into the PAR2 blocks.
                    QRegularExpressionMatch size = sSubjectSizeRegExp.match(subject);
                    if (size.hasMatch()) {
                        qint64 const fileSize = size.captured(1).toLongLong();
                        _dataSizeBytes += fileSize;
                        // n articles cover the file and the last one is short,
                        // so ceil(size/n) is a lower bound on the payload of a
                        // full article. The largest bound across files is the
                        // tightest one.
                        if (nbExpectedArticles > 0) {
                            qint64 const bound = ceilDiv(fileSize, nbExpectedArticles);
                            if (bound > _articleSizeFromSubjects)
                                _articleSizeFromSubjects = bound;
                        }
                    }
                }

                while (!xmlReader.atEnd()) {
                    QXmlStreamReader::TokenType type = xmlReader.readNext();
                    if (type == QXmlStreamReader::TokenType::EndElement
                        && xmlReader.name().compare(QLatin1String("file")) == 0) {
                        if (debugMode())
                            _cout << tr("The file '%1' has %2 articles in the nzb (expected: %3)")
                                         .arg(subject)
                                         .arg(nbArticles)
                                         .arg(nbExpectedArticles)
                                  << "\n"
                                  << MB_FLUSH;
                        if (nbArticles < nbExpectedArticles) {
                            if (!_quietMode)
                                _cout << tr("- %1 missing Article(s) in nzb for '%2'")
                                             .arg(nbExpectedArticles - nbArticles)
                                             .arg(subject)
                                      << "\n"
                                      << MB_FLUSH;

                            int const absent = nbExpectedArticles - nbArticles;
                            _nbMissingArticles += absent;
                            _nbMissingArticlesInNzb += absent;
                            // An article the nzb never lists is as lost as one
                            // the server dropped, and it hurts the same volume.
                            if (isPar2) {
                                _nbMissingPar2Articles += absent;
                                _par2Volumes[volumeIdx].nbMissingArticles += absent;
                            } else
                                _nbMissingDataArticles += absent;
                        }

                        break;
                    } else if (type == QXmlStreamReader::TokenType::StartElement
                               && xmlReader.name().compare(QLatin1String("segment")) == 0) {
                        ++nbArticles;
                        xmlReader.readNext();
                        QString const articleId = QString("<%1>").arg(
                                xmlReader.text().toString());
                        _articles.push(articleId);
                        if (isPar2) {
                            _articleVolume.insert(articleId, volumeIdx);
                            ++_nbPar2Articles;
                        } else
                            ++_nbDataArticles;
                    }
                }
            }
        }

        if (xmlReader.hasError()) {
            _cerr << "parsing error: " << xmlReader.errorString()
                  << " at line: " << xmlReader.lineNumber() << "\n"
                  << MB_FLUSH;
            return -2;
        }
        _nbListedArticles = _articles.size();
        _nbTotalArticles = _nbListedArticles + _nbMissingArticlesInNzb;
        _resolveSizes();
        if (!_quietMode) {
            _cout << tr("%1 has %2 articles (%3 data, %4 par2 in %5 volume(s))")
                         .arg(QFileInfo(_nzbPath).fileName())
                         .arg(_nbListedArticles)
                         .arg(_nbDataArticles)
                         .arg(_nbPar2Articles)
                         .arg(_par2Volumes.size())
                  << "\n"
                  << MB_FLUSH;
            if (_par2Volumes.isEmpty())
                _cout << tr("WARNING: this nzb carries no PAR2 file - nothing can be repaired")
                      << "\n"
                      << MB_FLUSH;
            else
                _cout << tr("PAR2 recovery blocks: %1").arg(totalRecoveryBlocks()) << "\n"
                      << MB_FLUSH;
        }
        return _nbListedArticles;
    } else {
        _cerr << tr("Error opening nzb file...") << "\n" << MB_FLUSH;
        return -1;
    }
}

void NzbCheck::checkPost()
{
    _timeStart.start();

    _nbCons = 0;
    for (NntpServerParams *srvParam : _nntpServers) {
        if (srvParam->nzbCheck)
            _nbCons += srvParam->nbCons;
    }

    _nbCons = std::min(_nbListedArticles, _nbCons);

    // Keep this invariant local as well as enforcing it in the CLI parser:
    // without a connection, no disconnected signal can ever end the run.
    if (_nbCons <= 0) {
        reportUnusable(tr("the servers enabled for nzb checking have no positive connection count"));
        QMetaObject::invokeMethod(qApp, "quit", Qt::QueuedConnection);
        return;
    }

    int nb = 0;
    for (NntpServerParams *srvParam : _nntpServers) {
        if (srvParam->nzbCheck) {
            for (int i = 1; i <= srvParam->nbCons; ++i) {
                NntpCheckCon *con = new NntpCheckCon(this, i, *srvParam);
                connect(con,
                        &NntpCheckCon::disconnected,
                        this,
                        &NzbCheck::onDisconnected,
                        Qt::DirectConnection);
                emit con->startConnection();

                _connections.insert(con);

                if (++nb == _nbCons)
                    break;
            }
            if (nb == _nbCons)
                break;
        }
    }

    if (debugMode())
        _cout << tr("Using %1 Connections").arg(_nbCons) << "\n" << MB_FLUSH;

    if (_dispProgressBar) {
        connect(&_progressbarTimer,
                &QTimer::timeout,
                this,
                &NzbCheck::onRefreshprogressbarBar,
                Qt::DirectConnection);
        _progressbarTimer.start(_refreshRate);
    }
}

void NzbCheck::reportUnusable(const QString &reason)
{
    // Idempotent on purpose: with --check_json the whole contract is that
    // stdout carries exactly one document. Two callers reporting the same
    // dead end would print two, and no consumer survives that.
    if (_unusable)
        return;
    _unusable = true;
    _cerr << tr("ERROR: check INCONCLUSIVE - %1").arg(reason) << "\n" << MB_FLUSH;
    if (_jsonOutput)
        _printJsonReport(0, reason);
}

void NzbCheck::_resolveSizes()
{
    // What the subjects say beats what the configuration says: the latter
    // describes how we would post, not how this nzb was actually made.
    if (_articleSizeFromSubjects > 0)
        _articleSize = _articleSizeFromSubjects;

    if (_par2BlockSize <= 0 && _articleSize > 0) {
        _par2BlockSize   = _articleSize;
        _blockSizeSource = tr("guessed from the article size");
    }
}

int NzbCheck::totalRecoveryBlocks() const
{
    int blocks = 0;
    for (Par2Volume const &vol : _par2Volumes)
        blocks += vol.blocks;
    return blocks;
}

int NzbCheck::usableRecoveryBlocks() const
{
    int blocks = 0;
    for (Par2Volume const &vol : _par2Volumes)
        blocks += vol.usableBlocks();
    return blocks;
}

bool NzbCheck::hasIntactPar2Metadata() const
{
    // The file description and main packets are repeated in every PAR2 file, so
    // a single intact one is enough to know what to repair.
    for (Par2Volume const &vol : _par2Volumes) {
        if (vol.isIntact())
            return true;
    }
    return false;
}

QPair<int, int> NzbCheck::damagedBlockRange() const
{
    int const missing = _nbMissingDataArticles;
    if (missing <= 0)
        return { 0, 0 };
    if (_par2BlockSize <= 0 || _articleSize <= 0)
        return { missing, missing }; // nothing better to say than one for one

    // Best case: the losses are contiguous, so they span a single run of bytes.
    qint64 clustered = ceilDiv(static_cast<qint64>(missing) * _articleSize, _par2BlockSize);
    // Worst case: each lost article sits astride a block boundary on its own,
    // so it takes out floor(A/B) whole blocks plus the two it straddles.
    qint64 const perArticle = _articleSize / _par2BlockSize + 1;
    qint64 scattered = static_cast<qint64>(missing) * perArticle;

    // Neither can exceed the number of blocks the data occupies.
    if (_dataSizeBytes > 0) {
        qint64 const dataBlocks = ceilDiv(_dataSizeBytes, _par2BlockSize);
        clustered = qMin(clustered, dataBlocks);
        scattered = qMin(scattered, dataBlocks);
    }
    clustered = qMax(clustered, qint64(1));
    return { static_cast<int>(clustered), static_cast<int>(qMax(clustered, scattered)) };
}

NzbCheck::Recovery NzbCheck::recoveryVerdict() const
{
    if (_nbMissingDataArticles <= 0)
        return Recovery::NotNeeded;
    if (_par2Volumes.isEmpty() || !hasIntactPar2Metadata() || usableRecoveryBlocks() <= 0)
        return Recovery::NoRedundancy;

    int const usable = usableRecoveryBlocks();
    QPair<int, int> const damaged = damagedBlockRange();
    if (damaged.second <= usable)
        return Recovery::Certain;
    if (damaged.first <= usable)
        return Recovery::LayoutDependent;
    return Recovery::Impossible;
}

NzbCheck::CheckStatus NzbCheck::checkStatus() const
{
    if (_unusable)
        return CheckStatus::Inconclusive;

    // Anything short of a full sweep means the answer cannot be trusted, no
    // matter how few articles came back missing.
    if (_nbListedArticles > 0 && _nbCheckedArticles < _nbListedArticles)
        return CheckStatus::Inconclusive;

    if (_nbMissingArticles == 0)
        return CheckStatus::Complete;

    Recovery const recovery = recoveryVerdict();
    if (recovery == Recovery::Impossible || recovery == Recovery::NoRedundancy)
        return CheckStatus::Unrecoverable;
    return CheckStatus::Missing;
}

void NzbCheck::_printRecoveryAnalysis()
{
    if (_par2Volumes.isEmpty() && _nbMissingDataArticles == 0 && _nbMissingPar2Articles == 0)
        return; // nothing lost and nothing to say about redundancy

    int const usable = usableRecoveryBlocks();
    int const total  = totalRecoveryBlocks();

    _cout << "\n" << tr("=== Recovery analysis ===") << "\n";
    _cout << tr("  Data articles: %1 (missing: %2)")
                 .arg(_nbDataArticles)
                 .arg(_nbMissingDataArticles)
          << "\n";
    _cout << tr("  PAR2 articles: %1 (missing: %2)")
                 .arg(_nbPar2Articles)
                 .arg(_nbMissingPar2Articles)
          << "\n";

    if (_par2Volumes.isEmpty()) {
        _cout << tr("  No PAR2 file: nothing can be repaired") << "\n" << MB_FLUSH;
        return;
    }

    _cout << tr("  Recovery blocks: %1 of %2 still usable").arg(usable).arg(total) << "\n";
    _cout << (hasIntactPar2Metadata()
                      ? tr("  PAR2 metadata: available, so a repair knows what to rebuild")
                      : tr("  PAR2 metadata: LOST - every PAR2 file is damaged"))
          << "\n";

    if (_nbMissingDataArticles > 0) {
        QPair<int, int> const damaged = damagedBlockRange();
        _cout << tr("  Damaged blocks: %1 to %2, depending on how the losses are spread "
                    "(block size: %3 bytes, %4)")
                     .arg(damaged.first)
                     .arg(damaged.second)
                     .arg(_par2BlockSize)
                     .arg(_blockSizeSource.isEmpty() ? tr("declared") : _blockSizeSource)
              << "\n";
    }

    switch (recoveryVerdict()) {
    case Recovery::NotNeeded:
        _cout << tr("  Verdict: COMPLETE - no data article is missing") << "\n";
        if (_nbMissingPar2Articles > 0 && total > 0)
            _cout << tr("  Warning: %1% of the recovery blocks are gone; the data is intact "
                        "today but it is less protected than it was")
                         .arg(100.0 * (total - usable) / total, 0, 'f', 1)
                  << "\n";
        break;
    case Recovery::Certain:
        _cout << tr("  Verdict: RECOVERABLE - the remaining blocks cover the loss whatever "
                    "its layout")
              << "\n";
        break;
    case Recovery::LayoutDependent:
        _cout << tr("  Verdict: PROBABLY RECOVERABLE - the blocks cover the loss only if it "
                    "is clustered. Try the repair before re-posting")
              << "\n";
        break;
    case Recovery::Impossible:
        _cout << tr("  Verdict: UNRECOVERABLE - the loss exceeds the remaining blocks even at "
                    "best. This post has to be re-posted from the source")
              << "\n";
        break;
    case Recovery::NoRedundancy:
        _cout << tr("  Verdict: UNRECOVERABLE - data is missing and no usable recovery block "
                    "is left")
              << "\n";
        break;
    }
    _cout << MB_FLUSH;
}

void NzbCheck::_printJsonReport(qint64 durationMs, const QString &error)
{
    CheckStatus const status = checkStatus();

    QString statusName;
    switch (status) {
    case CheckStatus::Complete:
        statusName = QStringLiteral("complete");
        break;
    case CheckStatus::Missing:
        statusName = QStringLiteral("missing");
        break;
    case CheckStatus::Unrecoverable:
        statusName = QStringLiteral("unrecoverable");
        break;
    case CheckStatus::Inconclusive:
        statusName = QStringLiteral("inconclusive");
        break;
    }

    QJsonObject articles;
    articles[QStringLiteral("total")]        = _nbTotalArticles;
    articles[QStringLiteral("checked")]      = _nbCheckedArticles;
    articles[QStringLiteral("missing")]      = _nbMissingArticles;
    articles[QStringLiteral("missingInNzb")] = _nbMissingArticlesInNzb;
    articles[QStringLiteral("data")]         = _nbDataArticles;
    articles[QStringLiteral("dataMissing")]  = _nbMissingDataArticles;
    articles[QStringLiteral("par2")]         = _nbPar2Articles;
    articles[QStringLiteral("par2Missing")]  = _nbMissingPar2Articles;

    QString recoveryName;
    switch (recoveryVerdict()) {
    case Recovery::NotNeeded:       recoveryName = QStringLiteral("notNeeded");       break;
    case Recovery::Certain:         recoveryName = QStringLiteral("certain");         break;
    case Recovery::LayoutDependent: recoveryName = QStringLiteral("layoutDependent"); break;
    case Recovery::Impossible:      recoveryName = QStringLiteral("impossible");      break;
    case Recovery::NoRedundancy:    recoveryName = QStringLiteral("noRedundancy");    break;
    }

    QPair<int, int> const damaged = damagedBlockRange();
    QJsonObject par2;
    par2[QStringLiteral("volumes")]           = _par2Volumes.size();
    par2[QStringLiteral("blocksTotal")]       = totalRecoveryBlocks();
    par2[QStringLiteral("blocksUsable")]      = usableRecoveryBlocks();
    par2[QStringLiteral("metadataAvailable")] = hasIntactPar2Metadata();
    par2[QStringLiteral("blockSize")]         = _par2BlockSize;
    par2[QStringLiteral("blockSizeSource")]   = _blockSizeSource.isEmpty()
                                                     ? QStringLiteral("declared")
                                                     : _blockSizeSource;
    par2[QStringLiteral("damagedBlocksMin")]  = damaged.first;
    par2[QStringLiteral("damagedBlocksMax")]  = damaged.second;
    par2[QStringLiteral("recovery")]          = recoveryName;

    QJsonObject root;
    root[QStringLiteral("nzb")]         = QFileInfo(_nzbPath).absoluteFilePath();
    root[QStringLiteral("status")]      = statusName;
    root[QStringLiteral("exitCode")]    = static_cast<int>(status);
    root[QStringLiteral("articles")]    = articles;
    root[QStringLiteral("par2")]        = par2;
    root[QStringLiteral("servers")]     = nbCheckingServers();
    root[QStringLiteral("connections")] = _nbCons;
    root[QStringLiteral("durationMs")]  = durationMs;
    if (!error.isEmpty())
        root[QStringLiteral("error")] = error;

    _cout << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)) << "\n"
          << MB_FLUSH;
}

int NzbCheck::nbCheckingServers()
{
    int nb = 0;
    for (NntpServerParams *srvParam : _nntpServers) {
        if (srvParam->nzbCheck)
            ++nb;
    }
    return nb;
}
