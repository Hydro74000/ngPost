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

#ifndef NZBCHECK_H
#define NZBCHECK_H
#include <climits>

#include <QCommandLineOption>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStack>
#include <QString>
#include <QTextStream>
#include <QTimer>
#include <QVector>
struct NntpServerParams;
class NntpCheckCon;

//! One PAR2 file of the post: either the conventional metadata base .par2, or
//! a .volNN+MM.par2, which carries MM recovery blocks.
struct Par2Volume
{
    QString subject;
    int     blocks                    = 0; //!< recovery blocks, from the .volNN+MM name
    bool    isVolume                  = false; //!< false for the metadata-only base .par2
    int     nbExpectedArticles        = 0;
    int     nbListedArticles          = 0;
    int     nbCheckedArticles         = 0;
    int     nbMissingArticles         = 0;
    int     nbMissingListedArticles = 0;

    bool isIntact() const
    {
        return nbExpectedArticles > 0 && nbListedArticles >= nbExpectedArticles
               && nbCheckedArticles >= nbListedArticles && nbMissingArticles == 0;
    }

    //! Recovery blocks still usable out of this file.
    //!
    //! PAR2 is a sequence of independently checksummed packets, and par2cmdline
    //! scans a file for the ones it can still read, so a volume missing one of
    //! its twenty articles is not worth zero -- it is worth roughly nineteen
    //! twentieths of its blocks. Counting it as zero (as ngPostEx does) declares
    //! repairable posts dead. Rounding down keeps the answer conservative.
    int usableBlocks() const;
};

//! Facts and bounds that belong to one data file. The NZB segment byte count is
//! the encoded article-body size; it is therefore only an upper bound on the
//! decoded payload. It must never be treated as an exact article size.
struct NzbDataFile
{
    int     nbExpectedArticles                 = 0;
    int     nbListedArticles                   = 0;
    int     nbListedArticlesWithBytes          = 0;
    int     nbMissingArticles                  = 0;
    int     nbMissingListedArticles            = 0;
    int     nbMissingListedArticlesWithBytes = 0;
    qint64  sizeBytes                          = 0;
    qint64  listedBytesUpper                   = 0;
    QVector<qint64> missingArticleBytesUpper;
    bool    articleByteBoundsConsistent        = true;
};

#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
#define MB_FLUSH flush
#else
#define MB_FLUSH Qt::flush
#endif

class NzbCheck : public QObject
{
    Q_OBJECT

private:
    // Every NZB ngPost writes ends its <file subject> with the file size:
    //     [001/846] - "backup.part01.rar" yEnc (1/25) 734003200
    // The historical pattern anchored the article count on the END of the
    // subject ("...\\(\\d+/(\\d+)\\)$"), so it never matched a single subject
    // ngPost itself produces: nbExpectedArticles stayed 0 and the "articles
    // missing from the nzb" check in parseNzb() was dead code. Match the yEnc
    // counter wherever it sits instead -- that also covers nyuu, ParPar and
    // NewsUP, whose subjects carry their own trailing fields.
    static constexpr const char *sNntpArticleYencSubjectStrRegExp
        = "yEnc\\s+\\(\\d+/(\\d+)\\)";

    QString _nzbPath;
    //! PAR2 articles are checked first, so the recovery capacity is known
    //! before a single data article is spent on.
    QStack<QString> _par2Queue;
    QStack<QString> _dataQueue;

    QTextStream _cout; //!< stream for stdout
    QTextStream _cerr; //!< stream for stderr

    int _nbTotalArticles;  //!< expected total, including articles absent from the NZB
    int _nbListedArticles; //!< Message-IDs present in the NZB and therefore checkable
    int _nbMissingArticles;
    int _nbCheckedArticles;
    int _nbMissingArticlesInNzb; //!< articles the nzb does not even list (subset of _nbMissingArticles)

    QList<NntpServerParams *> _nntpServers; //!< the servers parameters

    ushort _debug;

    QSet<NntpCheckCon *> _connections;

    bool _dispProgressBar;
    QTimer _progressbarTimer; //!< timer to refresh the upload information (progressbar bar, avg. speed)
    const int _refreshRate; //!< refresh rate

