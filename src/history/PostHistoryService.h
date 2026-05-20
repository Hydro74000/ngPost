//========================================================================
//
// Queued SQLite service for structured posting history.
//
//========================================================================

#ifndef POSTHISTORYSERVICE_H
#define POSTHISTORYSERVICE_H

#include "history/PostHistoryStore.h"

#include <QList>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <functional>

class QTextStream;

class PostHistoryWorker;

class PostHistoryService : public QObject
{
    Q_OBJECT

public:
    struct ResumeRow {
        qint64 postId = 0;
        QString name;
        QString status;
        QString state;
        int postedArticles = 0;
        int pendingArticles = 0;
        int failedArticles = 0;
        int unknownArticles = 0;
        QString reason;
    };

    struct HistorySnapshot {
        QList<PostHistoryStore::PostSummary> posts;
        QList<ResumeRow> resumeRows;
        QString error;
    };

    struct StatsSnapshot {
        QStringList groups;
        QList<PostHistoryStore::DayStats> days;
        QList<PostHistoryStore::GroupStats> groupStats;
        QList<PostHistoryStore::PostSummary> topPosts;
        QString error;
    };

    using HistorySnapshotCallback = std::function<void(const HistorySnapshot &)>;
    using StatsSnapshotCallback = std::function<void(const StatsSnapshot &)>;
    using DetailsCallback =
        std::function<void(bool, const PostHistoryStore::PostDetails &, const QString &)>;

    explicit PostHistoryService(const QString &dbPath = QString(),
                                bool storePasswords = true,
                                QObject *parent = nullptr);
    ~PostHistoryService() override;

    void configure(const QString &dbPath, bool storePasswords);
    bool initialize(QString *error = nullptr);
    bool markPostCrashedArticlesUnknown(QString *error = nullptr);
    bool cleanupInvalidResumePosts(QString *error = nullptr);

    qint64 createPost(const PostHistoryStore::PostRecord &record, QString *error = nullptr);
    bool updatePostStatus(qint64 postId,
                          const QString &status,
                          int nbFiles,
                          int nbArticles,
                          int nbFailedArticles,
                          qint64 sizeBytes,
                          const QString &avgSpeed,
                          QString *error = nullptr);
    bool markPostResuming(qint64 postId, QString *error = nullptr);
    bool setPostAbandoned(qint64 postId, QString *error = nullptr);
    bool purgeResumeData(qint64 postId, QString *error = nullptr);
    bool purgePassword(qint64 postId, QString *error = nullptr);
    bool deletePost(qint64 postId, QString *error = nullptr);

    qint64 upsertFile(const PostHistoryStore::FileRecord &record, QString *error = nullptr);
    void enqueueUpdateFileStatus(qint64 fileId, const QString &status);

    void enqueueArticlePosting(qint64 fileId,
                               int part,
                               const QString &msgId,
                               int attemptNo,
                               qint64 pos,
                               qint64 bytes);
    void enqueueArticlePosted(qint64 fileId,
                              int part,
                              const QString &msgId,
                              qint64 pos,
                              qint64 bytes);
    void enqueueArticleFailed(qint64 fileId,
                              int part,
                              const QString &msgId,
                              const QString &reason,
                              qint64 pos,
                              qint64 bytes);
    void enqueueArticleUnknown(qint64 fileId,
                               int part,
                               const QString &msgId,
                               const QString &reason,
                               qint64 pos,
                               qint64 bytes);
    bool flush(QString *error = nullptr);

    bool loadPostDetails(qint64 postId,
                         PostHistoryStore::PostDetails *details,
                         QString *error = nullptr);
    bool checkResume(qint64 postId, ResumeRow *row, QString *error = nullptr);

    QList<PostHistoryStore::PostSummary> listPosts(const PostHistoryStore::ListFilter &filter,
                                                   QString *error = nullptr);
    QList<PostHistoryStore::PostSummary> resumeCandidates(QString *error = nullptr);

    bool exportCsv(QTextStream &stream, bool includePasswords, QString *error = nullptr);
    bool importLegacyCsv(const QString &path, QString *error = nullptr);

    bool regenerateNzb(qint64 postId,
                       QTextStream &stream,
                       bool includePassword,
                       QStringList *warnings,
                       QString *error = nullptr);
    bool regenerateNzbToFile(qint64 postId,
                             const QString &outPath,
                             bool includePassword,
                             QStringList *warnings,
                             QString *error = nullptr);

    void requestHistorySnapshot(const PostHistoryStore::ListFilter &filter,
                                const QSet<qint64> &ignoredResumeIds,
                                QObject *receiver,
                                HistorySnapshotCallback callback);
    void requestStatsSnapshot(const QString &dateFrom,
                              const QString &dateTo,
                              const QString &groupFilter,
                              QObject *receiver,
                              StatsSnapshotCallback callback);
    void requestPostDetails(qint64 postId, QObject *receiver, DetailsCallback callback);

signals:
    void error(QString msg);

private:
    template<typename Func>
    void _invokeQueued(Func func);

    template<typename Func>
    bool _invokeBlocking(Func func);

    PostHistoryWorker *_worker;
    QThread _thread;
};

#endif // POSTHISTORYSERVICE_H
