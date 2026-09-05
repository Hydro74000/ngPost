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
#include <limits>

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
//! ".par2" at the end of the name: followed by the closing quote, by a space,
//! or by nothing at all when the subject stops there. A data file called
//! something.par2.rar is still not mistaken for one.
const QRegularExpression sPar2FileRegExp(QStringLiteral("\\.par2(?:\"|\\s|$)"),
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
    return numerator > 0 && denominator > 0
                   ? numerator / denominator + (numerator % denominator != 0)
                   : 0;
}

qint64 saturatedAdd(qint64 left, qint64 right)
{
    qint64 const limit = std::numeric_limits<qint64>::max();
    return right > 0 && left > limit - right ? limit : left + right;
}

qint64 maxBlocksTouched(qint64 bytes, qint64 blockSize)
{
    // An interval of L bytes starting at the least favourable byte offset can
    // touch ceil((L-1)/B)+1 blocks. In particular, one byte touches one block,
    // not two.
    return bytes > 0 ? saturatedAdd(ceilDiv(bytes - 1, blockSize), 1) : 0;
}

int boundedBlockCount(qint64 blocks)
{
    return blocks > INT_MAX ? INT_MAX : static_cast<int>(blocks);
}
} // namespace

int Par2Volume::usableBlocks() const
{
    if (blocks <= 0 || nbExpectedArticles <= 0)
        return 0;
    if (isIntact())
        return blocks;

    // An unanswered article is not a surviving one. On an incomplete check,
    // report only the pro-rata share supported by answers already received;
    // the guaranteed range remains zero until the whole volume is checked.
    int const confirmedPresent = qMin(
            nbExpectedArticles, nbCheckedArticles - nbMissingListedArticles);
    if (confirmedPresent <= 0)
        return 0;
    // Pro rata, less one block per missing article. Recovery packets do not
    // stop politely on article boundaries: a lost article takes its share of
    // them plus, at worst, the one it cuts in half.
    qint64 const share
            = (static_cast<qint64>(blocks) * confirmedPresent) / nbExpectedArticles;
    return static_cast<int>(qMax(qint64(0), share - nbMissingArticles));
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
        // NzbCheck owns its own stdout stream, so this has to respect quiet mode
        // by itself: under --check_json that stream carries the report and
        // nothing else. The json says stoppedEarly on its own.
        if (_earlyStop && !_quietMode) {
            // Say which fact stopped it: "beyond what the blocks can repair" is
            // a strange thing to read on a post that has no block at all.
            QString const reason
                    = (recoveryVerdict() == Recovery::NoPar2AtAll
                       || recoveryVerdict() == Recovery::NoRecoveryBlocks
                       || recoveryVerdict() == Recovery::NoUsableBlocks)
                              ? tr("there is nothing left to rebuild it with")
                              : tr("the loss is already beyond what the PAR2 blocks can repair");
            _cout << tr("Stopped after %1 of the %2 article(s) listed in the nzb: %3, so "
                        "checking the rest would not change the answer. Pass --%4 to check "
                        "everything anyway.")
                         .arg(_nbCheckedArticles)
                         .arg(_nbListedArticles)
                         .arg(reason)
                         .arg(QStringLiteral("check_full"))
                  << "\n"
                  << MB_FLUSH;
        } else if (nothingVerified && !_earlyStop) {
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
        } else if (!_earlyStop && _nbCheckedArticles < _nbListedArticles) {
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
            // Two separate tr() calls: lupdate cannot extract a string picked
            // at run time.
            QString const summary
                    = _earlyStop
                              ? tr("Nb Missing Article(s): at least %1/%2 (stopped early after "
                                   "%3 (%4 sec) using %5 connections on %6 server(s))")
                              : tr("Nb Missing Article(s): %1/%2 (check done in %3 (%4 sec) "
                                   "using %5 connections on %6 server(s))");
            _cout << summary
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
    , _par2Queue()
    , _dataQueue()
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
    , _articleOwner()
    , _dataArticleBytesUpper()
    , _dataFiles()
    , _nbPar2Articles(0)
    , _nbPar2Answered(0)
    , _nbDataArticles(0)
    , _nbDataFiles(0)
    , _nbDataFilesWithSize(0)
    , _nbMissingPar2Articles(0)
    , _nbMissingDataArticles(0)
    , _par2BlockSize(0)
    , _blockSizeSource()
    , _blockSizeMeasured(false)
    , _articleSizeMin(0)
    , _subjectArticleMin(0)
    , _subjectArticleMax(0)
    , _par2PhaseDone(false)
    , _earlyStop(false)
    , _checkFull(false)
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
                int volumeIdx = -1, dataFileIdx = -1;
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
                    // These bounds belong to this file. Treating them as one
                    // global article size breaks on concatenated NZBs and even
                    // on overlapping ranges from two different payload sizes.
                    NzbDataFile dataFile;
                    dataFile.nbExpectedArticles = nbExpectedArticles;
                    dataFileIdx = _dataFiles.size();
                    ++_nbDataFiles;
                    QRegularExpressionMatch size = sSubjectSizeRegExp.match(subject);
                    if (size.hasMatch()) {
                        ++_nbDataFilesWithSize;
                        qint64 const fileSize = size.captured(1).toLongLong();
                        dataFile.sizeBytes = fileSize;
                        // Equal-sized parts followed by a short tail are common,
                        // but yEnc does not require them. Keep this only as a
                        // rough slice-size estimate; recovery proofs below use
                        // segment@bytes as an upper bound instead.
                        if (nbExpectedArticles > 0) {
                            qint64 const low = ceilDiv(fileSize, nbExpectedArticles);
                            if (low > _subjectArticleMin)
                                _subjectArticleMin = low;
                            if (nbExpectedArticles > 1) {
                                qint64 const high = ceilDiv(fileSize, nbExpectedArticles - 1) - 1;
                                if (_subjectArticleMax <= 0 || high < _subjectArticleMax)
                                    _subjectArticleMax = high;
                            }
                        }
                    }
                    _dataFiles.append(dataFile);
                }

                while (!xmlReader.atEnd()) {
                    QXmlStreamReader::TokenType type = xmlReader.readNext();
                    if (type == QXmlStreamReader::TokenType::EndElement
                        && xmlReader.name().compare(QLatin1String("file")) == 0) {
                        if (isPar2) {
                            _par2Volumes[volumeIdx].nbListedArticles = nbArticles;
                        } else if (dataFileIdx >= 0) {
                            NzbDataFile &dataFile = _dataFiles[dataFileIdx];
                            dataFile.nbListedArticles = nbArticles;
                            // A complete NZB whose encoded bodies add up to less
                            // than the decoded file contradicts itself. Its byte
                            // attributes cannot support an authoritative bound.
                            if (dataFile.nbExpectedArticles > 0
                                && nbArticles >= dataFile.nbExpectedArticles
                                && dataFile.nbListedArticlesWithBytes == nbArticles
                                && dataFile.listedBytesUpper < dataFile.sizeBytes) {
                                dataFile.articleByteBoundsConsistent = false;
                            }
                        }
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
                            } else {
                                _nbMissingDataArticles += absent;
                                if (dataFileIdx >= 0) {
                                    _dataFiles[dataFileIdx].nbMissingArticles += absent;
                                }
                            }
                        }

                        break;
                    } else if (type == QXmlStreamReader::TokenType::StartElement
                               && xmlReader.name().compare(QLatin1String("segment")) == 0) {
                        ++nbArticles;
                        bool bytesOk = false;
                        qint64 const encodedBytes
                                = xmlReader.attributes().value("bytes").toLongLong(&bytesOk);
                        xmlReader.readNext();
                        QString const articleId = QString("<%1>").arg(
                                xmlReader.text().toString());
                        if (isPar2) {
                            _par2Queue.push(articleId);
                            _articleOwner.insert(articleId, volumeIdx);
                            ++_nbPar2Articles;
                        } else {
                            _dataQueue.push(articleId);
                            _articleOwner.insert(articleId, -dataFileIdx - 1);
                            ++_nbDataArticles;
                            if (dataFileIdx >= 0 && bytesOk && encodedBytes > 0) {
                                NzbDataFile &dataFile = _dataFiles[dataFileIdx];
                                ++dataFile.nbListedArticlesWithBytes;
                                dataFile.listedBytesUpper
                                        = saturatedAdd(dataFile.listedBytesUpper, encodedBytes);
                                _dataArticleBytesUpper.insert(articleId, encodedBytes);
                            }
                        }
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
        _nbListedArticles = _par2Queue.size() + _dataQueue.size();
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
    // This global intersection is only an estimate for an otherwise unknown
    // PAR2 slice size. Recovery proofs below never use it: their article
    // bounds remain local to each data file.
    if (_subjectArticleMin > 0) {
        if (_subjectArticleMax > 0 && _subjectArticleMax < _subjectArticleMin) {
            _articleSizeMin = 0;
        } else {
            _articleSizeMin = _subjectArticleMin;
        }
    }

    if (_par2BlockSize <= 0 && _articleSizeMin > 0) {
        // A slice size is not an article size -- PAR2 chooses its own, and
        // ParPar auto-tunes it. This is a placeholder to have something to
        // compute with, and recoveryVerdict() refuses to conclude on it.
        _par2BlockSize   = _articleSizeMin;
        _blockSizeSource = tr("guessed from the article size");
    }
}