    bool _quietMode;
    bool _jsonOutput; //!< --check_json: one machine readable object instead of the human report
    bool _unusable;   //!< the check could not even be attempted

    QElapsedTimer _timeStart;
    int _nbCons;
    int    _socketTimeOut; //!< ms a connection may stay silent before we drop it
    ushort _maxRetries;    //!< reconnections allowed to a single connection

    // ---- PAR2 recovery analysis ----
    QVector<Par2Volume> _par2Volumes;
    //! Which file each article belongs to, in one structure rather than two:
    //! >= 0 is an index into _par2Volumes, < 0 is -(data file index + 1).
    //! PAR2 slices restart at every file boundary, so knowing a loss happened
    //! is not enough -- the damage has to be counted file by file.
    QHash<QString, int> _articleOwner;
    //! Encoded body size from segment@bytes. For data articles only, and used
    //! solely as an upper bound on the decoded payload.
    QHash<QString, qint64> _dataArticleBytesUpper;
    QVector<NzbDataFile> _dataFiles;

    //! -1 would collide with data file 0, which encodes as -(0)-1.
    static constexpr int kUnknownOwner = INT_MIN;
    int _nbPar2Articles;
    int _nbPar2Answered; //!< PAR2 articles the server has actually answered on
    int _nbDataArticles;
    int _nbDataFiles;         //!< data files in the nzb
    int _nbDataFilesWithSize; //!< of which announce their size in the subject
    int _nbMissingPar2Articles;
    int _nbMissingDataArticles;

    qint64  _par2BlockSize;    //!< 0 while unknown
    QString _blockSizeSource;  //!< how it was obtained, for the report
    bool    _blockSizeMeasured; //!< false when we had to guess it from the nzb
    //! A global article payload is retained only to estimate an unknown PAR2
    //! slice size. It is never evidence for a recovery verdict: proofs use the
    //! per-file bounds in _dataFiles.
    qint64 _articleSizeMin;
    qint64 _subjectArticleMin; //!< tightest lower bound the subjects give
    qint64 _subjectArticleMax; //!< tightest upper bound they give, 0 if none

    bool _par2PhaseDone; //!< every PAR2 article has been handed out
    bool _earlyStop;     //!< the post is provably beyond repair; stop asking
    bool _checkFull;     //!< --check_full: never stop early

    static const int sDefaultRefreshRate = 200; //!< how often shall we refresh the progressbar bar?
    static const int sprogressbarBarWidth = 50;
    static const QRegularExpression sNntpArticleYencSubjectRegExp;

public slots:
    void onDisconnected(NntpCheckCon *con);
    void onRefreshprogressbarBar();

public:
    //! Outcome of a --check run. The value IS the process exit code, so it has
    //! to stay small and stable: scripts branch on it. (The pre-5.6 exit code
    //! was the raw number of missing articles, which POSIX truncated modulo
    //! 256 -- exactly 256 missing articles reported success.)
    enum class CheckStatus : int
    {
        Complete      = 0, //!< every article listed in the nzb is on the server
        Missing       = 1, //!< articles are missing, the PAR2 blocks can cover them
        Unrecoverable = 2, //!< missing beyond what the PAR2 volumes can repair
        Inconclusive  = 3  //!< the check could not be completed, the result means nothing
    };

    //! Where the missing data sits relative to the recovery blocks left.
    //!
    //! An article covers a contiguous byte range, so how many PAR2 blocks it
    //! damages depends on where that range falls against the block boundaries,
    //! which the nzb does not say. The answer is therefore a range, and when
    //! the recovery capacity falls inside it the honest verdict is "it depends
    //! on how the losses are spread" rather than a flat yes or no.
    enum class Recovery
    {
        NotNeeded,       //!< no data article is missing
        ProbablyRecoverable, //!< blocks suffice; vital metadata is only conventional
        LayoutDependent, //!< the estimates straddle the answer: try the repair
        Impossible,      //!< not covered even at the best end of every estimate
        NoPar2AtAll,      //!< the nzb lists no PAR2 file: a fact, not an estimate
        NoRecoveryBlocks, //!< PAR2 files, but not one recovery block between them
        NoUsableBlocks    //!< every PAR2 volume lost every one of its articles
    };

