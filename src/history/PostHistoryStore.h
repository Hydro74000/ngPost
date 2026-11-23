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
class QSqlQuery;
class QTextStream;

#include <QHash>
#include <functional>
#include <optional>

namespace queue
{
struct QueueEntry;
struct QueueTransaction;
struct RunOutcome;
struct AttemptResult;
struct FinalizationStep;
struct QueueCommit;
struct PostNowIntent;
enum class Domain;
}

class PostHistoryStore
{
public:
    struct PostRecord
    {
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

    struct FileRecord
    {
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

    struct ArticleRecord
    {
        qint64 fileId = 0;
        int part = 0;
        qint64 pos = 0;
        qint64 bytes = 0;     //!< slice of the SOURCE file this article carries
        qint64 bodyBytes = 0; //!< size of the posted article once yEnc encoded
        QString status = QStringLiteral("pending");
        QString msgId;
        QString error;
    };

    struct ArticleEvent
    {
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
        qint64 bytes = 0;     //!< slice of the SOURCE file this article carries
        qint64 bodyBytes = 0; //!< size of the posted article once yEnc encoded
        int attemptNo = 0;
        QString msgId;
        QString error;
    };

    struct PostSummary
    {
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

    struct FileSummary
    {
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

    struct ArticleSummary
    {
        qint64 fileId = 0;
        int part = 0;
        //! pos/bytes describe the slice of the SOURCE file. They are what
        //! resolveArticleSizeBytes() reconstructs the configured article size
        //! from, so they must stay the decoded sizes.
        qint64 pos = 0;
        qint64 bytes = 0;
        //! Size of the article as the server stores it (yEnc encoded body).
        //! This -- not `bytes` -- is what an nzb <segment bytes> must carry.
        //! 0 on rows written before schema v4.
        qint64 bodyBytes = 0;
        QString msgId;
        QString status;
    };

    struct PostDetails
    {
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
        //! par2 percentage of the original post, < 0 when it had none. Read
        //! from post_info, so it stays -1 for a post made before that table.
        int par2Pct = -1;
        //! Effective payload bytes per article of the original attempt. When
        //! an older row has no stored value, loadPostDetails() derives it only
        //! if the recorded part boundaries prove one unambiguous value.
        qint64 articleSizeBytes = -1;
        //! Distinguishes a legacy row with no value from a current row whose
        //! stored boundary exists but contradicts its file/article records.
        bool articleSizeWasStored = false;
        QList<FileSummary> files;
        QMap<qint64, QList<ArticleSummary>> articlesByFile;
    };

    //! Facts about a post that the aggregates of `posts` cannot express.
    //! Negative or empty means "not recorded", which is how posts made before
    //! this table existed come back.
    struct PostInfo
    {
        int par2Pct = -1;             //!< < 0: no par2 at all
        qint64 postSizeBytes = -1;    //!< archive + parity, copied .nfo excluded
        qint64 activeSeconds = -1;    //!< transfer time, cumulated over resumes
        qint64 articleSizeBytes = -1; //!< payload bytes per Usenet article
        QString sourcePath;           //!< raw input path, before folder expansion
        QString originalName;
        QString appVersion;
    };

    //! Everything needed to describe a post, without loading its files and
    //! articles: a post info file needs none of those, and a large post has
    //! hundreds of thousands of article rows.
    struct PostInfoRecord
    {
        PostSummary post;
        PostInfo info;
        QString nzbPath;
        QString rarName;
        QString rarPass;
        QString from;
        QString startedAt;        //!< ISO 8601 UTC, empty while the transfer has not begun
        int nbArticlesPosted = 0; //!< rows whose persisted status is exactly "posted"
        QMap<QString, MetaValue> meta;
        //! Posted artifacts, loaded without their article rows. They are used
        //! as overwrite guards by historical exports.
        QStringList filePaths;
        //! True when the row predates post_info: the missing facts are then
        //! rendered empty rather than guessed.
        bool partial = false;

        //! Converts to what the template engine consumes, turning the UTC of
        //! the database into local time.
        PostInfoData toPostInfoData() const;
    };

    //! Filter struct for listPosts(). All fields are optional (empty = no filter).
    struct ListFilter
    {
        QString status;
        QString search;
        QString group;
        bool onlyWithPassword = false;
        bool onlyWithErrors = false;
        QString dateFrom; //!< ISO date "YYYY-MM-DD"
        QString dateTo;   //!< ISO date "YYYY-MM-DD"
        int limit = 0;    //!< <= 0 means unlimited
        int offset = 0;   //!< ignored when limit <= 0
    };