int NzbCheck::totalRecoveryBlocks() const
{
    qint64 blocks = 0;
    for (Par2Volume const &vol : _par2Volumes)
        blocks = saturatedAdd(blocks, vol.blocks);
    return boundedBlockCount(blocks);
}

int NzbCheck::usableRecoveryBlocks() const
{
    return likelyUsableBlocks();
}

QPair<int, int> NzbCheck::usableBlockRange() const
{
    qint64 low = 0, high = 0;
    for (Par2Volume const &vol : _par2Volumes) {
        // Guaranteed: only a volume that lost nothing. PAR2 packets may appear
        // in any order and each carries its own checksum, so for a damaged
        // volume the format promises nothing -- every recovery packet could
        // have been in the very articles that went missing. A pro rata is a
        // sensible expectation, not a floor, and a floor is what a proof needs.
        if (vol.isIntact())
            low += vol.blocks;

        // At best the packets all sat in articles that survived, so only a
        // volume that lost everything is certainly worth nothing.
        bool const anythingLeft
                = vol.nbListedArticles > vol.nbMissingListedArticles;
        high += anythingLeft ? vol.blocks : 0;
    }
    return { boundedBlockCount(low), boundedBlockCount(qMax(low, high)) };
}

int NzbCheck::likelyUsableBlocks() const
{
    qint64 blocks = 0;
    for (Par2Volume const &vol : _par2Volumes)
        blocks = saturatedAdd(blocks, vol.usableBlocks());
    return boundedBlockCount(blocks);
}