    NzbCheck();
    ~NzbCheck();

    int parseNzb();
    void checkPost();
    int nbCheckingServers();

    // For ngPost integration
    inline int parseNzb(const QString &nzbPath);
    inline void checkPost(const QList<NntpServerParams *> &nntpServers);
    inline void setDispProgressBar(bool display);
    inline void setQuiet(bool quiet);
    inline void setJsonOutput(bool json);
    inline void setCheckFull(bool full);
    inline bool stoppedEarly() const;
    inline void setSocketTimeOut(int ms);
    inline void setMaxRetries(ushort nb);
    inline int socketTimeOut() const;
    inline ushort maxRetries() const;
    inline void setPar2BlockSize(qint64 bytes, const QString &source);
    inline void setArticleSize(qint64 bytes);

    inline void missingArticle(const QString &article);
    inline QString getNextArticle();
    //! Hand back an article whose answer never came, so a dropped connection
    //! does not silently remove it from the run.
    inline void requeueArticle(const QString &article);
    inline bool hasArticlesLeft() const;
    //! True when getNextArticle() returned nothing only because the PAR2 phase
    //! has not closed yet -- there is work left, just not yet handable out.
    inline bool waitingForPar2() const;
    //! \a article is the one that just got its answer, whatever that answer was.
    inline void articleChecked(const QString &article);
    //! True when this article belongs to a PAR2 file rather than to the data.
    inline bool isPar2Article(const QString &article) const;

    //! The check cannot even be attempted (nzb unreadable, no server able to
    //! run it). Reports it the way a finished check reports itself, so a caller
    //! gets a verdict -- and a json object -- either way.
    void reportUnusable(const QString &reason);

    inline int nbMissingArticles() const;
    CheckStatus checkStatus() const;

    // ---- PAR2 recovery analysis ----
    int  totalRecoveryBlocks() const;
    int  usableRecoveryBlocks() const;
    //! Every article of the conventional base .par2 was verified by STAT. This
    //! does not prove which PAR2 packets it contains.
    bool hasIntactPar2Index() const;
    //! True when every data file announced its size.
    inline bool dataSizeIsComplete() const;
    //! True when the PAR2 slice size was told to us rather than inferred. Only
    //! then may the analysis assert that something is beyond repair.
    inline bool blockSizeIsMeasured() const;
    //! Blocks the missing data articles damage, at the best and worst ends of
    //! everything we are unsure about: {clustered and small, scattered and big}.
    //! The upper end is -1 when nothing bounds it -- without an article size
    //! there is no ceiling to name, and a missing ceiling is not a high one.
    QPair<int, int> damagedBlockRange() const;
    //! {guaranteed, at best}. The guaranteed end counts only volumes that lost
    //! nothing: PAR2 packets may sit anywhere in a file and carry their own
    //! checksum, so for a damaged volume the format promises nothing at all --
    //! every recovery packet could have been in the articles that went missing.
    QPair<int, int> usableBlockRange() const;
    //! What a damaged volume most likely still holds, pro rata of the articles
    //! that survived. An expectation, shown to the reader, never used to prove
    //! anything.
    int likelyUsableBlocks() const;
    //! Number of PAR2 source slices occupied by all data files. Each file is
    //! rounded separately because source-slice numbering restarts at its
    //! boundary. Negative when the NZB does not announce every file size.
    qint64 totalDataBlocks() const;
    //! Share of source slices covered by likely usable recovery blocks and by
    //! the original block count. Negative when the denominator is unknown.
    QPair<double, double> redundancyPercent() const;
    //! The same percentage at the guaranteed and best-case ends.
    QPair<double, double> redundancyPercentRange() const;
    Recovery recoveryVerdict() const;
    inline int exitCode() const;
    inline bool debugMode() const;
    inline void setDebug(ushort level);

    inline void log(const QString &aMsg);
    inline void log(const char *aMsg);
    inline void log(const std::string &aMsg);
    inline void error(const QString &aMsg);
    inline void error(const char *aMsg);
    inline void error(const std::string &aMsg);

private:
    //! Weigh what is known right now against the redundancy left, and stop the
    //! run if it is provably beyond repair. Called on every loss and when the
    //! PAR2 phase closes, because the answer is a function of the state and not
    //! of the event that happened to reveal it.
    inline void _reevaluateEarlyStop();

