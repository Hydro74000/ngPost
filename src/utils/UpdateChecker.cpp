/*
 * Copyright (c) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
 * Copyright (c) 2024-2026 Hydro74000 <acymap@gmail.com>
 * Licensed under the GNU General Public License v3.0
 */

#include "UpdateChecker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>

#include "NgPost.h"

const QString UpdateChecker::sRepoOwner     = "Hydro74000";
const QString UpdateChecker::sRepoName      = "ngPost";
const QString UpdateChecker::sReleaseApiUrl =
        QStringLiteral("https://api.github.com/repos/%1/%2/releases/latest")
        .arg(UpdateChecker::sRepoOwner, UpdateChecker::sRepoName);
const QString UpdateChecker::sReleaseListApiUrl =
        QStringLiteral("https://api.github.com/repos/%1/%2/releases?per_page=20")
        .arg(UpdateChecker::sRepoOwner, UpdateChecker::sRepoName);

UpdateChecker::UpdateChecker(NgPost *ngPost, QNetworkAccessManager *netMgr, QObject *parent)
    : QObject(parent),
      _ngPost(ngPost),
      _netMgr(netMgr),
      _reply(nullptr),
      _assetSize(0)
{}

bool UpdateChecker::isAppImage()
{
    return qEnvironmentVariableIsSet("APPIMAGE") || qEnvironmentVariableIsSet("APPDIR");
}

QString UpdateChecker::stripVersionPrefix(const QString &tag)
{
    QString s = tag.trimmed();
    if (s.startsWith('v') || s.startsWith('V'))
        s = s.mid(1);
    return s;
}

namespace
{
//! Splits "5.5-unstable.20260824.107.abc" into "5.5" and
//! "unstable.20260824.107.abc". A tag without a '-' has no suffix.
void splitTag(const QString &tag, QString *numbers, QString *suffix)
{
    const int dash = tag.indexOf(QLatin1Char('-'));
    if (dash < 0)
    {
        *numbers = tag;
        suffix->clear();
        return;
    }
    *numbers = tag.left(dash);
    *suffix  = tag.mid(dash + 1);
}

//! Element-wise comparison of two dotted lists. Numeric elements compare as
//! numbers so 107 sorts after 99; anything else compares as text. Returns
//! -1, 0 or 1.
int compareDotted(const QString &a, const QString &b, bool numericOnly)
{
    const QStringList left  = a.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    const QStringList right = b.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    const int n = qMax(left.size(), right.size());
    for (int i = 0; i < n; ++i)
    {
        const QString l = i < left.size() ? left.at(i) : QString();
        const QString r = i < right.size() ? right.at(i) : QString();

        bool lok = false, rok = false;
        const int li = l.toInt(&lok);
        const int ri = r.toInt(&rok);
        if (numericOnly || (lok && rok))
        {
            // A missing element counts as 0, which is what makes 5.5 and
            // 5.5.0 the same release.
            const int lv = lok ? li : 0;
            const int rv = rok ? ri : 0;
            if (lv != rv)
                return lv < rv ? -1 : 1;
            continue;
        }
        if (l != r)
            return l < r ? -1 : 1;
    }
    return 0;
}
} // namespace

bool UpdateChecker::isPreRelease(const QString &tag)
{
    return stripVersionPrefix(tag).contains(QLatin1Char('-'));
}

QString UpdateChecker::buildTag()
{
#ifdef NGPOST_BUILD_TAG
    return QStringLiteral(NGPOST_BUILD_TAG);
#else
    return NgPost::sVersion;
#endif
}

bool UpdateChecker::isVersionNewer(const QString &candidate, const QString &current)
{
    QString candidateNumbers, candidateSuffix, currentNumbers, currentSuffix;
    splitTag(stripVersionPrefix(candidate), &candidateNumbers, &candidateSuffix);
    splitTag(stripVersionPrefix(current), &currentNumbers, &currentSuffix);

    const int byNumber = compareDotted(candidateNumbers, currentNumbers, true);
    if (byNumber != 0)
        return byNumber > 0;

    // Same numbers. Semver's rule: the stable release supersedes any
    // pre-release that led to it, and never the other way round.
    if (candidateSuffix.isEmpty() != currentSuffix.isEmpty())
        return currentSuffix.isEmpty() ? false : true;

    // Two pre-releases of the same number, or two identical stables. The
    // ordinal the release workflow builds -- date, run number -- orders them.
    return compareDotted(candidateSuffix, currentSuffix, false) > 0;
}

