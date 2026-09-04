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
        return 0; // nothing says how many articles this volume should have
    int const present = nbExpectedArticles - nbMissingArticles;
    if (present <= 0)
        return 0;
    // Pro rata, less one block per missing article. Recovery packets do not
    // stop politely on article boundaries: a lost article takes its share of
    // them plus, at worst, the one it cuts in half.
    qint64 const share = (static_cast<qint64>(blocks) * present) / nbExpectedArticles;
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
    , _dataFileMissing()
    , _dataFileLastArticle()
    , _dataFileLastMissing()
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
    , _articleSizeMax(0)
    , _subjectArticleMin(0)
    , _subjectArticleMax(0)
    , _dataSizeBytes(0)
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
                int volumeIdx = -1, dataFileIdx = -1, highestSegment = 0;
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
                    dataFileIdx = _dataFileMissing.size();
                    _dataFileMissing.append(0);
                    _dataFileLastArticle.append(QString());
                    _dataFileLastMissing.append(false);
                    ++_nbDataFiles;
                    QRegularExpressionMatch size = sSubjectSizeRegExp.match(subject);
                    if (size.hasMatch()) {
                        ++_nbDataFilesWithSize;
                        qint64 const fileSize = size.captured(1).toLongLong();
                        _dataSizeBytes += fileSize;
                        // n articles cover the file, the last one short, so
                        // (n-1)*A < size <= n*A. That pins A between
                        // ceil(size/n) and ceil(size/(n-1))-1. Every file
                        // constrains the same A, so the bounds intersect.
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
                            } else {
                                _nbMissingDataArticles += absent;
                                if (dataFileIdx >= 0) {
                                    _dataFileMissing[dataFileIdx] += absent;
                                    // The nzb stops short of the announced
                                    // count, so the final part is one of the
                                    // ones it does not carry.
                                    if (highestSegment < nbExpectedArticles)
                                        _dataFileLastMissing[dataFileIdx] = true;
                                }
                            }
                        }

                        break;
                    } else if (type == QXmlStreamReader::TokenType::StartElement
                               && xmlReader.name().compare(QLatin1String("segment")) == 0) {
                        ++nbArticles;
                        int const segmentNumber
                                = xmlReader.attributes().value("number").toInt();
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
                            // Segments need not be listed in order, so the last
                            // one is the highest number, not the last seen.
                            if (segmentNumber > highestSegment) {
                                highestSegment = segmentNumber;
                                if (dataFileIdx >= 0)
                                    _dataFileLastArticle[dataFileIdx] = articleId;
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
    // What the subjects say beats what the configuration says: the latter
    // describes how we would post, not how this nzb was actually made.
    if (_subjectArticleMin > 0) {
        if (_subjectArticleMax > 0 && _subjectArticleMax < _subjectArticleMin) {
            // The files cannot all share one article size -- an nzb built by
            // concatenating posts, or a file posted with different settings.
            // Contradictory constraints bound nothing, and collapsing them to
            // a point would turn a contradiction into a certainty.
            _articleSizeMin = 0;
            _articleSizeMax = 0;
        } else {
            _articleSizeMin = _subjectArticleMin;
            _articleSizeMax = _subjectArticleMax > 0 ? _subjectArticleMax : _subjectArticleMin;
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
    int blocks = 0;
    for (Par2Volume const &vol : _par2Volumes)
        blocks += vol.blocks;
    return blocks;
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
        if (vol.nbMissingArticles <= 0)
            low += vol.blocks;

        // At best the packets all sat in articles that survived, so only a
        // volume that lost everything is certainly worth nothing.
        bool const anythingLeft = vol.nbExpectedArticles <= 0
                                  || vol.nbMissingArticles < vol.nbExpectedArticles;
        high += anythingLeft ? vol.blocks : 0;
    }
    return { static_cast<int>(low), static_cast<int>(qMax(low, high)) };
}

int NzbCheck::likelyUsableBlocks() const
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
    // Without an article size nothing bounds the damage from above: a smaller
    // slice size means more blocks touched with no ceiling to name. At least
    // one block is gone, and that is the whole of what we know.
    if (_par2BlockSize <= 0 || _articleSizeMin <= 0 || _articleSizeMax <= 0)
        return { 1, -1 };

    // Best case: within one file the losses are contiguous, so they span a
    // single run of bytes. Across files they cannot be: PAR2 slices restart at
    // every file boundary, so two half-block losses in two files cost two
    // blocks, not one. The optimistic bound is therefore a sum over files.
    // The optimistic end takes the smallest article we cannot rule out: a
    // short last article really does damage less than a full one.
    qint64 clustered = 0;
    int    attributed = 0;
    for (int i = 0; i < _dataFileMissing.size(); ++i) {
        int const perFile = _dataFileMissing.at(i);
        if (perFile <= 0)
            continue;
        attributed += perFile;
        // The last article of a file holds the remainder, which may be a single
        // byte. When it is among the losses it costs one block, not a full
        // article's worth -- counting it as a full one is what turned a
        // one-block loss into four and sent a repairable post to be re-posted.
        bool const lastGone = _dataFileLastMissing.value(i, false);
        int const  fullOnes = lastGone ? perFile - 1 : perFile;
        qint64 blocks = ceilDiv(static_cast<qint64>(fullOnes) * _articleSizeMin, _par2BlockSize);
        if (lastGone)
            blocks += 1;
        clustered += qMax(blocks, qint64(1));
    }
    // Anything we could not pin to a file still costs at least one block each,
    // and at most the run they would span together.
    if (attributed < missing)
        clustered += ceilDiv(static_cast<qint64>(missing - attributed) * _articleSizeMin,
                             _par2BlockSize);

    // Worst case: a range of A bytes at an arbitrary offset spans
    // floor((A-1)/B) + 2 blocks -- the whole ones it covers plus the two it
    // straddles at either end.
    // ... and the pessimistic end the largest, since the same uncertainty cuts
    // both ways and must not be spent twice in our favour.
    qint64 const perArticle = (_articleSizeMax - 1) / _par2BlockSize + 2;
    qint64 scattered = static_cast<qint64>(missing) * perArticle;

    // Neither can exceed the number of blocks the data occupies -- but only a
    // complete size can cap anything; a partial one would cap too low and
    // understate the damage.
    if (dataSizeIsComplete()) {
        qint64 const dataBlocks = ceilDiv(_dataSizeBytes, _par2BlockSize);
        clustered = qMin(clustered, dataBlocks);
        scattered = qMin(scattered, dataBlocks);
    }
    clustered = qMax(clustered, qint64(1));
    return { static_cast<int>(clustered), static_cast<int>(qMax(clustered, scattered)) };
}

QPair<double, double> NzbCheck::redundancyPercent() const
{
    // Blocks alone say nothing: forty blocks over a 200 MB post is generous,
    // over a 40 GB one it is nothing. What predicts whether a post survives
    // Usenet is the share of itself it can rebuild.
    // A partial sum would understate the denominator and overstate the answer,
    // so the figure is only offered when every data file announced its size.
    if (!dataSizeIsComplete() || _par2BlockSize <= 0)
        return { -1.0, -1.0 };
    double const data = static_cast<double>(_dataSizeBytes);
    return { 100.0 * usableRecoveryBlocks() * _par2BlockSize / data,
             100.0 * totalRecoveryBlocks() * _par2BlockSize / data };
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
        // Blocks are useless without the packets that say what to rebuild.
        // Not finding a wholly intact PAR2 file does not prove those are gone
        // -- the format repeats them -- but it forbids promising they are there.
        if (hasIntactPar2Metadata())
            return Recovery::Certain;
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
    QPair<double, double> const redundancy = redundancyPercent();
    if (redundancy.first >= 0.0)
        _cout << tr("  Redundancy: %1% of the data can still be rebuilt (%2% when it was posted)")
                     .arg(redundancy.first, 0, 'f', 1)
                     .arg(redundancy.second, 0, 'f', 1)
              << "\n";
    // PAR2 repeats its metadata packets in every file and each packet carries
    // its own checksum, so a damaged file may still yield them. Not finding an
    // intact file proves nothing -- it only stops us promising anything.
    _cout << (hasIntactPar2Metadata()
                      ? tr("  PAR2 metadata: available, so a repair knows what to rebuild")
                      : tr("  PAR2 metadata: no PAR2 file is wholly intact, so it may or may "
                           "not still be readable"))
          << "\n";

    if (_nbMissingDataArticles > 0) {
        QPair<int, int> const damaged = damagedBlockRange();
        if (damaged.second < 0)
            _cout << tr("  Damaged blocks: at least %1, with no upper bound - the nzb does not "
                        "say how big an article is")
                         .arg(damaged.first)
                  << "\n";
        else
            _cout << tr("  Damaged blocks: %1 to %2, depending on how the losses are spread "
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
    case Recovery::Certain:         recoveryName = QStringLiteral("certain");         break;
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
    par2[QStringLiteral("metadataAvailable")] = hasIntactPar2Metadata();
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
        par2[QStringLiteral("redundancyPercentOriginal")] = redundancy.second;
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
