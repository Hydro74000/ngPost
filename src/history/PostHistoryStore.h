// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Structured posting history for ngPost.
//
//========================================================================

#ifndef POSTHISTORYSTORE_H
#define POSTHISTORYSTORE_H

#include "postinfo/PostInfoData.h"

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

class QSqlDatabase;
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
        //! How the original post was made. A resume must replay the obfuscation
        //! (it changes the poster and the group policy), but must NOT replay
        //! doCompress/doPar2: those are orders, and the archive already exists.
        bool obfuscateArticles = false;
        bool obfuscateFileName = false;
        bool doCompress = false;
        bool doPar2 = false;
        QList<FileSummary> files;
        QMap<qint64, QList<ArticleSummary>> articlesByFile;
    };

    //! Facts about a post that the aggregates of `posts` cannot express.
    //! Negative or empty means "not recorded", which is how posts made before
    //! this table existed come back.
    struct PostInfo {
        int par2Pct = -1;          //!< < 0: no par2 at all
        qint64 postSizeBytes = -1; //!< archive + parity, copied .nfo excluded
        qint64 activeSeconds = -1; //!< transfer time, cumulated over resumes
        QString sourcePath;        //!< raw input path, before folder expansion
        QString originalName;
        QString appVersion;
    };

    //! Everything needed to describe a post, without loading its files and
    //! articles: a post info file needs none of those, and a large post has
    //! hundreds of thousands of article rows.
    struct PostInfoRecord {
        PostSummary post;
        PostInfo info;
        QString nzbPath;
        QString rarName;
        QString rarPass;
        QString from;
        QString startedAt; //!< ISO 8601 UTC, empty while the transfer has not begun
        QMap<QString, MetaValue> meta;
        //! True when the row predates post_info: the missing facts are then
        //! rendered empty rather than guessed.
        bool partial = false;

        //! Converts to what the template engine consumes, turning the UTC of
        //! the database into local time.
        PostInfoData toPostInfoData() const;
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
        int limit = 0;    //!< <= 0 means unlimited
        int offset = 0;   //!< ignored when limit <= 0
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
    //! Same, plus the facts and the metadata of the post, all in one
    //! transaction so a post never exists without them.
    qint64 createPost(const PostRecord &record,
                      const PostInfo &info,
                      const QMap<QString, MetaValue> &meta,
                      QString *error = nullptr);

    //! Records when the transfer really started. Does nothing if it is already
    //! set: a resume must not rewrite the date of the original post.
    bool markPostStarted(qint64 postId, QString *error = nullptr);

    //! The nzb can be renamed to <name>_1.nzb when the first one already
    //! exists, after the history row was created. Keeps path and name in sync.
    bool updatePostNzbPath(qint64 postId, const QString &nzbPath, QString *error = nullptr);

    //! Written once, by the first attempt. A resume only handles the leftovers,
    //! so letting it write here would shrink the size of the whole post.
    bool setPostSizeIfUnset(qint64 postId, qint64 sizeBytes, QString *error = nullptr);

    //! Adds to the transfer time already recorded, so resumes accumulate.
    bool addActiveSeconds(qint64 postId, qint64 seconds, QString *error = nullptr);

    //! Upsert of the user metadata. The reserved key "password" is refused: it
    //! is a secret, handled like the archive password, not a metadata.
    bool setPostMeta(qint64 postId, const QMap<QString, MetaValue> &meta, QString *error = nullptr);

    bool loadPostInfoRecord(qint64 postId, PostInfoRecord *record, QString *error = nullptr);
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
    bool hasPostsAfter(const ListFilter &filter, QString *error = nullptr);

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

    //! Bump when the schema changes, and add the matching step in
    //! _migrateSchema(). v1: initial. v2: post_info and post_meta.
    static constexpr int kSchemaVersion = 2;

private:
    QString _dbPath;
    bool _storePasswords;
    bool _initialized;
    QString _initializedDbPath;

    bool _execSchema(QString *error);
    bool _migrateSchema(QSqlDatabase &db, QString *error);
    //! Write helpers reused by createPost() inside its own transaction.
    bool _writePostInfo(QSqlDatabase &db, qint64 postId, const PostInfo &info, QString *error);
    bool _writePostMeta(QSqlDatabase &db,
                        qint64 postId,
                        const QMap<QString, MetaValue> &meta,
                        QString *error);
    bool _exec(const QString &sql, QString *error);
    QString _connectionName() const;
};

#endif // POSTHISTORYSTORE_H
