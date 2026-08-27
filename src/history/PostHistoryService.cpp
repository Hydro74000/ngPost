// Copyright (C) 2024-2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// Queued SQLite service for structured posting history.
//
//========================================================================

#include "history/PostHistoryService.h"

#include "history/NzbHistoryRegenerator.h"
#include "history/ResumePlanner.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QIODevice>
#include <QMetaObject>
#include <QPointer>
#include <QSaveFile>
#include <QTextStream>
#include <QTimer>
#include <QtGlobal>

namespace
{

constexpr int sFlushIntervalMs = 100;
constexpr int sRetryIntervalMs = 250;
constexpr int sErrorThrottleMs = 2000;
constexpr int sMinBatchSize    = 32;
constexpr int sMaxBatchSize    = 512;

bool isBusyError(const QString &err)
{
    const QString lower = err.toLower();
    return lower.contains(QStringLiteral("database is locked"))
        || lower.contains(QStringLiteral("database table is locked"))
        || lower.contains(QStringLiteral("database is busy"))
        || lower.contains(QStringLiteral("unable to fetch row"));
}

} // namespace

class PostHistoryWorker : public QObject
{
public:
    PostHistoryWorker(const QString &dbPath, bool storePasswords, PostHistoryService *service)
        : QObject(nullptr)
        , _store(dbPath, storePasswords)
        , _service(service)
        , _flushTimer(this)
        , _retryTimer(this)
        , _lastFlushTimer()
        , _lastErrorTimer()
        , _batchSize(sMinBatchSize)
    {
        _flushTimer.setInterval(sFlushIntervalMs);
        _retryTimer.setSingleShot(true);
        connect(&_flushTimer, &QTimer::timeout, this, [this]() { flushArticleEventsOnce(); });
        connect(&_retryTimer, &QTimer::timeout, this, [this]() { flushArticleEventsOnce(); });
    }

    //! Starts the flush machinery only — deliberately without opening or
    //! creating the database.
    //!
    //! The service is built from NgPost's constructor, before the config file
    //! is read, so at this point the path is still the default one. Creating
    //! the database here meant every single run — `--help` and `--version`
    //! included — dropped a fully schema'd, permanently empty SQLite file at
    //! that default path, even when POST_DB pointed somewhere else entirely.
    //! Harmless data-wise, but it is what left stray ngPost_history.sqlite
    //! files in every config folder ngPost ever resolved, and a plausible
    //! looking empty history is a first-class red herring when someone is
    //! trying to work out where their posts went.
    //!
    //! Nothing needs it: every PostHistoryStore entry point opens with
    //! `if (!initialize(error))`, and NgPost calls _ensureHistoryStore()
    //! before any post and any history command. The database is therefore
    //! created on first real use, against the configured path, and a path
    //! that cannot be opened is now reported against the path that is
    //! actually in use rather than against a default nobody asked for.
    void start()
    {
        _lastFlushTimer.start();
        _lastErrorTimer.invalidate();
        _flushTimer.start();
    }

    void stop()
    {
        _flushTimer.stop();
        _retryTimer.stop();
        QString err;
        if (!flushArticleEventsBlocking(&err))
            reportError(QObject::tr("History service flush failed: %1").arg(err));
        _store.closeConnection();
    }

    void configure(const QString &dbPath, bool storePasswords)
    {
        QString err;
        if (!flushArticleEventsBlocking(&err)) {
            reportError(QObject::tr("History service flush failed: %1").arg(err));
            return;
        }
        _store.configure(dbPath, storePasswords);
    }

    void prepareForUse()
    {
        QString err;
        const bool ok = _store.markPostCrashedArticlesUnknown(&err)
            && _store.cleanupInvalidResumePosts(&err);
        if (!ok)
            reportError(err);
        emit _service->prepared(ok);
    }

    bool initialize(QString *error)
    {
        return _store.initialize(error);
    }

    bool markPostCrashedArticlesUnknown(QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.markPostCrashedArticlesUnknown(error);
    }

    bool cleanupInvalidResumePosts(QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.cleanupInvalidResumePosts(error);
    }

    qint64 createPost(const PostHistoryStore::PostRecord &record, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return 0;
        return _store.createPost(record, error);
    }

