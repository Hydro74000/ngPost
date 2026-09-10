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
    if (_reply || _busy) return;
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
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    req.setTransferTimeout(30000);

    _reply = _netMgr->get(req);
    _reply->setReadBufferSize(65536);
    QNetworkReply *reply = _reply;
    connect(reply, &QIODevice::readyRead, this, [reply] {
        QByteArray body = reply->property("boundedBody").toByteArray();
        const QByteArray chunk = reply->read(65536);
        if (body.size() + chunk.size() > 1024 * 1024) { reply->abort(); return; }
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
    if (body.size() > 1024 * 1024) return;
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

    if (!QRegularExpression(QStringLiteral("^[vV]?[0-9]+(?:[.][0-9]+)+(?:-[A-Za-z0-9.-]+)?$"))
             .match(_latestTag).hasMatch())
        return;
    _releasePageUrl = QUrl(QStringLiteral("https://github.com/Hydro74000/ngPost/releases/tag/") + _latestTag);

    // Update "last check" timestamp regardless of whether a newer version is available,
    // so we honour the once-per-day cadence even when already up to date.
    _ngPost->_lastUpdateCheckEpoch = QDateTime::currentSecsSinceEpoch();
    _ngPost->saveConfig();

    // A stable install is never offered a pre-release, whatever came back.
    // The endpoint already excludes them, so this only closes the door on a
    // future caller: isVersionNewer() alone would say yes to a HIGHER numbered
    // pre-release, which is right for an unstable build and wrong here.
    if (isPreRelease(_latestTag) && !isPreRelease(buildTag()))
    {
        qDebug() << "[UpdateChecker] ignoring pre-release" << _latestTag
                 << "for stable build" << buildTag();
        return;
    }

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
    if (!isTrustedDownloadUrl(_assetUrl) || _assetUrl != QUrl(
            QStringLiteral("https://github.com/Hydro74000/ngPost/releases/download/")
            + _latestTag + QLatin1Char('/') + wantedName))
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
    // The detached installer must survive application teardown after handoff.
    if (!_handoff) cancelDownload();
}

void UpdateChecker::cancelDownload()
{
    ++_generation;
    _cancelled = true;
    if (_work) {
        QFile cancelled(_work->filePath(QStringLiteral("cancelled")));
        cancelled.open(QIODevice::WriteOnly);
    }
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
}

void UpdateChecker::failDownload(const QString &message)
{
    // Invalidate detached-readiness timers before a retry can create new work.
    ++_generation;
    if (_work) {
        QFile cancelled(_work->filePath(QStringLiteral("cancelled")));
        cancelled.open(QIODevice::WriteOnly);
    }
    _busy = false;
    _downloadFile.reset();
    if (!_cancelled) emit downloadFailed(message);
}

void UpdateChecker::startDownloadAndInstall()
{
    if (_busy || isAppImage()) return;
    ++_generation;
    _cancelled = false;
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
    _installDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
    QDir bundle(_installDir);
    bundle.cdUp(); bundle.cdUp();
    _installDir = bundle.absolutePath();
#endif
    if (!QFileInfo::exists(_installDir + QStringLiteral("/.ngpost-installation"))) {
        failDownload(tr("This installation has no package ownership marker. Install a package manually after checking its SHA-256 hash before enabling automatic replacement."));
        return;
    }
    // Stage on the same filesystem. Replacing /usr/bin or another shared
    // directory is never an allowed updater operation.
    if (QDir(_installDir).isRoot() || _installDir == QDir::homePath()
        || _installDir == QLatin1String("/usr/bin") || _installDir == QLatin1String("/usr/local/bin")
        || !QFileInfo(QFileInfo(_installDir).absolutePath()).isWritable()) {
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
    downloadFile(manifest, QStringLiteral("manifest.json"), 1024 * 1024, [this] {
        downloadFile(_assetUrl, QStringLiteral("archive"), _assetSize, [this] { prepareInstall(); });
    });
}

void UpdateChecker::downloadFile(const QUrl &url, const QString &name, qint64 cap, std::function<void()> done)
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
        if (_downloadReply != reply || !_downloadFile) return;
        while (reply->bytesAvailable() && error->isEmpty() && !_cancelled) {
            const QByteArray data = reply->read(65536);
            if (data.isEmpty()) break;
            if (*written + data.size() > cap || _downloadFile->write(data) != data.size()) {
                *error = tr("Update exceeds its size limit or could not be written.");
                reply->abort();
                return;
            }
            *written += data.size();
        }
    };
    connect(reply, &QIODevice::readyRead, this, drain);
    connect(reply, &QNetworkReply::downloadProgress, this, &UpdateChecker::downloadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, drain, written, error, name, cap, done] {
        if (_downloadReply != reply) { reply->deleteLater(); return; }
        drain();
        _downloadReply = nullptr;
        reply->deleteLater();
        const bool ok = !_cancelled && error->isEmpty() && reply->error() == QNetworkReply::NoError
            && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
            && (name != QLatin1String("archive") || *written == cap)
            && _downloadFile && _downloadFile->flush();
        _downloadFile.reset();
        if (!ok) {
            failDownload(error->isEmpty() ? tr("Update canceled, truncated or refused by the server.") : *error);
            return;
        }
        done();
    });
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