bool NzbCheck::hasIntactPar2Index() const
{
    // The conventional base .par2 contains the vital packets. A recovery
    // volume cannot stand in for it here: the format requires only a Creator
    // packet in every PAR2 file; repeating Main/FileDesc/IFSC is recommended,
    // not mandatory, and STAT does not inspect packet contents.
    for (Par2Volume const &vol : _par2Volumes) {
        if (!vol.isVolume && vol.isIntact())
            return true;
    }
    return false;
}

QPair<int, int> NzbCheck::damagedBlockRange() const
{
    int const missing = _nbMissingDataArticles;
    if (missing <= 0)
        return { 0, 0 };
    if (_par2BlockSize <= 0)
        return { 1, -1 };

    qint64 damagedMin = 0;
    qint64 damagedMax = 0;
    int    attributed = 0;
    bool   upperBounded = true;
    for (NzbDataFile const &file : _dataFiles) {
        int const perFile = file.nbMissingArticles;
        if (perFile <= 0)
            continue;
        attributed += perFile;

        // With no usable size/count, this file still costs one source slice,
        // but the NZB gives no finite ceiling.
        if (file.sizeBytes <= 0 || file.nbExpectedArticles <= 0) {
            damagedMin = saturatedAdd(damagedMin, 1);
            upperBounded = false;
            continue;
        }

        qint64 missingListedBytesUpper = 0;
        for (qint64 bytes : file.missingArticleBytesUpper)
            missingListedBytesUpper = saturatedAdd(missingListedBytesUpper, bytes);

        // yEnc permits every part to have a different size. The only lower
        // bound available without downloading =ypart is what cannot fit in all
        // articles that may still be present. segment@bytes counts the encoded
        // body, so it safely overstates, never understates, decoded payload.
        qint64 minBytes = 1;
        int const possiblePresent = file.nbListedArticles - file.nbMissingListedArticles;
        int const possiblePresentWithBytes
                = file.nbListedArticlesWithBytes - file.nbMissingListedArticlesWithBytes;
        if (file.articleByteBoundsConsistent && possiblePresent >= 0
            && possiblePresentWithBytes == possiblePresent) {
            qint64 const possiblePresentBytes
                    = file.listedBytesUpper >= missingListedBytesUpper
                              ? file.listedBytesUpper - missingListedBytesUpper
                              : 0;
            if (possiblePresentBytes < file.sizeBytes)
                minBytes = file.sizeBytes - possiblePresentBytes;
        }
        qint64 const fileMin = qMax(qint64(1), ceilDiv(minBytes, _par2BlockSize));
        damagedMin = saturatedAdd(damagedMin, fileMin);

        // A file occupies sum(ceil(fileSize/B)) source slices, never a share of
        // ceil(sum(fileSizes)/B). That per-file cap is definitive even when a
        // one-article file gives no finite article-payload upper bound.
        qint64 const fileBlocks = ceilDiv(file.sizeBytes, _par2BlockSize);
        qint64 fileMax = fileBlocks;
        bool const everyMissingArticleHasAnUpperBound
                = file.nbMissingListedArticles == file.nbMissingArticles
                  && file.nbMissingListedArticlesWithBytes == file.nbMissingListedArticles;
        if (file.articleByteBoundsConsistent && everyMissingArticleHasAnUpperBound) {
            qint64 scattered = 0;
            for (qint64 bytes : file.missingArticleBytesUpper) {
                scattered = saturatedAdd(
                        scattered, maxBlocksTouched(bytes, _par2BlockSize));
            }
            fileMax = qMin(fileBlocks, scattered);
        }
        damagedMax = saturatedAdd(damagedMax, qMax(fileMin, fileMax));
    }

    // This is only reachable for an article that was not parsed from the NZB.
    // Its owner is unknown, so all such losses together prove one block and no
    // upper bound -- never manufacture a file boundary for them.
    if (attributed < missing) {
        damagedMin = saturatedAdd(damagedMin, 1);
        upperBounded = false;
    }

    damagedMin = qMax(damagedMin, qint64(1));
    if (!upperBounded)
        return { boundedBlockCount(damagedMin), -1 };
    damagedMax = qMax(damagedMin, damagedMax);
    return { boundedBlockCount(damagedMin), boundedBlockCount(damagedMax) };
}