    qint64 createPost(const PostHistoryStore::PostRecord &record,
                      const PostHistoryStore::PostInfo &info,
                      const QMap<QString, MetaValue> &meta,
                      QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return 0;
        return _store.createPost(record, info, meta, error);
    }

    bool markPostStarted(qint64 postId, QString *error)
    {
        return _store.markPostStarted(postId, error);
    }

    bool updatePostNzbPath(qint64 postId, const QString &nzbPath, QString *error)
    {
        return _store.updatePostNzbPath(postId, nzbPath, error);
    }

    bool setPostSizeIfUnset(qint64 postId, qint64 sizeBytes, QString *error)
    {
        return _store.setPostSizeIfUnset(postId, sizeBytes, error);
    }

    bool addActiveSeconds(qint64 postId, qint64 seconds, QString *error)
    {
        return _store.addActiveSeconds(postId, seconds, error);
    }

    bool finalizePost(qint64 postId,
                      const QString &status,
                      const QString &avgSpeed,
                      qint64 activeSeconds,
                      QString *error)
    {
        // The pending article events decide the counters this very call
        // recomputes; losing them silently would finalise a post on stale
        // numbers.
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.finalizePost(postId, status, avgSpeed, activeSeconds, error);
    }

    bool setPostMeta(qint64 postId, const QMap<QString, MetaValue> &meta, QString *error)
    {
        return _store.setPostMeta(postId, meta, error);
    }

    bool loadPostInfoRecord(qint64 postId, PostHistoryStore::PostInfoRecord *record, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.loadPostInfoRecord(postId, record, error);
    }

    bool updatePostStatus(qint64 postId,
                          const QString &status,
                          int nbFiles,
                          int nbArticles,
                          int nbFailedArticles,
                          qint64 sizeBytes,
                          const QString &avgSpeed,
                          QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.updatePostStatus(postId,
                                       status,
                                       nbFiles,
                                       nbArticles,
                                       nbFailedArticles,
                                       sizeBytes,
                                       avgSpeed,
                                       error);
    }

    bool markPostResuming(qint64 postId, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.markPostResuming(postId, error);
    }

    bool setPostAbandoned(qint64 postId, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.setPostAbandoned(postId, error);
    }

    bool purgeResumeData(qint64 postId, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.purgeResumeData(postId, error);
    }

    bool purgePassword(qint64 postId, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.purgePassword(postId, error);
    }

    bool deletePost(qint64 postId, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.deletePost(postId, error);
    }

    qint64 upsertFile(const PostHistoryStore::FileRecord &record, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return 0;
        return _store.upsertFile(record, error);
    }

    void enqueueUpdateFileStatus(qint64 fileId, const QString &status)
    {
        QString err;
        if (!flushArticleEventsBlocking(&err)) {
            reportError(QObject::tr("History service flush failed: %1").arg(err));
            return;
        }
        if (!_store.updateFileStatus(fileId, status, &err))
            reportError(QObject::tr("History file status update failed: %1").arg(err));
    }

    void enqueueArticleEvent(const PostHistoryStore::ArticleEvent &event)
    {
        if (!event.fileId || event.part <= 0)
            return;
        _articleEvents << event;
        if (_articleEvents.size() >= _batchSize && !_retryTimer.isActive())
            flushArticleEventsOnce();
    }

    bool flushArticleEventsBlocking(QString *error)
    {
        QElapsedTimer waitTimer;
        waitTimer.start();
        while (!_articleEvents.isEmpty()) {
            QString err;
            if (flushArticleEventsOnce(&err))
                continue;
            if (error)
                *error = err;
            if (!isBusyError(err))
                return false;
            if (waitTimer.elapsed() > 30000)
                return false;
            QThread::msleep(sRetryIntervalMs);
        }
        return true;
    }

    bool loadPostDetails(qint64 postId, PostHistoryStore::PostDetails *details, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        return _store.loadPostDetails(postId, details, error);
    }

    QList<PostHistoryStore::PostSummary> listPosts(const PostHistoryStore::ListFilter &filter,
                                                   QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return {};
        if (error)
            error->clear();
        return _store.listPosts(filter, error);
    }

    QList<PostHistoryStore::PostSummary> resumeCandidates(QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return {};
        if (error)
            error->clear();
        return _store.resumeCandidates(error);
    }

