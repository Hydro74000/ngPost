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

//! One PAR2 file of the post: either the base .par2, which carries only
//! metadata, or a .volNN+MM.par2, which carries MM recovery blocks.
struct Par2Volume
{
    QString subject;
    int     blocks             = 0;     //!< recovery blocks, from the .volNN+MM name
    bool    isVolume           = false; //!< false for the metadata-only base .par2
    int     nbExpectedArticles = 0;
    int     nbMissingArticles  = 0;

    bool isIntact() const { return nbMissingArticles == 0; }

    //! Recovery blocks still usable out of this file.
    //!
    //! PAR2 is a sequence of independently checksummed packets, and par2cmdline
    //! scans a file for the ones it can still read, so a volume missing one of
    //! its twenty articles is not worth zero -- it is worth roughly nineteen
    //! twentieths of its blocks. Counting it as zero (as ngPostEx does) declares
    //! repairable posts dead. Rounding down keeps the answer conservative.
    int usableBlocks() const;
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
    QHash<QString, int> _articleVolume; //!< par2 article id -> index in _par2Volumes
    int _nbPar2Articles;
    int _nbPar2Answered; //!< PAR2 articles the server has actually answered on
    int _nbDataArticles;
    int _nbDataFiles;         //!< data files in the nzb
    int _nbDataFilesWithSize; //!< of which announce their size in the subject
    int _nbMissingPar2Articles;
    int _nbMissingDataArticles;

    qint64  _par2BlockSize;    //!< 0 while unknown
    QString _blockSizeSource;  //!< how it was obtained, for the report
    qint64  _articleSize;             //!< decoded payload per article; derived or configured
    qint64  _articleSizeFromSubjects; //!< tightest bound the subjects give for it
    qint64  _dataSizeBytes;    //!< data announced by the subjects, 0 if they carry none

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
        Certain,         //!< even the worst layout is covered
        LayoutDependent, //!< covered only if the losses are clustered
        Impossible,      //!< even the best layout is not covered
        NoRedundancy     //!< no usable PAR2 block at all
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
    //! \a article is the one that just got its answer, whatever that answer was.
    inline void articleChecked(const QString &article);

    //! The check cannot even be attempted (nzb unreadable, no server able to
    //! run it). Reports it the way a finished check reports itself, so a caller
    //! gets a verdict -- and a json object -- either way.
    void reportUnusable(const QString &reason);

    inline int nbMissingArticles() const;
    CheckStatus checkStatus() const;

    // ---- PAR2 recovery analysis ----
    int  totalRecoveryBlocks() const;
    int  usableRecoveryBlocks() const;
    bool hasIntactPar2Metadata() const;
    //! True when every data file told us its size, so _dataSizeBytes is the
    //! whole of the data and not a fragment of it.
    inline bool dataSizeIsComplete() const;
    //! Blocks the missing data articles damage: {clustered, scattered}.
    QPair<int, int> damagedBlockRange() const;
    //! Share of the data the recovery blocks still cover, in percent, and what
    //! it was when the post was made. Negative when the nzb does not say how
    //! big the data is.
    QPair<double, double> redundancyPercent() const;
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
bool NzbCheck::dataSizeIsComplete() const
{
    return _nbDataFiles > 0 && _nbDataFilesWithSize == _nbDataFiles && _dataSizeBytes > 0;
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
        _par2BlockSize   = bytes;
        _blockSizeSource = source;
    }
}

void NzbCheck::setArticleSize(qint64 bytes)
{
    if (bytes > 0)
        _articleSize = bytes;
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

    // Which side of the post lost it decides whether anything can be repaired.
    auto const volume = _articleVolume.constFind(article);
    if (volume != _articleVolume.constEnd()) {
        ++_nbMissingPar2Articles;
        ++_par2Volumes[*volume].nbMissingArticles;
    } else {
        ++_nbMissingDataArticles;

        // Only ever stop on a loss that no layout can save. "Recoverable if the
        // losses are clustered" is precisely the case where giving up would
        // send someone to re-post something a repair would have fixed.
        if (!_earlyStop && !_checkFull && _par2PhaseDone) {
            Recovery const verdict = recoveryVerdict();
            if (verdict == Recovery::Impossible || verdict == Recovery::NoRedundancy)
                _earlyStop = true;
        }
    }
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
    if (_nbPar2Answered >= _nbPar2Articles)
        _par2PhaseDone = true;

    if (!_dataQueue.isEmpty())
        return _dataQueue.pop();
    return QString();
}

void NzbCheck::requeueArticle(const QString &article)
{
    if (article.isNull())
        return;
    if (_articleVolume.contains(article))
        _par2Queue.push(article);
    else
        _dataQueue.push(article);
}

bool NzbCheck::hasArticlesLeft() const
{
    return !_earlyStop && (!_par2Queue.isEmpty() || !_dataQueue.isEmpty());
}

void NzbCheck::articleChecked(const QString &article)
{
    ++_nbCheckedArticles;
    if (_articleVolume.contains(article))
        ++_nbPar2Answered;
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