void UpdateChecker::checkLatestRelease()
{
    if (isAppImage())
    {
        qDebug() << "[UpdateChecker] running as AppImage, skipping check (zsync handles updates)";
        return;
    }

    // A stable build asks GitHub for "the latest release", which by definition
    // ignores pre-releases. A pre-release build has to look at the list: what
    // supersedes it may be a newer unstable build, or the stable it leads to.
    const QUrl url(isPreRelease(buildTag()) ? QUrl(sReleaseListApiUrl) : QUrl(sReleaseApiUrl));
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "ngPost C++ app");
    req.setRawHeader("Accept",     "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    _reply = _netMgr->get(req);
    connect(_reply, &QNetworkReply::finished, this, &UpdateChecker::onReleaseInfoReceived);
}

void UpdateChecker::onReleaseInfoReceived()
{
    QNetworkReply *reply = static_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    _reply = nullptr;

    if (reply->error() != QNetworkReply::NoError)
    {
        qDebug() << "[UpdateChecker] release info request failed:" << reply->errorString();
        return;
    }

    const QByteArray body = reply->readAll();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || (!doc.isObject() && !doc.isArray()))
    {
        qDebug() << "[UpdateChecker] invalid JSON from GitHub:" << err.errorString();
        return;
    }

    // "/releases/latest" answers with one release, "/releases" with a list.
    // Pick the entry that supersedes this build by the most: the list is
    // ordered by creation date, but a stable published before a later unstable
    // is still the one to offer.
    QJsonObject root;
    if (doc.isObject())
        root = doc.object();
    else
    {
        const QString mine = buildTag();
        QString best;
        for (const QJsonValue &v : doc.array())
        {
            const QJsonObject candidate = v.toObject();
            if (candidate.value("draft").toBool())
                continue;
            const QString tag = candidate.value("tag_name").toString();
            if (tag.isEmpty() || !isVersionNewer(tag, mine))
                continue;
            if (best.isEmpty() || isVersionNewer(tag, best))
            {
                best = tag;
                root = candidate;
            }
        }
        if (best.isEmpty())
        {
            _ngPost->_lastUpdateCheckEpoch = QDateTime::currentSecsSinceEpoch();
            _ngPost->saveConfig();
            qDebug() << "[UpdateChecker] up to date (current" << mine << ")";
            return;
        }
    }
    _latestTag        = root.value("tag_name").toString();
    _releaseNotes     = root.value("body").toString();
    _releasePageUrl   = QUrl(root.value("html_url").toString());
    _assetUrl         = QUrl();
    _assetFileName.clear();
    _assetSize = 0;

    if (_latestTag.isEmpty())
        return;

    // Update "last check" timestamp regardless of whether a newer version is available,
    // so we honour the once-per-day cadence even when already up to date.
    _ngPost->_lastUpdateCheckEpoch = QDateTime::currentSecsSinceEpoch();
    _ngPost->saveConfig();

    if (!isVersionNewer(_latestTag, buildTag()))
    {
        qDebug() << "[UpdateChecker] up to date (current" << buildTag()
                 << "latest" << _latestTag << ")";
        return;
    }

    const QString wantedName = assetNameForCurrentOS(_latestTag);
    const QJsonArray assets  = root.value("assets").toArray();
    for (const QJsonValue &v : assets)
    {
        const QJsonObject a = v.toObject();
        if (a.value("name").toString() == wantedName)
        {
            _assetUrl      = QUrl(a.value("browser_download_url").toString());
            _assetFileName = wantedName;
            _assetSize     = static_cast<qint64>(a.value("size").toDouble());
            break;
        }
    }

    // Defence in depth: only accept GitHub-hosted asset URLs.
    if (!_assetUrl.isValid() || !_assetUrl.host().endsWith("github.com"))
    {
        qDebug() << "[UpdateChecker] no usable asset for OS, falling back to release page";
        _assetUrl.clear();
    }

    emit newVersionAvailable(_latestTag, _releaseNotes, _releasePageUrl);
}