    bool exportCsv(QTextStream &stream, bool includePasswords, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        if (error)
            error->clear();
        return _store.exportCsv(stream, includePasswords, error);
    }

    bool importLegacyCsv(const QString &path, QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        if (error)
            error->clear();
        return _store.importLegacyCsv(path, error);
    }

    bool regenerateNzb(qint64 postId,
                       QTextStream &stream,
                       bool includePassword,
                       QStringList *warnings,
                       QString *error)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        NzbHistoryRegenerator regenerator(&_store);
        return regenerator.writeNzb(postId, stream, includePassword, warnings, error);
    }

    bool regenerateNzbToFile(qint64 postId,
                             const QString &outPath,
                             bool includePassword,
                             QStringList *warnings,
                             QString *error,
                             const QString &passwordOverride)
    {
        if (!flushArticleEventsBlocking(error))
            return false;
        QSaveFile file(outPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            if (error)
                *error = QObject::tr("Could not open file for writing: %1 (%2)")
                             .arg(outPath, file.errorString());
            return false;
        }
        QTextStream stream(&file);
        NzbHistoryRegenerator regenerator(&_store);
        if (!regenerator.writeNzb(postId,
                                  stream,
                                  includePassword,
                                  warnings,
                                  error,
                                  passwordOverride)) {
            file.cancelWriting();
            return false;
        }
        stream.flush();
        if (stream.status() != QTextStream::Ok || !file.commit()) {
            if (error)
                *error = QObject::tr("Could not finish writing %1: %2")
                             .arg(outPath, file.errorString());
            return false;
        }
        return true;
    }

    bool checkResume(qint64 postId, PostHistoryService::ResumeRow *row, QString *error)
    {
        if (!row)
            return false;
        *row = PostHistoryService::ResumeRow();
        row->postId = postId;
        if (!flushArticleEventsBlocking(error)) {
            row->state = QStringLiteral("not_resumable");
            if (error)
                row->reason = *error;
            return false;
        }
        ResumePlanner planner(&_store);
        const ResumePlanner::Decision dec = planner.check(postId, error);
        row->state = dec.state == ResumePlanner::ResumeState::Resumable
            ? QStringLiteral("resumable")
            : dec.state == ResumePlanner::ResumeState::PartiallyResumable
                ? QStringLiteral("partial_resumable")
                : QStringLiteral("not_resumable");
        row->postedArticles = dec.postedArticles;
        row->pendingArticles = dec.pendingArticles;
        row->failedArticles = dec.failedArticles;
        row->unknownArticles = dec.unknownArticles;
        row->reason = dec.reason;
        return dec.state != ResumePlanner::ResumeState::NotResumable;
    }

    PostHistoryService::HistorySnapshot historySnapshot(const PostHistoryStore::ListFilter &filter,
                                                        const QSet<qint64> &ignoredResumeIds)
    {
        PostHistoryService::HistorySnapshot snapshot;
        snapshot.pageOffset = filter.offset > 0 ? filter.offset : 0;
        snapshot.pageLimit = filter.limit > 0 ? filter.limit : 0;
        snapshot.hasPreviousPage = snapshot.pageLimit > 0 && snapshot.pageOffset > 0;

        QString err;
        if (!flushArticleEventsBlocking(&err)) {
            snapshot.error = err;
            return snapshot;
        }
        err.clear();
        snapshot.posts = _store.listPosts(filter, &err);
        if (!err.isEmpty())
            snapshot.error = err;

        if (snapshot.error.isEmpty() && snapshot.pageLimit > 0) {
            err.clear();
            snapshot.hasNextPage = _store.hasPostsAfter(filter, &err);
            if (!err.isEmpty())
                snapshot.error = err;
        }

        err.clear();
        const QList<PostHistoryStore::PostSummary> candidates = _store.resumeCandidates(&err);
        if (!err.isEmpty() && snapshot.error.isEmpty())
            snapshot.error = err;

        ResumePlanner planner(&_store);
        for (const PostHistoryStore::PostSummary &post : candidates) {
            if (ignoredResumeIds.contains(post.id))
                continue;
            QString checkErr;
            const ResumePlanner::Decision dec = planner.check(post.id, &checkErr);
            if (!checkErr.isEmpty()) {
                snapshot.error = checkErr;
                snapshot.resumeRows.clear();
                return snapshot;
            }
            if (dec.state == ResumePlanner::ResumeState::NotResumable)
                continue;
            PostHistoryService::ResumeRow row;
            row.postId = post.id;
            row.name = post.nzbName;
            row.status = post.status;
            row.state = dec.state == ResumePlanner::ResumeState::Resumable
                ? QStringLiteral("resumable")
                : QStringLiteral("partial_resumable");
            row.postedArticles = dec.postedArticles;
            row.pendingArticles = dec.pendingArticles;
            row.failedArticles = dec.failedArticles;
            row.unknownArticles = dec.unknownArticles;
            row.reason = dec.reason;
            snapshot.resumeRows << row;
        }
        return snapshot;
    }

    PostHistoryService::StatsSnapshot statsSnapshot(const QString &dateFrom,
                                                    const QString &dateTo,
                                                    const QString &groupFilter)
    {
        PostHistoryService::StatsSnapshot snapshot;
        QString err;
        if (!flushArticleEventsBlocking(&err)) {
            snapshot.error = err;
            return snapshot;
        }

        err.clear();
        snapshot.groups = _store.allGroups(&err);
        if (!err.isEmpty() && snapshot.error.isEmpty())
            snapshot.error = err;

        err.clear();
        snapshot.days = _store.statsByDay(dateFrom, dateTo, groupFilter, &err);
        if (!err.isEmpty() && snapshot.error.isEmpty())
            snapshot.error = err;

        err.clear();
        snapshot.groupStats = _store.statsByGroup(dateFrom, dateTo, &err);
        if (!err.isEmpty() && snapshot.error.isEmpty())
            snapshot.error = err;

        err.clear();
        snapshot.topPosts = _store.topPostsBySize(20, &err);
        if (!err.isEmpty() && snapshot.error.isEmpty())
            snapshot.error = err;

        return snapshot;
    }