    //! Aggregated stats for one calendar day.
    struct DayStats
    {
        QString date; //!< "YYYY-MM-DD"
        int nbPosts = 0;
        int nbFailed = 0;
        qint64 totalBytes = 0;
        double avgSpeedBps = 0.0;
    };

    //! Aggregated stats per newsgroup.
    struct GroupStats
    {
        QString group;
        int nbPosts = 0;
        qint64 totalBytes = 0;
    };

    explicit PostHistoryStore(const QString &dbPath = QString(), bool storePasswords = true);
    ~PostHistoryStore();

#ifdef NGPOST_TESTING
    //! Shorten lock-contention tests without changing the production timeout.
    static void setBusyTimeoutForTest(int milliseconds);
    static void resetBusyTimeoutForTest();
#endif

    void configure(const QString &dbPath, bool storePasswords);
    void closeConnection();
    QString dbPath() const;
    bool storePasswords() const;

    bool initialize(QString *error = nullptr);

    //! Compatibility/legacy insertion. No post_info row is invented: callers
    //! that cannot provide the newer facts get a deliberately partial record.
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

    //! Terminal state of one attempt: the status and the time it took, written
    //! together so a crash in between cannot leave one without the other.
    bool finalizePost(qint64 postId,
                      const QString &status,
                      const QString &avgSpeed,
                      qint64 activeSeconds,
                      QString *error = nullptr);

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
    //! Normalize what crashed runs left, sparing the posts of live owners
    //! (liveOwners()).
    bool markPostCrashedArticlesUnknown(QString *error = nullptr);
    bool cleanupInvalidResumePosts(QString *error = nullptr);
    QStringList liveOwners() const;
    //! Where the run streams its NZB before the deliverable is published:
    //! recorded before its first article, so a recovery finds it.
    bool setRunScratchNzb(const QString &runId, const QString &path, QString *error = nullptr);
    //! Every file and article of the transfer is recorded: from now on the post
    //! is resumed from this manifest, never prepared again.
    bool markTransferManifestComplete(qint64 postId, QString *error = nullptr);

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
    QList<DayStats> statsByDay(const QString &dateFrom = QString(),
                               const QString &dateTo = QString(),
                               const QString &group = QString(),
                               QString *error = nullptr);
    QList<GroupStats> statsByGroup(const QString &dateFrom = QString(),
                                   const QString &dateTo = QString(),
                                   QString *error = nullptr);
    QList<PostSummary> topPostsBySize(int n = 20, QString *error = nullptr);
    QStringList allGroups(QString *error = nullptr);

    bool exportCsv(QTextStream &stream, bool includePasswords, QString *error = nullptr);
    bool importLegacyCsv(const QString &path, QString *error = nullptr);

    //! Bump when the schema changes, and add the matching step in
    //! _migrateSchema(). v1: initial. v2: post_info/post_meta. v3: the frozen
    //! article-size boundary required for a byte-identical resume.
    static constexpr int kSchemaVersion = 6;

    // --- queue manager (plan_queue.md §5), implemented in PostHistoryStoreQueue.cpp ---
    //! Identity of the process that owns what it admits (posts.owner_id).
    void setOwnerId(const QString &ownerId) { _ownerId = ownerId; }
    QString ownerId() const { return _ownerId; }
    //! Applies a whole queue transaction in one SQL transaction, or nothing.
    queue::QueueCommit commitQueueTransaction(const queue::QueueTransaction &transaction);
    QList<queue::QueueEntry> loadQueueEntries(const QString &queueId, QString *error = nullptr);
    QList<qint64> loadQueueOrder(const QString &queueId,
                                 queue::Domain domain,
                                 QString *error = nullptr);
    std::optional<queue::PostNowIntent> loadQueueIntent(const QString &queueId,
                                                        QString *error = nullptr);
    //! Start-up normalization of interrupted posts, limited to posts no live
    //! owner holds (legacy rows have none). A post still in a queue is never
    //! declared cancelled: its recovery decides from the facts.
    bool normalizeInterruptedPosts(const QStringList &liveOwners, QString *error = nullptr);
    //! The resume clean-up of start-up, with the same ownership limits.
    bool cleanupInvalidResumePosts(const QStringList &liveOwners, QString *error);
    //! Moves the entries of a dead owner's queue to @p toQueueId, after its
    //! entries of the same domain and in their own order, and hands their posts
    //! to @p toOwnerId. Its "post now" intent is abandoned. Returns the count.
    int claimQueue(const QString &fromQueueId,
                   const QString &fromOwnerId,
                   const QString &toQueueId,
                   const QString &toOwnerId,
                   queue::Domain domain,
                   QString *error = nullptr);
    //! Queue ids present in the database, with their entry count.
    QHash<QString, int> queueIds(QString *error = nullptr);

