/*
 * Copyright (c) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
 * Copyright (c) 2024-2026 Hydro74000 <acymap@gmail.com>
 * Licensed under the GNU General Public License v3.0
 */

#include "UpdateChecker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
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
#include <QTimer>
#include <QRegularExpression>

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

QString UpdateChecker::installationDirectory()
{
    QDir directory(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    if (directory.dirName() == QLatin1String("MacOS")) {
        directory.cdUp();
        if (directory.dirName() == QLatin1String("Contents"))
            directory.cdUp();
    }
#endif
    return directory.absolutePath();
}

bool UpdateChecker::isReplaceableInstallation(const QString &directory)
{
    const QFileInfo info(directory);
    const QString path = info.canonicalFilePath();
    const QFileInfo marker(directory + QStringLiteral("/.ngpost-installation"));
    // Inno Setup owns its uninstall database and optional components. A ZIP
    // directory swap would discard both; keep these installs on Setup.
    return !info.isSymLink() && !path.isEmpty() && marker.isFile() && !marker.isSymLink()
        && !QDir(path).isRoot() && path != QFileInfo(QDir::homePath()).canonicalFilePath()
        && path != QLatin1String("/usr/bin") && path != QLatin1String("/usr/local/bin")
        && QFileInfo(info.absolutePath()).isWritable()
        && QDir(directory).entryList({ QStringLiteral("unins*.exe") }, QDir::Files).isEmpty();
}

bool UpdateChecker::canInstallAutomatically() const
{
#if defined(Q_PROCESSOR_X86_64) || defined(Q_OS_MACOS)
    return !isAppImage() && isReplaceableInstallation(installationDirectory())
        && isTrustedDownloadUrl(_assetUrl) && _assetSize > 0 && _assetSize <= 1024LL * 1024 * 1024
        && (!QStandardPaths::findExecutable(QStringLiteral("python3")).isEmpty()
            || !QStandardPaths::findExecutable(QStringLiteral("python")).isEmpty());
#else
    return false; // Published Linux/Windows archives target x86_64 only.
#endif
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
    if (_reply || _busy)
        return;

    // A stable build asks GitHub for "the latest release", which by definition
    // ignores pre-releases. A pre-release build has to look at the list: what
    // supersedes it may be a newer unstable build, or the stable it leads to.
    const QUrl url(isPreRelease(buildTag()) ? QUrl(sReleaseListApiUrl) : QUrl(sReleaseApiUrl));
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "ngPost C++ app");
    req.setRawHeader("Accept",     "application/vnd.github+json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    req.setTransferTimeout(30000);

    _reply = _netMgr->get(req);
    _reply->setReadBufferSize(65536);
    QNetworkReply *reply = _reply;
    connect(reply, &QIODevice::readyRead, this, [reply] {
        QByteArray body = reply->property("boundedBody").toByteArray();
        const QByteArray chunk = reply->read(65536);
        if (body.size() + chunk.size() > qsizetype(1024) * 1024) {
            reply->abort();
            return;
        }
        body += chunk;
        reply->setProperty("boundedBody", body);
    });
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

    const QByteArray body = reply->property("boundedBody").toByteArray() + reply->readAll();
    if (body.size() > qsizetype(1024) * 1024)
        return;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || (!doc.isObject() && !doc.isArray()))
    {
        qDebug() << "[UpdateChecker] invalid JSON from GitHub:" << err.errorString();
        return;
    }

    const QJsonObject root = selectRelease(doc, buildTag());
    recordCheck();
    if (root.isEmpty())
        return;
    _latestTag        = root.value("tag_name").toString();
    _releaseNotes     = root.value("body").toString();
    _releasePageUrl   = QUrl(root.value("html_url").toString());
    _assetUrl         = QUrl();
    _assetFileName.clear();
    _assetSize = 0;

    _releasePageUrl = QUrl(QStringLiteral("https://github.com/Hydro74000/ngPost/releases/tag/") + _latestTag);

    const QString wantedName = assetNameForCurrentOS(_latestTag);
    const QJsonArray assets  = root.value("assets").toArray();
    for (const auto &v : assets) {
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
    if (!isTrustedDownloadUrl(_assetUrl) || _assetUrl != QUrl(
            QStringLiteral("https://github.com/Hydro74000/ngPost/releases/download/")
            + _latestTag + QLatin1Char('/') + wantedName))
    {
        qDebug() << "[UpdateChecker] no usable asset for OS, falling back to release page";
        _assetUrl.clear();
    }

    emit newVersionAvailable(_latestTag, _releaseNotes, _releasePageUrl);
}

QJsonObject UpdateChecker::selectRelease(const QJsonDocument &document, const QString &current)
{
    // GitHub's list is ordered by creation date, not version. A stable build
    // must filter prereleases before choosing the best remaining candidate.
    const auto releases = document.isArray() ? document.array() : QJsonArray{ document.object() };
    const QRegularExpression validTag(
        QStringLiteral("^[vV]?[0-9]+(?:[.][0-9]+)+(?:-[A-Za-z0-9.-]+)?$"));
    QJsonObject best;
    for (const auto &entry : releases) {
        const auto release = entry.toObject();
        const auto tag = release.value("tag_name").toString();
        if (release.value("draft").toBool() || !validTag.match(tag).hasMatch()
            || (!isPreRelease(current)
                && (release.value("prerelease").toBool() || isPreRelease(tag)))
            || !isVersionNewer(tag, current))
            continue;
        if (best.isEmpty() || isVersionNewer(tag, best.value("tag_name").toString()))
            best = release;
    }
    return best;
}

void UpdateChecker::recordCheck()
{
    if (_ngPost) {
        _ngPost->_lastUpdateCheckEpoch = QDateTime::currentSecsSinceEpoch();
        _ngPost->saveConfig();
    }
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

bool UpdateChecker::isTrustedDownloadUrl(const QUrl &url)
{
    return url.isValid() && url.scheme() == QLatin1String("https")
        && url.userInfo().isEmpty() && url.port(443) == 443
        && (url.host() == QLatin1String("github.com")
            || url.host() == QLatin1String("release-assets.githubusercontent.com")
            || url.host() == QLatin1String("objects.githubusercontent.com"));
}

UpdateChecker::~UpdateChecker()
{
    // No signal may leave the object from here on: a slot reacting to one
    // would run against a half-destroyed UpdateChecker.
    _destructing = true;
    // The detached installer must survive application teardown after handoff.
    if (!_handoff) cancelDownload();
}

bool UpdateChecker::_markCancelledForInstaller()
{
    // The detached installer polls for this file and aborts when it appears
    // (install_update.py, "cancelled"). It is the ONLY channel we have once
    // startDetached() has returned, so a silent failure here means a cancelled
    // update installs itself anyway -- after ngPost has already exited.
    if (!_work)
        return true; // nothing staged, nothing to call off

    QString const path = _work->filePath(QStringLiteral("cancelled"));
    QFile         cancelled(path);
    if (cancelled.open(QIODevice::WriteOnly)) {
        cancelled.close();
        return true;
    }

    // qWarning, not qDebug: release builds define QT_NO_DEBUG_OUTPUT, and this
    // is the one trace left when the GUI is already gone.
    qWarning().noquote() << QStringLiteral(
                                "[UpdateChecker] could not write '%1' (%2); a detached update "
                                "installer cannot be called off")
                                .arg(path, cancelled.errorString());
    return false;
}

void UpdateChecker::cancelDownload()
{
    ++_generation;
    _cancelled = true;
    bool const calledOff = _markCancelledForInstaller();
    if (_downloadReply) {
        auto reply = _downloadReply.data();
        _downloadReply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    _downloadFile.reset();
    if (_installer) {
        auto process = _installer.data();
        _installer = nullptr;
        disconnect(process, nullptr, this, nullptr);
        process->kill();
        process->waitForFinished(3000);
        process->deleteLater();
    }
    _busy = false;

    // Killing our own child says nothing about the detached installer: once
    // _detached is set, that process owns the install directory and only the
    // marker stops it. Telling the user the cancellation did not take is the
    // whole point -- silently returning would leave them believing it did.
    if (!calledOff && _detached && !_destructing)
        emit downloadFailed(tr("Could not cancel the update: ngPost failed to signal the installer "
                               "already running, and it may replace this installation. Check the "
                               "version after the next start."));
}

void UpdateChecker::failDownload(const QString &message)
{
    // Invalidate detached-readiness timers before a retry can create new work.
    ++_generation;
    bool const calledOff = _markCancelledForInstaller();
    _busy = false;
    _downloadFile.reset();
    if (_cancelled)
        return;

    QString reported = message;
    if (!calledOff && _detached) {
        reported += QLatin1Char(' ')
                  + tr("The installer already running could not be stopped either, and it may "
                       "replace this installation.");
    }
    emit downloadFailed(reported);
}

void UpdateChecker::startDownloadAndInstall()
{
    if (_busy || isAppImage()) return;
    ++_generation;
    _cancelled = false;
    // A previous attempt that reached startDetached() and then failed its
    // readiness wait leaves _detached set. Carrying that into a retry makes a
    // failed marker write claim an installer is running when none is, which
    // turns a warning the user must trust into one they learn to ignore.
    _detached = false;
    if (!isTrustedDownloadUrl(_assetUrl) || _assetSize <= 0 || _assetSize > 1024LL * 1024 * 1024) {
        failDownload(tr("No bounded, trusted update asset is available."));
        return;
    }
    _python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (_python.isEmpty()) _python = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (_python.isEmpty()) {
        failDownload(tr("Automatic updates require Python 3.9+. Install this release manually after checking its published SHA-256 hash."));
        return;
    }
    _installDir = installationDirectory();
    if (!QFileInfo::exists(_installDir + QStringLiteral("/.ngpost-installation"))) {
        failDownload(tr("This installation has no package ownership marker. Install a package manually after checking its SHA-256 hash before enabling automatic replacement."));
        return;
    }
    // Stage on the same filesystem. Replacing /usr/bin or another shared
    // directory is never an allowed updater operation.
    if (!canInstallAutomatically()) {
        failDownload(tr("This installation cannot be replaced safely; use its package installer."));
        return;
    }
    _work.reset(new QTemporaryDir(QFileInfo(_installDir).absolutePath() + QStringLiteral("/.ngpost-update-XXXXXX")));
    if (!_work->isValid() || !PathHelper::restrictToOwner(_work->path())) {
        failDownload(tr("Cannot create a private update directory."));
        return;
    }
    for (const QString &name : {QStringLiteral("install_update.py")}) {
        const QString target = _work->filePath(name);
        if (!QFile::copy(QStringLiteral(":/update/") + name, target)
            || !PathHelper::restrictToOwner(target)) {
            failDownload(tr("Cannot stage update verifier."));
            return;
        }
    }
    _busy = true;
    QUrl manifest = _assetUrl;
    manifest.setPath(_assetUrl.path().left(_assetUrl.path().lastIndexOf('/') + 1) + QStringLiteral("manifest.json"));
    manifest.setQuery(QString());
    downloadFile(manifest, QStringLiteral("manifest.json"), qint64(1024) * 1024, [this] {
        downloadFile(_assetUrl, QStringLiteral("archive"), _assetSize, [this] { prepareInstall(); });
    });
}

void UpdateChecker::downloadFile(const QUrl &url,
                                 const QString &name,
                                 qint64 cap,
                                 const std::function<void()> &done)
{
    if (_cancelled) return;
    _downloadFile.reset(new QFile(_work->filePath(name)));
    if (!_downloadFile->open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || !PathHelper::restrictToOwner(_downloadFile->fileName())) {
        failDownload(tr("Cannot write update file."));
        return;
    }
    QNetworkRequest req(url);
    req.setTransferTimeout(30000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::UserVerifiedRedirectPolicy);
    auto *reply = _netMgr->get(req);
    _downloadReply = reply;
    reply->setReadBufferSize(65536);
    connect(reply, &QNetworkReply::redirected, reply, [reply](const QUrl &redirect) {
        if (isTrustedDownloadUrl(redirect)) reply->redirectAllowed();
        else reply->abort();
    });
    auto written = std::make_shared<qint64>(0);
    auto error = std::make_shared<QString>();
    auto drain = [this, reply, cap, written, error] {
        _drainDownload(reply, cap, written.get(), error.get());
    };
    connect(reply, &QIODevice::readyRead, this, drain);
    connect(reply, &QNetworkReply::downloadProgress, this, &UpdateChecker::downloadProgress);
    connect(reply,
            &QNetworkReply::finished,
            this,
            [this, reply, drain, written, error, name, cap, done] {
        if (_downloadReply != reply) {
            reply->deleteLater();
            return;
        }
        drain();
        _downloadReply = nullptr;
        reply->deleteLater();
        _completeDownload(reply, name, *written, cap, *error, done);
    });
}

//! Moves what \a reply holds into the download file, \a cap bytes at most.
void UpdateChecker::_drainDownload(QNetworkReply *reply,
                                   qint64 cap,
                                   qint64 *written,
                                   QString *error)
{
    if (_downloadReply != reply || !_downloadFile)
        return;
    while (reply->bytesAvailable() && error->isEmpty() && !_cancelled) {
        const QByteArray data = reply->read(65536);
        if (data.isEmpty())
            break;
        if (*written + data.size() > cap || _downloadFile->write(data) != data.size()) {
            *error = tr("Update exceeds its size limit or could not be written.");
            reply->abort();
            return;
        }
        *written += data.size();
    }
}

//! Once \a reply has finished: keeps the file and calls \a done when it is
//! complete, fails the update otherwise. The archive must be exactly \a cap long.
void UpdateChecker::_completeDownload(QNetworkReply *reply,
                                      const QString &name,
                                      qint64 written,
                                      qint64 cap,
                                      const QString &error,
                                      const std::function<void()> &done)
{
    const bool ok = !_cancelled && error.isEmpty() && reply->error() == QNetworkReply::NoError
        && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
        && (name != QLatin1String("archive") || written == cap) && _downloadFile
        && _downloadFile->flush();
    _downloadFile.reset();
    if (!ok) {
        failDownload(error.isEmpty() ? tr("Update canceled, truncated or refused by the server.")
                                     : error);
        return;
    }
    done();
}

void UpdateChecker::prepareInstall()
{
    auto *process = new QProcess(this);
    _installer = process;
    QTimer::singleShot(120000, process, [process] {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && _installer == process) {
            _installer = nullptr;
            process->deleteLater();
            failDownload(tr("Cannot start update verifier."));
        }
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (_installer != process) return;
        _installer = nullptr;
        if (_cancelled) return;
        if (status != QProcess::NormalExit || code != 0) {
            failDownload(tr("SHA-256, extraction or package validation failed: %1")
                         .arg(QString::fromUtf8(process->readAll()).left(2048)));
            return;
        }
        const QString work = _work->path();
        if (!QProcess::startDetached(_python, {QStringLiteral("-I"), _work->filePath(QStringLiteral("install_update.py")),
            QStringLiteral("commit"), work, QStringLiteral("--pid"), QString::number(QCoreApplication::applicationPid())})) {
            failDownload(tr("Cannot start update transaction."));
            return;
        }
        // From here the installer is out of our process tree: the "cancelled"
        // marker is the only way left to stop it.
        _detached = true;
        // Keep files alive until the detached installer acknowledges readiness.
        _work->setAutoRemove(false);
        auto *timer = new QTimer(this);
        auto elapsed = std::make_shared<int>(0);
        const quint64 generation = _generation;
        connect(timer, &QTimer::timeout, this, [this, timer, work, elapsed, generation] {
            if (_cancelled || generation != _generation) { timer->stop(); timer->deleteLater(); return; }
            if (QFileInfo::exists(work + QStringLiteral("/ready"))) {
                timer->stop(); timer->deleteLater();
                _handoff = true;
                emit installStarting();
                QCoreApplication::quit();
            } else if (++*elapsed > 100 || QFileInfo::exists(work + QStringLiteral("/error.txt"))) {
                timer->stop(); timer->deleteLater();
                failDownload(tr("Update installer did not become ready. Previous installation retained."));
            }
        });
        timer->start(100);
    });
    process->start(_python, {QStringLiteral("-I"), _work->filePath(QStringLiteral("install_update.py")),
        QStringLiteral("prepare"), _work->path(), QStringLiteral("--tag"), _latestTag,
        QStringLiteral("--asset"), _assetFileName, QStringLiteral("--install"), _installDir});
}