private:
    bool flushArticleEventsOnce(QString *error = nullptr)
    {
        if (_articleEvents.isEmpty())
            return true;

        QString initErr;
        if (!_store.initialize(&initErr)) {
            if (error)
                *error = initErr;
            reportError(QObject::tr("History service initialization failed: %1").arg(initErr));
            return false;
        }

        const QList<PostHistoryStore::ArticleEvent> batch = _articleEvents;
        _articleEvents.clear();
        const qint64 elapsed = _lastFlushTimer.isValid() ? _lastFlushTimer.restart() : 0;

        QString err;
        if (!_store.applyArticleEvents(batch, &err)) {
            QList<PostHistoryStore::ArticleEvent> pending = batch;
            pending.append(_articleEvents);
            _articleEvents = pending;
            if (error)
                *error = err;
            if (isBusyError(err)) {
                if (!_retryTimer.isActive())
                    _retryTimer.start(sRetryIntervalMs);
                reportErrorThrottled(QObject::tr("History service busy, retrying: %1").arg(err));
            } else {
                reportErrorThrottled(QObject::tr("History service flush failed: %1").arg(err));
            }
            return false;
        }

        adaptBatchSize(batch.size(), elapsed);
        return true;
    }

    void adaptBatchSize(int flushedCount, qint64 elapsedMs)
    {
        if (elapsedMs <= 0)
            return;
        const double rate = flushedCount / static_cast<double>(elapsedMs);
        const int projected = static_cast<int>(rate * sFlushIntervalMs);
        _batchSize = qBound(sMinBatchSize, projected, sMaxBatchSize);
    }

    void reportErrorThrottled(const QString &msg)
    {
        if (_lastErrorTimer.isValid() && _lastErrorTimer.elapsed() < sErrorThrottleMs)
            return;
        _lastErrorTimer.restart();
        reportError(msg);
    }

    void reportError(const QString &msg)
    {
        QPointer<PostHistoryService> service(_service);
        QMetaObject::invokeMethod(_service, [service, msg]() {
            if (service)
                emit service->error(msg);
        }, Qt::QueuedConnection);
    }

    PostHistoryStore _store;
    PostHistoryService *_service;
    QList<PostHistoryStore::ArticleEvent> _articleEvents;
    QTimer _flushTimer;
    QTimer _retryTimer;
    QElapsedTimer _lastFlushTimer;
    QElapsedTimer _lastErrorTimer;
    int _batchSize;
};