QString UpdateChecker::assetNameForCurrentOS(const QString &tag) const
{
#if defined(Q_OS_WIN)
    return QString("ngPost-%1-windows-x86_64.zip").arg(tag);
#elif defined(Q_OS_MACOS)
    return QString("ngPost-%1-macos.zip").arg(tag);
#else
    return QString("ngPost-%1-linux-x86_64.tar.gz").arg(tag);
#endif
}

void UpdateChecker::startDownloadAndInstall()
{
    if (!_assetUrl.isValid())
    {
        QDesktopServices::openUrl(_releasePageUrl);
        return;
    }

#if defined(Q_OS_LINUX)
    if (!QFileInfo(QCoreApplication::applicationDirPath()).isWritable())
    {
        emit downloadFailed(tr("Unable to write to install directory %1. Opening release page instead.")
                            .arg(QCoreApplication::applicationDirPath()));
        QDesktopServices::openUrl(_releasePageUrl);
        return;
    }
#endif

    QNetworkRequest req(_assetUrl);
    req.setRawHeader("User-Agent", "ngPost C++ app");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    _reply = _netMgr->get(req);
    connect(_reply, &QNetworkReply::downloadProgress, this, &UpdateChecker::downloadProgress);
    connect(_reply, &QNetworkReply::finished,         this, &UpdateChecker::onAssetDownloadFinished);
}

void UpdateChecker::onAssetDownloadFinished()
{
    QNetworkReply *reply = static_cast<QNetworkReply *>(sender());
    reply->deleteLater();
    _reply = nullptr;

    if (reply->error() != QNetworkReply::NoError)
    {
        emit downloadFailed(reply->errorString());
        return;
    }

    const QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QDir().mkpath(tmpDir);
    const QString archivePath = QDir(tmpDir).filePath(_assetFileName);

    QFile out(archivePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        emit downloadFailed(tr("Cannot write to %1").arg(archivePath));
        return;
    }
    out.write(reply->readAll());
    out.close();

    emit installStarting();

#if defined(Q_OS_WIN)
    runInstallerWindows(archivePath);
#elif defined(Q_OS_MACOS)
    runInstallerMacOS(archivePath);
#else
    runInstallerLinux(archivePath);
#endif
}

