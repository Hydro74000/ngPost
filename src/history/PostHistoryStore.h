//========================================================================
//
// Structured posting history for ngPost.
//
//========================================================================

#ifndef POSTHISTORYSTORE_H
#define POSTHISTORYSTORE_H

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

class QTextStream;

class PostHistoryStore
{
public:
    struct PostRecord {
        QString nzbName;
        QString nzbPath;
        QString rarName;
        QString rarPass;
        QString passwordOrigin;
        QString from;
        QStringList groups;
        bool hasPassword = false;
        bool doCompress = false;
        bool doPar2 = false;
        bool obfuscateArticles = false;
        bool obfuscateFileName = false;
    };

    struct FileRecord {
        qint64 postId = 0;
        int ordinal = 0;
        QString originalPath;
        QString postedName;
        qint64 sizeBytes = 0;
        qint64 mtimeEpoch = 0;
        int totalArticles = 0;
        QStringList groups;
        QString status = QStringLiteral("pending");
    };

    struct ArticleRecord {
        qint64 fileId = 0;
        int part = 0;
        qint64 pos = 0;
        qint64 bytes = 0;
        QString status = QStringLiteral("pending");
        QString msgId;
        QString error;
    };

    struct ArticleEvent {
        enum class Kind {
            Posting,
            Posted,
            Failed,
            Unknown
        };

        Kind kind = Kind::Posted;
        qint64 fileId = 0;
        int part = 0;
        qint64 pos = 0;
        qint64 bytes = 0;
        int attemptNo = 0;
        QString msgId;
        QString error;
    };

    struct PostSummary {
        qint64 id = 0;
        QString nzbName;
        QString nzbPath;
        QString status;
        QString groups;
        QString createdAt;
        QString finishedAt;
        QString avgSpeed;
        qint64 sizeBytes = 0;
        int nbFiles = 0;
        int nbArticles = 0;
        int nbFailedArticles = 0;
        bool hasPassword = false;
        bool passwordStored = false;
        bool resumable = false;
        QString resumeReason;
    };

    struct FileSummary {
        qint64 id = 0;
        int ordinal = 0;
        QString originalPath;
        QString postedName;
        qint64 sizeBytes = 0;
        qint64 mtimeEpoch = 0;
        int totalArticles = 0;
        QString groups;
        QString status;
    };

    struct ArticleSummary {
        qint64 fileId = 0;
        int part = 0;
        qint64 bytes = 0;
        QString msgId;
        QString status;
    };

    struct PostDetails {
        PostSummary post;
        QString nzbPath;
        QString rarName;
        QString rarPass;
        QString passwordOrigin;
        QString from;
        QList<FileSummary> files;
        QMap<qint64, QList<ArticleSummary>> articlesByFile;
    };

    //! Filter struct for listPosts(). All fields are optional (empty = no filter).
    struct ListFilter {
        QString status;
        QString search;
        QString group;
        bool onlyWithPassword = false;
        bool onlyWithErrors   = false;
        QString dateFrom; //!< ISO date "YYYY-MM-DD"
        QString dateTo;   //!< ISO date "YYYY-MM-DD"
    };

    //! Aggregated stats for one calendar day.
    struct DayStats {
        QString date;          //!< "YYYY-MM-DD"
        int     nbPosts  = 0;
        int     nbFailed = 0;
        qint64  totalBytes = 0;
        double  avgSpeedBps = 0.0;
    };

    //! Aggregated stats per newsgroup.
    struct GroupStats {
        QString group;
        int    nbPosts    = 0;
        qint64 totalBytes = 0;
    };

    explicit PostHistoryStore(const QString &dbPath = QString(), bool storePasswords = true);
    ~PostHistoryStore();

    void configure(const QString &dbPath, bool storePasswords);
    void closeConnection();
    QString dbPath() const;
    bool storePasswords() const;

    bool initialize(QString *error = nullptr);

    qint64 createPost(const PostRecord &record, QString *error = nullptr);
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

    qint64 upsertFile(const FileRecord &record, QString *error = nullptr);
    bool updateFileStatus(qint64 fileId, const QString &status, QString *error = nullptr);
    bool upsertArticle(const ArticleRecord &record, QString *error = nullptr);
    bool updateArticlePayload(qint64 fileId,
                              int part,
                              qint64 pos,
                              qint64 bytes,
                              QString *error = nullptr);
    bool markArticlePosting(qint64 fileId,
                            int part,
                            const QString &msgId,
                            int attemptNo,
                            QString *error = nullptr);
    bool markArticlePosting(qint64 fileId,
                            int part,
                            const QString &msgId,
                            int attemptNo,
                            qint64 pos,
                            qint64 bytes,
                            QString *error = nullptr);
    bool markArticlePosted(qint64 fileId, int part, const QString &msgId, QString *error = nullptr);
    bool markArticlePosted(qint64 fileId,
                           int part,
                           const QString &msgId,
                           qint64 pos,
                           qint64 bytes,
                           QString *error = nullptr);
    bool markArticleFailed(qint64 fileId,
                           int part,
                           const QString &msgId,
                           const QString &err,
                           QString *error = nullptr);
    bool markArticleFailed(qint64 fileId,
                           int part,
                           const QString &msgId,
                           const QString &err,
                           qint64 pos,
                           qint64 bytes,
                           QString *error = nullptr);
    bool markArticleUnknown(qint64 fileId,
                            int part,
                            const QString &msgId,
                            const QString &err,
                            QString *error = nullptr);
    bool markArticleUnknown(qint64 fileId,
                            int part,
                            const QString &msgId,
                            const QString &err,
                            qint64 pos,
                            qint64 bytes,
                            QString *error = nullptr);
    bool applyArticleEvents(const QList<ArticleEvent> &events, QString *error = nullptr);
    bool markPostCrashedArticlesUnknown(QString *error = nullptr);
    bool cleanupInvalidResumePosts(QString *error = nullptr);

    // Primary query with full filter support.
    QList<PostSummary> listPosts(const ListFilter &filter, QString *error = nullptr);

    // Backward-compatible overload delegating to the primary.
    QList<PostSummary> listPosts(const QString &status = QString(),
                                 const QString &search = QString(),
                                 bool onlyWithPassword = false,
                                 QString *error = nullptr);

    QList<PostSummary> resumeCandidates(QString *error = nullptr);
    bool loadPostDetails(qint64 postId, PostDetails *details, QString *error = nullptr);

    // Stats queries.
    QList<DayStats>   statsByDay(const QString &dateFrom = QString(),
                                 const QString &dateTo   = QString(),
                                 const QString &group    = QString(),
                                 QString *error = nullptr);
    QList<GroupStats> statsByGroup(const QString &dateFrom = QString(),
                                   const QString &dateTo   = QString(),
                                   QString *error = nullptr);
    QList<PostSummary> topPostsBySize(int n = 20, QString *error = nullptr);
    QStringList allGroups(QString *error = nullptr);

    bool exportCsv(QTextStream &stream, bool includePasswords, QString *error = nullptr);
    bool importLegacyCsv(const QString &path, QString *error = nullptr);

private:
    QString _dbPath;
    bool _storePasswords;
    bool _initialized;
    QString _initializedDbPath;

    bool _execSchema(QString *error);
    bool _exec(const QString &sql, QString *error);
    QString _connectionName() const;
};

#endif // POSTHISTORYSTORE_H
