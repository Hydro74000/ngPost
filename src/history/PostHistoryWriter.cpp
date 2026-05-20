//========================================================================
//
// Asynchronous batched posting-history writer.
//
//========================================================================

#include "history/PostHistoryWriter.h"

namespace
{

constexpr int sFlushIntervalMs = 100;
constexpr int sMinBatchSize    = 32;
constexpr int sMaxBatchSize    = 512;

} // namespace

PostHistoryWriter::PostHistoryWriter(PostHistoryStore *store, QObject *parent)
    : QObject(parent)
    , _store(store)
    , _events()
    , _flushTimer(this)
    , _initialized(false)
    , _batchSize(sMinBatchSize)
{
    _flushTimer.setInterval(sFlushIntervalMs);
    connect(&_flushTimer, &QTimer::timeout, this, [this]() { flush(); });
}

void PostHistoryWriter::start()
{
    _ensureInitialized();
    _lastFlushTimer.start();
    _flushTimer.start();
}

void PostHistoryWriter::stop()
{
    _flushTimer.stop();
    flush();
}

void PostHistoryWriter::enqueueArticlePosting(qint64 fileId,
                                              int part,
                                              QString msgId,
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
    _enqueue(event);
}

void PostHistoryWriter::enqueueArticlePosted(qint64 fileId,
                                             int part,
                                             QString msgId,
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
    _enqueue(event);
}

void PostHistoryWriter::enqueueArticleFailed(qint64 fileId,
                                             int part,
                                             QString msgId,
                                             QString reason,
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
    _enqueue(event);
}

void PostHistoryWriter::enqueueArticleUnknown(qint64 fileId,
                                              int part,
                                              QString msgId,
                                              QString reason,
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
    _enqueue(event);
}

bool PostHistoryWriter::flush()
{
    if (_events.isEmpty())
        return true;
    if (!_ensureInitialized())
        return false;

    const QList<PostHistoryStore::ArticleEvent> batch = _events;
    _events.clear();
    const qint64 elapsed = _lastFlushTimer.restart();

    QString err;
    if (!_store->applyArticleEvents(batch, &err)) {
        QList<PostHistoryStore::ArticleEvent> pending = batch;
        pending.append(_events);
        _events = pending;
        emit error(tr("History writer flush failed: %1").arg(err));
        return false;
    }
    _adaptBatchSize(batch.size(), elapsed);
    return true;
}

void PostHistoryWriter::_adaptBatchSize(int flushedCount, qint64 elapsedMs)
{
    if (elapsedMs <= 0)
        return;
    const double rate = flushedCount / static_cast<double>(elapsedMs); // events/ms
    const int projected = static_cast<int>(rate * sFlushIntervalMs);
    _batchSize = qBound(sMinBatchSize, projected, sMaxBatchSize);
}

void PostHistoryWriter::_enqueue(const PostHistoryStore::ArticleEvent &event)
{
    if (!_store || !event.fileId || event.part <= 0)
        return;

    _events << event;
    if (_events.size() >= _batchSize)
        flush();
}

bool PostHistoryWriter::_ensureInitialized()
{
    if (_initialized)
        return true;
    if (!_store)
        return false;

    QString err;
    if (!_store->initialize(&err)) {
        emit error(tr("History writer initialization failed: %1").arg(err));
        return false;
    }
    _initialized = true;
    return true;
}