    void _printJsonReport(qint64 durationMs, const QString &error = QString());
    void _printRecoveryAnalysis();
    //! Settle _par2BlockSize / _articleSize once parsing is done, and record
    //! where each value came from so the report can own up to a guess.
    void _resolveSizes();
};

int NzbCheck::parseNzb(const QString &nzbPath)
{
    _nzbPath = nzbPath;
    return parseNzb();
}

void NzbCheck::checkPost(const QList<NntpServerParams *> &nntpServers)
{
    _nntpServers = nntpServers;
    checkPost();
}

void NzbCheck::setDispProgressBar(bool display)
{
    _dispProgressBar = display;
}
void NzbCheck::setQuiet(bool quiet)
{
    _quietMode = quiet;
}
void NzbCheck::setJsonOutput(bool json)
{
    _jsonOutput = json;
}
void NzbCheck::setCheckFull(bool full)
{
    _checkFull = full;
}
bool NzbCheck::stoppedEarly() const
{
    return _earlyStop;
}
bool NzbCheck::isPar2Article(const QString &article) const
{
    return _articleOwner.value(article, -1) >= 0;
}
bool NzbCheck::blockSizeIsMeasured() const
{
    return _blockSizeMeasured;
}
bool NzbCheck::dataSizeIsComplete() const
{
    return _nbDataFiles > 0 && _nbDataFilesWithSize == _nbDataFiles;
}
void NzbCheck::setSocketTimeOut(int ms)
{
    if (ms > 0)
        _socketTimeOut = ms;
}
void NzbCheck::setMaxRetries(ushort nb)
{
    _maxRetries = nb;
}
int NzbCheck::socketTimeOut() const
{
    return _socketTimeOut;
}
ushort NzbCheck::maxRetries() const
{
    return _maxRetries;
}

void NzbCheck::setPar2BlockSize(qint64 bytes, const QString &source)
{
    if (bytes > 0) {
        _par2BlockSize     = bytes;
        _blockSizeSource   = source;
        _blockSizeMeasured = true;
    }
}

void NzbCheck::setArticleSize(qint64 bytes)
{
    // The configured size describes how we would post, not how this nzb was
    // made, so it is only a starting point: whatever the subjects say wins.
    if (bytes > 0 && _articleSizeMin <= 0) {
        _articleSizeMin = bytes;
    }
}

void NzbCheck::missingArticle(const QString &article)
{
    // One line per missing article drowns the report on a post that has aged
    // out (tens of thousands of lines, and the progress bar redrawn between
    // each). The counter and the final summary carry the information; the
    // per-article detail is a debugging need.
    if (debugMode())
        _cout << (_dispProgressBar ? "\n" : "") << tr("+ Missing Article on server: ") << article
              << "\n"
              << MB_FLUSH;
    ++_nbMissingArticles;

    // Which side of the post lost it decides whether anything can be repaired,
    // and for data, which file lost it decides how many blocks it costs.
    int const owner = _articleOwner.value(article, kUnknownOwner);
    if (owner == kUnknownOwner) {
        // Not from this nzb: count it, attribute it to nothing.
        ++_nbMissingDataArticles;
    } else if (owner >= 0) {
        ++_nbMissingPar2Articles;
        ++_par2Volumes[owner].nbMissingArticles;
        ++_par2Volumes[owner].nbMissingListedArticles;
    } else {
        ++_nbMissingDataArticles;
        int const dataFile = -owner - 1;
        if (dataFile >= 0 && dataFile < _dataFiles.size()) {
            NzbDataFile &file = _dataFiles[dataFile];
            ++file.nbMissingArticles;
            ++file.nbMissingListedArticles;
            qint64 const bytesUpper = _dataArticleBytesUpper.value(article, 0);
            if (bytesUpper > 0) {
                ++file.nbMissingListedArticlesWithBytes;
                file.missingArticleBytesUpper.append(bytesUpper);
            }
        }
    }

    // Either kind of loss changes the balance: data raises what has to be
    // rebuilt, PAR2 lowers what can rebuild it. Hanging the decision off the
    // data branch alone left a volume dying after the data did unweighed.
    _reevaluateEarlyStop();
}