    // --- recoverable preparation (plan_queue.md §5.5, lot C5), PostHistoryStorePreparation.cpp ---
    //! Where a set of archives is prepared: recorded before anything is created
    //! on disk, so that a recovery knows what may be ours.
    struct ArtifactSetPlan
    {
        QString setId;
        qint64 postId = 0;
        QString runId;
        int generation = 0;
        quint64 recipeRevision = 0;
        QString root;       //!< effective TMP_DIR or TMP_RAM
        QString archiveDir; //!< archives of this generation only
        QString stagingDir; //!< sources moved aside, never mixed with the archives
    };
    //! One file of a verified set (post_run_artifacts).
    struct ArtifactRecord
    {
        int sequence = 0;
        QString path;
        qint64 size = 0;
        QString mtime;
        QString identityJson; //!< native identity: (device, inode) or (volume, file index)
        QString role;         //!< archive, par2, nfo, staged_source
        bool publish = false; //!< part of the explicit list of files to post
        QString groups;
        bool operator==(const ArtifactRecord &o) const
        {
            return sequence == o.sequence && path == o.path && size == o.size && mtime == o.mtime
                && identityJson == o.identityJson && role == o.role && publish == o.publish
                && groups == o.groups;
        }
    };
    struct ArtifactSetRecord
    {
        ArtifactSetPlan plan;
        QString state;
        bool retained = false;
        QString markerIdentityJson;
        QString supervision; //!< how the tools of the set are proven stopped
        QString toolState;   //!< idle, starting, running, quiescing, stopped_verified, unproven
        QString toolIdentityJson;
        qint64 bytes = 0;
        QList<ArtifactRecord> artifacts;
    };
    //! A source moved aside by an obfuscation, journaled before the rename.
    struct SourceMove
    {
        int sequence = 0;
        QString sourcePath;
        QString stagedPath;
        QString identityJson;
        QString state; //!< planned, moved, restored, blocked
        QString runId;
        QString setId;
    };
    //! The places of a new set (or of the one the queue just opened for the run).
    bool createArtifactSet(const ArtifactSetPlan &plan, QString *error = nullptr);
    //! The marker that proves a folder is the set's, once created.
    bool recordArtifactMarker(qint64 postId,
                              const QString &runId,
                              const QString &setId,
                              const QString &markerIdentityJson,
                              QString *error = nullptr);
    //! The tool of a set and its supervision: a tool starts only once how it is
    //! stopped is recorded, and its files are touched only once it is proven stopped.
    bool recordToolSupervision(qint64 postId,
                               const QString &runId,
                               const QString &setId,
                               const QString &capability,
                               const QString &toolState,
                               const QString &toolIdentityJson,
                               QString *error = nullptr);
    //! Intents to move sources: written before any rename, all or none.
    bool planSourceMoves(qint64 postId,
                         const QString &runId,
                         const QString &setId,
                         const QList<SourceMove> &moves,
                         QString *error = nullptr);
    //! What became of planned moves: moved, restored or blocked. Allowed after
    //! the post left the queue: the journal outlives the entry.
    bool resolveSourceMoves(qint64 postId,
                            const QString &runId,
                            const QList<int> &sequences,
                            const QString &state,
                            QString *error = nullptr);
    //! Moves not settled yet (planned, moved or blocked), oldest first.
    QList<SourceMove> unsettledSourceMoves(qint64 postId, QString *error = nullptr);
    //! Move @p sequence of @p runId, any state, once this store's session is
    //! checked to own its post (lot C5.7): what a rollback checks before it
    //! touches the disk.
    std::optional<SourceMove> ownedSourceMove(qint64 postId,
                                              const QString &runId,
                                              int sequence,
                                              QString *error = nullptr);
    //! The complete list of what a set prepared: the set becomes verified and
    //! the entry's current one. A partial list is refused.
    bool recordPreparedArtifacts(qint64 postId,
                                 const QString &runId,
                                 const QString &setId,
                                 const QList<ArtifactRecord> &artifacts,
                                 QString *error = nullptr);
    std::optional<ArtifactSetRecord> artifactSet(const QString &setId, QString *error = nullptr);
    //! What may be renamed back or deleted for a set: only what its tool is
    //! proven not to touch any more (no tool ran, or its stop is proven). A tool
    //! that only "exited" (capability unavailable) never qualifies.
    static bool toolStopProven(const ArtifactSetRecord &set)
    {
        return set.toolState == QStringLiteral("idle")
            || set.toolState == QStringLiteral("stopped_verified");
    }

private:
    QString _dbPath;
    bool _storePasswords;
    bool _initialized;
    QString _initializedDbPath;