qint64 NzbCheck::totalDataBlocks() const
{
    if (!dataSizeIsComplete() || _par2BlockSize <= 0)
        return -1;

    qint64 blocks = 0;
    for (NzbDataFile const &file : _dataFiles)
        blocks = saturatedAdd(blocks, ceilDiv(file.sizeBytes, _par2BlockSize));
    return blocks > 0 ? blocks : -1;
}

QPair<double, double> NzbCheck::redundancyPercent() const
{
    qint64 const sourceBlocks = totalDataBlocks();
    if (sourceBlocks <= 0)
        return { -1.0, -1.0 };
    return { 100.0 * usableRecoveryBlocks() / sourceBlocks,
             100.0 * totalRecoveryBlocks() / sourceBlocks };
}

QPair<double, double> NzbCheck::redundancyPercentRange() const
{
    qint64 const sourceBlocks = totalDataBlocks();
    if (sourceBlocks <= 0)
        return { -1.0, -1.0 };
    QPair<int, int> const usable = usableBlockRange();
    return { 100.0 * usable.first / sourceBlocks, 100.0 * usable.second / sourceBlocks };
}

NzbCheck::Recovery NzbCheck::recoveryVerdict() const
{
    if (_nbMissingDataArticles <= 0)
        return Recovery::NotNeeded;

    // Facts, in the sense that no estimate stands behind them.
    if (_par2Volumes.isEmpty())
        return Recovery::NoPar2AtAll;
    if (totalRecoveryBlocks() <= 0)
        return Recovery::NoRecoveryBlocks;
    QPair<int, int> const usable = usableBlockRange();
    if (usable.second <= 0)
        return Recovery::NoUsableBlocks;

    // Everything below rests on the slice size, and a slice size we inferred
    // bounds nothing: PAR2 picks its own, ParPar auto-tunes it, and a smaller
    // one means more damaged blocks with no ceiling we can name. So the honest
    // answer is the middle one -- try the repair -- in either direction.
    if (!_blockSizeMeasured)
        return Recovery::LayoutDependent;

    QPair<int, int> const damaged = damagedBlockRange();
    // Proof, not preference: the worst damage still fits inside the fewest
    // blocks we are sure of, or the least damage exceeds the most blocks we
    // could possibly have. Anything in between is genuinely undecided, and so
    // is a damage we could not bound from above.
    if (damaged.second >= 0 && damaged.second <= usable.first) {
        // STAT proves that the conventional base file can be downloaded, not
        // that it contains Main/FileDesc/IFSC. Their presence is recommended by
        // PAR2, not required, so even this favourable case remains probabilistic.
        if (hasIntactPar2Index())
            return Recovery::ProbablyRecoverable;
        return Recovery::LayoutDependent;
    }
    if (damaged.first > usable.second)
        return Recovery::Impossible;
    return Recovery::LayoutDependent;
}

