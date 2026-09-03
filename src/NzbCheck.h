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
#include <QObject>
#include <QSet>
#include <QStack>
#include <QString>
#include <QTextStream>
#include <QTimer>
struct NntpServerParams;
class NntpCheckCon;

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
    QStack<QString> _articles;

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
        Missing       = 1, //!< articles are missing; repairability not evaluated
        Unrecoverable = 2, //!< reserved: missing beyond what the PAR2 volumes can repair
        Inconclusive  = 3  //!< the check could not be completed, the result means nothing
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

    inline void missingArticle(const QString &article);
    inline QString getNextArticle();
    inline void articleChecked();

    //! The check cannot even be attempted (nzb unreadable, no server able to
    //! run it). Reports it the way a finished check reports itself, so a caller
    //! gets a verdict -- and a json object -- either way.
    void reportUnusable(const QString &reason);

    inline int nbMissingArticles() const;
    CheckStatus checkStatus() const;
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
}

QString NzbCheck::getNextArticle()
{
    if (_articles.isEmpty())
        return QString();
    else
        return _articles.pop();
}

void NzbCheck::articleChecked()
{
    ++_nbCheckedArticles;
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