void NzbCheck::_reevaluateEarlyStop()
{
    if (_earlyStop || _checkFull || !_par2PhaseDone || _nbMissingDataArticles <= 0)
        return;

    // Stopping is irreversible -- it stops asking -- so only a fact justifies
    // it. NoPar2AtAll and NoUsableBlocks are facts: the nzb lists no PAR2 file,
    // or every volume lost every one of its articles. Impossible now carries
    // its own proof, since recoveryVerdict() only returns it when the least
    // possible damage already exceeds the most blocks that could have survived,
    // measured against a slice size somebody actually told us.
    Recovery const verdict = recoveryVerdict();
    if (verdict == Recovery::NoPar2AtAll || verdict == Recovery::NoRecoveryBlocks
        || verdict == Recovery::NoUsableBlocks || verdict == Recovery::Impossible)
        _earlyStop = true;
}

QString NzbCheck::getNextArticle()
{
    if (_earlyStop)
        return QString();

    if (!_par2Queue.isEmpty())
        return _par2Queue.pop();

    // The queue emptying only means the last PAR2 article has been handed OUT.
    // With more connections than PAR2 articles the answers are still in flight,
    // and weighing a data loss against a redundancy we do not know yet would be
    // guessing. The phase closes when every one of them has answered.
    if (_nbPar2Answered >= _nbPar2Articles && !_par2PhaseDone) {
        _par2PhaseDone = true;
        // Everything already known -- losses seen while the phase was still
        // open, and articles the nzb never listed, counted back at parse time
        // -- has never been weighed against the redundancy. Weigh it now.
        _reevaluateEarlyStop();
        if (_earlyStop)
            return QString();
    }

    // The PAR2 queue is drained but its answers are still in flight. Handing
    // out data here is what made "PAR2 first" a suggestion rather than an
    // order: with more connections than PAR2 articles, all the spare ones
    // raced ahead and the redundancy was still unknown when their losses came
    // back. Hold them; waitingForPar2() tells the caller to come back shortly.
    if (!_par2PhaseDone)
        return QString();

    if (!_dataQueue.isEmpty())
        return _dataQueue.pop();
    return QString();
}

void NzbCheck::requeueArticle(const QString &article)
{
    if (article.isNull())
        return;
    if (isPar2Article(article))
        _par2Queue.push(article);
    else
        _dataQueue.push(article);
}

bool NzbCheck::hasArticlesLeft() const
{
    return !_earlyStop && (!_par2Queue.isEmpty() || !_dataQueue.isEmpty());
}

bool NzbCheck::waitingForPar2() const
{
    return !_earlyStop && !_par2PhaseDone && !_dataQueue.isEmpty();
}

void NzbCheck::articleChecked(const QString &article)
{
    ++_nbCheckedArticles;
    int const owner = _articleOwner.value(article, kUnknownOwner);
    if (owner >= 0 && owner < _par2Volumes.size()) {
        ++_nbPar2Answered;
        ++_par2Volumes[owner].nbCheckedArticles;
    }
}

int NzbCheck::nbMissingArticles() const
{
    return _nbMissingArticles;
}

int NzbCheck::exitCode() const
{
    return static_cast<int>(checkStatus());
}

bool NzbCheck::debugMode() const
{
    return _debug != 0;
}
void NzbCheck::setDebug(ushort level)
{
    _debug = level;
}

void NzbCheck::log(const QString &aMsg)
{
    _cout << aMsg << "\n" << MB_FLUSH;
}
void NzbCheck::log(const char *aMsg)
{
    _cout << aMsg << "\n" << MB_FLUSH;
}
void NzbCheck::log(const std::string &aMsg)
{
    _cout << aMsg.c_str() << "\n" << MB_FLUSH;
}

void NzbCheck::error(const QString &aMsg)
{
    _cerr << aMsg << "\n" << MB_FLUSH;
}
void NzbCheck::error(const char *aMsg)
{
    _cerr << aMsg << "\n" << MB_FLUSH;
}
void NzbCheck::error(const std::string &aMsg)
{
    _cerr << aMsg.c_str() << "\n" << MB_FLUSH;
}

#endif // NZBCHECK_H
