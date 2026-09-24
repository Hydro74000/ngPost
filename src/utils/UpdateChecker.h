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
#include <QPointer>
#include <QTemporaryDir>
#include <QFile>
#include <memory>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QJsonObject;
class QJsonDocument;

class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit UpdateChecker(QNetworkAccessManager *netMgr, QObject *parent = nullptr);
    ~UpdateChecker() override;

    void checkLatestRelease();

    static bool isAppImage();
    static QString installationDirectory();
    static bool isReplaceableInstallation(const QString &directory);
    bool canInstallAutomatically() const;
    static bool isTrustedDownloadUrl(const QUrl &url);

    //! The install popup interrupts, so it comes back once a day at most; in
    //! between, the status-bar link keeps the update visible. A last prompt in
    //! the future (the clock was set back) never silences it.
    static bool isPromptDue(qint64 lastPromptEpoch, qint64 nowEpoch);

    //! A release body as Markdown fit for display. Its release notes are
    //! release_notes.txt, plain text: "=====" banners around section titles and
    //! "--- Title ---" subsections, which Markdown would render as stray "="
    //! runs and titles lost in the text. Both become headings. The leading
    //! title and the SHA-256 section are left to the release page.
    static QString releaseNotesMarkdown(const QString &body);

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
    void cancelDownload();

private slots:
    void onReleaseInfoReceived();

private:
#ifdef NGPOST_TESTING
    friend class TestUpdateChecker;
#endif
    static QJsonObject selectRelease(const QJsonDocument &document, const QString &current);
    QString assetNameForCurrentOS(const QString &tag) const;
    void downloadFile(const QUrl &url,
                      const QString &name,
                      qint64 cap,
                      const std::function<void()> &done);
    void _drainDownload(QNetworkReply *reply, qint64 cap, qint64 *written, QString *error);
    void _completeDownload(QNetworkReply *reply,
                           const QString &name,
                           qint64 written,
                           qint64 cap,
                           const QString &error,
                           const std::function<void()> &done);
    void prepareInstall();
    void failDownload(const QString &message);

    //! Drops the "cancelled" marker the detached installer polls for. False
    //! when it could not be written, which is the only case where a cancelled
    //! update can still install itself; a warning names the path either way.
    //! True when nothing is staged -- there is then nothing to call off.
    bool _markCancelledForInstaller();

    static const QString sReleaseApiUrl;
    static const QString sReleaseListApiUrl;
    static const QString sRepoOwner;
    static const QString sRepoName;
    static constexpr qint64 sPromptIntervalSeconds = 24 * 3600;

    QNetworkAccessManager *_netMgr;
    QNetworkReply         *_reply;
    QPointer<QNetworkReply> _downloadReply;
    QPointer<QProcess> _installer;
    std::unique_ptr<QTemporaryDir> _work;
    std::unique_ptr<QFile> _downloadFile;
    QString _installDir, _python;
    bool _busy = false;
    bool _cancelled = false;
    bool _handoff = false;
    //! A detached installer has been started and is polling the work folder.
    //! Until then a failed "cancelled" marker costs nothing; after it, that
    //! marker is the only thing standing between a cancellation and an install.
    bool _detached = false;
    //! Set by the destructor so nothing emits from a half-destroyed object.
    bool _destructing = false;
    quint64 _generation = 0;

    QString _latestTag;
    QString _releaseNotes;
    QUrl    _assetUrl;
    QUrl    _releasePageUrl;
    QString _assetFileName;
    qint64  _assetSize;
};

#endif // UPDATECHECKER_H
