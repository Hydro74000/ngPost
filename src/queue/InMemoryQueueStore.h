// Copyright (C) 2026 Hydro74000 <acymap@gmail.com>
//========================================================================
//
// QueueStore kept in memory: atomic by construction, with failure injection
// so tests can prove nothing is published when a commit fails. Also the
// store of the internal J0 milestone, which must not touch any database.
//
//========================================================================

#ifndef QUEUE_INMEMORYQUEUESTORE_H
#define QUEUE_INMEMORYQUEUESTORE_H

#include "queue/QueueStore.h"

#include <QHash>

namespace queue
{

class InMemoryQueueStore : public QueueStore
{
public:
    QueueCommit commit(const QueueTransaction &transaction) override;

    QList<QueueEntry> loadEntries(const QString &queueId) const override;
    QList<qint64> loadOrder(const QString &queueId, Domain domain) const override;
    std::optional<PostNowIntent> loadIntent(const QString &queueId) const override;

    //! The next @p count commits fail with @p error and change nothing.
    void failNextCommits(int count, const QString &error = QStringLiteral("injected_failure"));

    int commitCount() const { return _commits; }
    //! Results ever recorded for a run: an interrupted run must appear once.
    int interruptionsRecorded(const QString &runId) const { return _interruptions.value(runId); }
    bool hasEntry(qint64 postId) const { return _entries.contains(postId); }

private:
    static QString orderKey(const QString &queueId, Domain domain);
    QString validate(const QueueTransaction &transaction) const;

    QHash<qint64, QueueEntry> _entries;
    QHash<QString, QList<qint64>> _orders;
    QHash<QString, PostNowIntent> _intents; //!< queue id -> intent
    QHash<QString, int> _interruptions;
    QHash<qint64, int> _runNumbers; //!< post id -> last run number
    qint64 _nextPostId = 1;
    int _failures = 0;
    QString _failureError;
    int _commits = 0;
};

} // namespace queue

#endif // QUEUE_INMEMORYQUEUESTORE_H
