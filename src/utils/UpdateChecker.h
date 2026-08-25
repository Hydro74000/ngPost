/*
 * Copyright (c) 2020 Matthieu Bruel <Matthieu.Bruel@gmail.com>
 * Copyright (c) 2024-2026 Hydro74000 <acymap@gmail.com>
 * Licensed under the GNU General Public License v3.0
 */

#ifndef UPDATECHECKER_H
#define UPDATECHECKER_H

#include <QObject>
#include <QString>
#include <QUrl>

class NgPost;
class QNetworkAccessManager;
class QNetworkReply;

class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit UpdateChecker(NgPost *ngPost, QNetworkAccessManager *netMgr, QObject *parent = nullptr);

    void checkLatestRelease();

    static bool    isAppImage();

    //! True when \a candidate supersedes \a current, both given as release
    //! tags. Numbers first; on a tie a stable release beats a pre-release of
    //! the same number ("v5.5" over "v5.5-unstable.20260824.107.abc"), and two
    //! pre-releases of one number are ordered by their build ordinal. Without
    //! that last part a build could never be offered the stable it leads to,
    //! since both read 5.5.
    static bool isVersionNewer(const QString &candidate, const QString &current);

    //! A tag carrying a pre-release suffix, i.e. anything after the numbers.
    static bool isPreRelease(const QString &tag);

    //! The release this binary was built as. The full tag when the release
    //! workflow provided one, the plain version otherwise (a local build).
    static QString buildTag();

    static QString stripVersionPrefix(const QString &tag);

    QString latestTag()      const { return _latestTag; }
    QString releaseNotes()   const { return _releaseNotes; }
    QUrl    releasePageUrl() const { return _releasePageUrl; }

signals:
    void newVersionAvailable(const QString &tag, const QString &notes, const QUrl &releasePage);
    void downloadProgress(qint64 received, qint64 total);
    void downloadFailed(const QString &msg);
    void installStarting();

public slots:
    void startDownloadAndInstall();

private slots:
    void onReleaseInfoReceived();
    void onAssetDownloadFinished();

private:
    QString assetNameForCurrentOS(const QString &tag) const;
    bool    runInstallerWindows(const QString &archivePath);
    bool    runInstallerMacOS(const QString &archivePath);
    bool    runInstallerLinux(const QString &archivePath);

    static const QString sReleaseApiUrl;
    static const QString sReleaseListApiUrl;
    static const QString sRepoOwner;
    static const QString sRepoName;
    static const qint64  sCheckIntervalSeconds = 86400; // once per day

    NgPost                *_ngPost;
    QNetworkAccessManager *_netMgr;
    QNetworkReply         *_reply;

    QString _latestTag;
    QString _releaseNotes;
    QUrl    _assetUrl;
    QUrl    _releasePageUrl;
    QString _assetFileName;
    qint64  _assetSize;
};

#endif // UPDATECHECKER_H