    bool _execSchema(QString *error);
    bool _migrateSchema(QSqlDatabase &db, QString *error);
    qint64 _createPost(const PostRecord &record,
                       const PostInfo *info,
                       const QMap<QString, MetaValue> &meta,
                       QString *error);
    //! Write helpers reused by createPost() inside its own transaction.
    bool _writePostInfo(QSqlDatabase &db, qint64 postId, const PostInfo &info, QString *error);
    bool _writePostMeta(QSqlDatabase &db,
                        qint64 postId,
                        const QMap<QString, MetaValue> &meta,
                        QString *error);
    bool _exec(const QString &sql, QString *error);
    QString _connectionName() const;

    // Queue manager (PostHistoryStoreQueue.cpp).
    QString _ownerId;
    void _widenToolStates(QSqlDatabase &db);
    // Recoverable preparation (PostHistoryStorePreparation.cpp).
    //! Runs @p work in one SQL transaction, or changes nothing.
    bool _preparationWrite(const std::function<bool(QSqlDatabase &, QString *)> &work,
                           QString *error);
    //! The set is the post's, opened by @p runId; its states are returned.
    bool _checkOwnSet(QSqlDatabase &db,
                      qint64 postId,
                      const QString &runId,
                      const QString &setId,
                      QString *state,
                      QString *toolState,
                      QString *error);
    QSqlDatabase _database(QString *error);
    static QString _now();
    static void _setQueryError(QString *error, const QSqlQuery &q);
    bool _migrateToV6(QSqlDatabase &db, QString *error);
    bool _refuseFutureSchema(QSqlDatabase &db, QString *error);

    bool _applyQueueTransaction(QSqlDatabase &db,
                                const queue::QueueTransaction &transaction,
                                queue::QueueCommit &result);
    bool _admitQueueEntry(QSqlDatabase &db, queue::QueueEntry &entry, QString *error);
    bool _admitExistingQueueEntry(QSqlDatabase &db, queue::QueueEntry &entry, QString *error);
    bool _checkPreconditions(QSqlDatabase &db,
                             const queue::QueueTransaction &transaction,
                             QString *error);
    bool _checkQueuedRun(QSqlDatabase &db, qint64 postId, const QString &runId, QString *error);
    bool _recordAttemptResult(QSqlDatabase &db,
                              const queue::AttemptResult &attempt,
                              QString *error);
    bool _finishQueuedRun(QSqlDatabase &db, const queue::FinalizationStep &step, QString *error);
    bool _releaseQueuedPost(QSqlDatabase &db, qint64 postId, QString *error);
    bool _recordRunOutcome(QSqlDatabase &db,
                           qint64 postId,
                           const queue::RunOutcome &outcome,
                           QString *error);
    bool _writeQueueEntry(QSqlDatabase &db,
                          const queue::QueueEntry &entry,
                          bool insert,
                          QString *error);
    bool _writeArtifactSets(QSqlDatabase &db, const queue::QueueEntry &entry, QString *error);
    bool _mirrorQueuedPost(QSqlDatabase &db, const queue::QueueEntry &entry, QString *error);
    bool _renewQueueRun(QSqlDatabase &db,
                        qint64 postId,
                        qint64 activeMs,
                        QString *runId,
                        int *runNo,
                        QString *error);
    bool _retireQueueEntry(QSqlDatabase &db, qint64 postId, QString *error);
    bool _writeQueueOrder(QSqlDatabase &db,
                          const QString &queueId,
                          queue::Domain domain,
                          const QList<qint64> &ids,
                          quint64 revision,
                          QString *error);
    bool _writeQueueIntent(QSqlDatabase &db,
                           const queue::QueueTransaction &transaction,
                           QString *error);
};

#endif // POSTHISTORYSTORE_H
