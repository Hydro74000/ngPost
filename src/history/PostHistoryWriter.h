//========================================================================
//
// Asynchronous batched posting-history writer.
//
//========================================================================

#ifndef POSTHISTORYWRITER_H
#define POSTHISTORYWRITER_H

#include "history/PostHistoryStore.h"

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QTimer>

class PostHistoryWriter : public QObject
{
    Q_OBJECT

public:
    explicit PostHistoryWriter(PostHistoryStore *store, QObject *parent = nullptr);

signals:
    void error(QString msg);

public slots:
    void start();
    void stop();

    void enqueueArticlePosting(qint64 fileId,
                               int part,
                               QString msgId,
                               int attemptNo,
                               qint64 pos,
                               qint64 bytes);
    void enqueueArticlePosted(qint64 fileId, int part, QString msgId, qint64 pos, qint64 bytes);
    void enqueueArticleFailed(qint64 fileId,
                              int part,
                              QString msgId,
                              QString reason,
                              qint64 pos,
                              qint64 bytes);
    void enqueueArticleUnknown(qint64 fileId,
                               int part,
                               QString msgId,
                               QString reason,
                               qint64 pos,
                               qint64 bytes);

    bool flush();

private:
    void _enqueue(const PostHistoryStore::ArticleEvent &event);
    bool _ensureInitialized();
    void _adaptBatchSize(int flushedCount, qint64 elapsedMs);

    PostHistoryStore *_store;
    QList<PostHistoryStore::ArticleEvent> _events;
    QTimer _flushTimer;
    QElapsedTimer _lastFlushTimer;
    bool _initialized;
    int _batchSize;
};

#endif // POSTHISTORYWRITER_H