template<typename Func>
void PostHistoryService::_invokeQueued(Func func)
{
    if (!_worker)
        return;
    QMetaObject::invokeMethod(_worker, [worker = _worker, func]() { func(worker); }, Qt::QueuedConnection);
}

template<typename Func>
bool PostHistoryService::_invokeBlocking(Func func)
{
    if (!_worker)
        return false;
    if (QThread::currentThread() == _worker->thread()) {
        func(_worker);
        return true;
    }
    return QMetaObject::invokeMethod(_worker,
                                     [worker = _worker, func]() { func(worker); },
                                     Qt::BlockingQueuedConnection);
}

PostHistoryService::PostHistoryService(const QString &dbPath,
                                       bool storePasswords,
                                       QObject *parent)
    : QObject(parent)
    , _worker(new PostHistoryWorker(dbPath, storePasswords, this))
    , _thread()
{
    _thread.setObjectName(QStringLiteral("PostHistoryService"));
    _worker->moveToThread(&_thread);
    connect(&_thread, &QThread::finished, _worker, &QObject::deleteLater);
    _thread.start();
    _invokeQueued([](PostHistoryWorker *worker) { worker->start(); });
}

PostHistoryService::~PostHistoryService()
{
    if (_thread.isRunning()) {
        _invokeBlocking([](PostHistoryWorker *worker) { worker->stop(); });
        _thread.quit();
        _thread.wait();
    }
    _worker = nullptr;
}

void PostHistoryService::configure(const QString &dbPath, bool storePasswords)
{
    _invokeBlocking([&](PostHistoryWorker *worker) { worker->configure(dbPath, storePasswords); });
}

void PostHistoryService::prepareForUse()
{
    _invokeQueued([](PostHistoryWorker *worker) { worker->prepareForUse(); });
}

bool PostHistoryService::initialize(QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) { ok = worker->initialize(&err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::markPostCrashedArticlesUnknown(QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) {
        ok = worker->markPostCrashedArticlesUnknown(&err);
    });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::cleanupInvalidResumePosts(QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) {
        ok = worker->cleanupInvalidResumePosts(&err);
    });
    if (error)
        *error = err;
    return ok;
}

qint64 PostHistoryService::createPost(const PostHistoryStore::PostRecord &record, QString *error)
{
    QString err;
    qint64 id = 0;
    _invokeBlocking([&](PostHistoryWorker *worker) { id = worker->createPost(record, &err); });
    if (error)
        *error = err;
    return id;
}

bool PostHistoryService::updatePostStatus(qint64 postId,
                                          const QString &status,
                                          int nbFiles,
                                          int nbArticles,
                                          int nbFailedArticles,
                                          qint64 sizeBytes,
                                          const QString &avgSpeed,
                                          QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) {
        ok = worker->updatePostStatus(postId,
                                      status,
                                      nbFiles,
                                      nbArticles,
                                      nbFailedArticles,
                                      sizeBytes,
                                      avgSpeed,
                                      &err);
    });
    if (error)
        *error = err;
    return ok;
}

qint64 PostHistoryService::createPost(const PostHistoryStore::PostRecord &record,
                                     const PostHistoryStore::PostInfo &info,
                                     const QMap<QString, MetaValue> &meta,
                                     QString *error)
{
    QString err;
    qint64 id = 0;
    _invokeBlocking(
        [&](PostHistoryWorker *worker) { id = worker->createPost(record, info, meta, &err); });
    if (error)
        *error = err;
    return id;
}