#if defined(Q_OS_WIN)
bool UpdateChecker::runInstallerWindows(const QString &archivePath)
{
    const QString tmpDir       = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString extractDir   = QDir(tmpDir).filePath("ngPost-update");
    const QString installDir   = QCoreApplication::applicationDirPath();
    const QString scriptPath   = QDir(tmpDir).filePath("ngPost-update.bat");
    const QString exePath      = QDir(installDir).filePath("ngPost.exe");

    QFile bat(scriptPath);
    if (!bat.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QTextStream s(&bat);
    s << "@echo off\r\n"
      << "timeout /t 2 /nobreak >nul\r\n"
      << "if exist \"" << QDir::toNativeSeparators(extractDir) << "\" rd /s /q \""
                       << QDir::toNativeSeparators(extractDir) << "\"\r\n"
      << "powershell -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '"
                       << QDir::toNativeSeparators(archivePath) << "' -DestinationPath '"
                       << QDir::toNativeSeparators(extractDir) << "' -Force\"\r\n"
      // The archive contains a single top-level folder ngPost-<tag>-windows-x86_64/
      << "for /d %%D in (\"" << QDir::toNativeSeparators(extractDir) << "\\*\") do (\r\n"
      << "    xcopy /E /Y /I /Q \"%%D\\*\" \"" << QDir::toNativeSeparators(installDir) << "\"\r\n"
      << ")\r\n"
      << "start \"\" \"" << QDir::toNativeSeparators(exePath) << "\"\r\n"
      << "rd /s /q \"" << QDir::toNativeSeparators(extractDir) << "\"\r\n"
      << "del /q \"" << QDir::toNativeSeparators(archivePath) << "\"\r\n"
      << "del /q \"%~f0\"\r\n";
    s.flush();
    bat.close();

    const bool ok = QProcess::startDetached(
        "cmd.exe", { "/c", QDir::toNativeSeparators(scriptPath) });
    if (ok)
        QCoreApplication::quit();
    return ok;
}
#else
bool UpdateChecker::runInstallerWindows(const QString &) { return false; }
#endif

#if defined(Q_OS_MACOS)
bool UpdateChecker::runInstallerMacOS(const QString &archivePath)
{
    // applicationDirPath() inside a bundle points to <App>.app/Contents/MacOS — climb 2 levels up.
    const QFileInfo appExe(QCoreApplication::applicationFilePath());
    QDir bundleDir = appExe.dir();
    bundleDir.cdUp(); // Contents
    bundleDir.cdUp(); // <App>.app
    const QString bundlePath  = bundleDir.absolutePath();
    const QString installRoot = QFileInfo(bundlePath).absolutePath();

    const QString tmpExtract = "/tmp/ngPost-update";
    const QString scriptPath = "/tmp/ngPost-update.sh";

    QFile sh(scriptPath);
    if (!sh.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QTextStream s(&sh);
    s << "#!/bin/bash\n"
      << "set -e\n"
      << "sleep 1\n"
      << "rm -rf '" << tmpExtract << "'\n"
      << "mkdir -p '" << tmpExtract << "'\n"
      << "unzip -q '" << archivePath << "' -d '" << tmpExtract << "'\n"
      << "rm -rf '" << bundlePath << "'\n"
      << "if [ -d '" << tmpExtract << "/ngPost.app' ]; then\n"
      << "    mv '" << tmpExtract << "/ngPost.app' '" << installRoot << "/'\n"
      << "else\n"
      << "    # archive may wrap the bundle in a subfolder\n"
      << "    APP=$(find '" << tmpExtract << "' -maxdepth 3 -name 'ngPost.app' -print -quit)\n"
      << "    [ -n \"$APP\" ] && mv \"$APP\" '" << installRoot << "/'\n"
      << "fi\n"
      << "rm -rf '" << tmpExtract << "' '" << archivePath << "'\n"
      << "open '" << bundlePath << "'\n"
      << "rm -f \"$0\"\n";
    s.flush();
    sh.close();
    QFile::setPermissions(scriptPath,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
        QFile::ReadGroup | QFile::ExeGroup |
        QFile::ReadOther | QFile::ExeOther);

    const bool ok = QProcess::startDetached("/bin/bash", { scriptPath });
    if (ok)
        QCoreApplication::quit();
    return ok;
}
#else
bool UpdateChecker::runInstallerMacOS(const QString &) { return false; }
#endif

#if defined(Q_OS_LINUX)
bool UpdateChecker::runInstallerLinux(const QString &archivePath)
{
    const QString installDir = QCoreApplication::applicationDirPath();
    const QString tmpExtract = "/tmp/ngPost-update";
    const QString scriptPath = "/tmp/ngPost-update.sh";
    const QString exePath    = QDir(installDir).filePath("ngPost");

    QFile sh(scriptPath);
    if (!sh.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QTextStream s(&sh);
    s << "#!/bin/bash\n"
      << "set -e\n"
      << "sleep 1\n"
      << "rm -rf '" << tmpExtract << "'\n"
      << "mkdir -p '" << tmpExtract << "'\n"
      << "tar -xzf '" << archivePath << "' -C '" << tmpExtract << "'\n"
      // Tarball contains a single top-level folder ngPost-<tag>-linux-x86_64/
      << "SRC=$(find '" << tmpExtract << "' -mindepth 1 -maxdepth 1 -type d -print -quit)\n"
      << "if [ -n \"$SRC\" ]; then\n"
      << "    cp -af \"$SRC\"/. '" << installDir << "/'\n"
      << "fi\n"
      << "chmod +x '" << exePath << "' 2>/dev/null || true\n"
      << "rm -rf '" << tmpExtract << "' '" << archivePath << "'\n"
      << "( '" << exePath << "' & ) >/dev/null 2>&1\n"
      << "rm -f \"$0\"\n";
    s.flush();
    sh.close();
    QFile::setPermissions(scriptPath,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
        QFile::ReadGroup | QFile::ExeGroup |
        QFile::ReadOther | QFile::ExeOther);

    const bool ok = QProcess::startDetached("/bin/bash", { scriptPath });
    if (ok)
        QCoreApplication::quit();
    return ok;
}
#else
bool UpdateChecker::runInstallerLinux(const QString &) { return false; }
#endif