NzbCheck::CheckStatus NzbCheck::checkStatus() const
{
    if (_unusable)
        return CheckStatus::Inconclusive;

    // Anything short of a full sweep means the answer cannot be trusted -- with
    // one exception: a sweep we cut short on purpose, having already proved the
    // post beyond repair. That is a verdict, not a gap.
    if (!_earlyStop && _nbListedArticles > 0 && _nbCheckedArticles < _nbListedArticles)
        return CheckStatus::Inconclusive;

    if (_nbMissingArticles == 0)
        return CheckStatus::Complete;

    Recovery const recovery = recoveryVerdict();
    if (recovery == Recovery::Impossible || recovery == Recovery::NoPar2AtAll
        || recovery == Recovery::NoRecoveryBlocks || recovery == Recovery::NoUsableBlocks)
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

    QPair<int, int> const usableRange = usableBlockRange();
    if (usableRange.first == usableRange.second)
        _cout << tr("  Recovery blocks: %1 of %2 still usable").arg(usableRange.first).arg(total)
              << "\n";
    else
        // Three numbers because they answer three questions: what can be
        // proven, what is likely, and what is not ruled out.
        _cout << tr("  Recovery blocks: %1 guaranteed, %2 likely, %3 at best, of %4")
                     .arg(usableRange.first)
                     .arg(likelyUsableBlocks())
                     .arg(usableRange.second)
                     .arg(total)
              << "\n";
    QPair<double, double> const redundancy      = redundancyPercent();
    QPair<double, double> const redundancyRange = redundancyPercentRange();
    if (redundancy.first >= 0.0) {
        if (redundancyRange.first == redundancyRange.second)
            _cout << tr("  Redundancy: %1% of the source slices can be rebuilt (%2% when it "
                        "was posted)")
                         .arg(redundancy.first, 0, 'f', 1)
                         .arg(redundancy.second, 0, 'f', 1)
                  << "\n";
        else
            _cout << tr("  Redundancy: %1% of source slices guaranteed, %2% likely, %3% at "
                        "best (%4% when posted)")
                         .arg(redundancyRange.first, 0, 'f', 1)
                         .arg(redundancy.first, 0, 'f', 1)
                         .arg(redundancyRange.second, 0, 'f', 1)
                         .arg(redundancy.second, 0, 'f', 1)
                  << "\n";
    }
    _cout << (hasIntactPar2Index()
                      ? tr("  PAR2 metadata: the conventional base index was verified in full, "
                           "but STAT cannot prove which packets it contains")
                      : tr("  PAR2 metadata: not proven by the nzb. Recovery volumes are not "
                           "required to repeat the vital packets, even when intact"))
          << "\n";

    if (_nbMissingDataArticles > 0) {
        QPair<int, int> const damaged = damagedBlockRange();
        if (damaged.second < 0)
            _cout << tr("  Damaged blocks: at least %1, with no upper bound - the nzb does not "
                        "provide enough trustworthy file and segment sizes")
                         .arg(damaged.first)
                  << "\n";
        else
            _cout << tr("  Damaged blocks: %1 to %2, depending on article and slice layout "
                        "(block size: %3 bytes, %4)")
                         .arg(damaged.first)
                         .arg(damaged.second)
                         .arg(_par2BlockSize)
                         .arg(_blockSizeSource.isEmpty() ? tr("declared") : _blockSizeSource)
                  << "\n";
        if (!_blockSizeMeasured)
            _cout << tr("  The slice size was inferred, not read, so this analysis will not "
                        "declare the post dead. Pass --par2_block_size to get a firm answer.")
                  << "\n";
    }

    // The nzb says which files exist, never which of them the PAR2 set actually
    // covers. ngPost itself copies the visible .nfo in after the PAR2 files are
    // generated, so that one is in the post but outside the recovery set: if
    // its article is the one that went missing, no amount of blocks brings it
    // back. Nothing in the nzb lets a reader tell, so the assumption is stated
    // rather than silently made.
    if (_nbMissingDataArticles > 0)
        _cout << tr("  Assumes every non-PAR2 file is covered by the recovery set. A file added "
                    "after the PAR2 files were built -- a .nfo kept visible, for instance -- is "
                    "not, and a loss there cannot be repaired.")
              << "\n";

    switch (recoveryVerdict()) {
    case Recovery::NotNeeded:
        _cout << tr("  Verdict: COMPLETE - no data article is missing") << "\n";
        if (_nbMissingPar2Articles > 0 && total > 0) {
            if (usableRange.first == usableRange.second)
                _cout << tr("  Warning: %1% of the recovery blocks are gone; the data is "
                            "intact today but it is less protected than it was")
                             .arg(100.0 * (total - usable) / total, 0, 'f', 1)
                      << "\n";
            else
                _cout << tr("  Warning: %1% to %2% of the recovery blocks may be gone (%3% "
                            "likely); the data is intact today but less protected")
                             .arg(100.0 * (total - usableRange.second) / total, 0, 'f', 1)
                             .arg(100.0 * (total - usableRange.first) / total, 0, 'f', 1)
                             .arg(100.0 * (total - usable) / total, 0, 'f', 1)
                      << "\n";
        }
        break;
    case Recovery::ProbablyRecoverable:
        _cout << tr("  Verdict: PROBABLY RECOVERABLE - the remaining blocks cover the loss, "
                    "and the conventional base index is available, but STAT cannot verify its "
                    "vital packets. Try the repair")
              << "\n";
        break;
    case Recovery::LayoutDependent:
        _cout << tr("  Verdict: INDETERMINATE - the nzb does not expose enough packet and "
                    "slice information to prove recovery or failure. Try the repair before "
                    "re-posting")
              << "\n";
        break;
    case Recovery::Impossible:
        _cout << tr("  Verdict: UNRECOVERABLE - the loss exceeds the remaining blocks even at "
                    "best. This post has to be re-posted from the source")
              << "\n";
        break;
    case Recovery::NoPar2AtAll:
        _cout << tr("  Verdict: UNRECOVERABLE - data is missing and the nzb carries no PAR2 "
                    "file to rebuild it with")
              << "\n";
        break;
    case Recovery::NoRecoveryBlocks:
        _cout << tr("  Verdict: UNRECOVERABLE - the PAR2 files carry no recovery block at all, "
                    "only the index; they can tell you what is broken, not mend it")
              << "\n";
        break;
    case Recovery::NoUsableBlocks:
        _cout << tr("  Verdict: UNRECOVERABLE - data is missing and every PAR2 volume lost "
                    "every one of its articles")
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
    // A run cut short knows a floor, not a total: it stopped asking.
    articles[QStringLiteral("missingIsALowerBound")] = _earlyStop;

    QString recoveryName;
    switch (recoveryVerdict()) {
    case Recovery::NotNeeded:       recoveryName = QStringLiteral("notNeeded");       break;
    case Recovery::ProbablyRecoverable:
        recoveryName = QStringLiteral("probablyRecoverable");
        break;
    case Recovery::LayoutDependent: recoveryName = QStringLiteral("layoutDependent"); break;
    case Recovery::Impossible:      recoveryName = QStringLiteral("impossible");      break;
    case Recovery::NoPar2AtAll:     recoveryName = QStringLiteral("noPar2AtAll");     break;
    case Recovery::NoRecoveryBlocks: recoveryName = QStringLiteral("noRecoveryBlocks"); break;
    case Recovery::NoUsableBlocks:  recoveryName = QStringLiteral("noUsableBlocks");  break;
    }

    QPair<int, int> const damaged = damagedBlockRange();
    QJsonObject par2;
    par2[QStringLiteral("volumes")]           = _par2Volumes.size();
    par2[QStringLiteral("blocksTotal")]       = totalRecoveryBlocks();
    QPair<int, int> const usableRange = usableBlockRange();
    par2[QStringLiteral("blocksUsable")]          = likelyUsableBlocks();
    par2[QStringLiteral("blocksUsableGuaranteed")] = usableRange.first;
    par2[QStringLiteral("blocksUsableMax")]       = usableRange.second;
    bool const baseIndexAvailable = hasIntactPar2Index();
    // STAT never downloads packet contents. Preserve the old field, but make
    // its epistemic meaning honest and expose the weaker fact separately.
    par2[QStringLiteral("metadataAvailable")] = false;
    par2[QStringLiteral("metadataProven")] = false;
    par2[QStringLiteral("metadataAssumedFromBaseIndex")] = baseIndexAvailable;
    par2[QStringLiteral("baseIndexAvailable")] = baseIndexAvailable;
    par2[QStringLiteral("metadataSource")] = baseIndexAvailable
                                                       ? QStringLiteral("assumedFromIntactBaseIndex")
                                                       : QStringLiteral("notProvenFromNzb");
    par2[QStringLiteral("blockSize")]         = _par2BlockSize;
    par2[QStringLiteral("blockSizeMeasured")] = _blockSizeMeasured;
    // Coverage of the data files by the recovery set is assumed, not read: the
    // nzb does not record which files the PAR2 volumes were built over.
    par2[QStringLiteral("coverageAssumed")]   = true;
    par2[QStringLiteral("blockSizeSource")]   = _blockSizeSource.isEmpty()
                                                     ? QStringLiteral("declared")
                                                     : _blockSizeSource;
    par2[QStringLiteral("damagedBlocksMin")]  = damaged.first;
    par2[QStringLiteral("damagedBlocksMax")]  = damaged.second;
    par2[QStringLiteral("recovery")]          = recoveryName;
    QPair<double, double> const redundancy = redundancyPercent();
    if (redundancy.first >= 0.0) {
        par2[QStringLiteral("redundancyPercent")]         = redundancy.first;
        par2[QStringLiteral("redundancyPercentLikely")]   = redundancy.first;
        par2[QStringLiteral("redundancyPercentOriginal")] = redundancy.second;
        QPair<double, double> const range = redundancyPercentRange();
        par2[QStringLiteral("redundancyPercentGuaranteed")] = range.first;
        par2[QStringLiteral("redundancyPercentMax")]        = range.second;
    }

    QJsonObject root;
    root[QStringLiteral("nzb")]         = QFileInfo(_nzbPath).absoluteFilePath();
    root[QStringLiteral("status")]      = statusName;
    root[QStringLiteral("exitCode")]    = static_cast<int>(status);
    root[QStringLiteral("articles")]    = articles;
    root[QStringLiteral("par2")]        = par2;
    root[QStringLiteral("servers")]     = nbCheckingServers();
    root[QStringLiteral("connections")] = _nbCons;
    root[QStringLiteral("durationMs")]  = durationMs;
    root[QStringLiteral("stoppedEarly")] = _earlyStop;
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