bool PostHistoryService::markPostStarted(qint64 postId, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) { ok = worker->markPostStarted(postId, &err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::updatePostNzbPath(qint64 postId, const QString &nzbPath, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking(
        [&](PostHistoryWorker *worker) { ok = worker->updatePostNzbPath(postId, nzbPath, &err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::setPostSizeIfUnset(qint64 postId, qint64 sizeBytes, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking(
        [&](PostHistoryWorker *worker) { ok = worker->setPostSizeIfUnset(postId, sizeBytes, &err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::addActiveSeconds(qint64 postId, qint64 seconds, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking(
        [&](PostHistoryWorker *worker) { ok = worker->addActiveSeconds(postId, seconds, &err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::setPostMeta(qint64 postId,
                                     const QMap<QString, MetaValue> &meta,
                                     QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) { ok = worker->setPostMeta(postId, meta, &err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::loadPostInfoRecord(qint64 postId,
                                            PostHistoryStore::PostInfoRecord *record,
                                            QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking(
        [&](PostHistoryWorker *worker) { ok = worker->loadPostInfoRecord(postId, record, &err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::finalizePost(qint64 postId,
                                      const QString &status,
                                      const QString &avgSpeed,
                                      qint64 activeSeconds,
                                      QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) {
        ok = worker->finalizePost(postId, status, avgSpeed, activeSeconds, &err);
    });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::markPostResuming(qint64 postId, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) { ok = worker->markPostResuming(postId, &err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::setPostAbandoned(qint64 postId, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) { ok = worker->setPostAbandoned(postId, &err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::purgeResumeData(qint64 postId, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) { ok = worker->purgeResumeData(postId, &err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::purgePassword(qint64 postId, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) { ok = worker->purgePassword(postId, &err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::deletePost(qint64 postId, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) { ok = worker->deletePost(postId, &err); });
    if (error)
        *error = err;
    return ok;
}

qint64 PostHistoryService::upsertFile(const PostHistoryStore::FileRecord &record, QString *error)
{
    QString err;
    qint64 id = 0;
    _invokeBlocking([&](PostHistoryWorker *worker) { id = worker->upsertFile(record, &err); });
    if (error)
        *error = err;
    return id;
}

void PostHistoryService::enqueueUpdateFileStatus(qint64 fileId, const QString &status)
{
    _invokeQueued([=](PostHistoryWorker *worker) { worker->enqueueUpdateFileStatus(fileId, status); });
}

void PostHistoryService::enqueueArticlePosting(qint64 fileId,
                                               int part,
                                               const QString &msgId,
                                               int attemptNo,
                                               qint64 pos,
                                               qint64 bytes)
{
    PostHistoryStore::ArticleEvent event;
    event.kind = PostHistoryStore::ArticleEvent::Kind::Posting;
    event.fileId = fileId;
    event.part = part;
    event.msgId = msgId;
    event.attemptNo = attemptNo;
    event.pos = pos;
    event.bytes = bytes;
    _invokeQueued([event](PostHistoryWorker *worker) { worker->enqueueArticleEvent(event); });
}

void PostHistoryService::enqueueArticlePosted(qint64 fileId,
                                              int part,
                                              const QString &msgId,
                                              qint64 pos,
                                              qint64 bytes)
{
    PostHistoryStore::ArticleEvent event;
    event.kind = PostHistoryStore::ArticleEvent::Kind::Posted;
    event.fileId = fileId;
    event.part = part;
    event.msgId = msgId;
    event.pos = pos;
    event.bytes = bytes;
    _invokeQueued([event](PostHistoryWorker *worker) { worker->enqueueArticleEvent(event); });
}

void PostHistoryService::enqueueArticleFailed(qint64 fileId,
                                              int part,
                                              const QString &msgId,
                                              const QString &reason,
                                              qint64 pos,
                                              qint64 bytes)
{
    PostHistoryStore::ArticleEvent event;
    event.kind = PostHistoryStore::ArticleEvent::Kind::Failed;
    event.fileId = fileId;
    event.part = part;
    event.msgId = msgId;
    event.error = reason;
    event.pos = pos;
    event.bytes = bytes;
    _invokeQueued([event](PostHistoryWorker *worker) { worker->enqueueArticleEvent(event); });
}

void PostHistoryService::enqueueArticleUnknown(qint64 fileId,
                                               int part,
                                               const QString &msgId,
                                               const QString &reason,
                                               qint64 pos,
                                               qint64 bytes)
{
    PostHistoryStore::ArticleEvent event;
    event.kind = PostHistoryStore::ArticleEvent::Kind::Unknown;
    event.fileId = fileId;
    event.part = part;
    event.msgId = msgId;
    event.error = reason;
    event.pos = pos;
    event.bytes = bytes;
    _invokeQueued([event](PostHistoryWorker *worker) { worker->enqueueArticleEvent(event); });
}

bool PostHistoryService::flush(QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) { ok = worker->flushArticleEventsBlocking(&err); });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::loadPostDetails(qint64 postId,
                                         PostHistoryStore::PostDetails *details,
                                         QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) {
        ok = worker->loadPostDetails(postId, details, &err);
    });
    if (error)
        *error = err;
    return ok;
}

QList<PostHistoryStore::PostSummary> PostHistoryService::listPosts(
    const PostHistoryStore::ListFilter &filter, QString *error)
{
    QString err;
    QList<PostHistoryStore::PostSummary> posts;
    _invokeBlocking([&](PostHistoryWorker *worker) { posts = worker->listPosts(filter, &err); });
    if (error)
        *error = err;
    return posts;
}

QList<PostHistoryStore::PostSummary> PostHistoryService::resumeCandidates(QString *error)
{
    QString err;
    QList<PostHistoryStore::PostSummary> posts;
    _invokeBlocking([&](PostHistoryWorker *worker) { posts = worker->resumeCandidates(&err); });
    if (error)
        *error = err;
    return posts;
}

bool PostHistoryService::exportCsv(QTextStream &stream, bool includePasswords, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) {
        ok = worker->exportCsv(stream, includePasswords, &err);
    });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::importLegacyCsv(const QString &path, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) {
        ok = worker->importLegacyCsv(path, &err);
    });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::regenerateNzb(qint64 postId,
                                       QTextStream &stream,
                                       bool includePassword,
                                       QStringList *warnings,
                                       QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) {
        ok = worker->regenerateNzb(postId, stream, includePassword, warnings, &err);
    });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::regenerateNzbToFile(qint64 postId,
                                             const QString &outPath,
                                             bool includePassword,
                                             QStringList *warnings,
                                             QString *error,
                                             const QString &passwordOverride)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) {
        ok = worker->regenerateNzbToFile(postId,
                                         outPath,
                                         includePassword,
                                         warnings,
                                         &err,
                                         passwordOverride);
    });
    if (error)
        *error = err;
    return ok;
}

bool PostHistoryService::checkResume(qint64 postId, ResumeRow *row, QString *error)
{
    QString err;
    bool ok = false;
    _invokeBlocking([&](PostHistoryWorker *worker) { ok = worker->checkResume(postId, row, &err); });
    if (error)
        *error = err;
    return ok;
}

void PostHistoryService::requestHistorySnapshot(const PostHistoryStore::ListFilter &filter,
                                                const QSet<qint64> &ignoredResumeIds,
                                                QObject *receiver,
                                                HistorySnapshotCallback callback)
{
    if (!receiver || !callback)
        return;
    QPointer<QObject> guard(receiver);
    _invokeQueued([=](PostHistoryWorker *worker) {
        const HistorySnapshot snapshot = worker->historySnapshot(filter, ignoredResumeIds);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(), [guard, callback, snapshot]() {
            if (guard)
                callback(snapshot);
        }, Qt::QueuedConnection);
    });
}

void PostHistoryService::requestStatsSnapshot(const QString &dateFrom,
                                              const QString &dateTo,
                                              const QString &groupFilter,
                                              QObject *receiver,
                                              StatsSnapshotCallback callback)
{
    if (!receiver || !callback)
        return;
    QPointer<QObject> guard(receiver);
    _invokeQueued([=](PostHistoryWorker *worker) {
        const StatsSnapshot snapshot = worker->statsSnapshot(dateFrom, dateTo, groupFilter);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(), [guard, callback, snapshot]() {
            if (guard)
                callback(snapshot);
        }, Qt::QueuedConnection);
    });
}

void PostHistoryService::requestPostDetails(qint64 postId, QObject *receiver, DetailsCallback callback)
{
    if (!receiver || !callback)
        return;
    QPointer<QObject> guard(receiver);
    _invokeQueued([=](PostHistoryWorker *worker) {
        PostHistoryStore::PostDetails details;
        QString err;
        const bool ok = worker->loadPostDetails(postId, &details, &err);
        if (!guard)
            return;
        QMetaObject::invokeMethod(guard.data(), [guard, callback, ok, details, err]() {
            if (guard)
                callback(ok, details, err);
        }, Qt::QueuedConnection);
    });
}
