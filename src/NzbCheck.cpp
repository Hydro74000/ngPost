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

                            _nbMissingArticles += nbExpectedArticles - nbArticles;
                            _nbMissingArticlesInNzb += nbExpectedArticles - nbArticles;
                        }

                        break;
                    } else if (type == QXmlStreamReader::TokenType::StartElement
                               && xmlReader.name().compare(QLatin1String("segment")) == 0) {
                        ++nbArticles;
                        xmlReader.readNext();
                        _articles.push(QString("<%1>").arg(xmlReader.text().toString()));
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
        if (!_quietMode)
            _cout << tr("%1 has %2 articles")
                         .arg(QFileInfo(_nzbPath).fileName())
                         .arg(_nbListedArticles)
                  << "\n"
                  << MB_FLUSH;
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

NzbCheck::CheckStatus NzbCheck::checkStatus() const
{
    if (_unusable)
        return CheckStatus::Inconclusive;

    // Anything short of a full sweep means the answer cannot be trusted, no
    // matter how few articles came back missing.
    if (_nbListedArticles > 0 && _nbCheckedArticles < _nbListedArticles)
        return CheckStatus::Inconclusive;

    return _nbMissingArticles == 0 ? CheckStatus::Complete : CheckStatus::Missing;
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

    QJsonObject root;
    root[QStringLiteral("nzb")]         = QFileInfo(_nzbPath).absoluteFilePath();
    root[QStringLiteral("status")]      = statusName;
    root[QStringLiteral("exitCode")]    = static_cast<int>(status);
    root[QStringLiteral("articles")]    = articles;
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
